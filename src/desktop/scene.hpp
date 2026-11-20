// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "sdlapp.h"
#include "seeklog.h"
#include "settings.h"
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <sstream>

namespace studio {
using Parameters = std::map<std::string, std::string>;
using Lines = std::deque<std::string>;
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
    std::shared_ptr<void> parser; // Gource borrows the parsed stream for its lifetime.
    std::unique_ptr<SDLApp> app;
};
// Use the engine's option registry. File input, seeking, process and window
// management belong to the host in an embedded live stream.
template<class Settings> struct SceneOptions : Settings {
    Parameters catalog() const {
        const std::set<std::string> managed{"path","log-format","git-branch","start-position","stop-position",
            "loop","loop-delay-seconds","detect-changes","stop-at-end","stop-on-idle","dont-stop","author-time"};
        Parameters result;
        for (const auto& item : this->arg_types)
            if (!this->conf_sections.count(item.first) && !managed.count(item.first))
                result.insert(item);
        return result;
    }
};
inline void apply_parameters(ConfSection& section, const Parameters& values, const Parameters& types) {
    for (const auto& [key, value] : values) {
        if (value.empty()) continue;
        if (types.at(key) == "multi-value") {
            std::istringstream lines(value);
            for (std::string item; std::getline(lines,item);)
                if (!item.empty()) section.addEntry(key,item);
        } else section.setEntry(key,value);
    }
}
const Parameters& gource_parameters();
const Parameters& logstalgia_parameters();
Scene gource_scene(Lines&, const Parameters&);
Scene logstalgia_scene(Lines&, const Parameters&);
}
