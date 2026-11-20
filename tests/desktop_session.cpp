// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform.hpp"
#include "session.hpp"
#include <iostream>

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
        while (std::getline(std::cin,line)) {
            const auto request = Json::parse(line);
            const auto command = request.at("command").get<std::string>();
            if (command == "stop") return 0;
            if (command == "datadir") {
                std::cout << Json(platform::utf8_path(platform::default_datadir())).dump() << std::endl;
                continue;
            }
            if (command == "connect")
                session.connect({request.value("network",-1),request.value("datadir",""),
                    request.value("cookie",""),request.value("url",""),request.value("zmq","")});
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
