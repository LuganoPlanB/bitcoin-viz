// SPDX-License-Identifier: GPL-3.0-or-later
#include "panel.hpp"
#include "platform.hpp"
#include <imkit/components.h>
#include <imkit/widgets.h>
#include <misc/cpp/imgui_stdlib.h>
#include <cfloat>

namespace studio {
namespace {
template<class Widget>
void setting(const char* label, float label_width, Widget draw) {
    if (!imkit::BeginSettingRow(label,label,label_width)) return;
    ImGui::SetNextItemWidth(-FLT_MIN);
    draw();
    imkit::EndSettingRow();
}

void parameters(Panel& p) {
    auto& values = p.values[p.engine];
    auto& key = p.selected[p.engine];
    const auto& types = p.engine ? logstalgia_parameters() : gource_parameters();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (imkit::BeginCombo("##Parameter",key.c_str())) {
        for (const auto& [name,type] : types) {
            if (name == "camera-mode" || name == "paddle-mode" || name == "hide-paddle" ||
                name == speed_key(p.engine) || name == background_key(p.engine))
                continue;
            if (imkit::Selectable(name.c_str(),name == key)) key = name;
        }
        imkit::EndCombo();
    }
    auto& value = values[key];
    const auto& type = types.at(key);
    ImGui::PushID(key.c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (type == "bool") {
        int choice = value.empty() ? 0 : (value == "true" ? 1 : 2);
        if (imkit::Combo("##Value",&choice,"Default\0On\0Off\0"))
            value = choice == 0 ? "" : (choice == 1 ? "true" : "false");
    } else if (type == "multi-value") {
        ImGui::InputTextMultiline("##Values",&value,ImVec2(-FLT_MIN,4 * ImGui::GetTextLineHeight()));
        if (key == "group")
            ImGui::TextWrapped("One group per line, e.g. Traffic,URI=^/traffic,40");
        else
            ImGui::TextDisabled("One entry per line");
    } else {
        const char* hint = "Engine default";
        if (type == "colour") hint = "RRGGBB";
        if (type == "int") hint = "Integer (blank for default)";
        if (type == "float") hint = "Number (blank for default)";
        ImGui::InputTextWithHint("##Value",hint,&value);
    }
    ImGui::TextDisabled("%s",live_parameter(p.engine,key) ? "Updates the current scene" : "Rebuilds the scene");
    ImGui::PopID();
}
}
void Panel::toggle() {
    visible = !visible;
    focus = visible;
    if (!visible) ImGui::SetWindowFocus(nullptr);
}

void Panel::draw(const Connection& connection) {
    if (!visible) return;
    const auto& style = ImGui::GetStyle();
    const auto viewport = ImGui::GetMainViewport()->WorkSize;
    const auto margin = 10 * style.FontScaleMain;
    const auto width = std::min({362 * style.FontScaleMain,viewport.x - 2 * margin,
        std::max(278 * style.FontScaleMain,viewport.x * .55f)});
    const auto max_height = viewport.y - 2 * margin;
    ImGui::SetNextWindowPos({margin,margin});
    ImGui::SetNextWindowSizeConstraints({width,0},{width,max_height});
    ImGui::SetNextWindowBgAlpha(.97f);
    if (std::exchange(focus,false)) ImGui::SetNextWindowFocus();
    const auto flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("Bitcoin Viz",&visible,flags)) { ImGui::End(); return; }
    if (imkit::Checkbox("Synthetic preview",&preview)) change_scene = true;
    constexpr const char* shortcut = "F1: Hide";
    const float shortcut_width = ImGui::CalcTextSize(shortcut).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f,ImGui::GetContentRegionAvail().x-shortcut_width));
    ImGui::TextDisabled("%s",shortcut);
    if (connection.status.starts_with("Retrying / ")) {
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,.72f,.45f,1));
        ImGui::TextWrapped("%s",connection.status.c_str());
        ImGui::PopStyleColor();
    } else if (!connection.status.empty()) {
        ImGui::TextWrapped("%s",connection.status.c_str());
    }

    const auto label_width = std::max(ImGui::CalcTextSize("Data directory").x,ImGui::CalcTextSize("Sequence feed").x)
        + style.ItemInnerSpacing.x;
    if (ImGui::BeginTabBar("Controls")) {
        if (ImGui::BeginTabItem("Scene")) {
            static constexpr const char* engines[]{"Gource","Logstalgia"};
            if (imkit::Segmented("Engine",&engine,engines)) change_scene = true;
            setting("Projection",label_width,[&] {
                if (imkit::Combo("##Projection",&projection[engine],engine ? logstalgia_modes : gource_modes,
                                 engine ? std::size(logstalgia_modes) : std::size(gource_modes)))
                    change_scene = true;
            });
            auto& opts = values[engine];
            if (engine) {
                auto& paddle = opts["paddle-mode"];
                auto& hidden = opts["hide-paddle"];
                const char* modes[]{"single","pid","vhost","Hidden"};
                int choice = hidden == "true" ? 3 : paddle == "single" ? 0 : paddle == "vhost" ? 2 : 1;
                setting("Paddles",label_width,[&] {
                    if (imkit::Combo("##Paddles",&choice,modes,std::size(modes))) {
                        hidden = choice == 3 ? "true" : "";
                        if (choice != 3) paddle = modes[choice];
                    }
                });
            } else {
                auto& camera = opts["camera-mode"];
                const char* modes[]{"overview","track"};
                int choice = camera == "track";
                setting("Camera",label_width,[&] {
                    if (imkit::Combo("##Camera",&choice,modes,std::size(modes))) camera = modes[choice];
                });
            }
            setting("Speed",label_width,[&] { ImGui::InputTextWithHint("##Speed","1.0x",&opts[speed_key(engine)]); });
            setting("Background",label_width,[&] {
                ImGui::InputTextWithHint("##Background","RRGGBB",&opts[background_key(engine)]);
            });
            if (!engine)
                setting("Visible limit",label_width,[&] { imkit::SliderInt("##Visible limit",&cap,1,20000); });

            if (imkit::ActionButton("Apply appearance")) apply_appearance = true;
            ImGui::SameLine();
            if (imkit::Button("Discard")) {
                opts = applied_values[engine];
                if (!engine) cap = applied_cap;
                error.clear();
            }
            if (!error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,.63f,.57f,1));
                ImGui::TextWrapped("%s",error.c_str());
                ImGui::PopStyleColor();
            }
            if (ImGui::CollapsingHeader("More parameters")) {
                parameters(*this);
                ImGui::TextWrapped("Bitcoin Viz supplies the data stream and manages the window.");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Node")) {
            const char* choices[]{"Auto (local)","Mainnet","Signet","Regtest","Testnet 4","Testnet 3"};
            int selection = node.network + 1;
            setting("Network",label_width,[&] {
                if (imkit::Combo("##Network",&selection,choices,6)) node.network = selection - 1;
            });
            const auto& network = networks[std::max(0,node.network)];
            auto base = node.datadir.empty() ? bitcoin_viz::platform::default_datadir() : std::filesystem::u8path(node.datadir);
            const auto url = "http://127.0.0.1:"+std::to_string(network.port);
            setting("RPC URL",label_width,[&] { ImGui::InputTextWithHint("##RPC URL",url.c_str(),&node.url); });
            setting("Data directory",label_width,[&] {
                const auto hint = bitcoin_viz::platform::utf8_path(bitcoin_viz::platform::default_datadir());
                ImGui::InputTextWithHint("##Data directory",hint.c_str(),&node.datadir);
            });
            setting("Cookie file",label_width,[&] {
                const auto hint = bitcoin_viz::platform::utf8_path(base/network.directory/".cookie");
                ImGui::InputTextWithHint("##Cookie file",hint.c_str(),&node.cookie);
            });
            setting("Sequence feed",label_width,[&] {
                ImGui::InputTextWithHint("##Sequence feed","Auto-discover",&node.zmq);
            });
            if (imkit::ActionButton("Connect")) { reconnect = true; preview = false; }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
}
