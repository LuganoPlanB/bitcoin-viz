// SPDX-License-Identifier: GPL-3.0-or-later
#include "wizard.hpp"
#include <zmq.h>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <sstream>

using namespace wizard;
namespace {
void check(bool condition, const std::string& message="Contract failed") { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F fn, const std::string& message="Expected rejection") {
    bool rejected=false;
    try { fn(); } catch (const std::exception&) { rejected=true; }
    check(rejected,message);
}
std::string h(unsigned int n) { std::ostringstream out; out << std::hex; out.width(64); out.fill('0'); out << n; return out.str(); }
Json tx(unsigned int n, Json inputs=Json::array(), int outputs=1, std::string script="51") {
    Json out=Json::array();
    for (int i=0;i<outputs;++i) out.push_back({{"n",i},{"value","0.000001"},{"scriptPubKey",{{"hex",script}}}});
    return {{"txid",h(n)},{"vin",inputs},{"vout",out}};
}
Json block(int height, Json transactions=Json::array()) {
    return {{"hash",h(static_cast<unsigned int>(height))},{"height",height},{"previousblockhash",h(static_cast<unsigned int>(height-1))},{"time",123},{"tx",transactions}};
}
struct Temp {
    std::filesystem::path path=std::filesystem::temp_directory_path()/("bwc-"+session_id());
    Temp() { std::filesystem::create_directory(path); }
    ~Temp() { std::error_code error; std::filesystem::remove_all(path,error); }
};
struct Fixture {
    Temp temp;
    std::filesystem::path path=temp.path/"observer.sqlite";
    Store store{path};
    Model model{store};
    Fixture() { Transaction t(store); store.put("node_run","test"); model.checkpoint(h(0),0,"test"); t.commit(); }
    bool cause(const std::string& name) { return !store.query("SELECT 1 FROM events WHERE cause=?",{name}).empty(); }
};
std::string bytes_le(std::uint64_t n, int width) {
    std::string bytes;
    for (int i=0;i<width;++i) bytes+=static_cast<char>((n>>(8*i))&255U);
    return bytes;
}
std::string capture_record(const std::string& command, const std::string& payload) {
    auto name=command; name.resize(12,'\0');
    return bytes_le(123456789,8)+name+bytes_le(payload.size(),4)+payload;
}
}
int main() {
    int passed=0,failed=0;
    auto test=[&](const char* name, auto fn) {
        try { fn(); ++passed; }
        catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    };
    test("confirmation removes only included members",[] {
        Fixture f; Transaction t(f.store);
        f.model.admission(h(10)); f.model.admission(h(11)); f.model.connect(block(1,Json::array({tx(10)}))); t.commit();
        check(f.store.count("mempool")==1 && f.store.query("SELECT txid FROM mempool")[0][0]==h(11)); check(f.cause("confirmation"));
    });
    test("multiple replacements retain identities",[] {
        Fixture f; Transaction t(f.store);
        f.model.admission(h(10)); f.model.admission(h(11)); f.model.removal(h(10),"replacement-observed"); f.model.removal(h(11),"replacement-observed"); f.model.admission(h(12)); t.commit();
        check(f.store.count("mempool")==1 && f.store.query("SELECT txid FROM mempool")[0][0]==h(12));
    });
    test("disconnect does not invent readmissions",[] {
        Fixture f; Transaction t(f.store);
        f.model.admission(h(10)); f.model.connect(block(1,Json::array({tx(10)}))); f.model.disconnect(h(1));
        check(f.store.count("mempool")==0 && f.store.count("coins")==0);
        f.model.reconcile_mempool(Json::array({h(10)}),77); t.commit(); check(f.store.count("mempool")==1);
    });
    test("intrablock creation spend undo leaves no residue",[] {
        Fixture f; Transaction t(f.store);
        f.model.connect(block(1,Json::array({tx(10),tx(11,Json::array({{{"txid",h(10)},{"vout",0}}}))})));
        check(f.store.count("coins")==1 && f.store.query("SELECT id FROM coins")[0][0]==h(11)+":0");
        f.model.disconnect(h(1)); t.commit(); check(f.store.count("coins")==0);
    });
    test("undo restores full previous coin",[] {
        Fixture f; Transaction t(f.store); f.model.connect(block(1,Json::array({tx(10)})));
        const auto old=f.store.query("SELECT data FROM coins")[0][0];
        f.model.connect(block(2,Json::array({tx(11,Json::array({{{"txid",h(10)},{"vout",0}}}))})));
        f.model.disconnect(h(2)); t.commit(); check(f.store.query("SELECT data FROM coins")[0][0]==old);
    });
    test("pending overlay never spends confirmed coin",[] {
        Fixture f; Transaction t(f.store); f.model.connect(block(1,Json::array({tx(10)}))); f.model.admission(h(11));
        f.model.pending(h(11),tx(11,Json::array({{{"txid",h(10)},{"vout",0}}})));
        check(f.store.count("coins")==1 && f.store.query("SELECT path FROM leaves WHERE path LIKE 'pending/%'").size()==1);
        f.model.removal(h(11)); t.commit(); check(f.store.count("coins")==1 && f.store.query("SELECT path FROM leaves WHERE path LIKE 'pending/%'").empty());
    });
    test("delayed metadata cannot resurrect removed member",[] {
        Fixture f; Transaction t(f.store); f.model.admission(h(10)); f.model.removal(h(10)); f.model.enrich(h(10),{{"band","fee-20-plus"}}); f.model.pending(h(10),tx(10)); t.commit();
        check(f.store.count("mempool")==0 && f.store.count("leaves")==0);
    });
    test("atomic rollback includes state tip undo actions and journal",[] {
        Fixture f; const auto events=f.store.count("events");
        try { Transaction t(f.store); f.model.connect(block(1,Json::array({tx(10)}))); throw std::runtime_error("interrupted"); } catch (const std::runtime_error&) {}
        check(f.store.get("tip")["hash"]==h(0) && f.store.count("coins")==0 && f.store.count("blocks")==0 && f.store.count("actions")==0 && f.store.count("events")==events);
    });
    test("cohort expiry is distinct and reversible",[] {
        Fixture f; f.model.window=1; Transaction t(f.store); f.model.connect(block(1,Json::array({tx(10)}))); f.model.connect(block(2));
        check(f.store.count("coins")==0 && f.cause("coverage-expiry") && !f.cause("spend"));
        f.model.disconnect(h(2)); t.commit(); check(f.store.count("coins")==1 && f.store.get("coverage_height")==1);
    });
    test("reorg below horizon requires explicit rebuild",[] {
        Fixture f; f.model.undo_horizon=1; Transaction t(f.store); f.model.connect(block(1,Json::array({tx(10)}))); f.model.connect(block(2)); f.model.disconnect(h(2));
        rejects([&] { f.model.disconnect(h(1)); }); t.commit();
    });
    test("parent mismatch cannot mutate state",[] {
        Fixture f; rejects([&] { Transaction t(f.store); f.model.connect(block(2,Json::array({tx(10)}))); t.commit(); }); check(f.store.count("coins")==0);
    });
    test("provably unspendable outputs excluded",[] {
        Fixture f; Transaction t(f.store); f.model.connect(block(1,Json::array({tx(10,Json::array(),1,"6a01"),tx(11,Json::array(),1,std::string(20002,'5')),tx(12)}))); t.commit();
        check(f.store.count("coins")==1);
    });
    test("renderer restart repopulates shared scene",[] {
        Fixture f; { Transaction t(f.store); f.model.admission(h(10)); t.commit(); }
        std::string output; Renderer r(f.path,"state",[&](const std::string& line) { output+=line; }); r.snapshot();
        check(output.find("|node:snapshot|A|mempool/")!=std::string::npos && r.visible().size()==1);
    });
    test("visible cap never truncates authoritative state",[] {
        Fixture f; { Transaction t(f.store); for (unsigned n=10;n<15;++n) f.model.admission(h(n)); t.commit(); }
        Renderer r(f.path,"state",[](const std::string&) {},2); r.snapshot(); check(r.visible().size()==2 && f.store.count("mempool")==5);
    });
    test("slow output releases read transaction and settles",[] {
        Fixture f; { Transaction t(f.store); f.model.admission(h(10)); t.commit(); }
        std::promise<void> entered,release; auto released=release.get_future().share(); auto started=entered.get_future();
        std::exception_ptr error; bool first=true;
        std::thread worker([&] {
            try { Renderer r(f.path,"state",[&](const std::string&) { if (first) { first=false; entered.set_value(); } if (released.wait_for(std::chrono::seconds(5))!=std::future_status::ready) throw std::runtime_error("output not released"); }); r.snapshot(); }
            catch (...) { error=std::current_exception(); }
        });
        const bool held=started.wait_for(std::chrono::seconds(2))==std::future_status::ready;
        bool wrote=false;
        try { if (held) { Transaction t(f.store); f.model.admission(h(11)); t.commit(); wrote=true; } } catch (...) {}
        release.set_value(); worker.join();
        check(held && wrote && !error && f.store.count("mempool")==2);
    });
    test("pruned renderer journal triggers scene rebuild",[] {
        Fixture f; { Transaction t(f.store); f.model.admission(h(10)); t.commit(); }
        Renderer r(f.path,"state",[](const std::string&) {}); r.snapshot();
        { Transaction t(f.store); f.model.removal(h(10)); f.model.admission(h(11)); f.store.prune(1); t.commit(); }
        r.step(); check(r.visible()==std::set<std::string>{mempool_path(h(11),Json::object())});
    });
    test("snapshot cursor does not lose concurrent updates",[] {
        Fixture f; std::string output;
        { Transaction t(f.store); f.model.admission(h(10)); t.commit(); }
        Renderer r(f.path,"state",[&](const std::string& line) { output+=line; }); r.snapshot();
        { Transaction t(f.store); f.model.removal(h(10)); f.model.admission(h(11)); t.commit(); }
        r.step(); check(r.visible()==std::set<std::string>{mempool_path(h(11),Json::object())}); check(output.find("|D|")!=std::string::npos);
    });
    test("peer counters are byte deltas not historical counts",[] {
        Fixture f; Json peers=Json::array({{{"id",4},{"conntime",123},{"bytesrecv_per_msg",{{"inv",100}}}}}); Transaction t(f.store);
        f.model.peers(peers,"run"); peers[0]["bytesrecv_per_msg"]["inv"]=160; f.model.peers(peers,"run"); t.commit();
        const auto rows=f.store.query("SELECT data FROM events WHERE cause='aggregate-bytes'");
        check(rows.size()==1 && Json::parse(rows[0][0].get<std::string>())["size_bytes"]==60);
        check(!f.store.query("SELECT 1 FROM leaves WHERE path LIKE 'traffic/unclassified/inv/%'").empty());
    });
    test("new run cannot reuse peer identity",[] {
        Fixture f; Transaction t(f.store); auto peer=Json::array({{{"id",4},{"conntime",123}}}); f.model.peers(peer,"run1"); f.model.peers(peer,"run2"); t.commit();
        check(f.store.count("peers")==1 && f.store.query("SELECT id FROM peers")[0][0].get<std::string>().find("run2")!=std::string::npos);
    });
    test("transport wrap and inner jumps are accepted",[] {
        Fixture f; RPC rpc("http://127.0.0.1:38332",f.temp.path/"unused.cookie"); Observer o(f.store,rpc,Options{});
        o.handle({h(10),'A',0xffffffffU,10}); o.handle({h(11),'A',0,100}); check(f.store.count("mempool")==2);
    });
    test("snapshot sequence is NEXT event boundary",[] {
        Fixture f; { Transaction t(f.store); f.model.reconcile_mempool(Json::array(),25); t.commit(); }
        RPC rpc("http://127.0.0.1:38332",f.temp.path/"unused.cookie"); Observer o(f.store,rpc,Options{});
        o.handle({h(10),'A',0,25}); o.handle({h(11),'A',1,24}); check(f.store.count("mempool")==1 && f.store.get("mempool_boundary")==26);
    });
    test("wire hash order and 64 bit sequence stay exact",[] {
        std::string hash; for (int i=0;i<32;++i) hash+=static_cast<char>(i);
        const auto seq=(std::uint64_t(1)<<60)+7;
        auto event=parse_sequence({"sequence",hash+"A"+bytes_le(seq,8),bytes_le(12,4)});
        check(event.hash.substr(0,8)=="00010203" && event.counter==12 && event.mempool_sequence==seq);
    });
    test("malformed multipart rejected",[] {
        rejects([] { parse_sequence({"sequence"}); });
        rejects([] { parse_sequence({"sequence",std::string(32,'x')+"A","1234"}); });
        rejects([] { parse_sequence({"sequence",std::string(32,'x')+"X","1234"}); });
        rejects([] { parse_sequence({"sequence",std::string(32,'x')+"C","123"}); });
    });
    test("current legacy and weight fee definitions stay honest",[] {
        check(fee_metadata({{"fees",{{"base","0.00001"}}},{"vsize_bip141",200}})["band"]=="fee-5-to-20");
        const auto derived=fee_metadata({{"fees",{{"base","0.00001"}}},{"weight",401}});
        check(derived["vsize_bip141"]==101 && derived["size_source"]=="ceil(weight/4)");
        check(fee_metadata({{"fees",{{"base","0.00001"}}},{"vsize",101}})["band"]=="fee-unknown");
    });
    test("satoshi arithmetic avoids binary floating point",[] {
        const auto parsed=parse_exact(R"({"a":0.00000001,"b":21000000.00000000,"c":1e-8,"counter":18446744073709551614})");
        check(satoshis(parsed["a"])==1 && satoshis(parsed["b"])==2100000000000000LL && satoshis(parsed["c"])==1);
        check(parsed["counter"].get<std::uint64_t>()==std::numeric_limits<std::uint64_t>::max()-1);
        check(satoshis("1.000000000")==100000000 && satoshis("-0.00000001")==-1);
        rejects([] { satoshis("0.000000001"); }); rejects([] { satoshis("99999999999999"); }); rejects([] { satoshis("nan"); });
    });
    test("JSON quotes and decimal lexemes remain distinct",[] {
        auto p=parse_exact(R"({"x":"not 0.0001","list":[1,2.30,true,null,{"a":-0.001}]})");
        check(p["x"]=="not 0.0001" && p["list"][0]==1 && p["list"][1]=="2.30" && p["list"][4]["a"]=="-0.001");
        rejects([] { parse_exact("{garbage}"); });
    });
    test("inventory hash order and witness identities retained",[] {
        const auto payload=std::string(1,'\2')+bytes_le(1,4)+bytes_le(10,8)+std::string(24,'\0')+bytes_le(5,4)+bytes_le(11,8)+std::string(24,'\0');
        const auto parsed=inventory(payload);
        check(parsed.first[0]["kind"]=="txid" && parsed.first[0]["hash"]==h(10) && parsed.first[1]["kind"]=="wtxid" && !parsed.second);
        rejects([&] { inventory(payload.substr(0,payload.size()-1)); });
    });
    test("capture partial record resumes once and reports coverage",[] {
        Fixture f; auto dir=f.temp.path/"capture"/"endpoint"; std::filesystem::create_directories(dir);
        const auto record=capture_record("ping","12345678");
        { std::ofstream stream(dir/"msgs_recv.dat",std::ios::binary); stream.write(record.data(),25); }
        CaptureReader reader(f.store,f.model,dir.parent_path()); check(reader.poll()==0 && reader.poll()==0);
        check(f.store.get("source_loss_count")==1 && f.cause("capture-incomplete-record"));
        { std::ofstream stream(dir/"msgs_recv.dat",std::ios::binary|std::ios::app); stream << record.substr(25); }
        check(reader.poll()==1 && reader.poll()==0);
        check(f.store.query("SELECT kind FROM events WHERE cause='message-capture'")[0][0]=="peer.message.incoming");
    });
    test("capture fairness across continuously busy files",[] {
        Fixture f; auto dir=f.temp.path/"capture"/"endpoint"; std::filesystem::create_directories(dir);
        for (const auto* name : {"msgs_recv.dat","msgs_sent.dat"}) { std::ofstream stream(dir/name,std::ios::binary); for (int i=0;i<5;++i) stream << capture_record("ping",""); }
        CaptureReader reader(f.store,f.model,dir.parent_path(),1); check(reader.poll()==1 && reader.poll()==1);
        auto rows=f.store.query("SELECT DISTINCT kind FROM events WHERE cause='message-capture'"); check(rows.size()==2);
    });
    test("capture truncation is explicit",[] {
        Fixture f; auto dir=f.temp.path/"capture"/"endpoint"; std::filesystem::create_directories(dir);
        { std::ofstream stream(dir/"msgs_recv.dat",std::ios::binary); stream << capture_record("ping","1234"); }
        CaptureReader reader(f.store,f.model,dir.parent_path()); check(reader.poll()==1);
        { std::ofstream stream(dir/"msgs_recv.dat",std::ios::binary|std::ios::trunc); stream << capture_record("pong",""); }
        check(reader.poll()==1 && f.cause("capture-rotation-or-truncation"));
    });
    test("inventory touches only existing transaction membership",[] {
        Fixture f; Transaction t(f.store); f.model.admission(h(10));
        f.model.message("captured-peer","incoming","inv",73,"message-capture",nullptr,Json::array({{{"kind","txid"},{"hash",h(10)}},{{"kind","txid"},{"hash",h(11)}}})); t.commit();
        check(f.store.count("mempool")==1 && !f.store.query("SELECT 1 FROM actions WHERE mode='state' AND action='M'").empty());
    });
    test("writer lock excludes competing collectors",[] {
        Temp temp; WriterLock lock(temp.path/"db.sqlite"); rejects([&] { WriterLock duplicate(temp.path/"db.sqlite"); });
    });
    test("untrusted labels cannot inject Gource fields or directories",[] {
        const auto clean=component("evil|peer\n../../user/name"); check(clean.find_first_of("|\n/")==std::string::npos);
        check(component("a/b")!=component("a|b") && component("..")!="..");
    });
    test("remote RPC and ZMQ endpoints rejected",[] {
        check(endpoint("unix:/tmp/sequence.sock")=="ipc:///tmp/sequence.sock");
        rejects([] { endpoint("tcp://0.0.0.0:28332"); }); rejects([] { endpoint("unix:relative.sock"); });
        rejects([] { RPC r("http://remote.example:8332","/tmp/cookie"); }); rejects([] { RPC r("http://user:pass@127.0.0.1:38332","/tmp/cookie"); });
    });
    test("real libzmq IPC subscriber receives multipart",[] {
        Temp temp; const auto address="ipc://"+(temp.path/"sequence.sock").string();
        void* context=zmq_ctx_new(); check(context!=nullptr);
        void* publisher=zmq_socket(context,ZMQ_PUB);
        if (!publisher) { zmq_ctx_term(context); throw std::runtime_error("publisher allocation failed"); }
        int linger=0; zmq_setsockopt(publisher,ZMQ_LINGER,&linger,sizeof(linger));
        if (zmq_bind(publisher,address.c_str())) { zmq_close(publisher); zmq_ctx_term(context); throw std::runtime_error("publisher bind failed"); }
        bool found=false;
        try {
            SequenceFeed feed(address); feed.start(); std::this_thread::sleep_for(std::chrono::milliseconds(300));
            const auto body=std::string(32,'\0')+"C", counter=bytes_le(7,4);
            check(zmq_send(publisher,"sequence",8,ZMQ_SNDMORE)>=0 && zmq_send(publisher,body.data(),body.size(),ZMQ_SNDMORE)>=0 && zmq_send(publisher,counter.data(),counter.size(),0)>=0);
            auto until=Clock::now()+std::chrono::seconds(2);
            while (Clock::now()<until) { if (auto e=feed.pop()) { found=e->hash==h(0) && e->label=='C' && e->counter==7; break; } std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        } catch (...) { zmq_close(publisher); zmq_ctx_term(context); throw; }
        zmq_close(publisher); zmq_ctx_term(context); check(found,"No real IPC notification received");
    });
    std::cout << "Native contracts: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
