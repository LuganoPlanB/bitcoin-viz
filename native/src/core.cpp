// SPDX-License-Identifier: GPL-3.0-or-later
#include "wizard.hpp"
#include <curl/curl.h>
#include <zmq.h>
#include <fstream>
#include <cerrno>
#include <limits>
#include <regex>
#include <sstream>

namespace wizard {
namespace {
// Public SAX interface preserves RPC decimal lexemes as strings. Integer IDs,
// counters and heights stay exact integers, including uint64 mempool sequences.
struct ExactSax : nlohmann::json_sax<Json> {
    Json root;
    std::vector<Json*> stack;
    std::vector<std::string> keys;
    std::string error;
    Json* add(Json value) {
        if (stack.empty()) { root = std::move(value); return &root; }
        auto& parent = *stack.back();
        if (parent.is_array()) { parent.push_back(std::move(value)); return &parent.back(); }
        parent[keys.back()] = std::move(value);
        return &parent[keys.back()];
    }
    bool null() override { add(nullptr); return true; }
    bool boolean(bool v) override { add(v); return true; }
    bool number_integer(number_integer_t v) override { add(v); return true; }
    bool number_unsigned(number_unsigned_t v) override { add(v); return true; }
    bool number_float(number_float_t, const string_t& raw) override { add(raw); return true; }
    bool string(string_t& v) override { add(v); return true; }
    bool binary(binary_t&) override { return false; }
    bool start_object(std::size_t) override { if (stack.size() >= 256) return false; stack.push_back(add(Json::object())); keys.emplace_back(); return true; }
    bool key(string_t& v) override { keys.back() = v; return true; }
    bool end_object() override { stack.pop_back(); keys.pop_back(); return true; }
    bool start_array(std::size_t) override { if (stack.size() >= 256) return false; stack.push_back(add(Json::array())); keys.emplace_back(); return true; }
    bool end_array() override { stack.pop_back(); keys.pop_back(); return true; }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception& e) override { error = e.what(); return false; }
};
struct CurlGlobal {
    CurlGlobal() { if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("libcurl initialization failed"); }
    ~CurlGlobal() { curl_global_cleanup(); }
};
void initialize_curl() { static CurlGlobal guard; (void)guard; }
struct Response { std::string data; bool failed = false; };
std::size_t receive_body(char* bytes, std::size_t size, std::size_t count, void* user) noexcept {
    auto& response = *static_cast<Response*>(user);
    constexpr std::size_t max_bytes = 64 * 1024 * 1024;
    if (size && count > std::numeric_limits<std::size_t>::max() / size) { response.failed = true; return 0; }
    const auto total = size * count;
    if (total > max_bytes - response.data.size()) { response.failed = true; return 0; }
    try { response.data.append(bytes,total); } catch (...) { response.failed = true; return 0; }
    return total;
}
std::string read_cookie(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot read RPC cookie (node may be stopped)");
    std::string result;
    std::getline(input,result);
    if (result.size() > 65536) throw std::runtime_error("RPC cookie too large");
    while (!result.empty() && (result.back() == '\r' || result.back() == '\n')) result.pop_back();
    if (result.empty() || result.find(':') == std::string::npos) throw std::runtime_error("Invalid RPC cookie");
    return result;
}
std::uint64_t little_endian(const std::string& bytes, std::size_t offset, std::size_t width) {
    std::uint64_t result = 0;
    for (std::size_t i = 0; i < width; ++i) result |= std::uint64_t(static_cast<unsigned char>(bytes.at(offset+i))) << (8*i);
    return result;
}
std::string hex_bytes(const std::string& bytes) {
    const char* digits = "0123456789abcdef";
    std::string result;
    for (char raw : bytes) { const auto c = static_cast<unsigned char>(raw); result += digits[c>>4U]; result += digits[c&15U]; }
    return result;
}
} // namespace

Json parse_exact(const std::string& input) {
    ExactSax sax;
    if (!Json::sax_parse(input,&sax)) throw std::runtime_error("Invalid or overly nested JSON response");
    return sax.root;
}
RPCError::RPCError(const std::string& method, int error_code, const std::string& message)
    : std::runtime_error(method + ": " + std::to_string(error_code) + ": " + message), code(error_code) {}
RPC::RPC(const std::string& url, std::filesystem::path cookie) : url_(url), cookie_(std::move(cookie)) {
    initialize_curl();
    std::unique_ptr<CURLU,decltype(&curl_url_cleanup)> parsed(curl_url(),curl_url_cleanup);
    if (!parsed || curl_url_set(parsed.get(),CURLUPART_URL,url.c_str(),0) != CURLUE_OK) throw std::runtime_error("Invalid RPC URL");
    const auto part = [&](CURLUPart field) {
        char* raw = nullptr;
        if (curl_url_get(parsed.get(),field,&raw,0) != CURLUE_OK) return std::string();
        std::string value(raw); curl_free(raw); return value;
    };
    const auto host = part(CURLUPART_HOST);
    if (part(CURLUPART_SCHEME) != "http" || (host != "127.0.0.1" && host != "localhost" && host != "[::1]" && host != "::1") ||
        !part(CURLUPART_USER).empty() || !part(CURLUPART_PASSWORD).empty())
        throw std::runtime_error("RPC must be a local http:// loopback URL without embedded credentials");
}
RPC::~RPC() { close(); }
void RPC::close() { if (curl_) curl_easy_cleanup(curl_); curl_ = nullptr; }
Json RPC::call(const std::string& method, const Json& params) {
    const auto credential = read_cookie(cookie_);
    cookie_marker = hash256(credential);
    const auto id = ++serial_;
    const auto body = Json({{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}}).dump();
    if (!curl_) curl_ = curl_easy_init();
    if (!curl_) throw std::runtime_error("libcurl handle allocation failed");
    Response response;
    std::unique_ptr<curl_slist,decltype(&curl_slist_free_all)> headers(curl_slist_append(nullptr,"Content-Type: application/json"),curl_slist_free_all);
    if (!headers) throw std::runtime_error("RPC header allocation failed");
    const auto option = [&](CURLoption key, auto value) {
        if (curl_easy_setopt(curl_,key,value) != CURLE_OK) throw std::runtime_error("libcurl option failed");
    };
    try {
        option(CURLOPT_URL,url_.c_str()); option(CURLOPT_USERPWD,credential.c_str());
        option(CURLOPT_HTTPAUTH,static_cast<long>(CURLAUTH_BASIC));
        option(CURLOPT_POSTFIELDS,body.c_str()); option(CURLOPT_POSTFIELDSIZE_LARGE,static_cast<curl_off_t>(body.size()));
        option(CURLOPT_HTTPHEADER,headers.get()); option(CURLOPT_WRITEFUNCTION,&receive_body); option(CURLOPT_WRITEDATA,&response);
        option(CURLOPT_NOSIGNAL,1L); option(CURLOPT_CONNECTTIMEOUT_MS,3000L); option(CURLOPT_TIMEOUT_MS,10000L);
        option(CURLOPT_FOLLOWLOCATION,0L); option(CURLOPT_PROXY,""); option(CURLOPT_NOPROXY,"*");
        const auto result = curl_easy_perform(curl_);
        if (result != CURLE_OK) throw RPCError(method,-1,response.failed ? "response exceeds resource limit" : curl_easy_strerror(result));
        long status_code = 0;
        if (curl_easy_getinfo(curl_,CURLINFO_RESPONSE_CODE,&status_code) != CURLE_OK) throw RPCError(method,-1,"HTTP status unavailable");
        if (status_code == 401) throw RPCError(method,401,"cookie rejected; check datadir/network");
        if (status_code != 200 && status_code != 500) throw RPCError(method,static_cast<int>(status_code),"unexpected HTTP response");
        auto decoded = parse_exact(response.data);
        if (decoded.contains("error") && !decoded["error"].is_null())
            throw RPCError(method,decoded["error"].at("code"),decoded["error"].at("message"));
        if (decoded.value("id",Json(nullptr)) != id) throw RPCError(method,-1,"response id mismatch");
        return decoded.at("result");
    } catch (...) { close(); throw; }
}
Sequence parse_sequence(const std::vector<std::string>& frames) {
    if (frames.size() != 3 || frames[0] != "sequence" || frames[2].size() != 4) throw std::runtime_error("Expected sequence/body/uint32 multipart frames");
    const auto& body = frames[1];
    if (body.size() != 33 && body.size() != 41) throw std::runtime_error("Invalid sequence body length");
    const char label = body[32];
    const bool mempool = label == 'A' || label == 'R';
    if ((!mempool && label != 'C' && label != 'D') || body.size() != (mempool ? 41U : 33U)) throw std::runtime_error("Invalid sequence label/body combination");
    return {hex_bytes(body.substr(0,32)),label,static_cast<std::uint32_t>(little_endian(frames[2],0,4)),
            mempool ? std::optional<std::uint64_t>(little_endian(body,33,8)) : std::nullopt};
}
std::string endpoint(const std::string& address) {
    std::string result = address.rfind("unix:",0) == 0 ? "ipc://" + address.substr(5) : address;
    if (result.rfind("ipc:///",0) == 0 && result.size() > 7) return result;
    std::smatch match;
    if (std::regex_match(result,match,std::regex(R"(tcp://(?:127\.0\.0\.1|\[::1\]):([0-9]{1,5}))"))) {
        const auto port = std::stoi(match[1]);
        if (port > 0 && port <= 65535) return result;
    }
    throw std::runtime_error("Use unix:/absolute/path, ipc:///absolute/path or loopback tcp:// for ZMQ");
}
SequenceFeed::SequenceFeed(std::string address, std::size_t capacity) : address_(endpoint(address)), capacity_(capacity) {
    if (!capacity || capacity > static_cast<std::size_t>(std::numeric_limits<int>::max())) throw std::runtime_error("Invalid ZMQ queue capacity");
}
SequenceFeed::~SequenceFeed() { close(); }
void SequenceFeed::start() {
    if (thread_.joinable()) throw std::runtime_error("Sequence subscriber already started");
    thread_ = std::thread([this] { receive(); });
    std::unique_lock<std::mutex> lock(mutex_);
    if (!ready_cv_.wait_for(lock,std::chrono::seconds(5),[this] { return ready_; })) throw std::runtime_error("ZMQ subscriber startup timed out");
    if (!error_.empty()) throw std::runtime_error(error_);
}
void SequenceFeed::close() { stop_.store(true); if (thread_.joinable()) thread_.join(); }
std::optional<Sequence> SequenceFeed::pop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) return std::nullopt;
    auto event = std::move(queue_.front()); queue_.pop_front(); return event;
}
std::string SequenceFeed::error() { std::lock_guard<std::mutex> lock(mutex_); return error_; }
void SequenceFeed::receive() {
    void* context = nullptr;
    void* socket = nullptr;
    try {
        context = zmq_ctx_new();
        if (!context) throw std::runtime_error("ZMQ context allocation failed");
        socket = zmq_socket(context,ZMQ_SUB);
        if (!socket) throw std::runtime_error("ZMQ socket allocation failed");
        int hwm = static_cast<int>(capacity_), linger = 0, timeout = 100;
        std::int64_t max_message = 1024 * 1024;
        if (zmq_setsockopt(socket,ZMQ_SUBSCRIBE,"sequence",8) || zmq_setsockopt(socket,ZMQ_RCVHWM,&hwm,sizeof(hwm)) ||
            zmq_setsockopt(socket,ZMQ_LINGER,&linger,sizeof(linger)) || zmq_setsockopt(socket,ZMQ_RCVTIMEO,&timeout,sizeof(timeout)) ||
            zmq_setsockopt(socket,ZMQ_MAXMSGSIZE,&max_message,sizeof(max_message)) || zmq_connect(socket,address_.c_str()))
            throw std::runtime_error("ZMQ subscriber configuration/connect failed");
        { std::lock_guard<std::mutex> lock(mutex_); ready_ = true; ready_cv_.notify_all(); }
        while (!stop_.load()) {
            zmq_pollitem_t item{socket,0,ZMQ_POLLIN,0};
            const int polled = zmq_poll(&item,1,100);
            if (polled < 0) { if (zmq_errno() == EINTR) continue; throw std::runtime_error("ZMQ poll failed"); }
            if (!polled) continue;
            std::vector<std::string> frames;
            bool more = false, invalid = false;
            do {
                zmq_msg_t message;
                if (zmq_msg_init(&message)) throw std::runtime_error("ZMQ frame allocation failed");
                if (zmq_msg_recv(&message,socket,0) < 0) {
                    zmq_msg_close(&message); invalid = true; break;
                }
                more = zmq_msg_more(&message) != 0;
                if (frames.size() < 3 && zmq_msg_size(&message) <= 1024)
                    frames.emplace_back(static_cast<const char*>(zmq_msg_data(&message)),zmq_msg_size(&message));
                else invalid = true;
                zmq_msg_close(&message);
            } while (more && !stop_.load());
            if (invalid || more) { ++dropped; continue; }
            try {
                auto event = parse_sequence(frames);
                std::lock_guard<std::mutex> lock(mutex_);
                if (queue_.size() >= capacity_) ++dropped;
                else queue_.push_back(std::move(event));
            } catch (const std::exception&) { ++dropped; }
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(mutex_); error_ = e.what(); ready_ = true; ready_cv_.notify_all();
    }
    if (socket) zmq_close(socket);
    if (context) zmq_ctx_term(context);
}
} // namespace wizard
