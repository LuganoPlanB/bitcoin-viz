// SPDX-License-Identifier: GPL-3.0-or-later
#include "logstalgia.h"
#include "scene.hpp"

namespace studio {
const Parameters& logstalgia_parameters() {
    static const auto types = [] {
        auto catalog = SceneOptions<LogstalgiaSettings>().catalog({"path","start-position","stop-position",
            "detect-changes","disable-progress","ffp","group-separators"});
        catalog["path-separators"] = "string"; // Registered under the wrong name upstream.
        catalog.at("background") = "colour";
        return catalog;
    }();
    return types;
}

namespace {
void import(LogstalgiaSettings& target, const Parameters& options) {
    ConfFile config;
    auto section = config.addSection("logstalgia");
    section->setEntry("path",".");
    section->setEntry("paddle-mode","pid");
    apply_parameters(*section,options,logstalgia_parameters());
    if (!section->getEntry("group")) section->addEntry("group","Bitcoin,URI=^/,100");
    target.importLogstalgiaSettings(config);
}
}

LiveAppearance validate_logstalgia_parameters(const Parameters& options) {
    LogstalgiaSettings draft;
    import(draft,options);
    return {draft.simulation_speed,draft.background_colour,{}};
}

void apply_logstalgia_appearance(const LiveAppearance& appearance, const Parameters& changes) {
    if (changes.count("simulation-speed")) settings.simulation_speed = appearance.speed;
    if (changes.count("background")) settings.background_colour = appearance.background;
}

Scene logstalgia_scene(Lines& lines, const Parameters& options) {
    import(settings,options);
    auto input = std::make_unique<SceneInput>(lines);
    auto app = std::make_unique<Logstalgia>("-",input.get());
    input.release(); // Logstalgia owns the stream now.
    return {{},std::move(app)};
}
}
