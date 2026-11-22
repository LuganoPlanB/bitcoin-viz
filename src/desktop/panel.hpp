// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "session.hpp"
#include "scene.hpp"

namespace studio {
class WindowVideoCapture;

struct Panel {
    int engine = 0;
    int projection[2]{};
    Parameters values[2];
    Parameters applied_values[2];
    std::string selected[2]{"bloom-intensity","glow-intensity"};
    NodeDraft node;
    NodeTest tested;
    bool open_node = false, testing = false, test_connection = false;
    bool visible = true, preview = false;
    bool focus = false;
    bool toggle_fullscreen = false;
    bool change_scene = true, apply_appearance = false, reconnect = false;
    int cap = 5000, applied_cap = 5000;
    std::string error;
    std::string fullscreen_error;
    std::string video_path;
    int video_quality = 95;
    bool include_overlay = false, toggle_video = false;
    void toggle();
    void draw(const Connection& connection, bool fullscreen, const WindowVideoCapture& video,
              const std::string& video_directory);
};
}
