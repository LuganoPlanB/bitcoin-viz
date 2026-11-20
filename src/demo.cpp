// SPDX-License-Identifier: GPL-3.0-or-later
#include "bitcoin_viz.hpp"
#include <sstream>

namespace bitcoin_viz {
namespace {
std::string hash_id(unsigned int n) { std::ostringstream out; out << std::hex; out.width(64); out.fill('0'); out << n; return out.str(); }
}
void demo(Store& s) {
    Model m(s);
    Transaction tx(s);
    s.put("node_run","SYNTHETIC-DEMO"); s.put("network","synthetic"); m.checkpoint(hash_id(0),100,"synthetic-demo");
    m.admission(hash_id(10),fee_metadata({{"fees",{{"base","0.000003"}}},{"weight",480}}));
    m.admission(hash_id(11),fee_metadata({{"fees",{{"base","0.000024"}}},{"weight",720}}));
    m.peers(Json::array({{{"id",7},{"conntime",1},{"network","ipv4"},{"connection_type","outbound-full-relay"}},
        {{"id",8},{"conntime",2},{"network","ipv6"},{"inbound",true}}}),"demo");
    for (const std::string command : {"version","verack","ping","addr","headers","cmpctblock","tx","inv"})
        m.message("demo-peer7-1","incoming",command,300,"synthetic-message");
    m.message("demo-peer7-1","outgoing","pong",8,"synthetic-message");
    m.message("demo-peer8-2","outgoing","inv",1800,"synthetic-message");
    m.message("demo-peer8-2","incoming","getdata",1800,"synthetic-message");
    m.message("demo-peer8-2","outgoing","tx",420,"synthetic-message");
    Json outputs=Json::array();
    for (int i=0;i<8;++i) outputs.push_back({{"n",i},{"value","0.001"},{"scriptPubKey",{{"hex","51"}}}});
    m.connect({{"hash",hash_id(1)},{"height",101},{"previousblockhash",hash_id(0)},
        {"tx",Json::array({{{"txid",hash_id(10)},{"vin",Json::array({{{"txid",hash_id(8)},{"vout",0}}})},{"vout",outputs}}})}});
    tx.commit();
}
} // namespace bitcoin_viz
