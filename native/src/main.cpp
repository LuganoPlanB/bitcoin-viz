// SPDX-License-Identifier: GPL-3.0-or-later
#include "wizard.hpp"
#include "platform.hpp"
#include "wizard_version.hpp"
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

namespace {
std::atomic<bool> interrupted{false};
static_assert(std::atomic<bool>::is_always_lock_free,"Signal stop flag must be lock free");
void signal_stop(int) { interrupted.store(true,std::memory_order_relaxed); }
using wizard::platform::Output;
using wizard::platform::OutputClosed;
std::int64_t integer(const std::string& value, bool positive=false) {
    if (value.empty() || value[0]=='-') throw std::runtime_error("Expected nonnegative integer");
    std::size_t used=0;
    auto n=std::stoll(value,&used);
    if (used!=value.size() || n<0 || (positive && !n)) throw std::runtime_error("Invalid integer option");
    return n;
}
double duration(const std::string& value) {
    std::size_t used=0;
    const auto n=std::stod(value,&used);
    if (used!=value.size() || !std::isfinite(n) || n<=0 || n>86400*365) throw std::runtime_error("Expected a finite positive interval");
    return n;
}
void help() {
    std::cout << "Bitcoin Wizard Cave native " BWC_VERSION " (C++17)\n"
      "Usage: bitcoin-wizard-cave-native <observe|live|render|status|events|demo|replay> [options]\n"
      "  --db PATH                     SQLite database (default .runtime/wizard.sqlite)\n"
      "  --mode state|protocol|utxo     Gource projection (default state)\n"
      "  --max-visible N               Cap renderer only (default 5000)\n"
      "  --network signet|regtest       Expected Core chain (default signet)\n"
      "  --datadir PATH --cookie PATH --rpc-url http://127.0.0.1:38332 --zmq unix:/PATH\n"
      "  --window-blocks N --undo-horizon N --bootstrap-blocks N --metadata-budget N\n"
      "  --peer-interval SECONDS --reconcile-interval SECONDS --duration SECONDS\n"
      "  --journal-keep N --capture-dir PATH\n"
      "  render: --once; events: --after ID --limit N; replay: --input FILE.jsonl\n"
      "Signet, local cookie RPC and the sequence publisher are the defaults.\n";
}
wizard::Options options(int argc, char** argv) {
    using namespace wizard;
    Options o;
    o.datadir=platform::default_datadir();
    if (argc<2) throw std::runtime_error("Missing command; use --help");
    o.command=argv[1];
    const std::set<std::string> commands{"observe","live","render","status","events","demo","replay"};
    if (!commands.count(o.command)) throw std::runtime_error("Unknown command: "+o.command);
    const bool collecting=o.command=="observe" || o.command=="live";
    const bool rendering=o.command=="live" || o.command=="render" || o.command=="demo";
    for (int i=2;i<argc;++i) {
        const std::string key=argv[i];
        if (key=="--help" || key=="-h") { help(); std::exit(0); }
        if (key=="--once" && o.command=="render") { o.once=true; continue; }
        if (i+1>=argc) throw std::runtime_error("Missing value for "+key);
        const std::string v=argv[++i];
        if (key=="--db") o.database=v;
        else if (rendering && key=="--mode") o.mode=v;
        else if (rendering && key=="--max-visible") o.max_visible=static_cast<std::size_t>(integer(v,true));
        else if (collecting && key=="--network") o.network=v;
        else if (collecting && key=="--datadir") o.datadir=v;
        else if (collecting && key=="--cookie") o.cookie=v;
        else if (collecting && key=="--rpc-url") o.rpc_url=v;
        else if (collecting && key=="--zmq") o.zmq=v;
        else if (collecting && key=="--window-blocks") o.window=integer(v);
        else if (collecting && key=="--undo-horizon") o.horizon=integer(v,true);
        else if (collecting && key=="--bootstrap-blocks") o.bootstrap=integer(v);
        else if (collecting && key=="--metadata-budget") o.metadata_budget=integer(v);
        else if (collecting && key=="--journal-keep") o.journal_keep=integer(v,true);
        else if (collecting && key=="--peer-interval") o.peer_interval=duration(v);
        else if (collecting && key=="--reconcile-interval") o.reconcile_interval=duration(v);
        else if (collecting && key=="--duration") o.duration=duration(v);
        else if (collecting && key=="--capture-dir") o.capture_dir=v;
        else if (o.command=="events" && key=="--after") o.after=integer(v);
        else if (o.command=="events" && key=="--limit") o.limit=integer(v,true);
        else if (o.command=="replay" && key=="--input") o.input=v;
        else throw std::runtime_error("Unknown option for "+o.command+": "+key);
    }
    if (o.mode!="state" && o.mode!="protocol" && o.mode!="utxo") throw std::runtime_error("Invalid --mode");
    if (o.network!="signet" && o.network!="regtest") throw std::runtime_error("--network must be signet or regtest");
    if (o.cookie.empty()) o.cookie=o.datadir/o.network/".cookie";
    if (o.rpc_url.empty()) o.rpc_url=std::string("http://127.0.0.1:")+(o.network=="signet" ? "38332" : "18443");
    if (o.command=="replay" && o.input.empty()) throw std::runtime_error("replay requires --input FILE.jsonl");
    return o;
}
}

namespace wizard {
namespace {
std::string hash_id(unsigned int n) { std::ostringstream out; out << std::hex; out.width(64); out.fill('0'); out << n; return out.str(); }
}
void demo(Store& s) {
    Model m(s);
    Transaction tx(s);
    s.put("node_run","SYNTHETIC-DEMO"); s.put("network","synthetic"); m.checkpoint(hash_id(0),100,"synthetic-demo");
    m.admission(hash_id(10),fee_metadata({{"fees",{{"base","0.000003"}}},{"weight",480}}));
    m.admission(hash_id(11),fee_metadata({{"fees",{{"base","0.000024"}}},{"weight",720}}));
    m.peers(Json::array({{{"id",7},{"conntime",1},{"network","ipv4"},{"connection_type","outbound-full-relay"}}}),"demo");
    for (const std::string command : {"version","verack","ping","addr","headers","cmpctblock","tx","inv"}) m.message("demo-peer7","incoming",command,300);
    Json outputs=Json::array();
    for (int i=0;i<8;++i) outputs.push_back({{"n",i},{"value","0.001"},{"scriptPubKey",{{"hex","51"}}}});
    m.connect({{"hash",hash_id(1)},{"height",101},{"previousblockhash",hash_id(0)},
        {"tx",Json::array({{{"txid",hash_id(10)},{"vin",Json::array({{{"txid",hash_id(8)},{"vout",0}}})},{"vout",outputs}}})}});
    tx.commit();
}
void replay(Store& s, const std::filesystem::path& input) {
    std::ifstream stream(input);
    if (!stream) throw std::runtime_error("Cannot read replay input");
    { Transaction tx(s); s.put("node_run","SYNTHETIC-REPLAY"); s.put("network","synthetic"); tx.commit(); }
    Model m(s);
    std::string line;
    std::size_t number=0;
    while (std::getline(stream,line)) {
        ++number;
        if (line.empty()) continue;
        try {
            const auto op=parse_exact(line);
            const auto kind=op.at("op").get<std::string>();
            Transaction tx(s);
            if (kind=="checkpoint") {
                m.window=op.value("window",std::int64_t(144)); m.undo_horizon=op.value("horizon",std::int64_t(288));
                m.checkpoint(op.at("hash"),op.at("height"),op.value("cause",std::string("controlled-replay")));
            } else if (kind=="admission") m.admission(op.at("txid"),op.value("data",Json(nullptr)),op.value("actor",std::string("node:mempool")),op.value("cause",std::string("admission")),op.value("source",Json(nullptr)));
            else if (kind=="fee") m.enrich(op.at("txid"),fee_metadata(op.at("entry")));
            else if (kind=="enrich") m.enrich(op.at("txid"),op.at("data"));
            else if (kind=="removal") m.removal(op.at("txid"),op.value("cause",std::string("unknown-removal")),op.value("actor",std::string("node:mempool")),op.value("source",Json(nullptr)));
            else if (kind=="pending") m.pending(op.at("txid"),op.at("transaction"));
            else if (kind=="snapshot") m.reconcile_mempool(op.at("txids"),op.at("boundary"));
            else if (kind=="connect") m.connect(op.at("block"));
            else if (kind=="disconnect") m.disconnect(op.at("hash"));
            else if (kind=="peers") m.peers(op.at("peers"),op.value("session",std::string("replay")),op.value("aggregate",true));
            else if (kind=="message") m.message(op.at("peer"),op.at("direction"),op.at("command"),op.at("size"),op.value("fidelity",std::string("message-capture")),op.value("observed_ns",Json(nullptr)),op.value("objects",Json(nullptr)));
            else if (kind=="loss") s.event("observer.source-loss","node:observer",op.value("cause",std::string("controlled-loss")));
            else if (kind=="prune") s.prune(op.at("keep"));
            else throw std::runtime_error("Unknown replay operation");
            tx.commit();
        } catch (const std::exception& e) { throw std::runtime_error("Replay line "+std::to_string(number)+": "+e.what()); }
    }
    if (stream.bad()) throw std::runtime_error("Replay input read failed");
}
} // namespace wizard

int main(int argc, char** argv) {
    using namespace wizard;
    platform::initialize_process();
#ifndef _WIN32
    std::signal(SIGPIPE,SIG_IGN);
#endif
    std::signal(SIGINT,signal_stop); std::signal(SIGTERM,signal_stop);
    try {
        if (argc==2 && (std::string(argv[1])=="--help" || std::string(argv[1])=="-h")) { help(); return 0; }
        if (argc==2 && std::string(argv[1])=="--version") { std::cout << BWC_VERSION " (native C++17)\n"; return 0; }
        const auto o=options(argc,argv);
        std::atomic<bool> stop{false};
        const auto stopped=[&] { return stop.load() || interrupted.load(std::memory_order_relaxed); };
        if (o.command=="status") { Store s(o.database,true); std::cout << status(s).dump(2) << '\n'; return 0; }
        if (o.command=="events") {
            Store s(o.database,true);
            for (const auto& row : s.query("SELECT id,observed_ns,node_run,kind,actor,cause,data FROM events WHERE id>? ORDER BY id LIMIT ?",{o.after,o.limit})) {
                Json event{{"schema",1},{"id",row[0]},{"observed_ns",row[1]},{"node_run",row[2]},{"kind",row[3]},
                    {"actor",row[4]},{"cause",row[5]},{"data",Json::parse(row[6].get<std::string>())}};
                std::cout << event.dump() << '\n';
            }
            return 0;
        }
        if (o.command=="render") {
            Output out(stopped);
            Renderer renderer(o.database,o.mode,[&](const std::string& line) { out.write(line); },o.max_visible);
            renderer.run(stopped,!o.once); return 0;
        }
        WriterLock writer(o.database);
        if ((o.command=="demo" || o.command=="replay") && std::filesystem::exists(o.database)) throw std::runtime_error("Synthetic input requires a NEW --db to protect existing observations");
        Store s(o.database);
        if (o.command=="demo") {
            demo(s); Output out(stopped); Renderer renderer(o.database,o.mode,[&](const std::string& line) { out.write(line); },o.max_visible);
            renderer.run(stopped,false); return 0;
        }
        if (o.command=="replay") { replay(s,o.input); std::cout << status(s).dump(2) << '\n'; return 0; }
        RPC rpc(o.rpc_url,o.cookie);
        Observer observer(s,rpc,o);
        std::unique_ptr<Output> output;
        std::thread renderer;
        std::exception_ptr render_error;
        if (o.command=="live") {
            output=std::make_unique<Output>(stopped);
            renderer=std::thread([&] {
                try { Renderer r(o.database,o.mode,[&](const std::string& line) { output->write(line); },o.max_visible); r.run(stopped); }
                catch (const OutputClosed&) { stop.store(true); }
                catch (...) { render_error=std::current_exception(); stop.store(true); }
            });
        }
        try { observer.run(stopped); }
        catch (...) { stop.store(true); if (renderer.joinable()) renderer.join(); throw; }
        stop.store(true);
        if (renderer.joinable()) renderer.join();
        if (render_error) std::rethrow_exception(render_error);
        return 0;
    } catch (const OutputClosed&) { return 0; }
    catch (const std::exception& e) { std::cerr << "bitcoin-wizard-cave-native: " << e.what() << '\n'; return 1; }
}
