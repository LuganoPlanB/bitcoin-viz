#pragma once

#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

struct sqlite3;

namespace bitcoin_viz {
using Json = nlohmann::json;
using Params = std::vector<Json>;
using Rows = std::vector<std::vector<Json>>;
using Clock = std::chrono::steady_clock;

std::int64_t now_ns();
std::string text(const Json& value);
std::string hash256(const std::string& input);
std::string component(const std::string& input);
std::string session_id();
Json parse_exact(const std::string& input);
std::int64_t satoshis(const Json& amount);
Json fee_metadata(const Json& entry);
std::string mempool_path(const std::string& txid, const Json& data);
std::string coin_path(const Json& coin);
std::string block_path(std::int64_t height, const std::string& hash, const std::string& branch = "recent");

class Store {
    sqlite3* db_ = nullptr;
public:
    explicit Store(const std::filesystem::path& path, bool readonly = false);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    void sql(const std::string& sql);
    Rows query(const std::string& sql, const Params& params = {});
    void exec(const std::string& sql, const Params& params = {});
    Json get(const std::string& key, const Json& fallback = nullptr);
    void put(const std::string& key, const Json& value);
    std::int64_t event(const std::string& kind, const std::string& actor = "node:observer",
                       const Json& cause = nullptr, Json data = Json::object());
    void leaf(const std::string& mode, const std::string& path, const std::string& actor,
              const std::string& color, const Json& data, std::int64_t event,
              bool remove = false, bool pulse = false);
    void prune(std::int64_t keep);
    std::int64_t count(const std::string& table);
};

class Transaction {
    Store& store_;
    bool done_ = false;
public:
    explicit Transaction(Store& store, bool write = true);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit();
};

class WriterLock {
    std::intptr_t handle_ = -1;
public:
    explicit WriterLock(const std::filesystem::path& database);
    ~WriterLock();
    WriterLock(const WriterLock&) = delete;
    WriterLock& operator=(const WriterLock&) = delete;
};

struct RebuildRequired : std::runtime_error { using std::runtime_error::runtime_error; };

class Model {
    Store& s_;
    Json coin(const std::string& outpoint);
    void set_coin(const std::string& outpoint, const Json& coin, const std::string& actor, const std::string& cause);
    void trim_chain(std::int64_t height);
public:
    std::int64_t window, undo_horizon, recent;
    explicit Model(Store& store, std::int64_t window_blocks = 144, std::int64_t horizon = 288, std::int64_t recent_blocks = 12);
    void admission(const std::string& txid, Json data = nullptr, const std::string& actor = "node:mempool",
                   const std::string& cause = "admission", const Json& source = nullptr);
    void enrich(const std::string& txid, const Json& data);
    void removal(const std::string& txid, const std::string& cause = "unknown-removal",
                 const std::string& actor = "node:mempool", const Json& source = nullptr);
    void pending(const std::string& txid, const Json& transaction);
    void reconcile_mempool(const Json& txids, std::uint64_t boundary);
    void checkpoint(const std::string& hash, std::int64_t height, const std::string& cause);
    void connect(const Json& block);
    void disconnect(const std::string& hash);
    void peers(const Json& peers, const std::string& session, bool aggregate = true);
    void message(const std::string& peer, const std::string& direction, const std::string& command,
                 std::int64_t size, const std::string& fidelity = "message-capture",
                 const Json& observed_ns = nullptr, const Json& objects = nullptr);
};

struct Sequence {
    std::string hash;
    char label = 0;
    std::uint32_t counter = 0;
    std::optional<std::uint64_t> mempool_sequence;
};
Sequence parse_sequence(const std::vector<std::string>& frames);
std::string endpoint(const std::string& address);

class SequenceFeed {
    std::string address_;
    std::size_t capacity_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::condition_variable ready_cv_;
    std::deque<Sequence> queue_;
    bool ready_ = false;
    std::string error_;
    void receive();
public:
    std::atomic<std::uint64_t> dropped{0};
    explicit SequenceFeed(std::string address, std::size_t capacity = 8192);
    ~SequenceFeed();
    SequenceFeed(const SequenceFeed&) = delete;
    SequenceFeed& operator=(const SequenceFeed&) = delete;
    void start();
    void close();
    std::optional<Sequence> pop();
    std::string error();
};

struct RPCError : std::runtime_error {
    int code;
    RPCError(const std::string& method, int code, const std::string& message);
};
class RPC {
    CURL* curl_ = nullptr;
    std::string url_;
    std::filesystem::path cookie_;
    std::uint64_t serial_ = 0;
public:
    std::string cookie_marker;
    RPC(const std::string& url, std::filesystem::path cookie);
    ~RPC();
    RPC(const RPC&) = delete;
    RPC& operator=(const RPC&) = delete;
    Json call(const std::string& method, const Json& params = Json::array());
    void close();
};

std::pair<Json, bool> inventory(const std::string& payload, std::size_t limit = 1000);
class CaptureReader {
    Store& s_;
    Model& model_;
    std::filesystem::path directory_;
    std::size_t budget_, position_ = 0;
    void partial(const std::string& key, const Json& previous, const std::filesystem::path& path,
                 const std::string& cause = "capture-incomplete-record");
public:
    CaptureReader(Store& store, Model& model, std::filesystem::path directory, std::size_t budget = 100);
    std::size_t poll();
};

class Renderer {
    Store s_;
    std::string mode_;
    std::function<void(const std::string&)> output_;
    std::size_t max_visible_;
    std::set<std::string> visible_;
    std::int64_t cursor_ = 0, last_time_ = 0;
    std::uint64_t suppressed_ = 0;
    void line(std::int64_t stamp, const std::string& actor, const std::string& action,
              const std::string& path, const std::string& color);
public:
    Renderer(const std::filesystem::path& database, std::string mode,
             std::function<void(const std::string&)> output, std::size_t max_visible = 5000);
    void snapshot();
    std::size_t step();
    void run(const std::function<bool()>& stopped, bool follow = true);
    const std::set<std::string>& visible() const { return visible_; }
};

struct Options {
    std::string command, mode = "state", network = "signet", rpc_url, zmq;
    std::filesystem::path database = ".runtime/bitcoin-viz.sqlite", datadir, cookie, capture_dir, input;
    std::int64_t window = 144, horizon = 288, bootstrap = 6, metadata_budget = 32, journal_keep = 100000;
    std::size_t max_visible = 5000;
    std::int64_t after = 0, limit = 100;
    double peer_interval = 2, reconcile_interval = 15, duration = 0;
    bool once = false;
};

class Observer {
    Store& s_;
    RPC& rpc_;
    Options options_;
    std::unique_ptr<SequenceFeed> feed_;
    std::unique_ptr<CaptureReader> capture_;
    std::optional<std::uint32_t> counter_;
    std::string cookie_marker_;
    std::size_t metadata_cursor_ = 0;
    std::uint64_t last_drop_ = 0;
    void rebuild(const Json& chain, const std::string& cause);
    void metadata();
public:
    Model model;
    Observer(Store& store, RPC& rpc, Options options);
    void identify();
    Json sync_chain();
    void reconcile(std::string cause);
    void handle(const Sequence& notification);
    void run(const std::function<bool()>& stopped);
};

Json status(Store& store);
void demo(Store& store);
void replay(Store& store, const std::filesystem::path& input);
} // namespace bitcoin_viz
