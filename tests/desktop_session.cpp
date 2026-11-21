// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform.hpp"
#include "session.hpp"
#include <iostream>

namespace {
using bitcoin_viz::Json;
studio::Node read_node(const Json& request, const studio::Node& defaults = {}) {
    return {request.value("network",defaults.network),request.value("datadir",defaults.datadir),
        request.value("cookie",defaults.cookie),request.value("url",defaults.url),request.value("zmq",defaults.zmq)};
}
Json write_node(const studio::Node& node) {
    return {{"network",node.network},{"datadir",node.datadir},{"cookie",node.cookie},
        {"url",node.url},{"zmq",node.zmq}};
}
}

// Drive the same Session used by the desktop host without an OpenGL context.
int main() {
    using bitcoin_viz::Json;
    namespace fs = std::filesystem;
    namespace platform = bitcoin_viz::platform;
    try {
        platform::initialize_process();
        std::string line;
        if (!std::getline(std::cin,line)) return 1;
        studio::Session session(fs::u8path(Json::parse(line).get<std::string>()));
        studio::NodeDraft draft;
        while (std::getline(std::cin,line)) {
            const auto request = Json::parse(line);
            const auto command = request.at("command").get<std::string>();
            if (command == "stop") return 0;
            if (command == "datadir") {
                std::cout << Json(platform::utf8_path(platform::default_datadir())).dump() << std::endl;
                continue;
            }
            if (command == "desktop-cookie") {
                std::cout << Json(platform::utf8_path(platform::desktop_mainnet_cookie())).dump() << std::endl;
                continue;
            }
            if (command == "suggest") {
                const auto suggestion = studio::suggest_local_node();
                std::cout << (suggestion ? write_node(*suggestion) : Json()).dump() << std::endl;
                continue;
            }
            if (command == "test") {
                const auto tested = studio::test_node(read_node(request));
                std::cout << Json({{"status",tested.status},
                    {"target",tested.target ? write_node(*tested.target) : Json()}}).dump() << std::endl;
                continue;
            }
            if (command == "draft") {
                if (request.contains("suggestion")) draft.prefill(read_node(request.at("suggestion")));
                else {
                    const auto before = draft.settings;
                    draft.settings = read_node(request,before);
                    draft.update(before);
                }
                std::cout << write_node(draft.settings).dump() << std::endl;
                continue;
            }
            if (command == "connect")
                session.connect(read_node(request));
            else if (command != "state")
                throw std::runtime_error("Unknown Session test command");
            const auto state = session.state();
            std::cout << Json({{"status",state.status},{"database",platform::utf8_path(state.database)},
                {"generation",state.generation}}).dump() << std::endl;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Session test driver: " << error.what() << '\n';
        return 1;
    }
}
