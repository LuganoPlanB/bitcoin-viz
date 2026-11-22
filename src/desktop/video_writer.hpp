// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>

namespace studio {
struct VideoSettings {
    int width = 0, height = 0;
    int fps = 30;
    int quality = 95;
    // Zero uses avilib's normal 1 GB / 4 GB OpenDML segment limits.
    std::uint64_t riff_limit_bytes = 0;
};

// A silent, fixed-size RGB24 take. All operations run on the calling thread.
// Each successful write adds one frame at the configured constant frame rate.
class VideoWriter {
public:
    VideoWriter();
    ~VideoWriter();
    VideoWriter(const VideoWriter&) = delete;
    VideoWriter& operator=(const VideoWriter&) = delete;
    bool start(const std::filesystem::path& path, VideoSettings settings);
    // The caller keeps this seekable stream alive until stop/destruction.
    bool start(std::ostream& output, VideoSettings settings);
    // Input must contain (height-1)*stride + width*3 bytes. Zero stride is packed.
    bool write_frame(const std::uint8_t* rgb, std::size_t bytes,
                     int stride = 0, bool bottom_up = false);
    // Idempotent. A failed take keeps returning false until a new start.
    bool stop();
    bool active() const;
    std::uint64_t frames() const;
    const std::string& error() const;
private:
    struct State;
    std::unique_ptr<State> state;
    bool prepare(VideoSettings settings);
    bool fail(const std::string& message);
};
}
