// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>

namespace studio {
struct LabelElision {
    std::string_view prefix;
    std::string_view suffix;
};

using EventLabelFormatter = std::function<std::string(const std::string&)>;

// Try literal rules in order. A rule must match both ends and leave visible text.
inline std::string_view elide_event_label(std::string_view label, std::initializer_list<LabelElision> rules) {
    for (const auto& [prefix,suffix] : rules) {
        if (prefix.empty() && suffix.empty()) continue;
        if (prefix.size() >= label.size() || suffix.size() >= label.size() - prefix.size()) continue;
        if (label.substr(0,prefix.size()) == prefix && label.substr(label.size()-suffix.size()) == suffix)
            return label.substr(prefix.size(),label.size()-prefix.size()-suffix.size());
    }
    return label;
}
}
