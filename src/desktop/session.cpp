// SPDX-License-Identifier: GPL-3.0-or-later
#include "session.hpp"
#include "platform.hpp"

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
    connection = {"Connecting",{}};
    ++generation;
    changed.notify_one();
}
Connection Session::state() {
    std::lock_guard lock(mutex);
    return connection;
}
void Session::run() {
    std::unique_lock lock(mutex);
    changed.wait(lock,[&]{ return closing || generation != 0; });
    while (!closing) {
        const auto current = generation.load();
        const auto request = node;
        lock.unlock();
        const auto stopped = [&]{ return closing || generation != current; };
        auto report = [&](std::string message) {
            std::lock_guard guard(mutex);
            if (!stopped()) connection.status = std::move(message);
        };
        try {
            const auto base = request.datadir.empty() ? platform::default_datadir() : std::filesystem::u8path(request.datadir);
            std::string failure = "No local node cookie found. Start Bitcoin Core or edit Node settings.";
            for (int i=0; i<int(std::size(networks)) && !stopped(); ++i) {
                if (request.network >= 0 && request.network != i) continue;
                const auto& network = networks[i];
                Options o;
                o.network = network.chain;
                o.cookie = request.cookie.empty() ? base/network.directory/".cookie" : std::filesystem::u8path(request.cookie);
                o.rpc_url = request.url.empty() ? "http://127.0.0.1:"+std::to_string(network.port) : request.url;
                o.zmq = request.zmq;
                o.allow_rpc_polling = true;
                if (request.network < 0 && !std::filesystem::exists(o.cookie)) continue;
                RPC rpc(o.rpc_url,o.cookie);
                try {
                    auto info = rpc.call("getblockchaininfo");
                    if (info.at("chain") != o.network) throw std::runtime_error("RPC endpoint returned another chain");
                } catch (const std::exception& e) {
                    failure = o.network + " / " + e.what();
                    continue;
                }
                o.database = directory/(o.network+"-"+hash256(o.rpc_url+"|"+platform::utf8_path(o.cookie)).substr(0,16)+".sqlite");
                WriterLock writer(o.database);
                Store store(o.database);
                {
                    std::lock_guard guard(mutex);
                    if (stopped()) break;
                    connection.database = o.database;
                }
                o.report = [&](const std::string& status){ report(o.network+" / "+status); };
                Observer observer(store,rpc,o);
                observer.run(stopped);
                break;
            }
            if (!stopped()) report(failure);
        } catch (const std::exception& e) { report(e.what()); }
        lock.lock();
        changed.wait_for(lock,std::chrono::seconds(5),stopped);
    }
}
}
