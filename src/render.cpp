// SPDX-License-Identifier: GPL-3.0-or-later
#include "bitcoin_viz.hpp"
#include <algorithm>
#include <iostream>
#include <sstream>

namespace bitcoin_viz {
Renderer::Renderer(const std::filesystem::path& database, std::string mode,
                   std::function<void(const std::string&)> output, std::size_t max_visible)
    : s_(database,true), mode_(std::move(mode)), output_(std::move(output)), max_visible_(max_visible) {
    if (!max_visible_) throw std::runtime_error("--max-visible must be positive");
    if (mode_ != "state" && mode_ != "protocol" && mode_ != "utxo") throw std::runtime_error("Invalid visualization mode");
    if (s_.get("schema") != 1) throw std::runtime_error("Unsupported observer database schema");
}
void Renderer::line(std::int64_t stamp, const std::string& actor, const std::string& action,
                    const std::string& path, const std::string& color) {
    if (path.find_first_of("|\n\r") != std::string::npos) throw std::runtime_error("Unsafe Gource path");
    std::istringstream parts(path);
    std::string part;
    if (path.empty() || path.back() == '/') throw std::runtime_error("Unsafe Gource path");
    while (std::getline(parts,part,'/')) if (part.empty() || part == "." || part == "..") throw std::runtime_error("Unsafe Gource path");
    if (color.size() != 6 || color.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) throw std::runtime_error("Unsafe leaf color");
    last_time_ = std::max(last_time_,stamp);
    output_(std::to_string(last_time_) + "|" + component(actor) + "|" + action + "|" + path + "|" + color + "\n");
}
void Renderer::snapshot() {
    Rows rows;
    std::int64_t cursor, total;
    Json coverage;
    {
        Transaction tx(s_,false);
        cursor = s_.query("SELECT coalesce(max(id),0) FROM actions")[0][0];
        rows = s_.query("SELECT path,actor,color FROM leaves WHERE mode=? ORDER BY path LIMIT ?",{mode_,max_visible_});
        total = s_.query("SELECT count(*) FROM leaves WHERE mode=?",{mode_})[0][0];
        coverage = s_.get("coverage_height");
        tx.commit(); // No read transaction survives a potentially blocked output.
    }
    const auto stamp = now_ns()/1000000000;
    for (const auto& path : visible_) line(stamp,"view:resync","D",path,"999999");
    visible_.clear();
    for (const auto& row : rows) { line(stamp,"node:snapshot","A",row[0],row[2]); visible_.insert(row[0]); }
    cursor_ = cursor;
    std::cerr << "bitcoin-viz: mode=" << mode_ << " visible=" << visible_.size() << '/' << total
              << "; UTXO creation-height boundary=" << coverage << '\n';
}
std::size_t Renderer::step() {
    // Test cursor existence and fetch rows in ONE snapshot, so pruning cannot
    // race between the check and read and silently lose a semantic deletion.
    Rows rows;
    bool resnapshot = false;
    {
        Transaction tx(s_,false);
        resnapshot = !s_.get("journal_floor").is_null() && cursor_ != 0 && s_.query("SELECT event_id FROM actions WHERE id=?",{cursor_}).empty();
        if (!resnapshot) rows = s_.query(R"SQL(SELECT a.id,a.action,a.path,a.actor,a.color,e.observed_ns FROM actions a
            JOIN events e ON e.id=a.event_id WHERE a.id>? AND a.mode=? ORDER BY a.id LIMIT 1000)SQL",{cursor_,mode_});
        tx.commit();
    }
    if (resnapshot) { snapshot(); return 0; }
    for (const auto& row : rows) {
        cursor_ = row[0];
        auto action = row[1].get<std::string>();
        auto path = row[2].get<std::string>();
        if (action == "D") { if (!visible_.erase(path)) continue; }
        else if (!visible_.count(path)) {
            if (visible_.size() >= max_visible_) { ++suppressed_; continue; }
            visible_.insert(path); action = "A";
        }
        line(row[5].get<std::int64_t>()/1000000000,row[3],action,path,row[4]);
    }
    return rows.size();
}
void Renderer::run(const std::function<bool()>& stopped, bool follow) {
    snapshot();
    while (follow && !stopped()) if (!step()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cerr << "bitcoin-viz: renderer suppressed " << suppressed_ << " additions at visible limit\n";
}
Json status(Store& store) {
    Transaction tx(store,false);
    Json result = Json::object();
    for (const auto& row : store.query("SELECT key,value FROM meta")) {
        auto key = row[0].get<std::string>();
        if (key.rfind("capture:",0) != 0) result[key] = Json::parse(row[1].get<std::string>());
    }
    for (const std::string table : {"coins","mempool","peers","blocks","events","actions"}) result["counts"][table] = store.count(table);
    result["leaves"] = Json::object();
    for (const auto& row : store.query("SELECT mode,count(*) FROM leaves GROUP BY mode")) result["leaves"][row[0].get<std::string>()] = row[1];
    result["coverage"] = "observed outputs only; complete membership is claimed only within retained creation cohorts";
    result["protocol"] = "aggregate byte deltas unless capture is enabled; byte deltas are not message counts";
    tx.commit(); return result;
}
} // namespace bitcoin_viz
