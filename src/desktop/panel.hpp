// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "session.hpp"
#include "scene.hpp"

namespace studio {
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
    bool change_scene = true, apply_appearance = false, reconnect = false;
    int cap = 5000, applied_cap = 5000;
    std::string error;
    void toggle();
    void draw(const Connection& connection);
};
}
