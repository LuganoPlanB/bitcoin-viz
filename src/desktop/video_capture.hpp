// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "video_writer.hpp"
#include <chrono>
#include <functional>

namespace studio {

// One producer supplies pixels on its own thread. The capture worker owns the
// codec and output. Calls to start, sample and stop must be on the producer thread.
class VideoCapture {
public:
    using Clock = std::chrono::steady_clock;
    using ReadPixels = std::function<bool(std::uint8_t* rgb, int stride)>;

    explicit VideoCapture(std::function<Clock::time_point()> now = Clock::now);
    ~VideoCapture();
    VideoCapture(const VideoCapture&) = delete;
    VideoCapture& operator=(const VideoCapture&) = delete;

    bool start(const std::filesystem::path& path, VideoSettings settings);
    // The stream must remain alive and untouched until stop returns.
    bool start(std::ostream& output, VideoSettings settings);
    // Called synchronously and at most once per due slot, only with a reserved
    // buffer. Supply bottom-up RGB24 with the given pitch and return true on success.
    bool sample(const ReadPixels& read_pixels);
    // Close input at the current time and wait for the worker to finalize.
    // The error is retained until the next start attempt.
    bool stop();
    bool active() const;
    std::string error() const;

private:
    struct State;
    using OpenOutput = std::function<bool(VideoWriter&, VideoSettings)>;
    std::function<Clock::time_point()> now;
    std::unique_ptr<State> state;

    bool start(VideoSettings settings, const OpenOutput& open_output);
};

}
