// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "session.hpp"
#include "scene.hpp"

namespace studio {
struct Panel {
    int engine = 0;
    int projection[2]{};
    Parameters values[2];
    std::string selected[2]{"bloom-intensity","glow-intensity"};
    Node node;
    bool visible = true, preview = false;
    bool apply = true, reconnect = false;
    int cap = 5000;
    std::string error;
    const char* mode() const;
    void draw(const Connection& connection, const char* gpu);
};
}
