// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_capture.hpp"
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <new>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
constexpr int width = 17, height = 9;
constexpr std::size_t pixel_bytes = width * height * 3;
std::atomic<unsigned> allocations{0};
constexpr auto never_fail = std::numeric_limits<unsigned>::max();
std::atomic<unsigned> fail_at{never_fail};
std::array<std::atomic<void*>, 3> live_buffers{};
}

// Track just the fixed-size pixel arrays and inject partial startup failure.
void* operator new[](std::size_t size) {
    if (size == pixel_bytes && allocations == fail_at.load())
        throw std::bad_alloc();
    auto* memory = std::malloc(size ? size : 1);
    if (!memory) throw std::bad_alloc();
    if (size == pixel_bytes) {
        ++allocations;
        for (auto& buffer : live_buffers) {
            void* empty = nullptr;
            if (buffer.compare_exchange_strong(empty, memory)) break;
        }
    }
    return memory;
}

void operator delete[](void* memory) noexcept {
    if (memory) {
        for (auto& buffer : live_buffers) {
            void* expected = memory;
            if (buffer.compare_exchange_strong(expected, nullptr)) break;
        }
    }
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept { ::operator delete[](memory); }

namespace {
using studio::VideoCapture;
using studio::VideoSettings;
using namespace std::chrono_literals;
constexpr VideoSettings settings{width, height};

void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

unsigned live_pixel_count() {
    unsigned count = 0;
    for (const auto& buffer : live_buffers) count += buffer.load() != nullptr;
    return count;
}

class Consumer : public std::stringbuf {
public:
    void hold_next_frame() {
        const std::lock_guard<std::mutex> lock(mutex);
        hold = true;
    }

    bool wait_for_hold() {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 3s, [&] { return held; });
    }

    void release(bool fail = false) {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            hold = false;
            fail_jpeg = fail;
        }
        changed.notify_all();
    }

    bool wait_for_frames(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 3s, [&] { return frames.size() >= count; });
    }

    // Used after joining the capture worker.
    std::vector<std::string> frames;
    bool held = false;
    std::thread::id writer_thread;

protected:
    std::streamsize xsputn(const char* data, std::streamsize n) override {
        if (n < 2 || static_cast<unsigned char>(data[0]) != 0xff ||
            static_cast<unsigned char>(data[1]) != 0xd8)
            return std::stringbuf::xsputn(data, n);
        std::unique_lock<std::mutex> lock(mutex);
        writer_thread = std::this_thread::get_id();
        if (hold) {
            held = true;
            changed.notify_all();
            const bool released = changed.wait_for(lock, 5s, [&] { return !hold; });
            held = false;
            if (!released) return 0;
        }
        if (fail_jpeg) return 0;
        const auto written = std::stringbuf::xsputn(data, n);
        if (written == n) frames.emplace_back(data, n);
        changed.notify_all();
        return written;
    }

private:
    std::mutex mutex;
    std::condition_variable changed;
    bool hold = false, fail_jpeg = false;
};

struct ReleaseGate {
    Consumer& consumer;
    ~ReleaseGate() { consumer.release(); }
};

struct Take {
    Consumer consumer;
    std::ostream output{&consumer};
    VideoCapture::Clock::time_point now{};
    VideoCapture capture{[this] { return now; }};
    ReleaseGate release{consumer};
    const std::thread::id render_thread = std::this_thread::get_id();
    unsigned reads = 0;
    std::set<const std::uint8_t*> addresses;

    void at_frame(int frame, int hz = 30) {
        const auto ns = (static_cast<std::int64_t>(frame) * 1000000000 + hz - 1) / hz;
        now = VideoCapture::Clock::time_point(
            std::chrono::ceil<VideoCapture::Clock::duration>(std::chrono::nanoseconds(ns)));
    }
    bool sample() {
        return capture.sample([&](std::uint8_t* pixels, int stride) {
            check(render_thread == std::this_thread::get_id(), "readback left render thread");
            check(stride == width * 3, "incorrect RGB pitch for an odd width");
            addresses.insert(pixels);
            std::memset(pixels, 30 + 6 * reads++, pixel_bytes);
            return true;
        });
    }
    void start() { check(capture.start(output, settings), "start capture"); }
    void check_finalized() {
        check(!capture.active() && !consumer.held && live_pixel_count() == 0,
              "capture resources did not settle");
        check(consumer.str().find("idx1") != std::string::npos, "AVI was not finalized");
        if (reads)
            check(consumer.writer_thread != render_thread, "JPEG chunk written on render thread");
    }
};

bool wait_for_stop_input(const VideoCapture& capture) {
    const auto deadline = VideoCapture::Clock::now() + 3s;
    while (capture.active() && VideoCapture::Clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    return !capture.active();
}

void rates(int hz) {
    Take take;
    const auto before = allocations.load();
    check(!take.sample() && take.capture.stop(), "idle capture");
    check(allocations == before && take.reads == 0 && take.consumer.frames.empty(), "idle does work");
    take.start();
    check(allocations == before + 3 && live_pixel_count() == 3, "pool allocation count");
    int previous_slot = -1;
    for (int frame = 0; frame < hz; ++frame) {
        take.at_frame(frame, hz);
        const int slot = frame * 30 / hz;
        check(take.sample() == (slot != previous_slot), "unexpected sample cadence");
        if (slot != previous_slot)
            check(take.consumer.wait_for_frames(slot + 1), "consumer did not reach slot");
        previous_slot = slot;
    }
    take.now = VideoCapture::Clock::time_point(999ms);
    check(take.capture.stop() && take.capture.stop(), "cadence finalization");
    check(take.reads == static_cast<unsigned>(hz < 30 ? hz : 30), "incorrect readback count");
    check(take.consumer.frames.size() == 30, "one second must contain 30 frames");
    for (std::size_t slot = 1; slot < 30; ++slot) {
        const auto& frames = take.consumer.frames;
        const bool repeat = hz == 15 && slot % 2 == 1;
        check((frames[slot] == frames[slot - 1]) == repeat, "incorrect repeated JPEG slot");
    }
    check(allocations == before + 3, "pixel buffers reallocated");
    take.check_finalized();
    std::cout << hz << " Hz: " << take.reads << " readbacks, 30 AVI frames, 3 allocations, 0 held\n";
}

void saturation() {
    Take take;
    const auto before = allocations.load();
    take.start();
    take.consumer.hold_next_frame();
    check(take.sample() && take.consumer.wait_for_hold(), "worker did not enter held write");
    take.at_frame(1);
    check(take.sample(), "second buffer unavailable");
    take.at_frame(2);
    check(take.sample(), "third buffer unavailable");
    for (int slot = 3; slot < 120; ++slot) {
        take.at_frame(slot);
        check(!take.sample(), "saturated pool performed readback");
    }
    check(take.reads == 3 && take.addresses.size() == 3 &&
          allocations == before + 3 && live_pixel_count() == 3, "saturated pool size");
    bool closed_before_release = false;
    std::thread release([&] {
        closed_before_release = wait_for_stop_input(take.capture);
        take.consumer.release();
    });
    const bool ok = take.capture.stop();
    release.join();
    check(closed_before_release && ok, "stop did not close input while write was held");
    check(take.consumer.frames.size() == 120, "stop cutoff did not cover elapsed slots");
    check(take.consumer.frames[0] != take.consumer.frames[1] &&
          take.consumer.frames[1] != take.consumer.frames[2], "queued samples lost or reordered");
    for (std::size_t i = 3; i < take.consumer.frames.size(); ++i)
        check(take.consumer.frames[i] == take.consumer.frames[2], "dropped frame was not duplicated");
    take.check_finalized();
    std::cout << "Saturation: 3 buffers/readbacks, 117 repeated frames, stop during write, 0 held\n";
}

void empty_and_late() {
    Take empty;
    empty.start();
    check(empty.capture.stop() && empty.consumer.frames.empty(), "immediate stop manufactured a JPEG");
    empty.check_finalized();

    Take late;
    late.start();
    late.at_frame(3);
    check(late.sample() && late.capture.stop(), "late first sample");
    check(late.consumer.frames.size() == 4, "leading slots not covered");
    for (const auto& frame : late.consumer.frames)
        check(frame == late.consumer.frames.front(), "leading slots changed first image");
    late.check_finalized();
    std::cout << "Immediate stop: 0 frames, late first sample: 4 frames, 0 held\n";
}

void failure_and_restart() {
    Take take;
    take.start();
    take.consumer.hold_next_frame();
    check(take.sample() && take.consumer.wait_for_hold(), "failure precondition: worker did not reach write");
    take.at_frame(1);
    bool closed_during_read = false;
    check(!take.capture.sample([&](std::uint8_t* pixels, int) {
        take.consumer.release(true);
        closed_during_read = wait_for_stop_input(take.capture);
        std::memset(pixels, 0, pixel_bytes);
        return true;
    }), "capture accepted pixels after write failure");
    check(closed_during_read && !take.capture.stop() && !take.capture.stop(), "failed write was not sticky");
    check(take.capture.error() == "AVI frame write failed" &&
          live_pixel_count() == 0 && !take.consumer.held, "failed take did not release its buffers");
    Consumer recovered;
    std::ostream recovered_output(&recovered);
    check(take.capture.start(recovered_output, settings), "restart after write failure");
    const bool cleared = take.capture.error().empty();
    const bool read = take.sample();
    const bool stopped = take.capture.stop();
    check(cleared && read && stopped && recovered.frames.size() == 1, "restart did not produce a new frame");
    check(live_pixel_count() == 0, "restarted capture leaked buffers");
    std::cout << "Injected write failure during readback: sticky failure, successful restart, 0 held\n";
}

void allocation_failure() {
    Take take;
    const auto before = allocations.load();
    fail_at = before + 1;
    const bool started = take.capture.start(take.output, settings);
    fail_at = never_fail;
    check(!started && allocations == before + 1 && live_pixel_count() == 0 &&
          !take.capture.stop() && !take.capture.error().empty() && !take.sample() &&
          take.consumer.str().empty(), "partial allocation failure was not cleaned up");
    take.start();
    check(!take.capture.sample([](std::uint8_t*, int) { return false; }) &&
          !take.capture.stop() && take.capture.error() == "Cannot read video pixels",
          "failed readback was not surfaced");
    take.check_finalized();
    std::cout << "Injected partial allocation and readback failures: 0 held, no leaked buffers\n";
}
}

int main() {
    try {
        for (int hz : {15, 30, 60}) rates(hz);
        saturation();
        empty_and_late();
        failure_and_restart();
        allocation_failure();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
