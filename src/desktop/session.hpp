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
    int network = 0; // -1 infers the chain of an explicit endpoint and cookie pair.
    std::string datadir, cookie, url, zmq;
    bool operator==(const Node& other) const;
};
struct NodeDraft {
    Node settings;
    void prefill(Node suggestion);
    bool update(const Node& previous);
private:
    bool edited = false;
    bool suggested_cookie = false;
};
struct NodeTest {
    std::string status;
    std::optional<Node> target;
};
std::optional<Node> suggest_local_node();
NodeTest test_node(const Node& node, const std::function<bool()>& stopped = {});
struct Connection {
    std::string status;
    std::filesystem::path database;
    std::uint64_t generation = 0;
};
class Session {
    std::filesystem::path directory;
    std::mutex mutex;
    std::condition_variable changed;
    Node node;
    Connection connection;
    std::atomic<std::uint64_t> generation{0};
    std::atomic<bool> closing{false};
    std::thread worker;
    void observe(const Node& request, std::uint64_t current);
    void run();
public:
    explicit Session(std::filesystem::path directory);
    ~Session();
    void connect(Node requested);
    Connection state();
};
}
