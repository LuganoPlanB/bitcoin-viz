// SPDX-License-Identifier: GPL-3.0-or-later
#include "window_video_capture.hpp"
#include <GL/glew.h>
#include <SDL.h>
#include <array>
#include <iostream>
#include <utility>

namespace studio {
namespace {
bool read_back_buffer(int width, int height, std::uint8_t* rgb, int stride) {
    if (stride != width * 3) return false;

    GLint buffer, pixel_buffer;
    glGetIntegerv(GL_READ_BUFFER, &buffer);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pixel_buffer);
    constexpr std::array<GLenum, 4> pack{
        GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS};
    constexpr std::array<GLint, 4> packed_rgb{1, 0, 0, 0};
    std::array<GLint, pack.size()> saved;
    for (std::size_t entry = 0; entry < pack.size(); ++entry) {
        glGetIntegerv(pack[entry], &saved[entry]);
        glPixelStorei(pack[entry], packed_rgb[entry]);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    const bool read = glGetError() == GL_NO_ERROR;
    glReadBuffer(static_cast<GLenum>(buffer));
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(pixel_buffer));
    for (std::size_t entry = 0; entry < pack.size(); ++entry)
        glPixelStorei(pack[entry], saved[entry]);
    return read;
}
}

void WindowVideoCapture::report(std::string status) {
    message = std::move(status);
    std::cerr << "Video capture: " << message << '\n';
}

bool WindowVideoCapture::start(const std::filesystem::path& path, int quality, bool include_overlay) {
    if (capture.active()) return false;
    if (take_open) stop("Stopped");
    SDL_GL_GetDrawableSize(window, &width, &height);
    if (width <= 0 || height <= 0 || (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) {
        report("Cannot start while the window drawable is unavailable");
        return false;
    }
    selected = include_overlay ? VideoPicture::Composited : VideoPicture::Scene;
    VideoSettings settings{width, height};
    settings.quality = quality;
    if (!capture.start(path, settings)) {
        report(capture.error());
        return false;
    }
    take_open = true;
    report("Started " + std::to_string(width) + "x" + std::to_string(height));
    return true;
}

bool WindowVideoCapture::stop(std::string_view reason) {
    if (!take_open) return capture.error().empty();
    take_open = false;
    const bool saved = capture.stop();
    report(saved ? std::string(reason) : capture.error());
    return saved;
}

void WindowVideoCapture::update_drawable(int drawable_width, int drawable_height, bool minimized) {
    if (!take_open) return;
    if (minimized) stop("Window minimized");
    else if (drawable_width <= 0 || drawable_height <= 0) stop("Drawable unavailable");
    else if (width != drawable_width || height != drawable_height) stop("Drawable size changed");
    else if (!capture.active()) stop("Stopped");
}

bool WindowVideoCapture::sample(VideoPicture picture) {
    return take_open && picture == selected && capture.sample([this](std::uint8_t* rgb, int pitch) {
        return read_back_buffer(width, height, rgb, pitch);
    });
}

}
