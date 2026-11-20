// SPDX-License-Identifier: GPL-3.0-or-later
#include "panel.hpp"
#include "platform.hpp"
#include <imkit/components.h>
#include <imkit/widgets.h>
#include <misc/cpp/imgui_stdlib.h>

namespace studio {
namespace {
constexpr const char* tree[]{"state","protocol","utxo"};
constexpr const char* flow[]{"traffic","mempool","blocks","utxo","all"};

void parameters(Panel& p) {
    auto& values = p.values[p.engine];
    auto& key = p.selected[p.engine];
    const auto& types = p.engine ? logstalgia_parameters() : gource_parameters();
    if (imkit::BeginCombo("Parameter",key.c_str())) {
        for (const auto& [name,type] : types)
            if (imkit::Selectable(name.c_str(),name == key)) key = name;
        imkit::EndCombo();
    }
    auto& value = values[key];
    ImGui::PushID(key.c_str());
    if (types.at(key) == "bool") {
        int choice = value.empty() ? 0 : (value == "true" ? 1 : 2);
        if (imkit::Combo("Value",&choice,"Default\0On\0Off\0"))
            value = choice == 0 ? "" : (choice == 1 ? "true" : "false");
    } else if (types.at(key) == "multi-value") {
        ImGui::InputTextMultiline("Values",&value,ImVec2(0,64));
        ImGui::TextDisabled("One entry per line");
    } else {
        ImGui::InputTextWithHint("Value","Engine default",&value);
    }
    ImGui::PopID();
}
}
const char* Panel::mode() const { return engine ? flow[projection[1]] : tree[projection[0]]; }

void Panel::draw(const Connection& connection, const char* gpu) {
    if (!visible) return;
    const auto size = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({16,16},ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({std::min(405.f,size.x-24),std::min(515.f,size.y-24)},ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(.95f);
    if (!ImGui::Begin("Bitcoin Viz",&visible)) { ImGui::End(); return; }
    ImGui::PushItemWidth(-106);
    ImGui::TextColored({1,.68f,.29f,1},"%s",preview ? "SYNTHETIC PREVIEW" : "NODE OBSERVER");
    ImGui::TextWrapped("%s",connection.status.c_str());
    ImGui::Spacing();
    if (ImGui::BeginTabBar("Controls")) {
        if (ImGui::BeginTabItem("Scene")) {
            static constexpr const char* engines[]{"Gource","Logstalgia"};
            if (imkit::Segmented("Engine",&engine,engines)) apply = true;
            if (imkit::Combo("Projection",&projection[engine],engine ? flow : tree,engine ? 5 : 3)) apply = true;
            ImGui::SeparatorText("Appearance");
            auto& opts = values[engine];
            auto& camera = opts[engine ? "paddle-mode" : "camera-mode"];
            const char* cameras[]{"overview","track"};
            const char* paddles[]{"single","pid","vhost"};
            const auto choices = engine ? paddles : cameras;
            int selected_mode = engine ? 1 : 0;
            for (int i=0;i<(engine ? 3 : 2);++i) if (camera == choices[i]) selected_mode=i;
            if (imkit::Combo(engine ? "Paddles" : "Camera",&selected_mode,choices,engine ? 3 : 2))
                camera = choices[selected_mode];
            auto& speed = opts[engine ? "simulation-speed" : "time-scale"];
            float rate = speed.empty() ? 1.f : std::strtof(speed.c_str(),nullptr);
            if (imkit::SliderFloat("Speed",&rate,.1f,engine ? 30.f : 4.f,"%.1fx")) speed=std::to_string(rate);
            if (!engine) imkit::SliderInt("Visible limit",&cap,1,20000);
            if (ImGui::CollapsingHeader("More parameters")) parameters(*this);
            if (imkit::ActionButton("Apply appearance")) apply = true;
            ImGui::Separator();
            if (imkit::Checkbox("Synthetic preview",&preview)) apply = true;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Node")) {
            const char* choices[]{"Auto (local)","Mainnet","Signet","Regtest","Testnet 4","Testnet 3"};
            int selection = node.network + 1;
            if (imkit::Combo("Network",&selection,choices,6)) node.network = selection - 1;
            const auto& network = networks[std::max(0,node.network)];
            auto base = node.datadir.empty() ? bitcoin_viz::platform::default_datadir() : std::filesystem::u8path(node.datadir);
            const auto url = "http://127.0.0.1:"+std::to_string(network.port);
            ImGui::InputTextWithHint("RPC URL",url.c_str(),&node.url);
            ImGui::InputTextWithHint("Data directory",bitcoin_viz::platform::utf8_path(bitcoin_viz::platform::default_datadir()).c_str(),&node.datadir);
            ImGui::InputTextWithHint("Cookie file",bitcoin_viz::platform::utf8_path(base/network.directory/".cookie").c_str(),&node.cookie);
            ImGui::InputTextWithHint("Sequence feed","Auto-discover (optional)",&node.zmq);
            if (imkit::ActionButton("Connect")) { reconnect = true; preview = false; }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (!error.empty()) ImGui::TextWrapped("%s",error.c_str());
    ImGui::Spacing();
    ImGui::TextDisabled("F1  Controls");
    ImGui::TextWrapped("OpenGL  %s",gpu);
    ImGui::PopItemWidth();
    ImGui::End();
}
}
