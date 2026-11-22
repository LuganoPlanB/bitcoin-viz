// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_writer.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
constexpr int width = 161, height = 95;
std::vector<std::uint8_t> pattern(int frame) {
    std::vector<std::uint8_t> rgb(width * height * 3);
    // Large asymmetric red/blue fields establish orientation; a changing green
    // patch, checker/noise and a tiny "F" glyph establish order/detail.
    std::uint32_t random = 1234567u + static_cast<unsigned>(frame);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        auto* p = &rgb[static_cast<std::size_t>((y * width + x) * 3)];
        p[0] = y < height / 2 ? 230 : 20;
        p[1] = 20;
        p[2] = y < height / 2 ? 20 : 230;
        if (x >= 50 && x < 100 && y >= 10 && y < 35) {
            p[0] = p[2] = 10; p[1] = static_cast<std::uint8_t>(40 + frame * 2);
        }
        if (x >= 110 && y >= 10 && y < 80 && (frame & 1)) {
            random = random * 1664525u + 1013904223u;
            p[0] = static_cast<std::uint8_t>(random >> 24);
            p[1] = static_cast<std::uint8_t>(random >> 16);
            p[2] = static_cast<std::uint8_t>(random >> 8);
        }
        if (x >= 10 && x < 30 && y >= 10 && y < 35 &&
            (x < 14 || y < 14 || (y >= 20 && y < 24))) p[0] = p[1] = p[2] = 255;
    }
    return rgb;
}

class FaultBuffer : public std::stringbuf {
public:
    bool fail_write = false, fail_seek = false, fail_flush = false;
protected:
    std::streamsize xsputn(const char* data, std::streamsize n) override {
        return fail_write ? 0 : std::stringbuf::xsputn(data, n);
    }
    pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode mode) override {
        return fail_seek ? pos_type(off_type(-1)) : std::stringbuf::seekoff(off, dir, mode);
    }
    pos_type seekpos(pos_type pos, std::ios_base::openmode mode) override {
        return fail_seek ? pos_type(off_type(-1)) : std::stringbuf::seekpos(pos, mode);
    }
    int sync() override { return fail_flush ? -1 : std::stringbuf::sync(); }
};

void fixtures(const std::filesystem::path& directory) {
    studio::VideoWriter writer;
    studio::VideoSettings settings{width, height};
    check(writer.stop() && writer.stop(), "empty stop");
    check(!writer.write_frame(nullptr, 0), "write without open");
    check(!writer.start(directory / "missing" / "bad.avi", settings), "rejected path");
    check(!writer.error().empty() && !writer.active() && !writer.stop(), "path error retained");
    check(!writer.start(directory / "missing" / std::filesystem::u8path("bad caf\xc3\xa9.avi"), settings),
          "rejected Unicode path");
    check(writer.error().find("bad caf\xc3\xa9.avi") != std::string::npos,
          "Unicode filename retained in error");
    check(!writer.start(directory / "bad.avi", {0, height}), "invalid dimensions");
    check(!writer.start(directory / "bad.avi", {width, height, 30, 101}), "invalid quality");
    check(!writer.start(directory / "bad.avi", {width, height, 0}), "invalid frame rate");
    check(writer.start(directory / "empty.avi", settings), writer.error());
    check(writer.stop() && writer.frames() == 0, "empty open/stop");
    auto rgb = pattern(0);
    check(writer.start(directory / "single.avi", settings), writer.error());
    check(writer.write_frame(rgb.data(), rgb.size()), writer.error());
    check(writer.frames() == 1 && writer.stop() && writer.stop(), "single frame finalization");
    check(writer.start(directory / std::filesystem::u8path("take caf\xc3\xa9.avi"), settings), writer.error());
    check(writer.write_frame(rgb.data(), rgb.size()) && writer.stop(), "Unicode filename");
    check(writer.start(directory / "invalid-buffer.avi", settings), writer.error());
    check(!writer.write_frame(rgb.data(), 1) && !writer.active() && !writer.stop(), "short input failure");

    for (int mode = 0; mode < 3; ++mode) {
        FaultBuffer buffer;
        std::ostream stream(&buffer);
        check(writer.start(stream, settings), writer.error());
        check(writer.write_frame(rgb.data(), rgb.size()), writer.error());
        if (mode == 0) {
            buffer.fail_write = true;
            check(!writer.write_frame(rgb.data(), rgb.size()), "write failure propagation");
        } else {
            buffer.fail_seek = mode == 1;
            buffer.fail_flush = mode == 2;
            check(!writer.stop(), "seek/flush failure propagation");
        }
        check(!writer.active() && !writer.error().empty() && !writer.stop(), "failed save stays failed");
        // Release/finalize once: disabling the fault must not turn stop into success.
        buffer.fail_write = buffer.fail_seek = buffer.fail_flush = false;
        const auto bytes = buffer.str();
        check(!writer.stop() && bytes == buffer.str(), "idempotent failed finalization");
        std::ostringstream recovered;
        check(writer.start(recovered, settings) && writer.error().empty(), "reopen after failure");
        check(writer.write_frame(rgb.data(), rgb.size()) && writer.stop(), "recovered take");
    }
    FaultBuffer overflow;
    std::ostream limited(&overflow);
    auto tiny = settings; tiny.riff_limit_bytes = 4096;
    check(writer.start(limited, tiny), writer.error());
    const auto header_end = overflow.str().find("movi") + 4;
    std::string header;
    bool rejected = false;
    for (int i = 0; i < 140; ++i) {
        auto noisy = pattern(1);
        if (!writer.write_frame(noisy.data(), noisy.size())) { rejected = true; break; }
        if (i == 0) header = overflow.str().substr(0, header_end);
    }
    check(rejected && !writer.stop(), "reserved superindex capacity must fail safely");
    check(writer.frames() == 128 && overflow.str().substr(0, header_end) == header,
        "superindex exhaustion must preserve reserved header boundaries");

    for (const auto& name : {"normal", "rollover", "quality"}) {
        auto current = settings;
        if (std::string(name) == "rollover") current.riff_limit_bytes = 24000;
        if (std::string(name) == "quality") current.quality = 45;
        check(writer.start(directory / (std::string(name) + ".avi"), current), writer.error());
        for (int frame = 0; frame < 90; ++frame) {
            auto image = pattern(frame);
            // Alternating storage orientation and padded input rows exercise both paths.
            std::vector<std::uint8_t> padded(static_cast<std::size_t>(height * (width * 3 + 7)));
            for (int row = 0; row < height; ++row) {
                const int destination = (frame & 1) ? height - 1 - row : row;
                std::copy_n(image.data() + row * width * 3, width * 3,
                    padded.data() + destination * (width * 3 + 7));
            }
            check(writer.write_frame(padded.data(), padded.size(), width * 3 + 7, (frame & 1) != 0), writer.error());
        }
        check(writer.frames() == 90 && writer.stop(), "sequence stop");
    }
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected fixture output directory");
        std::filesystem::create_directories(argv[1]);
        fixtures(argv[1]);
        std::cout << "Writer contracts passed: paths, buffers, empty/single, failures/recovery, superindex bound, three 90-frame sequences\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
