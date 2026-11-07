// SPDX-License-Identifier: GPL-3.0-or-later
#include "bitcoin_viz.hpp"
#include <algorithm>
#include <iostream>
#include <limits>

namespace bitcoin_viz {
namespace {
struct Record {
    std::string view, path, code, color, paddle;
    std::int64_t bytes = 0;
    bool success = true;
};
// The custom format has no quoting mechanism. Escape delimiters and controls in
// tooltip fields; model identities/path components already use component().
std::string field(const std::string& value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string clean;
    for (unsigned char c : value) {
        if (c == '|' || c == '%' || c < 32 || c == 127) {
            clean += '%'; clean += hex[c >> 4]; clean += hex[c & 15];
        } else clean += static_cast<char>(c);
    }
    return clean;
}
std::string script_type(const Json& data) {
    const auto coin = data.value("coin",Json(nullptr)).is_null()
        ? data.value("previous",Json::object()) : data.at("coin");
    if (!coin.is_object()) return "unknown";
    const auto script = coin.value("script",std::string());
    if (script.size()==44 && script.rfind("0014",0)==0) return "p2wpkh";
    if (script.size()==68 && script.rfind("0020",0)==0) return "p2wsh";
    if (script.size()==68 && script.rfind("5120",0)==0) return "p2tr";
    if (script.size()==50 && script.rfind("76a914",0)==0 && script.substr(46)=="88ac") return "p2pkh";
    if (script.size()==46 && script.rfind("a914",0)==0 && script.substr(44)=="87") return "p2sh";
    return "other";
}
std::optional<Record> map_event(const std::string& kind, const Json& cause, const Json& data) {
    Record r;
    const auto reason = cause.is_string() ? cause.get<std::string>() : std::string("unknown");
    if (kind=="peer.message.incoming" || kind=="peer.message.outgoing") {
        const bool incoming = kind=="peer.message.incoming";
        const auto fidelity = data.value("fidelity",reason);
        const auto direction = incoming ? std::string("incoming") : std::string("outgoing");
        r = {"traffic","/traffic/"+component(fidelity)+"/"+direction+"/"+component(data.value("command",std::string("unknown"))),
             incoming ? "RX" : "TX",incoming ? "6FC5D2" : "E6AB72",direction};
        r.bytes = data.value("size_bytes",std::int64_t(0));
    } else if (kind=="mempool.add") {
        // Snapshot population is inventory, not newly observed admissions.
        if (reason=="snapshot-population") return std::nullopt;
        const auto metadata = data.value("metadata",Json::object());
        r = {"mempool","/mempool/admitted/"+component(metadata.value("band",std::string("fee-unknown"))),
             "ADMIT","E9B949","mempool"};
    } else if (kind=="mempool.remove") {
        // A snapshot difference proves absence, without its time or reason.
        if (reason=="snapshot-difference") return std::nullopt;
        r = {"mempool","/mempool/removed/"+component(reason),
             reason=="confirmation" ? "CONFIRM" : "REMOVE",reason=="confirmation" ? "68B89B" : "AAAAAA","mempool"};
    } else if (kind=="mempool.metadata") {
        const auto metadata = data.value("metadata",Json::object());
        r = {"mempool","/mempool/classified/"+component(metadata.value("band",std::string("fee-unknown"))),
             "METADATA","B9A2DD","mempool"};
    } else if (kind=="chain.connect" || kind=="chain.disconnect") {
        const bool connected = kind=="chain.connect";
        r = {"blocks",std::string("/chain/")+(connected ? "connected/" : "disconnected/")+component(text(data.value("height",Json("unknown")))),
             connected ? "BLOCK" : "REORG",connected ? "7EA6E0" : "DE7777","chain"};
        r.bytes = data.value("size_bytes",Json(nullptr)).is_null() ? 0 : data.at("size_bytes").get<std::int64_t>();
        r.success = connected;
    } else if (kind=="utxo.create" || kind=="utxo.spend" || kind=="utxo.undo-creation" || kind=="utxo.undo-restore" || kind=="utxo.coverage-expiry") {
        const auto action = kind.substr(5);
        const bool undo = action.rfind("undo-",0)==0;
        r = {"utxo","/utxo/"+action+"/"+script_type(data),
             undo ? "UNDO" : action=="coverage-expiry" ? "EXPIRE" : action=="create" ? "CREATE" : "SPEND",
             undo ? "DE7777" : action=="coverage-expiry" ? "999999" : "68B89B","utxo"};
        r.success = !undo;
    } else if (kind=="observer.source-loss" || kind=="observer.resync" || kind=="observer.rebuild") {
        const bool gap = kind=="observer.source-loss";
        r = {"health","/observer/"+kind.substr(9)+"/"+component(reason),
             gap ? "GAP" : kind=="observer.resync" ? "RESYNC" : "REBUILD",gap ? "DE7777" : "B9A2DD","observer"};
        r.success = !gap;
    } else return std::nullopt;
    if (r.bytes<0) throw std::runtime_error("Negative Logstalgia byte size in journal");
    return r;
}
} // namespace

LogstalgiaRenderer::LogstalgiaRenderer(const std::filesystem::path& database, std::string mode,
    std::function<void(const std::string&)> output, std::optional<std::int64_t> after)
    : s_(database,true), mode_(std::move(mode)), output_(std::move(output)) {
    if (!std::set<std::string>{"traffic","mempool","blocks","utxo","all"}.count(mode_))
        throw std::runtime_error("Invalid Logstalgia mode");
    if (s_.get("schema")!=1) throw std::runtime_error("Unsupported observer database schema");
    if (after && *after<0) throw std::runtime_error("Negative journal cursor");
    cursor_ = after ? *after : s_.query("SELECT coalesce(max(id),0) FROM events")[0][0].get<std::int64_t>();
}
void LogstalgiaRenderer::emit(std::int64_t stamp, const std::string& kind, const std::string& actor,
    const Json& cause, const Json& data, std::int64_t id) {
    const auto r = map_event(kind,cause,data);
    if (!r || (mode_!="all" && mode_!=r->view && r->view!="health")) return;
    last_time_ = std::max(last_time_,std::max<std::int64_t>(1,stamp));
    std::string detail = "event="+std::to_string(id)+";kind="+kind+";cause="+text(cause);
    for (const auto* key : {"txid","blockhash","outpoint","transactions","fidelity","source_observed_ns"})
        if (data.contains(key) && !data[key].is_null()) detail += ";"+std::string(key)+"="+text(data[key]);
    // Eleven documented fields. Non-traffic lifecycle events with no measured
    // serialized size use zero bytes, never satoshis or vbytes masquerading as bytes.
    output_(std::to_string(last_time_)+"|"+component(actor)+"|"+r->path+"|"+r->code+"|"+
        std::to_string(r->bytes)+"|"+(r->success ? "1" : "0")+"|"+r->color+"||"+
        field(detail)+"|bitcoin-viz|"+r->paddle+"\n");
}
std::size_t LogstalgiaRenderer::step() {
    Rows rows;
    std::int64_t floor = 0;
    bool gap = false;
    {
        Transaction tx(s_,false);
        floor = s_.get("journal_floor",0);
        gap = cursor_<floor;
        const auto start = std::max(cursor_,floor);
        rows = s_.query("SELECT id,observed_ns,kind,actor,cause,data FROM events WHERE id>? AND id<=? ORDER BY id LIMIT 1000",
            {start,end_.value_or(std::numeric_limits<std::int64_t>::max())});
        tx.commit();
    } // Release the read snapshot before writing to a potentially blocked pipe.
    if (gap) {
        std::cerr << "bitcoin-viz: Logstalgia journal gap after " << cursor_ << "; retained floor=" << floor << '\n';
        const auto stamp = rows.empty() ? now_ns()/1000000000 : rows[0][1].get<std::int64_t>()/1000000000;
        emit(stamp,"observer.source-loss","view:logstalgia","journal-retention-gap",Json::object(),floor);
        cursor_ = floor;
    }
    for (const auto& row : rows) {
        emit(row[1].get<std::int64_t>()/1000000000,row[2],row[3],row[4],Json::parse(row[5].get<std::string>()),row[0]);
        cursor_ = row[0];
    }
    return rows.size();
}
void LogstalgiaRenderer::run(const std::function<bool()>& stopped, bool follow) {
    if (!follow) end_ = s_.query("SELECT coalesce(max(id),0) FROM events")[0][0].get<std::int64_t>();
    while (!stopped()) {
        if (!step()) {
            if (!follow) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}
} // namespace bitcoin_viz
