// SPDX-License-Identifier: GPL-3.0-or-later
#include "panel.hpp"
#include "platform.hpp"
#include <backends/imgui_impl_opengl2.h>
#include <backends/imgui_impl_sdl2.h>
#include <imkit/theme.h>
#include <bitset>
#include <iostream>

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
public:
    void init() {
        IMGUI_CHECKVERSION();
        context = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        platform_ready = ImGui_ImplSDL2_InitForOpenGL(display.sdl_window,display.gl_context);
        if (platform_ready) renderer_ready = ImGui_ImplOpenGL2_Init();
        if (!renderer_ready) throw std::runtime_error("Cannot initialize the overlay");
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

class View {
    fs::path assets;
    Lines lines;
    Scene scene;
    std::unique_ptr<Renderer> tree;
    std::unique_ptr<LogstalgiaRenderer> flow;
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
public:
    explicit View(fs::path root) : assets(std::move(root)) {}
    ~View() { reset(); }
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
    void open(const Panel& panel, const fs::path& database, bool history) {
        reset();
        const auto dir = platform::utf8_path(assets/(panel.engine ? "Logstalgia" : "Gource")) + "/";
        texturemanager.setDir(dir);
        fontmanager.setDir(dir+"fonts/");
        shadermanager.setDir(dir+"shaders/");
        gSDLAppResourceDir = dir;
        auto emit = [this](const std::string& record) {
            lines.emplace_back(record,0,record.find_last_not_of("\r\n")+1);
        };
        if (!database.empty()) {
            if (panel.engine) {
                flow = std::make_unique<LogstalgiaRenderer>(database,panel.mode(),emit,
                    history ? std::optional<std::int64_t>(0) : std::nullopt);
                flow->step();
            } else {
                tree = std::make_unique<Renderer>(database,panel.mode(),emit,panel.cap);
                tree->snapshot();
            }
        }
        scene = panel.engine ? logstalgia_scene(lines,panel.values[1]) : gource_scene(lines,panel.values[0]);
        scene.app->init();
        std::cerr << "Scene: " << (panel.engine ? "Logstalgia / " : "Gource / ") << panel.mode() << '\n';
    }
    void frame(float t, float dt) {
        if (!scene.app) { display.clear(); return; }
        const auto now = SDL_GetTicks();
        if (SDL_TICKS_PASSED(now,next_read) && lines.size()<5000) {
            if (tree) tree->step();
            if (flow) flow->step();
            next_read = now+100;
        }
        scene.app->fps = ImGui::GetIO().Framerate;
        scene.app->update(t,dt);
    }
    void event(SDL_Event e) {
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
        const auto scale = display.viewport_dpi_ratio;
        if (e.type == SDL_MOUSEMOTION && (scene_buttons || !io.WantCaptureMouse)) {
            e.motion.x *= scale.x;
            e.motion.y *= scale.y;
            e.motion.xrel *= scale.x;
            e.motion.yrel *= scale.y;
            scene.app->mouseMove(&e.motion);
        }
        if ((e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) &&
            e.button.button > 0 && e.button.button <= 32) {
            const Uint32 mask = Uint32{1} << (e.button.button - 1);
            if (e.type == SDL_MOUSEBUTTONUP ? (scene_buttons & mask) != 0 : !io.WantCaptureMouse) {
                if (e.type == SDL_MOUSEBUTTONUP) scene_buttons &= ~mask;
                else scene_buttons |= mask;
                e.button.x *= scale.x;
                e.button.y *= scale.y;
                scene.app->mouseClick(&e.button);
            }
        }
        if (e.type == SDL_MOUSEWHEEL && !io.WantCaptureMouse) scene.app->mouseWheel(&e.wheel);
        // Window, config and export shortcuts are owned by the host.
        if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && scene_key(e.key.keysym.sym)) {
            const auto code = e.key.keysym.scancode;
            if (code < 0 || code >= SDL_NUM_SCANCODES) return;
            if (e.type == SDL_KEYDOWN && !e.key.repeat) scene_keys[code] = !io.WantCaptureKeyboard;
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

int run(bool preview, fs::path recording) {
    const auto assets = sdl_path(SDL_GetBasePath())/"viz-data";
    const auto prefs = sdl_path(SDL_GetPrefPath("Bitcoin Viz","Observer"));
    const DisplayLifetime display_lifetime;
    SDLAppInit("Bitcoin Viz","bitcoin-viz-desktop");
    display.enableHighDPIAwareness(true);
    display.enableVsync(true);
    display.enableResize(true);
    display.init("Bitcoin Viz",1200,760,false);
    SDL_SetWindowMinimumSize(display.sdl_window, 320, 240);
    Overlay overlay;
    overlay.init();
    auto theme = imkit::MakeTheme(imkit::ThemePreset::Midnight);
    imkit::SetAccent(theme,{1.f,.64f,.24f,1});
    imkit::ApplyTheme(theme);
    const char* gpu = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    std::cerr << "OpenGL: " << gpu << '\n';
    {
        Panel panel;
        panel.preview = preview;
        Preview sample;
        View view(assets);
        Session session(prefs);
        if (!preview && recording.empty()) session.connect(panel.node);
        bool running = true;
        fs::path active;
        Uint64 last = SDL_GetPerformanceCounter();
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL2_ProcessEvent(&event);
                if (event.type == SDL_QUIT ||
                    (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
                     event.window.windowID == SDL_GetWindowID(display.sdl_window))) {
                    running = false;
                    break;
                }
                if (event.type == SDL_KEYDOWN && !event.key.repeat && event.key.keysym.sym == SDLK_F1)
                    panel.visible = !panel.visible;
                view.event(event);
            }
            if (!running) break;
            int drawable_width, drawable_height;
            SDL_GL_GetDrawableSize(display.sdl_window, &drawable_width, &drawable_height);
            if (drawable_width <= 0 || drawable_height <= 0 ||
                (SDL_GetWindowFlags(display.sdl_window) & SDL_WINDOW_MINIMIZED)) {
                last = SDL_GetPerformanceCounter();
                SDL_Delay(10);
                continue;
            }
            ImGui_ImplOpenGL2_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();
            auto state = session.state();
            if (!recording.empty()) state = {"Recording / "+platform::utf8_path(recording.filename()),recording};
            if (panel.preview) state = {"Generated node activity",sample.open()};
            if (state.database != active || panel.apply) {
                active = state.database;
                panel.apply = false;
                try {
                    view.open(panel,active,panel.preview || !recording.empty());
                    panel.error.clear();
                } catch (const std::exception& e) {
                    view.reset();
                    panel.error=e.what();
                    std::cerr << "Scene error: " << e.what() << '\n';
                }
            }
            auto now = SDL_GetPerformanceCounter();
            const float dt = float(double(now-last)/SDL_GetPerformanceFrequency());
            last = now;
            try { view.frame(SDL_GetTicks()*.001f,std::min(dt,.1f)); }
            catch (const std::exception& e) { view.reset(); panel.error=e.what(); }
            panel.draw(state,gpu);
            if (panel.reconnect || (!panel.preview && state.status.empty() && recording.empty())) {
                panel.reconnect=false;
                recording.clear();
                session.connect(panel.node);
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
            display.update();
            SDL_Delay(1);
        }
    }
    return 0;
}
}
int main(int argc, char** argv) {
    try {
        bitcoin_viz::platform::initialize_process();
        SDL_SetMainReady();
        if (argc==1) return run(false,{});
        if (argc==2 && std::string(argv[1])=="--demo") return run(true,{});
        if (argc==3 && std::string(argv[1])=="--db") return run(false,std::filesystem::u8path(argv[2]));
        std::cerr << "Usage: bitcoin-viz-desktop [--demo | --db PATH]\n";
        return argc==2 && std::string(argv[1])=="--help" ? 0 : 1;
    } catch (const std::exception& e) { std::cerr << "Bitcoin Viz: " << e.what() << '\n'; return 1; }
}
