// SPDX-License-Identifier: GPL-3.0-or-later
#include "bitcoin_viz.hpp"
#include <algorithm>
#include <iostream>
#include <limits>

namespace bitcoin_viz {
namespace {
bool wait_stop(const std::function<bool()>& stopped, double seconds) {
    const auto end = Clock::now() + std::chrono::duration<double>(seconds);
    while (!stopped() && Clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    return stopped();
}
}
Observer::Observer(Store& store, RPC& rpc, Options options)
    : s_(store), rpc_(rpc), options_(std::move(options)), model(store,options_.window,options_.horizon) {
    if (!options_.capture_dir.empty()) capture_ = std::make_unique<CaptureReader>(s_,model,options_.capture_dir);
}
void Observer::identify() {
    const auto chain = rpc_.call("getblockchaininfo");
    if (chain.at("chain") != options_.network) throw std::runtime_error("Core network differs from --network " + options_.network);
    const auto network = rpc_.call("getnetworkinfo");
    const auto genesis = rpc_.call("getblockhash",Json::array({0}));
    if (!s_.get("genesis").is_null() && s_.get("genesis") != genesis) throw std::runtime_error("Database belongs to a different chain; choose a new --db");
    {
        Transaction tx(s_);
        s_.put("genesis",genesis); s_.put("network",options_.network); s_.put("core_version",network.at("version"));
        s_.put("node_run",session_id()); s_.put("window_blocks",model.window); s_.put("undo_horizon",model.undo_horizon);
        s_.event("observer.session","node:snapshot","startup-or-node-restart",{{"core_version",network.at("version")},{"network",options_.network}});
        tx.commit();
    }
    cookie_marker_ = rpc_.cookie_marker;
    counter_.reset();
    if (options_.zmq.empty()) {
        for (const auto& notifier : rpc_.call("getzmqnotifications")) {
            if (notifier.at("type") == "pubsequence") { options_.zmq = notifier.at("address"); break; }
        }
        if (options_.zmq.empty()) throw std::runtime_error("Core has no sequence publisher; configure zmqpubsequence=unix:/.../sequence.sock");
    }
    options_.zmq = endpoint(options_.zmq);
}
void Observer::rebuild(const Json& chain, const std::string& cause) {
    const auto height = std::max(chain.value("pruneheight",std::int64_t(0)),chain.at("blocks").get<std::int64_t>()-options_.bootstrap);
    const auto hash = rpc_.call("getblockhash",Json::array({height})).get<std::string>();
    Transaction tx(s_); model.checkpoint(hash,height,cause); tx.commit();
}
Json Observer::sync_chain() {
    const auto chain = rpc_.call("getblockchaininfo");
    if (chain.at("chain") != options_.network) throw std::runtime_error("Core changed network");
    { Transaction tx(s_); s_.put("initialblockdownload",chain.value("initialblockdownload",Json(nullptr))); tx.commit(); }
    if (s_.get("tip").is_null()) rebuild(chain,"initial-observed-window");
    auto tip = s_.get("tip");
    const auto target_height = chain.at("blocks").get<std::int64_t>();
    if (model.window && target_height-tip.at("height").get<std::int64_t>() > std::max(model.window,options_.bootstrap))
        rebuild(chain,"catchup-exceeds-rolling-window");
    while (true) {
        tip = s_.get("tip");
        const auto header = rpc_.call("getblockheader",Json::array({tip.at("hash")}));
        if (header.value("confirmations",std::int64_t(-1)) >= 0) break;
        try { Transaction tx(s_); model.disconnect(tip.at("hash")); tx.commit(); }
        catch (const RebuildRequired& e) { rebuild(chain,e.what()); break; }
    }
    tip = s_.get("tip");
    const auto height = tip.at("height").get<std::int64_t>();
    for (auto h = height+1; h <= std::min(target_height,height+32); ++h) {
        const auto hash = rpc_.call("getblockhash",Json::array({h}));
        Json block;
        try { block = rpc_.call("getblock",Json::array({hash,2})); }
        catch (const RPCError& e) {
            if (e.code != -1 && e.code != -5) throw;
            rebuild(chain,"pruned-or-unavailable-history");
            throw RebuildRequired("Block content unavailable; rebuilt checkpoint, retry required");
        }
        Transaction tx(s_); model.connect(block); tx.commit();
    }
    return s_.get("tip");
}
void Observer::reconcile(std::string cause) {
    rpc_.call("uptime");
    if (!cookie_marker_.empty() && cookie_marker_ != rpc_.cookie_marker) {
        identify();
        if (feed_) { feed_.reset(); feed_ = std::make_unique<SequenceFeed>(options_.zmq); feed_->start(); last_drop_ = 0; }
        cause = "node-restart";
    }
    for (int i = 0; i < 5; ++i) {
        const auto tip = sync_chain();
        const auto before = rpc_.call("getbestblockhash");
        const auto snapshot = rpc_.call("getrawmempool",Json::array({false,true}));
        const auto after = rpc_.call("getbestblockhash");
        if (before != after || after != tip.at("hash")) continue;
        Transaction tx(s_);
        model.reconcile_mempool(snapshot.at("txids"),snapshot.at("mempool_sequence").get<std::uint64_t>());
        s_.put("last_reconcile_ns",now_ns()); s_.put("synchronized",true);
        s_.event("observer.resync","node:snapshot",cause,{{"tip",tip},{"mempool_count",snapshot.at("txids").size()},
            {"mempool_sequence",text(snapshot.at("mempool_sequence"))},{"history_complete",cause == "periodic" ? Json(nullptr) : Json(false)}});
        tx.commit(); return;
    }
    throw RebuildRequired("Chain changed across five snapshot attempts");
}
void Observer::metadata() {
    const auto rows = s_.query("SELECT txid FROM mempool ORDER BY txid");
    if (rows.empty()) return;
    const auto budget = std::min(static_cast<std::size_t>(options_.metadata_budget),rows.size());
    for (std::size_t i = 0; i < budget; ++i) {
        const auto txid = rows[metadata_cursor_++ % rows.size()][0].get<std::string>();
        try {
            const auto entry = rpc_.call("getmempoolentry",Json::array({txid}));
            const auto data = fee_metadata(entry);
            const auto transaction = rpc_.call("getrawtransaction",Json::array({txid,true}));
            Transaction tx(s_); model.enrich(txid,data); model.pending(txid,transaction); tx.commit();
        } catch (const RPCError& e) {
            if (e.code != -5 && e.code != -8) throw;
            Transaction tx(s_); s_.event("observer.metadata-missing","node:metadata","concurrent-membership-change",{{"txid",txid}}); tx.commit();
        }
    }
}
void Observer::handle(const Sequence& notification) {
    const auto previous_session = s_.get("node_run");
    const bool gap = counter_ && static_cast<std::uint32_t>(*counter_ + std::uint32_t(1)) != notification.counter;
    counter_ = notification.counter;
    if (gap) {
        reconcile("zmq-counter-gap-or-publisher-restart");
        if (previous_session != s_.get("node_run")) return; // Do not apply a queued event from the old publisher run.
    }
    if (notification.label == 'C' || notification.label == 'D') { reconcile("chain-notification"); return; }
    if (!notification.mempool_sequence) throw std::runtime_error("Missing mempool sequence");
    const auto sequence = *notification.mempool_sequence;
    if (sequence < s_.get("mempool_boundary",0).get<std::uint64_t>()) return;
    Json source{{"kind","zmq"},{"topic","sequence"},{"notification_sequence",notification.counter},{"mempool_sequence",std::to_string(sequence)}};
    Transaction tx(s_);
    if (notification.label == 'A') model.admission(notification.hash,nullptr,"node:mempool","admission",source);
    else if (notification.label == 'R') model.removal(notification.hash,"unknown-removal","node:mempool",source);
    else throw std::runtime_error("Invalid sequence label");
    if (sequence == std::numeric_limits<std::uint64_t>::max()) throw RebuildRequired("Mempool sequence exhausted");
    s_.put("mempool_boundary",sequence+1); tx.commit();
}
void Observer::run(const std::function<bool()>& stopped) {
    identify();
    feed_ = std::make_unique<SequenceFeed>(options_.zmq);
    feed_->start();
    const auto started = Clock::now();
    auto next_peer = started, next_reconcile = started, next_metadata = started;
    std::cerr << "bitcoin-viz: " << options_.network << ", Core " << s_.get("core_version") << ", ZMQ " << options_.zmq << '\n';
    try {
        while (!stopped() && (options_.duration == 0 || std::chrono::duration<double>(Clock::now()-started).count() < options_.duration)) {
            const auto now = Clock::now();
            if (!feed_->error().empty()) throw std::runtime_error("ZMQ receiver failed: " + feed_->error());
            try {
                if (now >= next_reconcile) {
                    reconcile(s_.get("last_reconcile_ns").is_null() ? "startup" : "periodic");
                    next_reconcile = Clock::now() + std::chrono::milliseconds(static_cast<long>(options_.reconcile_interval*1000));
                    Transaction tx(s_); s_.prune(options_.journal_keep); tx.commit();
                }
                const auto dropped = feed_->dropped.load();
                if (dropped != last_drop_) {
                    { Transaction tx(s_); s_.event("observer.source-loss","node:observer","ingress-overflow-or-malformed",{{"dropped",dropped-last_drop_}}); tx.commit(); }
                    last_drop_ = dropped; reconcile("ingress-loss");
                }
                for (int i = 0; i < 128; ++i) { auto event = feed_->pop(); if (!event) break; handle(*event); }
                if (now >= next_peer) {
                    auto peers = rpc_.call("getpeerinfo");
                    { Transaction tx(s_); model.peers(peers,s_.get("node_run"),!capture_); s_.put("protocol_fidelity",capture_ ? "message-capture" : "aggregate-bytes"); tx.commit(); }
                    if (capture_) capture_->poll();
                    next_peer = Clock::now() + std::chrono::milliseconds(static_cast<long>(options_.peer_interval*1000));
                }
                if (now >= next_metadata) {
                    metadata(); next_metadata = Clock::now() + std::chrono::milliseconds(static_cast<long>(options_.peer_interval*1000));
                }
            } catch (const std::exception& e) {
                rpc_.close();
                { Transaction tx(s_); s_.put("synchronized",false); s_.event("observer.source-loss","node:observer","rpc-or-chain-gap",{{"error",e.what()}}); s_.prune(options_.journal_keep); tx.commit(); }
                std::cerr << "bitcoin-viz: recovery needed: " << e.what() << '\n';
                next_reconcile = Clock::time_point::min();
                wait_stop(stopped,std::min(2.0,options_.peer_interval));
            }
            wait_stop(stopped,0.05);
        }
    } catch (...) {
        feed_.reset(); rpc_.close();
        Transaction tx(s_); s_.put("synchronized",false); s_.event("observer.stopped","node:observer","collector-error"); tx.commit();
        throw;
    }
    feed_.reset(); rpc_.close();
    Transaction tx(s_); s_.put("synchronized",false); s_.event("observer.stopped","node:observer","collector-exit"); tx.commit();
}
} // namespace bitcoin_viz
