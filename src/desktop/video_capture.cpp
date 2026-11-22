// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_capture.hpp"
#include <array>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace studio {
namespace {
constexpr std::size_t buffer_count = 3;
using Pixels = std::unique_ptr<std::uint8_t[]>;
using Slot = std::uint64_t;
struct Frame {
    Pixels pixels;
    Slot slot = 0;
};
}

struct VideoCapture::State {
    VideoSettings settings;
    int stride = 0;
    std::size_t bytes = 0;
    Clock::time_point origin;
    std::optional<Slot> last_attempt;
    std::thread worker;
#ifdef BITCOIN_VIZ_TEST_VIDEO_CAPTURE
    VideoCaptureMeasurements* profile = nullptr;
#endif

    // The mutex covers buffer ownership transfers and session status only.
    // Neither the reader nor the encoder holds it while working on pixels.
    mutable std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool active = false;
    std::string error;
    std::optional<Slot> cutoff;
    std::array<Pixels, buffer_count> free;
    std::size_t free_count = 0;
    std::array<Frame, buffer_count> pending;
    std::size_t head = 0, pending_count = 0;

    bool allocate();
    Slot slot_at(Clock::time_point time) const;
    void run(const OpenOutput& open_output);
    bool drain(VideoWriter& writer);
    void finish(std::string failure);
};

bool VideoCapture::State::allocate() {
    // AVI uses signed 16-bit dimensions, which also bounds the packed stride.
    if (settings.width <= 0 || settings.width > 32767 ||
        settings.height <= 0 || settings.height > 32767) {
        error = "Invalid video capture dimensions";
        return false;
    }
    stride = settings.width * 3;
    if (static_cast<std::size_t>(settings.height) >
        std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(stride)) {
        error = "Video capture buffer is too large";
        return false;
    }
    bytes = static_cast<std::size_t>(stride) * static_cast<std::size_t>(settings.height);
    for (auto& pixels : free) pixels.reset(new std::uint8_t[bytes]);
    free_count = buffer_count;
    return true;
}

Slot VideoCapture::State::slot_at(Clock::time_point time) const {
    using namespace std::chrono;
    if (time <= origin) return 0;
    const auto elapsed = time - origin;
    const auto seconds = duration_cast<std::chrono::seconds>(elapsed);
    const auto nanos = duration_cast<nanoseconds>(elapsed - seconds);
    return static_cast<Slot>(seconds.count()) * settings.fps +
           static_cast<Slot>(nanos.count()) * settings.fps / 1000000000;
}

void VideoCapture::State::run(const OpenOutput& open_output) {
    try {
        VideoWriter writer;
        if (!open_output(writer, settings)) {
            finish(writer.error());
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(mutex);
            active = true;
            started = true;
        }
        changed.notify_one();
        const bool drained = drain(writer);
        const bool finalized = writer.stop();
        if (drained && finalized) finish({});
        else finish(writer.error().empty() ? "Cannot write video take" : writer.error());
    } catch (const std::exception& e) {
        finish(std::string("Video capture worker failed: ") + e.what());
    }
}

bool VideoCapture::State::drain(VideoWriter& writer) {
    const auto repeat_until = [&](Slot count) {
        while (writer.frames() < count)
            if (!writer.repeat_frame()) return false;
        return true;
    };
    while (true) {
        Frame frame;
        std::optional<Slot> stop_slot;
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&] { return pending_count || cutoff.has_value(); });
            if (pending_count) {
                frame = std::move(pending[head]);
                head = (head + 1) % buffer_count;
                --pending_count;
            } else {
                stop_slot = cutoff;
            }
        }
        if (stop_slot) return !writer.frames() || repeat_until(*stop_slot + 1);

        // If the first drawable arrives late, its JPEG also covers the leading
        // slots. Later gaps use the previous image up to the new frame's slot.
        const bool first = writer.frames() == 0;
        if (!first && !repeat_until(frame.slot)) return false;
#ifdef BITCOIN_VIZ_TEST_VIDEO_CAPTURE
        const auto before_encode = profile ? Clock::now() : Clock::time_point{};
#endif
        if (!writer.write_frame(frame.pixels.get(), bytes, stride, true)) return false;
#ifdef BITCOIN_VIZ_TEST_VIDEO_CAPTURE
        if (profile) profile->encode_ms.push_back(
            std::chrono::duration<double,std::milli>(Clock::now() - before_encode).count());
#endif
        if (first && !repeat_until(frame.slot + 1)) return false;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            free[free_count++] = std::move(frame.pixels);
        }
    }
}

void VideoCapture::State::finish(std::string failure) {
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (error.empty()) error = std::move(failure);
        active = false;
        started = true;
        for (auto& pixels : free) pixels.reset();
        for (auto& frame : pending) frame.pixels.reset();
        free_count = pending_count = 0;
    }
    changed.notify_one();
}

VideoCapture::VideoCapture(std::function<Clock::time_point()> clock) : now(std::move(clock)) {}

VideoCapture::~VideoCapture() { stop(); }

bool VideoCapture::start(const std::filesystem::path& path, VideoSettings settings) {
    return start(settings, [&](VideoWriter& writer, VideoSettings s) { return writer.start(path, s); });
}

bool VideoCapture::start(std::ostream& output, VideoSettings settings) {
    return start(settings, [&](VideoWriter& writer, VideoSettings s) { return writer.start(output, s); });
}

bool VideoCapture::start(VideoSettings settings, const OpenOutput& open_output) {
    if (active()) return false;
    stop(); // Join a failed worker before replacing the take.
    state = std::make_unique<State>();
    state->settings = settings;
#ifdef BITCOIN_VIZ_TEST_VIDEO_CAPTURE
    state->profile = profile;
#endif
    try {
        if (!state->allocate()) return false;
        state->worker = std::thread([take = state.get(), open_output] { take->run(open_output); });
    } catch (const std::exception& e) {
        state->finish(std::string("Cannot start video capture: ") + e.what());
        return false;
    }
    std::unique_lock<std::mutex> lock(state->mutex);
    state->changed.wait(lock, [&] { return state->started; });
    state->origin = now();
    return state->active;
}

bool VideoCapture::sample(const ReadPixels& read_pixels) {
    if (!state) return false;
    auto& take = *state;
    Pixels pixels;
    Slot slot;
    {
        const std::lock_guard<std::mutex> lock(take.mutex);
        if (!take.active) return false;
        slot = take.slot_at(now());
        if (take.last_attempt && slot <= *take.last_attempt) return false;
        take.last_attempt = slot;
        if (!take.free_count) return false;
        pixels = std::move(take.free[--take.free_count]);
    }
    bool read = false;
    try { read = read_pixels(pixels.get(), take.stride); } catch (...) {}
    {
        const std::lock_guard<std::mutex> lock(take.mutex);
        // A sink failure may have closed the take during readback. This buffer
        // is locally owned until submission and can be released here safely.
        if (!take.active) return false;
        if (read) {
            take.pending[(take.head + take.pending_count) % buffer_count] = {std::move(pixels), slot};
            ++take.pending_count;
#ifdef BITCOIN_VIZ_TEST_VIDEO_CAPTURE
            if (take.profile && take.pending_count > take.profile->peak_queued)
                take.profile->peak_queued = take.pending_count;
#endif
        } else {
            take.error = "Cannot read video pixels";
            take.active = false;
            take.cutoff = slot;
        }
    }
    take.changed.notify_one();
    return read;
}

bool VideoCapture::stop() {
    if (!state) return true;
    auto& take = *state;
    {
        const std::lock_guard<std::mutex> lock(take.mutex);
        if (take.active) {
            take.cutoff = take.slot_at(now());
            take.active = false;
        }
    }
    take.changed.notify_one();
    if (take.worker.joinable()) take.worker.join();
    return take.error.empty();
}

bool VideoCapture::active() const {
    if (!state) return false;
    const std::lock_guard<std::mutex> lock(state->mutex);
    return state->active;
}

std::string VideoCapture::error() const {
    if (!state) return {};
    const std::lock_guard<std::mutex> lock(state->mutex);
    return state->error;
}

}
