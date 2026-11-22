// SPDX-License-Identifier: GPL-3.0-or-later
#include "panel.hpp"
#include "platform.hpp"
#include "window_video_capture.hpp"
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

void recording_controls(Panel& panel, const WindowVideoCapture& video, const std::string& directory,
                        float label_width) {
    const bool active = video.status() == VideoTakeStatus::Recording;
    ImGui::BeginDisabled(active);
    setting("Output path",label_width,[&] {
        ImGui::InputTextWithHint("##Output path","Automatic dated name",&panel.video_path);
    });
    setting("JPEG quality",label_width,[&] {
        imkit::SliderInt("##JPEG quality",&panel.video_quality,1,100,"%d",ImGuiSliderFlags_AlwaysClamp);
    });
    imkit::Checkbox("Include overlay",&panel.include_overlay);
    ImGui::EndDisabled();

    if (imkit::Button(active ? "Stop###VideoTake" : "Start###VideoTake")) panel.toggle_video = true;
    ImGui::SetItemTooltip("Keyboard shortcut: R");
    ImGui::SameLine();
    const auto seconds = video.elapsed().count();
    ImGui::Text("Elapsed %02lld:%02lld",static_cast<long long>(seconds / 60),
                static_cast<long long>(seconds % 60));

    switch (video.status()) {
    case VideoTakeStatus::Idle:
        if (panel.video_path.empty()) {
            ImGui::TextUnformatted("Automatic folder");
            ImGui::TextWrapped("%s",directory.c_str());
        }
        break;
    case VideoTakeStatus::Recording:
        ImGui::TextUnformatted("Recording");
        break;
    case VideoTakeStatus::Saved:
        if (video.detail() == "Stopped") ImGui::TextUnformatted("Saved");
        else ImGui::TextWrapped("Saved (%s)",video.detail().c_str());
        break;
    case VideoTakeStatus::Error:
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,.63f,.57f,1));
        ImGui::TextWrapped("%s",video.detail().c_str());
        ImGui::PopStyleColor();
        break;
    }
    if (!video.path().empty()) ImGui::TextWrapped("%s",bitcoin_viz::platform::utf8_path(video.path()).c_str());
}
}
void Panel::toggle() {
    visible = !visible;
    focus = visible;
    if (!visible) ImGui::SetWindowFocus(nullptr);
}

void Panel::draw(const Connection& connection, bool fullscreen, const WindowVideoCapture& video,
                 const std::string& video_directory) {
    if (!visible) return;
    const auto& style = ImGui::GetStyle();
    const auto& viewport = *ImGui::GetMainViewport();
    const auto margin = 10 * style.FontScaleMain;
    const auto width = std::min({362 * style.FontScaleMain,viewport.WorkSize.x - 2 * margin,
        std::max(278 * style.FontScaleMain,viewport.WorkSize.x * .55f)});
    const auto max_height = viewport.WorkSize.y - 2 * margin;
    ImGui::SetNextWindowPos({viewport.WorkPos.x+margin,viewport.WorkPos.y+margin},ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints({width,0},{width,max_height});
    ImGui::SetNextWindowBgAlpha(.97f);
    if (std::exchange(focus,false)) ImGui::SetNextWindowFocus();
    const auto flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("Bitcoin Viz",&visible,flags)) { ImGui::End(); return; }
    if (imkit::Checkbox("Synthetic preview",&preview)) change_scene = true;
    const float preview_right = ImGui::GetItemRectMax().x;
    ImGui::SetItemTooltip("Show generated Bitcoin activity. Turn off to return to your node or recording.");
    constexpr const char* fullscreen_label = "Fullscreen (F)";
    const float switch_width = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize(fullscreen_label).x;
    const float content_right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (preview_right + style.ItemSpacing.x + switch_width <= content_right) ImGui::SameLine();
    if (imkit::Switch(fullscreen_label,&fullscreen)) toggle_fullscreen = true;
    if (!fullscreen_error.empty()) ImGui::TextWrapped("%s",fullscreen_error.c_str());
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
        const auto node_flags = std::exchange(open_node,false) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        if (ImGui::BeginTabItem("Node",nullptr,node_flags)) {
            auto& settings = node.settings;
            const auto before = settings;
            const char* choices[]{"Detect from endpoint","Mainnet","Signet","Regtest","Testnet 4","Testnet 3"};
            int selection = settings.network + 1;
            setting("Network",label_width,[&] {
                if (imkit::Combo("##Network",&selection,choices,6)) settings.network = selection - 1;
            });
            const auto& network = networks[std::max(0,settings.network)];
            const auto url = settings.network < 0 ? "Loopback HTTP URL" : "http://127.0.0.1:"+std::to_string(network.port);
            setting("RPC URL",label_width,[&] { ImGui::InputTextWithHint("##RPC URL",url.c_str(),&settings.url); });
            setting("Data directory",label_width,[&] {
                ImGui::InputTextWithHint("##Data directory","Custom Core data directory",&settings.datadir);
            });
            setting("Cookie file",label_width,[&] {
                ImGui::InputTextWithHint("##Cookie file","RPC cookie path",&settings.cookie);
            });
            setting("Sequence feed",label_width,[&] {
                ImGui::InputTextWithHint("##Sequence feed","Auto-discover",&settings.zmq);
            });
            if (node.update(before)) tested = {};
            ImGui::BeginDisabled(testing);
            if (imkit::ActionButton("Test Connection")) { test_connection = true; tested = {}; }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!tested.target.has_value());
            if (imkit::Button("Connect")) { reconnect = true; preview = false; }
            ImGui::EndDisabled();
            if (testing) ImGui::TextDisabled("Testing connection…");
            else if (!tested.status.empty()) {
                if (!tested.target) ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,.72f,.45f,1));
                ImGui::TextWrapped("%s",tested.status.c_str());
                if (!tested.target) ImGui::PopStyleColor();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Recording")) {
            recording_controls(*this,video,video_directory,label_width);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
}
