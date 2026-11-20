// SPDX-License-Identifier: GPL-3.0-or-later
#include "gource.h"
#include "formats/custom.h"
#include "stringhash.h"
#include "scene.hpp"
#include <SDL_image.h>
#include <filesystem>
#include <fstream>

namespace studio {
namespace {
struct DraftSettings {
    int hash_seed = gStringHashSeed; // The native settings importer updates this shared value.
    SceneOptions<GourceSettings> settings;
    ~DraftSettings() {
        for (const auto* filters : {&settings.file_filters, &settings.file_show_filters,
                                    &settings.user_filters, &settings.user_show_filters})
            for (auto filter : *filters) delete filter;
        gStringHashSeed = hash_seed;
    }
};

void import(GourceSettings& settings, const Parameters& options) {
    ConfFile config;
    auto section = config.addSection("gource");
    section->setEntry("path",".");
    section->setEntry("file-idle-time","0");
    if (!options.count("seconds-per-day")) section->setEntry("realtime","true");
    section->setEntry("disable-auto-skip","true");
    apply_parameters(*section,options,gource_parameters());
    settings.importGourceSettings(config);
}

void validate_image(const char* key, const std::string& file) {
    if (file.empty()) return;
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(file)))
        throw std::runtime_error(std::string(key) + " needs an image file");
    std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> image(IMG_Load(file.c_str()),SDL_FreeSurface);
    if (!image) throw std::runtime_error(std::string(key) + " cannot load image: " + IMG_GetError());
}
}

const Parameters& gource_parameters() {
    static const auto types = [] {
        DraftSettings draft;
        auto catalog = draft.settings.catalog({"path","log-format","git-branch","start-position","stop-position",
            "start-date","stop-date","stop-at-time","loop","loop-delay-seconds","stop-at-end","stop-on-idle",
            "dont-stop","author-time","file-idle-time-at-end","hide-progress"});
        // The importer has these effective types, even where the CLI registry uses string.
        catalog["user-idle-time"] = "float";
        catalog.at("file-idle-time") = "int";
        for (auto key : {"max-file-lag","user-scale"}) catalog.at(key) = "float";
        for (auto key : {"background-colour","font-colour","highlight-colour","selection-colour",
                         "dir-colour","caption-colour","filename-colour"})
            catalog.at(key) = "colour";
        return catalog;
    }();
    return types;
}

LiveAppearance validate_gource_parameters(const Parameters& options) {
    DraftSettings draft;
    import(draft.settings,options);
    const auto& settings = draft.settings;
    validate_image("logo",settings.logo);
    validate_image("background-image",settings.background_image);
    validate_image("default-user-image",settings.default_user_image);
    if (!settings.caption_file.empty()) {
        const auto path = std::filesystem::u8path(settings.caption_file);
        if (!std::filesystem::is_regular_file(path) || !std::ifstream(path))
            throw std::runtime_error("caption-file needs a readable file");
    }
    return {settings.time_scale, settings.background_colour, settings.camera_mode};
}

void apply_gource_appearance(SDLApp& app, const LiveAppearance& appearance, const Parameters& changes) {
    if (changes.count("time-scale")) gGourceSettings.time_scale = appearance.speed;
    if (changes.count("background-colour")) gGourceSettings.background_colour = appearance.background;
    if (changes.count("camera-mode")) static_cast<Gource&>(app).setCameraMode(appearance.camera);
}

std::shared_ptr<RCommitLog> gource_parser(Lines& lines) {
    auto input = std::make_unique<SceneInput>(lines);
    auto parser = std::make_shared<CustomLog>(input.get());
    input.release(); // CustomLog owns the stream now.
    return parser;
}

Scene gource_scene(Lines& lines, const Parameters& options) {
    import(gGourceSettings,options);
    gGourceSettings.path = "-";
    gGourceSettings.file_graphic = nullptr;
    auto parser = gource_parser(lines);
    return {parser,std::make_unique<Gource>(nullptr,parser.get())};
}
}
