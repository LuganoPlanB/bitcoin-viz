// SPDX-License-Identifier: GPL-3.0-or-later
#include "bitcoin_viz.hpp"
#include "platform.hpp"
#include <algorithm>
#include <cerrno>
#include <limits>

namespace bitcoin_viz {
namespace {
std::uint64_t le(const std::string& bytes, std::size_t offset, std::size_t count) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < count; ++i) value |= std::uint64_t(static_cast<unsigned char>(bytes.at(offset+i))) << (8*i);
    return value;
}
std::string reversed_hex(const std::string& bytes) {
    const char* digits = "0123456789abcdef";
    std::string result;
    for (auto i = bytes.rbegin(); i != bytes.rend(); ++i) { auto c = static_cast<unsigned char>(*i); result += digits[c>>4U]; result += digits[c&15U]; }
    return result;
}
}
std::pair<Json,bool> inventory(const std::string& payload, std::size_t limit) {
    if (payload.empty()) throw std::runtime_error("Empty inventory");
    const auto first = static_cast<unsigned char>(payload[0]);
    const std::size_t width = first < 253 ? 0 : first == 253 ? 2 : first == 254 ? 4 : 8;
    if (payload.size() < 1+width) throw std::runtime_error("Truncated inventory count");
    const auto count = width ? le(payload,1,width) : first;
    const auto offset = 1+width;
    if (count > 50000 || payload.size() != offset+count*36) throw std::runtime_error("Invalid inventory length");
    Json objects = Json::array();
    for (std::size_t i = 0; i < std::min<std::uint64_t>(count,limit); ++i) {
        const auto start = offset+i*36;
        const auto type = le(payload,start,4)&0x3fffffffU;
        std::string kind;
        switch (type) {
            case 1: kind="txid"; break; case 2: kind="blockhash"; break; case 3: kind="filtered-blockhash"; break;
            case 4: kind="compact-blockhash"; break; case 5: kind="wtxid"; break; default: kind="unknown";
        }
        objects.push_back({{"kind",kind},{"hash",reversed_hex(payload.substr(start+4,32))}});
    }
    return {objects,count>limit};
}
CaptureReader::CaptureReader(Store& store, Model& model, std::filesystem::path directory, std::size_t budget)
    : s_(store), model_(model), directory_(std::move(directory)), budget_(budget) {
    if (!std::filesystem::is_directory(directory_) || !budget_) throw std::runtime_error("--capture-dir must be an existing directory; budget must be positive");
}
void CaptureReader::partial(const std::string& key, const Json& previous, const std::filesystem::path& path, const std::string& cause) {
    if (!previous.value("partial",false)) s_.event("observer.source-loss","node:observer",cause,{{"file",path.string()}});
    auto data = previous; data["partial"]=true; s_.put(key,data);
}
std::size_t CaptureReader::poll() {
    std::vector<std::filesystem::path> paths;
    for (const auto& peer : std::filesystem::directory_iterator(directory_)) {
        if (!peer.is_directory()) continue;
        for (const auto& file : std::filesystem::directory_iterator(peer)) {
            const auto name = file.path().filename().string();
            if ((name == "msgs_recv.dat" || name == "msgs_sent.dat") && file.is_regular_file()) paths.push_back(file.path());
        }
    }
    std::sort(paths.begin(),paths.end());
    if (paths.empty()) return 0;
    const auto start = position_%paths.size();
    std::size_t remaining = budget_;
    for (std::size_t i = 0; i < paths.size() && remaining; ++i) {
        const auto& path = paths[(start+i)%paths.size()];
        position_=(start+i+1)%paths.size();
        platform::CaptureFile file(path);
        if (!file.valid()) continue;
        auto stat=file.info();
        const auto key="capture:"+hash256(platform::utf8_path(std::filesystem::canonical(path)));
        auto previous=s_.get(key,Json::object());
        auto offset=previous.value("offset",std::uint64_t(0));
        const Json identity=Json::array({stat.device,stat.inode});
        Transaction tx(s_);
        if (!previous.empty() && (previous.value("identity",Json(nullptr)) != identity || stat.size < offset)) {
            s_.event("observer.source-loss","node:observer","capture-rotation-or-truncation",{{"file",path.string()}});
            offset=0; previous=Json::object();
        }
        previous["offset"]=offset; previous["identity"]=identity;
        while (remaining) {
            auto header=file.read_at(offset,24);
            if (header.empty()) break;
            if (header.size()!=24) { partial(key,previous,path); break; }
            const auto timestamp=le(header,0,8), size=le(header,20,4);
            if (size>4000000) { partial(key,previous,path,"oversize-or-invalid-capture"); break; }
            stat=file.info();
            if (stat.size<offset || stat.size-offset < 24+size) { partial(key,previous,path); break; }
            auto command=header.substr(8,12);
            if (auto zero=command.find('\0'); zero!=std::string::npos) command.resize(zero);
            Json objects=nullptr;
            if (command=="inv" || command=="getdata" || command=="notfound") {
                try {
                    auto parsed=inventory(file.read_at(offset+24,static_cast<std::size_t>(size)));
                    objects=std::move(parsed.first);
                    if (parsed.second) s_.event("observer.coverage","node:observer","inventory-link-sampling",{{"file",path.string()},{"limit",1000}});
                } catch (const std::exception& e) { s_.event("observer.source-loss","node:observer","incomplete-inventory",{{"error",e.what()},{"file",path.string()}}); }
            }
            if (timestamp>std::uint64_t(std::numeric_limits<std::int64_t>::max()/1000)) {
                partial(key,previous,path,"invalid-capture-timestamp"); break;
            }
            const auto peer="capture-"+text(s_.get("node_run"))+"-"+component(platform::utf8_path(path.parent_path().filename()))+"-"+std::to_string(stat.inode);
            model_.message(peer,path.filename()=="msgs_recv.dat" ? "incoming" : "outgoing",command,static_cast<std::int64_t>(size),
                           "message-capture",timestamp*1000,objects);
            offset+=24+size; --remaining;
            previous={{"offset",offset},{"identity",identity}}; s_.put(key,previous);
        }
        tx.commit();
    }
    return budget_-remaining;
}
} // namespace bitcoin_viz
