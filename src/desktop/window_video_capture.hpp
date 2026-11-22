// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "video_capture.hpp"
#include <string_view>

struct SDL_Window;

namespace studio {

enum class VideoPicture { Scene, Composited };

// Lives on the SDL/OpenGL thread and finishes the take before window teardown.
class WindowVideoCapture {
public:
    explicit WindowVideoCapture(SDL_Window* window) : window(window) {}

    bool start(const std::filesystem::path& path, int quality, bool include_overlay);
    bool stop(std::string_view reason);
    void update_drawable(int width, int height, bool minimized);
    bool sample(VideoPicture picture);
    bool active() const { return capture.active(); }
    const std::string& status() const { return message; }

private:
    void report(std::string status);

    SDL_Window* window;
    VideoCapture capture;
    VideoPicture selected = VideoPicture::Scene;
    int width = 0, height = 0;
    bool take_open = false;
    std::string message;
};

}
