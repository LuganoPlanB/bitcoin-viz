// SPDX-License-Identifier: GPL-3.0-or-later
#include "session.hpp"
#include "platform.hpp"
#include <algorithm>

namespace studio {
using namespace bitcoin_viz;
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
        if (request.network < -1 || request.network >= int(std::size(networks)))
            throw std::runtime_error("Unknown Bitcoin network");
        const bool automatic = request.network == -1;
        const bool explicit_pair = !request.url.empty() && !request.cookie.empty();
        const auto base = request.datadir.empty() ? platform::default_datadir() : std::filesystem::u8path(request.datadir);
        std::string failure = "No local node cookie found. Start Bitcoin Core or edit Node settings.";
        for (int i=0; i<int(std::size(networks)) && !stopped(); ++i) {
            if (!automatic && request.network != i) continue;
            const auto& network = networks[i];
            Options o;
            o.network = network.chain;
            o.cookie = request.cookie.empty() ? base/network.directory/".cookie" : std::filesystem::u8path(request.cookie);
            o.rpc_url = request.url.empty() ? "http://127.0.0.1:"+std::to_string(network.port) : request.url;
            o.zmq = request.zmq;
            o.allow_rpc_polling = true;
            std::error_code cookie_error;
            if (!std::filesystem::is_regular_file(o.cookie,cookie_error)) {
                if (automatic && request.cookie.empty()) continue;
                throw std::runtime_error("Cannot read RPC cookie (node may be stopped)");
            }
            std::unique_ptr<RPC> rpc;
            try {
                rpc = std::make_unique<RPC>(o.rpc_url,o.cookie,stopped);
                const auto chain = rpc->call("getblockchaininfo").at("chain").get<std::string>();
                if (automatic && explicit_pair) {
                    if (std::none_of(std::begin(networks),std::end(networks),
                            [&](const Network& candidate){ return candidate.chain == chain; }))
                        throw std::runtime_error("RPC endpoint returned an unsupported chain");
                    o.network = chain;
                } else if (chain != o.network) {
                    throw std::runtime_error("RPC endpoint returned another chain");
                }
            } catch (const std::exception& e) {
                failure = (automatic && explicit_pair ? std::string() : o.network + " / ") + e.what();
                if (!automatic || explicit_pair) break;
                continue;
            }
            if (stopped()) return;
            o.database = directory/(o.network+"-"+hash256(o.rpc_url+"|"+platform::utf8_path(o.cookie)).substr(0,16)+".sqlite");
            WriterLock writer(o.database);
            Store store(o.database);
            o.report = [&](ObserverState state, const std::string& detail) {
                const char* phase = "Reconnecting";
                if (state == ObserverState::synchronizing) phase = "Synchronizing";
                if (state == ObserverState::connected) phase = "Connected";
                publish(o.network+" / "+phase+" / "+detail,state == ObserverState::connected ? &o.database : nullptr);
            };
            Observer observer(store,*rpc,o);
            observer.run(stopped);
            return;
        }
        publish("Retrying / " + failure);
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
