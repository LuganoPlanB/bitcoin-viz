// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_writer.hpp"
#include "avilib.h"
#include <turbojpeg.h>
#include <limits>
#include <stdexcept>

namespace studio {
namespace {
struct CompressorDelete { void operator()(void* p) const { if (p) tjDestroy(p); } };
struct JpegDelete { void operator()(unsigned char* p) const { tjFree(p); } };
struct AviDelete { void operator()(void* p) const { if (p) aviwriter_destroy(p); } };
}
struct VideoWriter::State {
    std::unique_ptr<void, CompressorDelete> encoder;
    std::unique_ptr<unsigned char, JpegDelete> jpeg;
    std::unique_ptr<void, AviDelete> avi;
    unsigned long capacity = 0;
    std::uint32_t jpeg_length = 0;
    VideoSettings settings;
    std::uint64_t frame_count = 0;
    bool opened = false;
    bool failed = false;
    std::string message;
};

VideoWriter::VideoWriter() : state(std::make_unique<State>()) {}
VideoWriter::~VideoWriter() { try { stop(); } catch (...) {} }
bool VideoWriter::active() const { return state->opened; }
std::uint64_t VideoWriter::frames() const { return state->frame_count; }
const std::string& VideoWriter::error() const { return state->message; }

bool VideoWriter::fail(const std::string& message) {
    state->message = message;
    state->failed = true;
    // close is attempted once, then the vendor destroy observes an already closed writer.
    if (state->opened) {
        state->opened = false;
        aviwriter_close(state->avi.get());
    }
    state->avi.reset();
    state->jpeg.reset();
    state->encoder.reset();
    return false;
}

bool VideoWriter::prepare(VideoSettings settings) {
    if (active()) { state->message = "A video take is already active"; return false; }
    state = std::make_unique<State>();
    state->settings = settings;
    // AVI rcFrame uses signed 16-bit dimensions; JPEG's dimensions are bounded too.
    if (settings.width <= 0 || settings.height <= 0 || settings.width > 32767 ||
        settings.height > 32767 || settings.fps < 1 || settings.fps > 240 ||
        settings.quality < 1 || settings.quality > 100)
        return fail("Invalid video dimensions, frame rate or JPEG quality");
    state->encoder.reset(tjInitCompress());
    if (!state->encoder) return fail("Cannot create JPEG compressor");
    state->capacity = tjBufSize(settings.width, settings.height, TJSAMP_444);
    if (state->capacity == static_cast<unsigned long>(-1) ||
        state->capacity > static_cast<unsigned long>(std::numeric_limits<int>::max()))
        return fail("JPEG output buffer is too large");
    state->jpeg.reset(tjAlloc(static_cast<int>(state->capacity)));
    if (!state->jpeg) return fail("Cannot allocate JPEG output buffer");
    state->avi.reset(aviwriter_create());
    constexpr std::uint32_t mjpg = 0x47504a4d;
    if (!state->avi || !aviwriter_set_video_props(state->avi.get(), 0,
            settings.width, settings.height, mjpg, 0, settings.fps) ||
        (settings.riff_limit_bytes &&
         !aviwriter_set_riff_limit(state->avi.get(), settings.riff_limit_bytes)))
        return fail("Cannot configure AVI stream");
    return true;
}

bool VideoWriter::start(const std::filesystem::path& path, VideoSettings settings) {
    try {
        if (!prepare(settings)) return false;
        const auto utf8 = path.u8string();
        // u8string has char8_t elements since C++20; avilib consumes UTF-8 bytes.
        const std::string filename(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        if (!aviwriter_open(state->avi.get(), filename.c_str()))
            return fail("Cannot open AVI output: " + filename);
        state->opened = true;
        return true;
    } catch (const std::exception& e) { return fail(std::string("Cannot start video: ") + e.what()); }
}
bool VideoWriter::start(std::ostream& output, VideoSettings settings) {
    try {
        if (!prepare(settings)) return false;
        if (!aviwriter_open_stream(state->avi.get(), output)) return fail("Cannot open seekable AVI output");
        state->opened = true;
        return true;
    } catch (const std::exception& e) { return fail(std::string("Cannot start video: ") + e.what()); }
}

bool VideoWriter::write_frame(const std::uint8_t* rgb, std::size_t bytes, int stride, bool bottom_up) {
    if (!active()) return false;
    try {
        const auto& s = state->settings;
        if (!stride) stride = s.width * 3;
        const auto row_bytes = static_cast<std::size_t>(s.width) * 3;
        if (!rgb || stride < 0 || static_cast<std::size_t>(stride) < row_bytes ||
            bytes < static_cast<std::size_t>(s.height - 1) * static_cast<std::size_t>(stride) + row_bytes)
            return fail("Invalid RGB frame buffer or stride");
        auto* output = state->jpeg.get();
        // tjCompress2 returns the actual length here; reset capacity for every image.
        unsigned long length = state->capacity;
        const int flags = TJFLAG_NOREALLOC | (bottom_up ? TJFLAG_BOTTOMUP : 0);
        if (tjCompress2(state->encoder.get(), rgb, s.width, stride, s.height,
                TJPF_RGB, &output, &length, TJSAMP_444, s.quality, flags) < 0)
            return fail(std::string("JPEG compression failed: ") + tjGetErrorStr2(state->encoder.get()));
        if (!length || length > state->capacity || length > std::numeric_limits<std::uint32_t>::max())
            return fail("Invalid compressed JPEG length");
        state->jpeg_length = static_cast<std::uint32_t>(length);
        return repeat_frame();
    } catch (const std::exception& e) { return fail(std::string("Video frame failed: ") + e.what()); }
}

bool VideoWriter::repeat_frame() {
    if (!active() || !state->jpeg_length) return false;
    try {
        if (!aviwriter_write_chunk(state->avi.get(), 0, state->jpeg.get(), state->jpeg_length))
            return fail("AVI frame write failed");
        ++state->frame_count;
        return true;
    } catch (const std::exception& e) { return fail(std::string("Video frame failed: ") + e.what()); }
}

bool VideoWriter::stop() {
    if (!active()) return !state->failed;
    state->opened = false;
    try {
        if (!aviwriter_close(state->avi.get())) return fail("AVI finalization failed (write, seek, flush or close)");
        state->avi.reset();
        state->jpeg.reset();
        state->encoder.reset();
        return true;
    } catch (const std::exception& e) { return fail(std::string("AVI finalization failed: ") + e.what()); }
}
}
