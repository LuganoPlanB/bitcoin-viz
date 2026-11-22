// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "video_capture.hpp"
#include <string_view>

struct SDL_Window;

namespace studio {

enum class VideoPicture { Scene, Composited };
enum class VideoTakeStatus { Idle, Recording, Saved, Error };

// Lives on the SDL/OpenGL thread and finishes the take before window teardown.
class WindowVideoCapture {
public:
    explicit WindowVideoCapture(SDL_Window* window);
    ~WindowVideoCapture();

    bool start(const std::filesystem::path& directory, std::string_view custom_path,
               int quality, bool include_overlay);
    bool stop(std::string_view reason);
    void update_drawable(int width, int height, bool minimized);
    bool sample(VideoPicture picture);
    bool active() const { return capture.active(); }
    VideoTakeStatus status() const { return take_status; }
    const std::filesystem::path& path() const { return output_path; }
    std::chrono::seconds elapsed() const;
    const std::string& detail() const { return message; }
#ifdef BITCOIN_VIZ_TEST_VIDEO_CAPTURE
    void measure(VideoCaptureMeasurements* measurements) { capture.measure(measurements); }
#endif

private:
    struct Output;
    void report(VideoTakeStatus status, std::string detail);
    bool fail(std::string detail);

    SDL_Window* window;
    std::unique_ptr<Output> output;
    VideoCapture capture;
    VideoPicture selected = VideoPicture::Scene;
    int width = 0, height = 0;
    bool take_open = false;
    VideoTakeStatus take_status = VideoTakeStatus::Idle;
    std::filesystem::path output_path;
    VideoCapture::Clock::time_point started;
    std::chrono::seconds stopped_elapsed{};
    std::string message;
};

}
