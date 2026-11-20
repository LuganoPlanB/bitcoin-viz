// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "bitcoin_viz.hpp"

namespace studio {
struct Network {
    const char* chain;
    const char* directory;
    int port;
};
inline constexpr Network networks[]{{"main","",8332},{"signet","signet",38332},{"regtest","regtest",18443},
    {"testnet4","testnet4",48332},{"test","testnet3",18332}};
struct Node {
    int network = -1; // Auto probes local standard cookies, mainnet first.
    std::string datadir, cookie, url, zmq;
};
struct Connection {
    std::string status;
    std::filesystem::path database;
};
class Session {
    std::filesystem::path directory;
    std::mutex mutex;
    std::condition_variable changed;
    Node node;
    Connection connection;
    std::atomic<unsigned> generation{0};
    std::atomic<bool> closing{false};
    std::thread worker;
    void run();
public:
    explicit Session(std::filesystem::path directory);
    ~Session();
    void connect(Node requested);
    Connection state();
};
}
