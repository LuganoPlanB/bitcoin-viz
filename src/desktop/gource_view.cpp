// SPDX-License-Identifier: GPL-3.0-or-later
#include "gource.h"
#include "formats/custom.h"
#include "scene.hpp"

namespace studio {
const Parameters& gource_parameters() {
    static const auto types = SceneOptions<GourceSettings>().catalog();
    return types;
}
Scene gource_scene(Lines& lines, const Parameters& options) {
    ConfFile config;
    auto section = config.addSection("gource");
    section->setEntry("path",".");
    section->setEntry("file-idle-time","0");
    section->setEntry("realtime","true");
    section->setEntry("disable-auto-skip","true");
    apply_parameters(*section,options,gource_parameters());
    gGourceSettings.importGourceSettings(config);
    gGourceSettings.path = "-";
    gGourceSettings.file_graphic = nullptr;
    auto input = std::make_unique<SceneInput>(lines);
    auto parser = std::make_shared<CustomLog>(input.get());
    input.release(); // CustomLog owns the stream now.
    return {parser,std::make_unique<Gource>(nullptr,parser.get())};
}
}
