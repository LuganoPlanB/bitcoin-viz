// SPDX-License-Identifier: GPL-3.0-or-later
#include "session.hpp"
#include "platform.hpp"
#include <algorithm>
#include <tuple>

namespace studio {
using namespace bitcoin_viz;
namespace {
Options resolve_node(const Node& request) {
    if (request.network < -1 || request.network >= int(std::size(networks)))
        throw std::runtime_error("Unknown Bitcoin network");
    if (request.network == -1 && (request.url.empty() || request.cookie.empty()))
        throw std::runtime_error("Detect network needs an explicit RPC URL and cookie file");
    const auto& network = networks[std::max(0,request.network)];
    Options options;
    options.network = request.network == -1 ? "" : network.chain;
    options.rpc_url = request.url.empty() ? "http://127.0.0.1:"+std::to_string(network.port) : request.url;
    options.zmq = request.zmq;
    options.allow_rpc_polling = true;
    if (!request.cookie.empty()) {
        options.cookie = std::filesystem::u8path(request.cookie);
    } else if (!request.datadir.empty()) {
        options.cookie = std::filesystem::u8path(request.datadir)/network.directory/".cookie";
    } else if (!request.url.empty()) {
        throw std::runtime_error("Enter a cookie file or data directory for this RPC URL");
    } else {
        options.cookie = request.network == 0 ? platform::desktop_mainnet_cookie() :
            platform::default_datadir()/network.directory/".cookie";
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(options.cookie,error))
        throw std::runtime_error("Cannot read RPC cookie (node may be stopped)");
    return options;
}

void verify_chain(Options& options, RPC& rpc) {
    const auto chain = rpc.call("getblockchaininfo").at("chain").get<std::string>();
    if (!options.network.empty() && chain != options.network)
        throw std::runtime_error("RPC endpoint returned another chain");
    if (std::none_of(std::begin(networks),std::end(networks),
                    [&](const Network& candidate){ return candidate.chain == chain; }))
        throw std::runtime_error("RPC endpoint returned an unsupported chain");
    options.network = chain;
}
}

bool Node::operator==(const Node& other) const {
    return std::tie(network,datadir,cookie,url,zmq) ==
        std::tie(other.network,other.datadir,other.cookie,other.url,other.zmq);
}

void NodeDraft::prefill(Node suggestion) {
    if (edited || !(settings == Node{})) return;
    settings = std::move(suggestion);
    suggested_cookie = true;
}

bool NodeDraft::update(const Node& previous) {
    if (settings == previous) return false;
    edited = true;
    if (suggested_cookie) {
        const bool endpoint_changed = settings.url != previous.url;
        const bool network_changed = settings.network != previous.network;
        const bool cookie_changed = settings.cookie != previous.cookie;
        if (endpoint_changed || network_changed || settings.datadir != previous.datadir) {
            if (!cookie_changed) settings.cookie.clear();
            if (network_changed && !endpoint_changed) settings.url.clear();
            suggested_cookie = false;
        } else if (cookie_changed) {
            suggested_cookie = false;
        }
    }
    return true;
}

std::optional<Node> suggest_local_node() {
    const auto cookie = platform::desktop_mainnet_cookie();
    std::error_code error;
    if (!std::filesystem::is_regular_file(cookie,error)) return std::nullopt;
    try {
        if (!tcp_loopback_reachable(8332)) return std::nullopt;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
    return Node{0,{},platform::utf8_path(cookie),"http://127.0.0.1:8332",{}};
}

NodeTest test_node(const Node& node, const std::function<bool()>& stopped) {
    try {
        auto options = resolve_node(node);
        RPC rpc(options.rpc_url,options.cookie,stopped);
        verify_chain(options,rpc);
        auto target = node;
        target.url = options.rpc_url;
        target.cookie = platform::utf8_path(options.cookie);
        const auto network = std::find_if(std::begin(networks),std::end(networks),
            [&](const Network& candidate){ return candidate.chain == options.network; });
        target.network = static_cast<int>(network-std::begin(networks));
        return {"Verified / " + options.network,std::move(target)};
    } catch (const std::exception& e) {
        return {e.what(),std::nullopt};
    }
}

Session::Session(std::filesystem::path dir) : directory(std::move(dir)), worker(&Session::run,this) {}
Session::~Session() {
    { std::lock_guard lock(mutex); closing = true; }
    changed.notify_one();
    worker.join();
}
void Session::connect(Node requested) {
    std::lock_guard lock(mutex);
    node = std::move(requested);
    connection = {"Connecting",{},++generation};
    changed.notify_one();
}
Connection Session::state() {
    std::lock_guard lock(mutex);
    return connection;
}
void Session::observe(const Node& request, std::uint64_t current) {
    const auto stopped = [this,current]{ return closing || generation != current; };
    const auto publish = [&](std::string status, const std::filesystem::path* database = nullptr) {
        std::lock_guard lock(mutex);
        if (stopped()) return;
        connection.status = std::move(status);
        if (database) connection.database = *database;
    };
    try {
        auto options = resolve_node(request);
        RPC rpc(options.rpc_url,options.cookie,stopped);
        verify_chain(options,rpc);
        if (stopped()) return;
        options.database = directory/(options.network+"-"+
            hash256(options.rpc_url+"|"+platform::utf8_path(options.cookie)).substr(0,16)+".sqlite");
        WriterLock writer(options.database);
        Store store(options.database);
        options.report = [&](ObserverState state, const std::string& detail) {
            const char* phase = "Reconnecting";
            if (state == ObserverState::synchronizing) phase = "Synchronizing";
            if (state == ObserverState::connected) phase = "Connected";
            publish(options.network+" / "+phase+" / "+detail,state == ObserverState::connected ? &options.database : nullptr);
        };
        Observer observer(store,rpc,options);
        observer.run(stopped);
    } catch (const std::exception& e) {
        const std::filesystem::path empty;
        publish(std::string("Retrying / ") + e.what(),&empty);
    }
}
void Session::run() {
    std::unique_lock lock(mutex);
    changed.wait(lock,[&]{ return closing || generation != 0; });
    while (!closing) {
        const auto current = generation.load();
        const auto request = node;
        lock.unlock();
        observe(request,current);
        lock.lock();
        changed.wait_for(lock,std::chrono::seconds(5),[&]{ return closing || generation != current; });
    }
}
}
