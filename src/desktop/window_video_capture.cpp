// SPDX-License-Identifier: GPL-3.0-or-later
#include "window_video_capture.hpp"
#include <GL/glew.h>
#include <SDL.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif

namespace studio {
namespace {
std::string dated_stem() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &now) != 0) throw std::runtime_error("Cannot read local time");
#else
    if (!localtime_r(&now, &local)) throw std::runtime_error("Cannot read local time");
#endif
    char stem[40];
    if (!std::strftime(stem, sizeof(stem), "bitcoin-viz-%Y-%m-%d-%H%M%S", &local))
        throw std::runtime_error("Cannot format the take timestamp");
    return stem;
}

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

// Keep the exclusively created file open through finalization. Reopening the
// pathname to hand it to avilib would allow a different file to be truncated.
struct WindowVideoCapture::Output : std::streambuf {
    std::ostream stream{this};
    std::FILE* file = nullptr;

    ~Output() override { close(); }

    int open(const std::filesystem::path& path) {
#ifdef _WIN32
        const int descriptor = _wopen(path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT,
                                     _S_IREAD | _S_IWRITE);
        if (descriptor < 0) return errno;
        file = _fdopen(descriptor, "wb");
#else
        const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (descriptor < 0) return errno;
        file = fdopen(descriptor, "wb");
#endif
        if (file) return 0;
        const int error = errno;
#ifdef _WIN32
        _close(descriptor);
#else
        ::close(descriptor);
#endif
        return error;
    }

    bool close() { return !file || std::fclose(std::exchange(file, nullptr)) == 0; }

    std::streamsize xsputn(const char* bytes, std::streamsize length) override {
        return static_cast<std::streamsize>(std::fwrite(bytes, 1, static_cast<std::size_t>(length), file));
    }

    int_type overflow(int_type byte) override {
        if (traits_type::eq_int_type(byte, traits_type::eof())) return traits_type::not_eof(byte);
        return std::fputc(traits_type::to_char_type(byte), file) == EOF ? traits_type::eof() : byte;
    }

    int sync() override { return std::fflush(file); }

    pos_type seekoff(off_type offset, std::ios_base::seekdir direction, std::ios_base::openmode) override {
        const int from = direction == std::ios_base::beg ? SEEK_SET :
            direction == std::ios_base::cur ? SEEK_CUR : SEEK_END;
#ifdef _WIN32
        if (_fseeki64(file, offset, from) == 0) return _ftelli64(file);
#else
        if (std::in_range<off_t>(offset) && fseeko(file, static_cast<off_t>(offset), from) == 0)
            return ftello(file);
#endif
        return pos_type(off_type(-1));
    }

    pos_type seekpos(pos_type position, std::ios_base::openmode mode) override {
        return seekoff(position, std::ios_base::beg, mode);
    }
};

WindowVideoCapture::WindowVideoCapture(SDL_Window* window) : window(window) {}
WindowVideoCapture::~WindowVideoCapture() = default;

void WindowVideoCapture::report(VideoTakeStatus status, std::string detail) {
    take_status = status;
    message = std::move(detail);
    std::cerr << "Video capture: " << message << '\n';
}

bool WindowVideoCapture::fail(std::string detail) {
    report(VideoTakeStatus::Error, std::move(detail));
    return false;
}

std::chrono::seconds WindowVideoCapture::elapsed() const {
    return take_open ? std::chrono::duration_cast<std::chrono::seconds>(VideoCapture::Clock::now() - started)
                     : stopped_elapsed;
}

bool WindowVideoCapture::start(const std::filesystem::path& directory, std::string_view custom_path,
                               int quality, bool include_overlay) {
    namespace fs = std::filesystem;
    if (capture.active()) return false;
    if (take_open) stop("Stopped");
    output_path.clear();
    stopped_elapsed = {};
    SDL_GL_GetDrawableSize(window, &width, &height);
    if (width <= 0 || height <= 0 || (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED))
        return fail("Cannot start while the window drawable is unavailable");
    selected = include_overlay ? VideoPicture::Composited : VideoPicture::Scene;
    VideoSettings settings{width, height};
    settings.quality = quality;
    try {
        auto file = std::make_unique<Output>();
        int error;
        if (custom_path.empty()) {
            const auto folder = fs::absolute(directory);
            output_path = folder;
            fs::create_directories(folder);
            const auto stem = dated_stem();
            for (std::uint64_t suffix = 0; ; ++suffix) {
                const auto ending = suffix ? "-" + std::to_string(suffix) : "";
                output_path = folder / (stem + ending + ".avi");
                error = file->open(output_path);
                if (error != EEXIST) break;
            }
        } else {
            output_path = fs::absolute(fs::u8path(custom_path));
            error = file->open(output_path);
        }
        if (error == EEXIST) return fail("Output already exists");
        if (error) return fail("Cannot create video output: " + std::generic_category().message(error));
        if (!capture.start(file->stream, settings)) {
            capture.stop();
            return fail(capture.error());
        }
        output = std::move(file);
    } catch (const std::exception& e) {
        return fail(std::string("Cannot start video: ") + e.what());
    }
    started = VideoCapture::Clock::now();
    take_open = true;
    report(VideoTakeStatus::Recording, "Started " + std::to_string(width) + "x" + std::to_string(height));
    return true;
}

bool WindowVideoCapture::stop(std::string_view reason) {
    if (!take_open) return capture.error().empty();
    stopped_elapsed = elapsed();
    take_open = false;
    const bool saved = capture.stop();
    const bool closed = output->close();
    output.reset();
    if (!saved) return fail(capture.error());
    if (!closed) return fail("Cannot close AVI output");
    report(VideoTakeStatus::Saved, std::string(reason));
    return true;
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
