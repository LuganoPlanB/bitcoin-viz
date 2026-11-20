// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "sdlapp.h"
#include "seeklog.h"
#include "settings.h"
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

class RCommitLog;
namespace studio {
using Parameters = std::map<std::string, std::string>;
using Lines = std::deque<std::string>;
inline constexpr const char* gource_modes[]{"state","protocol","utxo"};
inline constexpr const char* logstalgia_modes[]{"traffic","mempool","blocks","utxo","all"};

class SceneInput final : public BaseLog {
    Lines& lines;
public:
    explicit SceneInput(Lines& source) : lines(source) {}
    bool getNextLine(std::string& line) override {
        if (lines.empty()) return false;
        line = std::move(lines.front());
        lines.pop_front();
        return true;
    }
};
struct Scene {
    std::shared_ptr<RCommitLog> parser; // Gource borrows the parsed stream for its lifetime.
    std::unique_ptr<SDLApp> app;
};

struct LiveAppearance {
    float speed;
    vec3 background;
    std::string camera;
};

inline const char* speed_key(bool logstalgia) {
    return logstalgia ? "simulation-speed" : "time-scale";
}
inline const char* background_key(bool logstalgia) {
    return logstalgia ? "background" : "background-colour";
}
inline bool live_parameter(bool logstalgia, const std::string& key) {
    return key == speed_key(logstalgia) || key == background_key(logstalgia)
        || (!logstalgia && key == "camera-mode");
}

// Use the engine's option registry. File input, seeking, process and window
// management belong to the host in an embedded live stream.
template<class Settings> struct SceneOptions : Settings {
    Parameters catalog(const std::set<std::string>& managed) const {
        Parameters result;
        for (const auto& item : this->arg_types)
            if (!this->conf_sections.count(item.first) && !managed.count(item.first))
                result.insert(item);
        return result;
    }
};

// The native importer checks semantic ranges and combinations. Its numeric
// getters use atoi/atof, so check the whole value before handing it over.
inline void validate_parameter_type(const std::string& key, const std::string& value, const std::string& type) {
    ConfEntry entry(key,value);
    bool valid = true;
    if (type == "int" || type == "float") {
        std::istringstream input(value);
        input.imbue(std::locale::classic());
        if (type == "int") {
            int n;
            valid = bool(input >> n);
        } else {
            float n;
            valid = bool(input >> n) && std::isfinite(n);
        }
        valid = valid && (input >> std::ws).eof();
    } else if (type == "bool") {
        valid = entry.isBool();
    } else if (type == "colour") {
        valid = value.size() == 6 && value.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
        if (!valid && entry.isVec3()) {
            std::istringstream input(value.substr(value.find('(')+1));
            input.imbue(std::locale::classic());
            vec3 channels;
            char comma1, comma2, close;
            valid = bool(input >> channels.r >> comma1 >> channels.g >> comma2 >> channels.b >> close)
                && comma1 == ',' && comma2 == ',' && close == ')' && (input >> std::ws).eof();
        }
    }
    if (!valid)
        throw std::runtime_error(key + (type == "colour" ? " expects RRGGBB or vec3(r,g,b)" : " has an invalid value"));
}

inline void apply_parameters(ConfSection& section, const Parameters& values, const Parameters& types) {
    for (const auto& [key, value] : values) {
        const auto type = types.find(key);
        if (type == types.end()) throw std::runtime_error(key + " is not a scene option");
        if (value.empty()) continue;
        if (type->second == "multi-value") {
            std::istringstream lines(value);
            for (std::string item; std::getline(lines,item);) {
                if (!item.empty() && item.back() == '\r') item.pop_back();
                if (item.find_first_not_of(" \t") != std::string::npos) section.addEntry(key,item);
            }
        } else {
            validate_parameter_type(key,value,type->second);
            section.setEntry(key,value);
        }
    }
}
const Parameters& gource_parameters();
const Parameters& logstalgia_parameters();
LiveAppearance validate_gource_parameters(const Parameters&);
LiveAppearance validate_logstalgia_parameters(const Parameters&);
void apply_gource_appearance(SDLApp&, const LiveAppearance&, const Parameters& changes);
void apply_logstalgia_appearance(const LiveAppearance&, const Parameters& changes);
std::shared_ptr<RCommitLog> gource_parser(Lines&);
Scene gource_scene(Lines&, const Parameters&);
Scene logstalgia_scene(Lines&, const Parameters&);
}
