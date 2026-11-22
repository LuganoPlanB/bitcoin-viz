// SPDX-License-Identifier: GPL-3.0-or-later
#include "panel.hpp"
#include "platform.hpp"
#include "bitcoin_viz_version.hpp"
#include "window_video_capture.hpp"
#include <backends/imgui_impl_opengl2.h>
#include <backends/imgui_impl_sdl2.h>
#include <imkit/theme.h>
#include <bitset>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>

namespace {
using namespace studio;
using namespace bitcoin_viz;
namespace fs = std::filesystem;
fs::path sdl_path(char* raw) {
    std::unique_ptr<char,decltype(&SDL_free)> path(raw,SDL_free);
    if (!path) throw std::runtime_error(SDL_GetError());
    return fs::u8path(path.get());
}

struct DisplayLifetime {
    ~DisplayLifetime() {
        display.quit();
        SDL_Quit();
    }
};

class Overlay {
    ImGuiContext* context = nullptr;
    bool platform_ready = false;
    bool renderer_ready = false;
    imkit::Theme theme = imkit::MakeTheme(imkit::ThemePreset::Midnight);
    float scale = 0;
    int width = 0, height = 0, framebuffer_width = 0, framebuffer_height = 0;
public:
    void init(const fs::path& assets) {
        IMGUI_CHECKVERSION();
        context = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigWindowsMoveFromTitleBarOnly = true;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        platform_ready = ImGui_ImplSDL2_InitForOpenGL(display.sdl_window,display.gl_context);
        if (platform_ready) renderer_ready = ImGui_ImplOpenGL2_Init();
        if (!renderer_ready) throw std::runtime_error("Cannot initialize the overlay");
        theme.typography.body = 15;
        imkit::SetDensity(theme,imkit::Density::Compact);
        imkit::SetAccent(theme,{1.f,.64f,.24f,1});
        // Reuse the proportional font shipped with Gource. Read the native
        // filesystem path before handing the buffer to ImGui's atlas.
        std::ifstream font(assets/"Gource"/"fonts"/"FreeSans.ttf",std::ios::binary | std::ios::ate);
        const auto size = static_cast<std::streamoff>(font.tellg());
        if (size > 0 && size <= std::numeric_limits<int>::max()) {
            std::unique_ptr<void,decltype(&ImGui::MemFree)> bytes(IM_ALLOC(static_cast<std::size_t>(size)),ImGui::MemFree);
            if (bytes && font.seekg(0).read(static_cast<char*>(bytes.get()),size))
                io.FontDefault = io.Fonts->AddFontFromMemoryTTF(bytes.release(),static_cast<int>(size));
        }
    }
    void new_frame(int drawable_width, int drawable_height) {
        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        // The content scale sets the overlay's metrics. The backend independently
        // maps logical coordinates to pixels for high-density drawables.
        const float content_scale = std::clamp(ImGui_ImplSDL2_GetContentScaleForWindow(display.sdl_window),
                                               1.0f,imkit::ThemeScaleMaximum);
        const auto size = ImGui::GetIO().DisplaySize;
        const bool scale_changed = scale != content_scale;
        if (scale_changed) {
            scale = content_scale;
            imkit::ApplyTheme(theme,scale);
        }
        if (scale_changed || width != size.x || height != size.y ||
            framebuffer_width != drawable_width || framebuffer_height != drawable_height) {
            width = static_cast<int>(size.x);
            height = static_cast<int>(size.y);
            framebuffer_width = drawable_width;
            framebuffer_height = drawable_height;
            std::cerr << "Overlay: " << width << 'x' << height << " logical, " << framebuffer_width << 'x'
                << framebuffer_height << " drawable, " << std::lround(scale*100) << "% content scale\n";
        }
        ImGui::NewFrame();
    }
    ~Overlay() {
        if (renderer_ready) ImGui_ImplOpenGL2_Shutdown();
        if (platform_ready) ImGui_ImplSDL2_Shutdown();
        if (context) ImGui::DestroyContext(context);
    }
};

bool scene_key(SDL_Keycode key) {
    switch (key) {
    case SDLK_SPACE: case SDLK_PLUS: case SDLK_MINUS: case SDLK_EQUALS:
    case SDLK_HOME: case SDLK_END: case SDLK_PAGEUP: case SDLK_PAGEDOWN:
    case SDLK_LEFT: case SDLK_RIGHT: case SDLK_UP: case SDLK_DOWN:
        return true;
    default:
        return false;
    }
}

void release_mouse() {
    SDL_SetRelativeMouseMode(SDL_FALSE);
    SDL_SetWindowGrab(display.sdl_window, SDL_FALSE);
    SDL_ShowCursor(SDL_ENABLE);
}

struct SceneSource {
    fs::path database;
    std::uint64_t generation = 0;
    bool history = false;

    bool operator==(const SceneSource&) const = default;
};

struct SceneRequest {
    int engine;
    int projection;
    int cap;
    SceneSource source;
    Parameters options;

    bool same_stream(const SceneRequest& other) const {
        return engine == other.engine && projection == other.projection && cap == other.cap &&
            source == other.source;
    }
    const char* mode() const {
        return engine ? logstalgia_modes[projection] : gource_modes[projection];
    }
};

Parameters changed_parameters(const Parameters& before, const Parameters& after) {
    Parameters changes;
    for (const auto& [key,value] : after) {
        const auto previous = before.find(key);
        if (previous == before.end() || previous->second != value) changes.emplace(key,value);
    }
    for (const auto& [key,value] : before)
        if (!after.count(key)) changes.emplace(key,"");
    return changes;
}

class View {
    fs::path assets;
    Lines lines;
    Scene scene;
    std::unique_ptr<Renderer> tree;
    std::unique_ptr<LogstalgiaRenderer> flow;
    std::optional<SceneRequest> applied;
    Uint32 next_read = 0;
    Uint32 scene_buttons = 0;
    std::bitset<SDL_NUM_SCANCODES> scene_keys;

    void focus_lost() {
        if (scene.app) {
            for (int key = 0; key < SDL_NUM_SCANCODES; ++key) {
                if (!scene_keys[key]) continue;
                SDL_KeyboardEvent release{};
                release.type = SDL_KEYUP;
                release.keysym.scancode = static_cast<SDL_Scancode>(key);
                release.keysym.sym = SDL_GetKeyFromScancode(release.keysym.scancode);
                scene.app->keyPress(&release);
            }
            for (Uint8 button = 1; button <= 32; ++button) {
                if (!(scene_buttons & (Uint32{1} << (button - 1)))) continue;
                SDL_MouseButtonEvent release{};
                release.type = SDL_MOUSEBUTTONUP;
                release.button = button;
                scene.app->mouseClick(&release);
            }
        }
        scene_keys.reset();
        scene_buttons = 0;
        release_mouse();
    }

    void resize() {
        int width, height;
        SDL_GetWindowSize(display.sdl_window, &width, &height);
        int drawable_width, drawable_height;
        SDL_GL_GetDrawableSize(display.sdl_window, &drawable_width, &drawable_height);
        if (width <= 0 || height <= 0 || drawable_width <= 0 || drawable_height <= 0) return;
        display.resize(width,height);
        if (scene.app) scene.app->resize(width,height);
    }
    void reset() {
        scene.app.reset();
        scene.parser.reset();
        fontmanager.unload();
        fontmanager.purge();
        shadermanager.purge();
        texturemanager.purge();
        tree.reset();
        flow.reset();
        lines.clear();
        scene_keys.reset();
        scene_buttons = 0;
        next_read = SDL_GetTicks();
        release_mouse();
    }
    void open(const SceneRequest& request) {
        reset();
        const auto dir = platform::utf8_path(assets/(request.engine ? "Logstalgia" : "Gource")) + "/";
        texturemanager.setDir(dir);
        fontmanager.setDir(dir+"fonts/");
        shadermanager.setDir(dir+"shaders/");
        gSDLAppResourceDir = dir;
        auto emit = [this](const std::string& record) {
            lines.emplace_back(record,0,record.find_last_not_of("\r\n")+1);
        };
        if (!request.source.database.empty()) {
            if (request.engine) {
                flow = std::make_unique<LogstalgiaRenderer>(request.source.database,request.mode(),emit,
                    request.source.history ? std::optional<std::int64_t>(0) : std::nullopt);
                flow->step();
            } else {
                tree = std::make_unique<Renderer>(request.source.database,request.mode(),emit,request.cap);
                tree->snapshot();
            }
        }
        scene = request.engine ? logstalgia_scene(lines,request.options,request.mode()) : gource_scene(lines,request.options);
        scene.app->init();
        std::cerr << "Scene: " << (request.engine ? "Logstalgia / " : "Gource / ") << request.mode() << '\n';
    }
public:
    explicit View(fs::path root) : assets(std::move(root)) {}
    ~View() { reset(); }
    void clear() {
        reset();
        applied.reset();
    }
    const std::optional<SceneRequest>& current() const { return applied; }
    void focus_overlay() { focus_lost(); }
    void sync_drawable(int width, int height) {
        if (display.width != width || display.height != height) resize();
    }
    void apply(SceneRequest request) {
        // Retire a previous source before any validation or initialization can fail.
        if (applied && applied->source != request.source) clear();
        const auto changes = applied ? changed_parameters(applied->options,request.options) : Parameters{};
        const bool same_stream = applied && applied->same_stream(request);
        if (same_stream && changes.empty()) return;
        const auto appearance = request.engine ? validate_logstalgia_parameters(request.options)
                                               : validate_gource_parameters(request.options);
        if (same_stream && std::all_of(changes.begin(),changes.end(),[&](const auto& option) {
            return live_parameter(request.engine,option.first);
        })) {
            if (request.engine) apply_logstalgia_appearance(appearance,changes);
            else apply_gource_appearance(*scene.app,appearance,changes);
            std::cerr << "Scene appearance updated: " << request.mode() << '\n';
        } else {
            try {
                open(request);
            } catch (const std::exception& error) {
                // The engines share graphics managers and mutable native settings.
                // Only a request from the same source remains eligible for recovery.
                if (applied) {
                    try { open(*applied); }
                    catch (const std::exception& recovery) {
                        clear();
                        throw std::runtime_error(std::string(error.what()) + "; scene recovery failed: " + recovery.what());
                    }
                } else {
                    clear();
                }
                throw;
            }
        }
        applied = std::move(request);
    }
    void frame(float t, float dt) {
        if (!scene.app) {
            display.setClearColour(vec4(.1f,.1f,.1f,1.f));
            display.clear();
            return;
        }
        const auto now = SDL_GetTicks();
        if (SDL_TICKS_PASSED(now,next_read) && lines.size()<5000) {
            if (tree) tree->step();
            if (flow) flow->step();
            next_read = now+100;
        }
        scene.app->fps = ImGui::GetIO().Framerate;
        scene.app->update(t,dt);
    }
    void event(SDL_Event e, bool overlay_visible) {
        if (e.type == SDL_WINDOWEVENT) {
            switch (e.window.event) {
            case SDL_WINDOWEVENT_FOCUS_LOST: focus_lost(); break;
            case SDL_WINDOWEVENT_SIZE_CHANGED:
            case SDL_WINDOWEVENT_RESTORED:
#if SDL_VERSION_ATLEAST(2,0,18)
            case SDL_WINDOWEVENT_DISPLAY_CHANGED:
#endif
                resize();
                break;
            }
        }
        if (!scene.app) return;
        const auto& io=ImGui::GetIO();
        const bool mouse_captured = overlay_visible && io.WantCaptureMouse;
        const bool keys_captured = overlay_visible && (io.WantCaptureKeyboard || io.WantTextInput);
        const auto scale = display.viewport_dpi_ratio;
        if (e.type == SDL_MOUSEMOTION && (scene_buttons || !mouse_captured)) {
            e.motion.x *= scale.x;
            e.motion.y *= scale.y;
            e.motion.xrel *= scale.x;
            e.motion.yrel *= scale.y;
            scene.app->mouseMove(&e.motion);
        }
        if ((e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) &&
            e.button.button > 0 && e.button.button <= 32) {
            const Uint32 mask = Uint32{1} << (e.button.button - 1);
            if (e.type == SDL_MOUSEBUTTONUP ? (scene_buttons & mask) != 0 : !mouse_captured) {
                if (e.type == SDL_MOUSEBUTTONUP) scene_buttons &= ~mask;
                else scene_buttons |= mask;
                e.button.x *= scale.x;
                e.button.y *= scale.y;
                scene.app->mouseClick(&e.button);
            }
        }
        if (e.type == SDL_MOUSEWHEEL && !mouse_captured) scene.app->mouseWheel(&e.wheel);
        // Window, config and export shortcuts are owned by the host.
        if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && scene_key(e.key.keysym.sym)) {
            const auto code = e.key.keysym.scancode;
            if (code < 0 || code >= SDL_NUM_SCANCODES) return;
            if (e.type == SDL_KEYDOWN && !e.key.repeat) scene_keys[code] = !keys_captured;
            if (scene_keys[code]) scene.app->keyPress(&e.key);
            if (e.type == SDL_KEYUP) scene_keys[code] = false;
        }
    }
};

struct Preview {
    fs::path dir;
    fs::path open() {
        if (dir.empty()) {
            const auto path = fs::temp_directory_path()/("bitcoin-viz-"+session_id());
            if (!fs::create_directory(path)) throw std::runtime_error("Cannot create preview directory");
            dir=path;
            Store store(dir/"preview.sqlite");
            demo(store);
        }
        return dir/"preview.sqlite";
    }
    ~Preview() { std::error_code ec; if (!dir.empty()) fs::remove_all(dir,ec); }
};

struct NodeVerification {
    std::atomic<bool> closing{false};
    std::future<std::pair<Node,NodeTest>> result;
    ~NodeVerification() { closing = true; }

    void start(Node node) {
        result = std::async(std::launch::async,[this,node = std::move(node)] {
            return std::make_pair(node,test_node(node,[this] { return closing.load(); }));
        });
    }
};

#ifdef BITCOIN_VIZ_TEST_VIDEO_CAPTURE
struct VideoSmoke {
    fs::path directory;
    unsigned take = 0;
    bool verified = false;

    void key(WindowVideoCapture& video, SDL_Keycode code) {
        if (code == SDLK_F8) video.stop("Stopped");
        if ((code == SDLK_F6 || code == SDLK_F7) && !video.active()) {
            const auto path = directory / ("take-" + std::to_string(++take) + ".avi");
            if (fs::exists(path) || !video.start(path, 95, code == SDLK_F7))
                throw std::runtime_error("Video smoke start rejected: " + platform::utf8_path(path));
            verified = false;
        }
    }

    void sample(WindowVideoCapture& video, VideoPicture picture) {
        if (verified || !video.active()) {
            video.sample(picture);
            return;
        }
        // Challenge the readback with a different buffer selection, nonpacked
        // rows and a bound PBO. Restore the test setup before the renderer resumes.
        GLint saved_read, saved_pbo;
        glGetIntegerv(GL_READ_BUFFER, &saved_read);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &saved_pbo);
        GLuint pbo;
        glGenBuffers(1, &pbo);
        glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
        glReadBuffer(GL_FRONT);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 8);
        glPixelStorei(GL_PACK_ROW_LENGTH, 23);
        glPixelStorei(GL_PACK_SKIP_ROWS, 2);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 3);
        const bool read = video.sample(picture);
        GLint buffer, binding, alignment, length, rows, pixels;
        glGetIntegerv(GL_READ_BUFFER, &buffer);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &binding);
        glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &length);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &rows);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &pixels);
        glPopClientAttrib();
        glReadBuffer(static_cast<GLenum>(saved_read));
        glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(saved_pbo));
        glDeleteBuffers(1, &pbo);
        if (buffer != GL_FRONT || static_cast<GLuint>(binding) != pbo ||
            alignment != 8 || length != 23 || rows != 2 || pixels != 3)
            throw std::runtime_error("Video readback changed GL pack or read-buffer state");
        if (read) {
            verified = true;
            std::cerr << "Video smoke: restored GL read buffer and pack state ("
                      << (picture == VideoPicture::Scene ? "scene" : "composited") << ")\n";
        }
    }
};
#endif

int run(bool preview, fs::path recording, bool smoke = false, std::optional<Node> smoke_node = {}
#ifdef BITCOIN_VIZ_TEST_VIDEO_CAPTURE
        , std::optional<VideoSmoke> video_smoke = {}
#endif
) {
    const auto assets = sdl_path(SDL_GetBasePath())/"viz-data";
    const auto prefs = sdl_path(SDL_GetPrefPath("Bitcoin Viz","Observer"));
    const DisplayLifetime display_lifetime;
    SDLAppInit("Bitcoin Viz","bitcoin-viz-desktop");
    display.enableHighDPIAwareness(true);
    display.enableVsync(true);
    display.enableResize(true);
    try {
        display.init("Bitcoin Viz",1200,760,false);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Cannot open a desktop OpenGL window: ") + e.what());
    }
    const auto* gpu = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const auto* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    if (!gpu || !version || !vendor) throw std::runtime_error("OpenGL driver did not report its identity");
    std::cerr << "OpenGL: " << gpu << " / " << version << " / " << vendor << '\n';
    if (!GLEW_VERSION_2_1)
        throw std::runtime_error("OpenGL 2.1 compatibility is required. Check the system graphics driver.");
    SDL_SetWindowMinimumSize(display.sdl_window, 320, 240);
    Overlay overlay;
    overlay.init(assets);
    {
        Panel panel;
        panel.preview = preview;
        panel.open_node = !preview && recording.empty();
        Preview sample;
        View view(assets);
        WindowVideoCapture video(display.sdl_window);
        const auto capture_picture = [&](VideoPicture picture) {
#ifdef BITCOIN_VIZ_TEST_VIDEO_CAPTURE
            if (video_smoke) {
                video_smoke->sample(video, picture);
                return;
            }
#endif
            video.sample(picture);
        };
        Session session(prefs);
        std::future<std::optional<Node>> suggestion;
        NodeVerification verification;
        if (smoke_node) {
            panel.node.settings = *smoke_node;
            panel.test_connection = true;
            panel.projection[1] = 2; // Logstalgia blocks projection for the regtest fixture.
        } else if (panel.open_node) {
            suggestion = std::async(std::launch::async,suggest_local_node);
        }
        bool running = true;
        SceneSource observed_source;
        Uint64 last = SDL_GetPerformanceCounter();
        int smoke_frames = 0;
        const auto smoke_deadline = SDL_GetTicks() + 15000;
        const auto window = SDL_GetWindowID(display.sdl_window);
        while (running) {
            if (smoke && SDL_TICKS_PASSED(SDL_GetTicks(),smoke_deadline))
                throw std::runtime_error("Timed out waiting for both desktop renderers and the overlay. "
                    + session.state().status);
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL2_ProcessEvent(&event);
                if (event.type == SDL_QUIT ||
                    (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
                     event.window.windowID == SDL_GetWindowID(display.sdl_window))) {
                    running = false;
                    break;
                }
                if (event.type == SDL_KEYDOWN && !event.key.repeat && event.key.keysym.sym == SDLK_F1) {
                    panel.toggle();
                    if (panel.visible) view.focus_overlay();
                }
                if (event.type == SDL_WINDOWEVENT && event.window.windowID == window &&
                    event.window.event == SDL_WINDOWEVENT_MINIMIZED)
                    video.stop("Window minimized");
#ifdef BITCOIN_VIZ_TEST_VIDEO_CAPTURE
                if (video_smoke && event.type == SDL_KEYDOWN && !event.key.repeat)
                    video_smoke->key(video, event.key.keysym.sym);
#endif
                view.event(event,panel.visible);
            }
            if (!running) break;
            int drawable_width, drawable_height;
            SDL_GL_GetDrawableSize(display.sdl_window, &drawable_width, &drawable_height);
            const bool minimized = (SDL_GetWindowFlags(display.sdl_window) & SDL_WINDOW_MINIMIZED) != 0;
            video.update_drawable(drawable_width, drawable_height, minimized);
            if (drawable_width <= 0 || drawable_height <= 0 || minimized) {
                last = SDL_GetPerformanceCounter();
                SDL_Delay(10);
                continue;
            }
            view.sync_drawable(drawable_width,drawable_height);
            overlay.new_frame(drawable_width,drawable_height);
            if (suggestion.valid() && suggestion.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                if (auto node = suggestion.get(); node && !panel.testing && panel.tested.status.empty())
                    panel.node.prefill(std::move(*node));
            }
            if (verification.result.valid() &&
                verification.result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                auto [node,tested] = verification.result.get();
                panel.testing = false;
                if (node == panel.node.settings) panel.tested = std::move(tested);
                if (smoke_node) {
                    if (!panel.tested.target) throw std::runtime_error("Regtest smoke connection: " + panel.tested.status);
                    panel.reconnect = true;
                }
            }
            auto state = session.state();
            if (!recording.empty()) state = {"Recording / "+platform::utf8_path(recording.filename()),recording};
            if (panel.preview) state = {"Generated node activity",sample.open()};
            const SceneSource source{state.database,state.generation,panel.preview || !recording.empty() || bool(smoke_node)};
            const bool appearance = std::exchange(panel.apply_appearance,false);
            const bool selection = std::exchange(panel.change_scene,false);
            if (source != observed_source || selection || appearance) {
                observed_source = source;
                try {
                    auto options = appearance ? panel.values[panel.engine] : panel.applied_values[panel.engine];
                    std::erase_if(options,[](const auto& option) { return option.second.empty(); });
                    const auto cap = panel.engine ? 0 : appearance ? panel.cap : panel.applied_cap;
                    view.apply({panel.engine,panel.projection[panel.engine],cap,source,options});
                    if (appearance) {
                        panel.applied_values[panel.engine] = std::move(options);
                        if (!panel.engine) panel.applied_cap = cap;
                    }
                    panel.error.clear();
                } catch (const std::exception& e) {
                    if (view.current()) {
                        panel.engine = view.current()->engine;
                        panel.projection[panel.engine] = view.current()->projection;
                    }
                    panel.error=e.what();
                    std::cerr << "Scene error: " << e.what() << '\n';
                }
            }
            auto now = SDL_GetPerformanceCounter();
            const float dt = float(double(now-last)/SDL_GetPerformanceFrequency());
            last = now;
            try { view.frame(SDL_GetTicks()*.001f,std::min(dt,.1f)); }
            catch (const std::exception& e) { view.clear(); panel.error=e.what(); }
            capture_picture(VideoPicture::Scene);
            const bool fullscreen = (SDL_GetWindowFlags(display.sdl_window) & SDL_WINDOW_FULLSCREEN) != 0;
            panel.draw(state,fullscreen);
            const bool fullscreen_shortcut = ImGui::Shortcut(ImGuiKey_F,ImGuiInputFlags_RouteGlobal);
            const bool fullscreen_requested = std::exchange(panel.toggle_fullscreen,false) || fullscreen_shortcut;
            if (std::exchange(panel.test_connection,false)) {
                panel.testing = true;
                verification.start(panel.node.settings);
            }
            if (std::exchange(panel.reconnect,false) && panel.tested.target) {
                recording.clear();
                session.connect(*panel.tested.target);
            }
            ImGui::Render();
            // The fixed-function ImGui backend reads vertices from CPU memory.
            if (GLEW_VERSION_2_0) glUseProgram(0);
            if (GLEW_VERSION_1_3) {
                glActiveTexture(GL_TEXTURE0);
                glClientActiveTexture(GL_TEXTURE0);
            }
            if (GLEW_VERSION_1_5) {
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
            }
            ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
            capture_picture(VideoPicture::Composited);
            display.update();
            if (fullscreen_requested) {
                panel.fullscreen_error.clear();
                if (SDL_SetWindowFullscreen(display.sdl_window,fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) {
                    panel.fullscreen_error = std::string("Cannot change fullscreen mode: ") + SDL_GetError();
                    std::cerr << panel.fullscreen_error << '\n';
                }
            }
            if (smoke) {
                if (!panel.error.empty()) throw std::runtime_error(panel.error);
                const auto& active = view.current();
                const bool source_ready = !smoke_node ||
                    (!state.database.empty() && state.status.starts_with("regtest / Connected / RPC + ZMQ sequence"));
                if (active && active->source == source && source_ready && ImGui::GetDrawData()->TotalVtxCount > 0) {
                    if (SDL_GetWindowID(display.sdl_window) != window || view.current()->engine != panel.engine)
                        throw std::runtime_error("Desktop smoke test changed windows or missed an engine switch");
                    if (++smoke_frames == 4) {
                        panel.engine = 1;
                        panel.change_scene = true;
                    } else if (smoke_frames == 8) {
                        if (smoke_node) std::cerr << "Desktop smoke node: " << state.status << '\n';
                        std::cerr << "Desktop smoke passed: Gource, Logstalgia and overlay in one OpenGL window\n";
                        return 0;
                    }
                }
            }
            SDL_Delay(1);
        }
        video.stop("Window closed");
    }
    return smoke ? 1 : 0;
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        bitcoin_viz::platform::initialize_process();
        SDL_SetMainReady();
        if (argc==1) return run(false,{});
        const auto option = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path{};
        if (argc==2 && option=="--demo") return run(true,{});
        if (argc==2 && option=="--smoke-test") return run(true,{},true);
#ifdef BITCOIN_VIZ_TEST_VIDEO_CAPTURE
        if (argc==4 && option=="--smoke-test" && std::filesystem::path(argv[2])=="--video") {
            const std::filesystem::path output(argv[3]);
            if (!std::filesystem::is_directory(output))
                throw std::runtime_error("Video smoke requires an existing output directory");
            return run(true,{},false,{},VideoSmoke{output});
        }
#endif
        if (argc==3 && option=="--smoke-test" && std::filesystem::path(argv[2])=="--regtest") {
            std::string line;
            if (!std::getline(std::cin,line)) throw std::runtime_error("Regtest smoke requires a fixture on stdin");
            const auto fixture = bitcoin_viz::Json::parse(line);
            return run(false,{},true,studio::Node{2,fixture.at("datadir").get<std::string>(),
                fixture.at("cookie").get<std::string>(),fixture.at("rpc_url").get<std::string>(),{}});
        }
        if (argc==2 && option=="--version") {
            std::cout << BITCOIN_VIZ_VERSION << '\n';
            return 0;
        }
        if (argc==3 && option=="--db") return run(false,std::filesystem::path(argv[2]));
        std::cerr << "Usage: bitcoin-viz-desktop [--demo | --db PATH | --version | --smoke-test]\n";
        return argc==2 && option=="--help" ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "Bitcoin Viz: " << e.what() << '\n';
#if defined(_WIN32) || defined(__APPLE__)
        if (argc==1) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Bitcoin Viz",e.what(),nullptr);
#endif
        return 1;
    }
}
