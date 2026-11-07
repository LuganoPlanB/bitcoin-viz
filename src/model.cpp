// SPDX-License-Identifier: GPL-3.0-or-later
#include "bitcoin_viz.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <sstream>

namespace bitcoin_viz {
namespace {
std::string replace_colon(std::string s) { std::replace(s.begin(), s.end(), ':', '-'); return s; }
std::string cohort(std::int64_t h) { const auto base = h / 100 * 100; return std::to_string(base) + "-" + std::to_string(base + 99); }
std::string decimal_rate(std::int64_t fee, std::int64_t size) {
    std::string result = std::to_string(fee / size);
    auto remainder = fee % size;
    std::size_t significant = result == "0" ? 0 : result.size();
    if (!remainder) return result;
    result += '.';
    while (remainder && significant < 28) {
        remainder *= 10;
        const auto digit = remainder / size;
        result += static_cast<char>('0' + digit);
        if (significant || digit) ++significant;
        remainder %= size;
    }
    if (remainder * 2 > size || (remainder * 2 == size && (result.back() - '0') % 2)) {
        auto i = result.size();
        while (i) {
            --i;
            if (result[i] == '.') continue;
            if (result[i] != '9') { ++result[i]; break; }
            result[i] = '0';
            if (i == 0) result.insert(result.begin(), '1');
        }
    }
    return result;
}
std::string traffic_category(const std::string& command) {
    if (std::set<std::string>{"tx","wtxidrelay","sendtxrcncl"}.count(command)) return "transactions";
    if (std::set<std::string>{"block","headers","getheaders","cmpctblock","getblocktxn","blocktxn","sendcmpct","sendheaders"}.count(command)) return "blocks";
    if (std::set<std::string>{"addr","addrv2","getaddr","sendaddrv2"}.count(command)) return "discovery";
    if (std::set<std::string>{"version","verack","ping","pong","feefilter","sendtxpackages"}.count(command)) return "control";
    return "unclassified";
}
} // namespace

std::int64_t satoshis(const Json& amount) {
    // Use decimal lexemes, never binary floating point, for monetary conversion.
    auto raw = text(amount);
    if (raw.empty() || raw.size() > 128) throw std::runtime_error("Invalid monetary amount");
    std::size_t pos = 0;
    bool negative = false;
    if (raw[pos] == '-' || raw[pos] == '+') { negative = raw[pos] == '-'; ++pos; }
    std::string digits;
    int fraction = 0;
    bool dot = false;
    while (pos < raw.size() && raw[pos] != 'e' && raw[pos] != 'E') {
        const char c = raw[pos++];
        if (c == '.' && !dot) { dot = true; continue; }
        if (c < '0' || c > '9') throw std::runtime_error("Invalid monetary amount");
        digits += c;
        if (dot) ++fraction;
    }
    if (digits.empty()) throw std::runtime_error("Invalid monetary amount");
    int exponent = 0;
    if (pos < raw.size()) {
        std::size_t consumed = 0;
        const auto exp = raw.substr(pos + 1);
        exponent = std::stoi(exp, &consumed);
        if (consumed != exp.size() || exponent < -100 || exponent > 100) throw std::runtime_error("Invalid monetary exponent");
    }
    int scale = 8 + exponent - fraction;
    while (scale < 0 && !digits.empty() && digits.back() == '0') { digits.pop_back(); ++scale; }
    if (digits.empty()) return 0;
    if (scale < 0) throw std::runtime_error("Non-integral satoshi amount");
    const auto first = digits.find_first_not_of('0');
    if (first == std::string::npos) return 0;
    digits.erase(0, first);
    if (digits.size() + static_cast<std::size_t>(scale) > 19) throw std::runtime_error("Monetary amount overflow");
    digits.append(static_cast<std::size_t>(scale), '0');
    std::int64_t result = 0;
    for (char c : digits) {
        const auto digit = c - '0';
        if (result > (std::numeric_limits<std::int64_t>::max() - digit) / 10) throw std::runtime_error("Monetary amount overflow");
        result = result * 10 + digit;
    }
    return negative ? -result : result;
}
Json fee_metadata(const Json& entry) {
    Json fee = entry.value("fees", Json::object()).value("base", Json(nullptr));
    Json size = entry.value("vsize_bip141", Json(nullptr));
    std::string source = "vsize_bip141";
    if (size.is_null() && entry.contains("weight") && !entry["weight"].is_null()) {
        auto weight = entry["weight"].get<std::int64_t>();
        if (weight < 0 || weight > 16000000) throw std::runtime_error("Invalid transaction weight");
        size = (weight + 3) / 4; source = "ceil(weight/4)";
    }
    auto fee_sat = fee.is_null() ? Json(nullptr) : Json(satoshis(fee));
    if (!fee_sat.is_null() && fee_sat.get<std::int64_t>() < 0) throw std::runtime_error("Negative base fee");
    std::string band = "fee-unknown";
    Json rate = nullptr;
    if (!size.is_null() && (size.get<std::int64_t>() <= 0 || size.get<std::int64_t>() > 4000000)) size = nullptr;
    if (!fee_sat.is_null() && !size.is_null()) {
        const auto n = fee_sat.get<std::int64_t>(), d = size.get<std::int64_t>();
        band = n < d ? "fee-below-1" : n < d * 5 ? "fee-1-to-5" : n < d * 20 ? "fee-5-to-20" : "fee-20-plus";
        rate = decimal_rate(n, d);
    }
    return {{"fee_sat",fee_sat},{"vsize_bip141",size},{"size_source",size.is_null() ? "unknown" : source},
            {"sat_per_vb",rate},{"band",band},{"depends",entry.value("depends",Json::array())},
            {"spentby",entry.value("spentby",Json::array())},{"wtxid",entry.value("wtxid",Json(nullptr))}};
}
std::string mempool_path(const std::string& id, const Json& data) { return "mempool/" + data.value("band",std::string("fee-unknown")) + "/" + id + ".tx"; }
std::string coin_path(const Json& coin) {
    const auto height = coin.at("height").get<std::int64_t>();
    return "utxo/" + cohort(height) + "/" + std::to_string(height) + "-" + coin.at("block").get<std::string>() + "/" + replace_colon(coin.at("id")) + ".coin";
}
std::string block_path(std::int64_t height, const std::string& hash, const std::string& branch) {
    return "chain/" + branch + "/" + cohort(height) + "/" + std::to_string(height) + "-" + hash + ".block";
}
Model::Model(Store& store, std::int64_t window_blocks, std::int64_t horizon, std::int64_t recent_blocks)
    : s_(store), window(window_blocks), undo_horizon(horizon), recent(recent_blocks) {}
void Model::admission(const std::string& txid, Json data, const std::string& actor, const std::string& cause, const Json& source) {
    if (!s_.query("SELECT data FROM mempool WHERE txid=?",{txid}).empty()) { if (!data.is_null()) enrich(txid,data); return; }
    if (data.is_null() || data.empty()) data = {{"band","fee-unknown"}};
    s_.exec("INSERT INTO mempool VALUES(?,?)",{txid,data.dump()});
    auto e = s_.event("mempool.add",actor,cause,{{"txid",txid},{"metadata",data},{"source",source}});
    s_.leaf("state",mempool_path(txid,data),actor,"E9B949",data,e);
}
void Model::enrich(const std::string& txid, const Json& data) {
    auto rows = s_.query("SELECT data FROM mempool WHERE txid=?",{txid});
    if (rows.empty()) return;
    auto old = Json::parse(rows[0][0].get<std::string>());
    if (old == data) return;
    auto e = s_.event("mempool.metadata","node:metadata",nullptr,{{"txid",txid},{"metadata",data}});
    if (mempool_path(txid,old) != mempool_path(txid,data)) s_.leaf("state",mempool_path(txid,old),"node:metadata","E9B949",old,e,true);
    s_.leaf("state",mempool_path(txid,data),"node:metadata","E9B949",data,e);
    s_.exec("UPDATE mempool SET data=? WHERE txid=?",{data.dump(),txid});
}
void Model::removal(const std::string& txid, const std::string& cause, const std::string& actor, const Json& source) {
    auto rows = s_.query("SELECT data FROM mempool WHERE txid=?",{txid});
    if (rows.empty()) return;
    auto data = Json::parse(rows[0][0].get<std::string>());
    auto e = s_.event("mempool.remove",actor,cause,{{"txid",txid},{"source",source}});
    s_.leaf("state",mempool_path(txid,data),actor,"E9B949",data,e,true);
    for (const auto& row : s_.query("SELECT path FROM leaves WHERE mode='utxo' AND path LIKE ?",{"pending/" + txid + "/%"}))
        s_.leaf("utxo",row[0],actor,"DDA0DD",Json::object(),e,true);
    s_.exec("DELETE FROM mempool WHERE txid=?",{txid});
}
void Model::pending(const std::string& txid, const Json& transaction) {
    if (s_.query("SELECT 1 FROM mempool WHERE txid=?",{txid}).empty()) return;
    auto e = s_.event("utxo.pending","node:mempool","pending-overlay",{{"txid",txid}});
    for (const auto& vin : transaction.value("vin",Json::array())) {
        if (!vin.contains("txid")) continue;
        const auto id = vin.at("txid").get<std::string>() + ":" + text(vin.at("vout"));
        if (!s_.query("SELECT 1 FROM coins WHERE id=?",{id}).empty())
            s_.leaf("utxo","pending/" + txid + "/" + replace_colon(id) + ".pending","node:mempool","DDA0DD",{{"outpoint",id}},e);
    }
}
void Model::reconcile_mempool(const Json& txids, std::uint64_t boundary) {
    std::set<std::string> current, target;
    for (const auto& row : s_.query("SELECT txid FROM mempool")) current.insert(row[0]);
    for (const auto& id : txids) target.insert(id.get<std::string>());
    for (const auto& id : current) if (!target.count(id)) removal(id,"snapshot-difference","node:snapshot");
    for (const auto& id : target) if (!current.count(id)) admission(id,nullptr,"node:snapshot","snapshot-population");
    s_.put("mempool_boundary",boundary);
}
void Model::checkpoint(const std::string& hash, std::int64_t height, const std::string& cause) {
    auto e = s_.event("observer.rebuild","node:snapshot",cause,{{"checkpoint",hash},{"height",height},
        {"coverage",window ? "rolling-observed-outputs" : "fixed-observed-outputs"}});
    for (const std::string mode : {"state","utxo"}) {
        for (const auto& row : s_.query("SELECT path FROM leaves WHERE mode=?",{mode})) {
            const auto path = row[0].get<std::string>();
            if (mode == "state" && path.rfind("chain/",0) != 0) continue;
            s_.leaf(mode,path,"node:snapshot","999999",Json::object(),e,true);
        }
    }
    s_.exec("DELETE FROM coins"); s_.exec("DELETE FROM blocks");
    s_.put("tip",{{"hash",hash},{"height",height}});
    s_.put("checkpoint",{{"hash",hash},{"height",height}});
    s_.put("coverage_height",height+1);
}
Json Model::coin(const std::string& outpoint) {
    const auto rows = s_.query("SELECT data FROM coins WHERE id=?",{outpoint});
    return rows.empty() ? Json(nullptr) : Json::parse(rows[0][0].get<std::string>());
}
void Model::set_coin(const std::string& outpoint, const Json& value, const std::string& actor, const std::string& cause) {
    auto old = coin(outpoint);
    auto e = s_.event("utxo." + cause,actor,cause,{{"outpoint",outpoint},{"previous",old},{"coin",value}});
    if (!old.is_null()) s_.leaf("utxo",coin_path(old),actor,"68B89B",old,e,true);
    if (value.is_null()) s_.exec("DELETE FROM coins WHERE id=?",{outpoint});
    else {
        s_.exec("INSERT OR REPLACE INTO coins VALUES(?,?,?)",{outpoint,value.at("height"),value.dump()});
        s_.leaf("utxo",coin_path(value),actor,"68B89B",value,e);
    }
}
void Model::connect(const Json& block) {
    const auto tip = s_.get("tip");
    const auto hash = block.at("hash").get<std::string>();
    if (!tip.is_null() && tip.at("hash") == hash) return;
    const auto height = block.at("height").get<std::int64_t>();
    if (tip.is_null() || tip.at("hash") != block.value("previousblockhash",std::string()) ||
        height != tip.at("height").get<std::int64_t>() + 1) throw RebuildRequired("chain-parent-mismatch");
    Json undo = Json::array();
    std::int64_t untracked = 0;
    for (const auto& tx : block.at("tx")) {
        const auto txid = tx.at("txid").get<std::string>();
        const auto actor = "tx:" + txid;
        removal(txid,"confirmation","node:chain");
        for (const auto& vin : tx.value("vin",Json::array())) {
            if (!vin.contains("txid")) continue;
            const auto id = vin.at("txid").get<std::string>() + ":" + text(vin.at("vout"));
            auto old = coin(id);
            if (old.is_null()) { ++untracked; continue; }
            undo.push_back(Json::array({id,old})); set_coin(id,nullptr,actor,"spend");
        }
        bool coinbase = false;
        for (const auto& vin : tx.value("vin",Json::array())) if (vin.contains("coinbase")) coinbase = true;
        for (const auto& vout : tx.value("vout",Json::array())) {
            auto script = vout.value("scriptPubKey",Json::object()).value("hex",std::string());
            if (height == 0 || script.rfind("6a",0) == 0 || script.size() > 20000) continue;
            const auto id = txid + ":" + text(vout.at("n"));
            Json value{{"id",id},{"height",height},{"block",hash},{"value_sat",satoshis(vout.at("value"))},{"script",script},{"coinbase",coinbase}};
            undo.push_back(Json::array({id,coin(id)})); set_coin(id,value,actor,"create");
        }
    }
    const auto previous = s_.get("coverage_height",height).get<std::int64_t>();
    const auto coverage = window ? std::max(previous,height-window+1) : previous;
    for (const auto& row : s_.query("SELECT id,data FROM coins WHERE height<?",{coverage})) {
        undo.push_back(Json::array({row[0],Json::parse(row[1].get<std::string>())}));
        set_coin(row[0],nullptr,"view:retention","coverage-expiry");
    }
    s_.exec("INSERT INTO blocks VALUES(?,?,?,?,?)",{hash,height,tip.at("hash"),undo.dump(),previous});
    auto e = s_.event("chain.connect","node:chain","connection",{{"blockhash",hash},{"height",height},
        {"header_time",block.value("time",Json(nullptr))},{"outside_window_inputs",untracked},
        {"transactions",block.at("tx").size()},{"size_bytes",block.value("size",Json(nullptr))},
        {"weight",block.value("weight",Json(nullptr))}});
    s_.leaf("state",block_path(height,hash),"node:chain","7EA6E0",{{"height",height}},e);
    s_.leaf("state",block_path(height,hash,"disconnected"),"node:chain","DE7777",Json::object(),e,true);
    trim_chain(height);
    s_.exec("DELETE FROM blocks WHERE height<=?",{height-undo_horizon});
    s_.put("tip",{{"hash",hash},{"height",height}}); s_.put("coverage_height",coverage);
}
void Model::trim_chain(std::int64_t height) {
    for (const auto& row : s_.query("SELECT path,data FROM leaves WHERE mode='state' AND path LIKE 'chain/%'")) {
        if (Json::parse(row[1].get<std::string>()).value("height",height) <= height-recent) {
            const auto e = s_.event("view.expiry","view:retention","display-expiry",{{"path",row[0]}});
            s_.leaf("state",row[0],"view:retention","999999",Json::object(),e,true);
        }
    }
}
void Model::disconnect(const std::string& hash) {
    const auto tip = s_.get("tip");
    if (tip.is_null() || tip.at("hash") != hash) throw RebuildRequired("disconnect-tip-mismatch");
    auto rows = s_.query("SELECT height,parent,undo,coverage FROM blocks WHERE hash=?",{hash});
    if (rows.empty()) throw RebuildRequired("undo-horizon-or-checkpoint-crossed");
    const auto& row = rows[0];
    const auto height = row[0].get<std::int64_t>();
    const auto actor = "reorg:" + hash;
    auto undo = Json::parse(row[2].get<std::string>());
    for (auto i = undo.rbegin(); i != undo.rend(); ++i) set_coin((*i)[0],(*i)[1],actor,(*i)[1].is_null() ? "undo-creation" : "undo-restore");
    const auto e = s_.event("chain.disconnect",actor,"reorganization",{{"blockhash",hash},{"height",height}});
    s_.leaf("state",block_path(height,hash),actor,"7EA6E0",Json::object(),e,true);
    s_.leaf("state",block_path(height,hash,"disconnected"),actor,"DE7777",{{"height",height}},e);
    s_.exec("DELETE FROM blocks WHERE hash=?",{hash});
    s_.put("tip",{{"hash",row[1]},{"height",height-1}}); s_.put("coverage_height",row[3]);
}
void Model::peers(const Json& peers, const std::string& session, bool aggregate) {
    auto rows = s_.query("SELECT id,data FROM peers");
    std::map<std::string,Json> current;
    for (const auto& row : rows) current.emplace(row[0].get<std::string>(),Json::parse(row[1].get<std::string>()));
    std::set<std::string> seen;
    for (const auto& peer : peers) {
        const auto key = session + "-peer" + text(peer.at("id")) + "-" + text(peer.value("conntime",Json(0)));
        seen.insert(key);
        const auto role = peer.value("inbound",false) ? std::string("inbound") : component(peer.value("connection_type",std::string("outbound")));
        const auto path = "network/" + role + "/" + component(peer.value("network",std::string("unknown"))) + "/" + key + ".peer";
        Json data{{"path",path},{"recv",peer.value("bytesrecv_per_msg",Json::object())},{"sent",peer.value("bytessent_per_msg",Json::object())}};
        auto it = current.find(key);
        Json old = it == current.end() ? Json(nullptr) : it->second;
        if (old.is_null() || old.at("path") != path) {
            auto e = s_.event(old.is_null() ? "peer.snapshot" : "peer.metadata","node:network","poll",{{"peer_session",key}});
            for (const std::string mode : {"state","protocol"}) {
                if (!old.is_null()) s_.leaf(mode,old.at("path"),"node:network","B9A2DD",Json::object(),e,true);
                s_.leaf(mode,path,"node:network","B9A2DD",{{"peer_session",key}},e);
            }
        }
        if (!old.is_null() && aggregate) {
            for (const auto& field : {"recv","sent"}) {
                for (auto item = data[field].begin(); item != data[field].end(); ++item) {
                    const auto delta = item.value().get<std::int64_t>() - old[field].value(item.key(),std::int64_t(0));
                    if (delta < 0) s_.event("observer.source-loss","node:network","counter-reset",{{"peer_session",key},{"command",item.key()}});
                    else if (delta) message(key,std::string(field) == "recv" ? "incoming" : "outgoing",item.key(),delta,"aggregate-bytes");
                }
            }
        }
        s_.exec("INSERT OR REPLACE INTO peers VALUES(?,?)",{key,data.dump()});
    }
    for (const auto& row : rows) {
        const auto key = row[0].get<std::string>();
        if (seen.count(key)) continue;
        auto e = s_.event("peer.absent","node:network","poll-difference",{{"peer_session",key}});
        for (const std::string mode : {"state","protocol"}) s_.leaf(mode,current.at(key).at("path"),"node:network","B9A2DD",Json::object(),e,true);
        s_.exec("DELETE FROM peers WHERE id=?",{key});
    }
}
void Model::message(const std::string& peer, const std::string& direction, const std::string& command,
                    std::int64_t size, const std::string& fidelity, const Json& observed_ns, const Json& objects) {
    if ((direction != "incoming" && direction != "outgoing") || size < 0) throw std::runtime_error("Invalid message direction or size");
    const auto actor = "peer:" + component(peer);
    auto category = traffic_category(command);
    if (!objects.is_null() && !objects.empty() && (command == "inv" || command == "getdata" || command == "notfound")) {
        bool transactions = true, blocks = true;
        for (const auto& obj : objects) {
            const auto kind = obj.value("kind",std::string());
            transactions = transactions && (kind == "txid" || kind == "wtxid");
            blocks = blocks && (kind == "blockhash" || kind == "filtered-blockhash" || kind == "compact-blockhash");
        }
        category = transactions ? "transactions" : blocks ? "blocks" : "mixed-inventory";
    }
    const auto e = s_.event("peer.message." + direction,actor,fidelity,{{"command",command},{"size_bytes",size},
        {"source_observed_ns",observed_ns},{"objects",objects},{"fidelity",fidelity}});
    const auto path = "traffic/" + category + "/" + component(command) + "/" + direction + ".activity";
    s_.leaf("protocol",path,actor,direction == "incoming" ? "6FC5D2" : "E6AB72",Json::object(),e,false,true);
    if (!objects.is_null()) for (const auto& obj : objects) {
        if (obj.value("kind",std::string()) != "txid") continue;
        const auto rows = s_.query("SELECT data FROM mempool WHERE txid=?",{obj.at("hash")});
        if (!rows.empty()) {
            auto data = Json::parse(rows[0][0].get<std::string>());
            s_.leaf("state",mempool_path(obj.at("hash"),data),actor,"E9B949",data,e,false,true);
        }
    }
}
} // namespace bitcoin_viz
