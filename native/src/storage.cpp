// SPDX-License-Identifier: GPL-3.0-or-later
#include "wizard.hpp"
#include <sqlite3.h>
#include <openssl/evp.h>
#include <array>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>

namespace wizard {
std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string text(const Json& value) { return value.is_string() ? value.get<std::string>() : value.dump(); }
std::string hash256(const std::string& input) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int size = 0;
    if (EVP_Digest(input.data(), input.size(), bytes.data(), &size, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("SHA256 failed");
    std::ostringstream out;
    for (unsigned int i = 0; i < size; ++i) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(bytes[i]);
    return out.str();
}
std::string component(const std::string& input) {
    std::string clean;
    for (char raw : input) {
        const auto c = static_cast<unsigned char>(raw);
        if ((c & 0xc0U) == 0x80U) continue; // One replacement per UTF-8 codepoint, like Python's regex.
        if (clean.size() == 96) break;
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ':' || c == '-';
        clean += allowed ? static_cast<char>(c) : '_';
    }
    if (clean.empty() || clean == "." || clean == "..") clean = "unknown";
    if (clean != input) clean += "-" + hash256(input).substr(0, 12);
    return clean;
}
std::string session_id() {
    std::random_device random;
    std::ostringstream out;
    for (int i = 0; i < 8; ++i) out << std::hex << std::setw(2) << std::setfill('0') << (random() & 0xffU);
    return out.str();
}

Store::Store(const std::filesystem::path& path, bool readonly) {
    if (!readonly && path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    const int flags = readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    if (sqlite3_open_v2(path.u8string().c_str(), &db_, flags | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        std::string error = db_ ? sqlite3_errmsg(db_) : "allocation failed";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("SQLite open: " + error);
    }
    try {
        sqlite3_busy_timeout(db_, 10000);
        if (readonly) return;
        sql("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
        sql(R"SQL(
          CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);
          CREATE TABLE IF NOT EXISTS mempool(txid TEXT PRIMARY KEY,data TEXT NOT NULL);
          CREATE TABLE IF NOT EXISTS peers(id TEXT PRIMARY KEY,data TEXT NOT NULL);
          CREATE TABLE IF NOT EXISTS coins(id TEXT PRIMARY KEY,height INTEGER NOT NULL,data TEXT NOT NULL);
          CREATE INDEX IF NOT EXISTS coins_height ON coins(height);
          CREATE TABLE IF NOT EXISTS blocks(hash TEXT PRIMARY KEY,height INTEGER NOT NULL,parent TEXT NOT NULL,undo TEXT NOT NULL,coverage INTEGER NOT NULL);
          CREATE TABLE IF NOT EXISTS leaves(mode TEXT NOT NULL,path TEXT NOT NULL,actor TEXT NOT NULL,color TEXT NOT NULL,data TEXT NOT NULL,PRIMARY KEY(mode,path));
          CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY AUTOINCREMENT,observed_ns INTEGER NOT NULL,node_run TEXT,kind TEXT NOT NULL,actor TEXT NOT NULL,cause TEXT,data TEXT NOT NULL);
          CREATE TABLE IF NOT EXISTS actions(id INTEGER PRIMARY KEY AUTOINCREMENT,event_id INTEGER NOT NULL,mode TEXT NOT NULL,action TEXT NOT NULL,path TEXT NOT NULL,actor TEXT NOT NULL,color TEXT NOT NULL);
          CREATE INDEX IF NOT EXISTS actions_event ON actions(event_id);
        )SQL");
        const auto schema = get("schema");
        if (!schema.is_null() && schema != 1) throw std::runtime_error("Unsupported observer database schema");
        Transaction tx(*this);
        put("schema", 1);
        tx.commit();
    } catch (...) { sqlite3_close(db_); db_ = nullptr; throw; }
}
Store::~Store() { if (db_) sqlite3_close(db_); }
void Store::sql(const std::string& statement) {
    char* message = nullptr;
    if (sqlite3_exec(db_, statement.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
        std::string error = message ? message : sqlite3_errmsg(db_);
        sqlite3_free(message);
        throw std::runtime_error("SQLite: " + error);
    }
}
Rows Store::query(const std::string& statement, const Params& params) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_, statement.c_str(), -1, &raw, nullptr) != SQLITE_OK)
        throw std::runtime_error("SQLite prepare: " + std::string(sqlite3_errmsg(db_)));
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> handle(raw, sqlite3_finalize);
    if (sqlite3_bind_parameter_count(raw) != static_cast<int>(params.size())) throw std::runtime_error("SQL parameter count mismatch");
    for (std::size_t i = 0; i < params.size(); ++i) {
        const auto& p = params[i];
        int result = SQLITE_ERROR;
        const int index = static_cast<int>(i + 1);
        if (p.is_null()) result = sqlite3_bind_null(raw, index);
        else if (p.is_string()) {
            const auto& str = p.get_ref<const std::string&>();
            result = sqlite3_bind_text64(raw, index, str.data(), str.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
        } else if (p.is_number_integer() || p.is_boolean()) {
            result = sqlite3_bind_int64(raw, index, p.is_boolean() ? (p.get<bool>() ? 1 : 0) : p.get<std::int64_t>());
        } else throw std::runtime_error("Unsupported SQL bind type");
        if (result != SQLITE_OK) throw std::runtime_error("SQLite bind failed");
    }
    Rows rows;
    int result;
    while ((result = sqlite3_step(raw)) == SQLITE_ROW) {
        std::vector<Json> row;
        for (int i = 0; i < sqlite3_column_count(raw); ++i) {
            switch (sqlite3_column_type(raw, i)) {
                case SQLITE_NULL: row.emplace_back(nullptr); break;
                case SQLITE_INTEGER: row.emplace_back(sqlite3_column_int64(raw, i)); break;
                case SQLITE_FLOAT: row.emplace_back(sqlite3_column_double(raw, i)); break;
                default: row.emplace_back(std::string(reinterpret_cast<const char*>(sqlite3_column_text(raw, i)),
                                          static_cast<std::size_t>(sqlite3_column_bytes(raw, i))));
            }
        }
        rows.push_back(std::move(row));
    }
    if (result != SQLITE_DONE) throw std::runtime_error("SQLite step: " + std::string(sqlite3_errmsg(db_)));
    return rows;
}
void Store::exec(const std::string& statement, const Params& params) { (void)query(statement, params); }
Json Store::get(const std::string& key, const Json& fallback) {
    const auto rows = query("SELECT value FROM meta WHERE key=?", {key});
    return rows.empty() ? fallback : Json::parse(rows[0][0].get<std::string>());
}
void Store::put(const std::string& key, const Json& value) { exec("INSERT OR REPLACE INTO meta VALUES(?,?)", {key, value.dump()}); }
std::int64_t Store::event(const std::string& kind, const std::string& actor, const Json& cause, Json data) {
    if (kind == "observer.source-loss") {
        put("source_loss_count", get("source_loss_count", 0).get<std::int64_t>() + 1);
        auto loss = data;
        loss["observed_ns"] = now_ns(); loss["cause"] = cause;
        put("last_source_loss", loss);
    }
    exec("INSERT INTO events(observed_ns,node_run,kind,actor,cause,data) VALUES(?,?,?,?,?,?)",
         {now_ns(), get("node_run"), kind, actor, cause, data.dump()});
    return sqlite3_last_insert_rowid(db_);
}
void Store::leaf(const std::string& mode, const std::string& path, const std::string& actor,
                 const std::string& color, const Json& data, std::int64_t id, bool remove, bool pulse) {
    auto old = query("SELECT data FROM leaves WHERE mode=? AND path=?", {mode, path});
    std::string action;
    if (remove) {
        if (old.empty()) return;
        exec("DELETE FROM leaves WHERE mode=? AND path=?", {mode, path}); action = "D";
    } else {
        // Compare JSON semantically: Python's encoder uses different whitespace/Unicode escapes.
        if (!old.empty() && Json::parse(old[0][0].get<std::string>()) == data && !pulse) return;
        action = old.empty() ? "A" : "M";
        exec("INSERT OR REPLACE INTO leaves VALUES(?,?,?,?,?)", {mode, path, actor, color, data.dump()});
    }
    exec("INSERT INTO actions(event_id,mode,action,path,actor,color) VALUES(?,?,?,?,?,?)", {id, mode, action, path, actor, color});
}
void Store::prune(std::int64_t keep) {
    if (keep <= 0) return;
    const auto last = query("SELECT coalesce(max(id),0) FROM events")[0][0].get<std::int64_t>();
    const auto boundary = std::max<std::int64_t>(0, last - keep);
    exec("DELETE FROM actions WHERE event_id<=?", {boundary});
    exec("DELETE FROM events WHERE id<=?", {boundary});
    put("journal_floor", boundary);
}
std::int64_t Store::count(const std::string& table) {
    static const std::set<std::string> allowed{"mempool","peers","coins","blocks","events","actions","leaves"};
    if (!allowed.count(table)) throw std::runtime_error("Unknown observer table");
    return query("SELECT count(*) FROM " + table)[0][0].get<std::int64_t>();
}
Transaction::Transaction(Store& store, bool write) : store_(store) { store_.sql(write ? "BEGIN IMMEDIATE" : "BEGIN"); }
Transaction::~Transaction() { if (!done_) { try { store_.sql("ROLLBACK"); } catch (...) {} } }
void Transaction::commit() { store_.sql("COMMIT"); done_ = true; }

} // namespace wizard
