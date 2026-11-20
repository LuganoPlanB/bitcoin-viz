// SPDX-License-Identifier: GPL-3.0-or-later
#include "logstalgia.h"
#include "scene.hpp"

namespace studio {
const Parameters& logstalgia_parameters() {
    static const auto types = SceneOptions<LogstalgiaSettings>().catalog();
    return types;
}
Scene logstalgia_scene(Lines& lines, const Parameters& options) {
    ConfFile config;
    auto section = config.addSection("logstalgia");
    section->setEntry("path",".");
    section->setEntry("paddle-mode","pid");
    if (!options.count("group")) section->addEntry("group","Bitcoin,URI=^/,100");
    apply_parameters(*section,options,logstalgia_parameters());
    settings.importLogstalgiaSettings(config);
    auto input = std::make_unique<SceneInput>(lines);
    auto app = std::make_unique<Logstalgia>("-",input.get());
    input.release(); // Logstalgia owns the stream now.
    return {{},std::move(app)};
}
}
