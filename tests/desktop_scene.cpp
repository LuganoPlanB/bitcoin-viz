// SPDX-License-Identifier: GPL-3.0-or-later
#include "bitcoin_viz.hpp"
#include "platform.hpp"
#include "scene.hpp"
#include "Gource/src/formats/commitlog.h"
#include "Gource/src/gource_settings.h"
#include "Logstalgia/src/custom.h"
#include "Logstalgia/src/settings.h"
#include <array>
#include <iostream>

namespace {
using namespace bitcoin_viz;
using namespace studio;
namespace fs = std::filesystem;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F operation, const std::string& key) {
    try {
        operation();
    } catch (const std::exception& error) {
        check(std::string(error.what()).find(key) != std::string::npos,
            "Expected an error about " + key + ", received " + error.what());
        return;
    }
    throw std::runtime_error("Accepted invalid " + key);
}

struct Fixture {
    const fs::path directory = fs::temp_directory_path()/("viz-scene-"+session_id());
    const fs::path path = directory/"journal.sqlite";
    ~Fixture() { std::error_code error; fs::remove_all(directory,error); }

    void seed() const {
        Store store(path);
        Transaction transaction(store);
        const auto event = [&](const std::string& kind, const Json& values) {
            const auto id = store.event(kind,"fixture-node","fixture",values);
            store.exec("UPDATE events SET observed_ns=? WHERE id=?",{1700000000000000000LL,id});
            return id;
        };
        const auto traffic = event("peer.message.incoming",{{"command","headers"},{"fidelity","recorded"},{"size_bytes",517}});
        const auto admitted = event("mempool.add",{{"metadata",{{"band","5-10"}}}});
        const auto block = event("chain.connect",{{"height",812345},{"size_bytes",1200}});
        const auto coin = event("utxo.create",{{"coin",{{"script","0014"+std::string(40,'e')}}}});
        const auto health = event("observer.resync",Json::object());
        store.leaf("state","inventory/fee-a","fixture-node","E8A94A",Json::object(),admitted);
        store.leaf("state","inventory/fee-b","fixture-node","E8A94A",Json::object(),block);
        store.leaf("state","inventory/fee-c","fixture-node","E8A94A",Json::object(),health);
        store.leaf("protocol","messages/headers","fixture-node","6FC5D2",Json::object(),traffic);
        store.leaf("protocol","messages/ping","fixture-node","6FC5D2",Json::object(),traffic);
        store.leaf("utxo","cohort/observed/p2wpkh","fixture-node","68B89B",Json::object(),coin);
        transaction.commit();
    }
};

Lines records(const fs::path& path, bool logstalgia, const char* mode) {
    Lines lines;
    const auto emit = [&](const std::string& line) { lines.emplace_back(line,0,line.find_last_not_of("\r\n")+1); };
    if (logstalgia) LogstalgiaRenderer(path,mode,emit,0).step();
    else Renderer(path,mode,emit).snapshot();
    return lines;
}

void projections(const fs::path& path) {
    const std::vector<std::vector<std::string>> trees{
        {"/inventory/fee-a","/inventory/fee-b","/inventory/fee-c"},
        {"/messages/headers","/messages/ping"},
        {"/cohort/observed/p2wpkh"},
    };
    for (std::size_t index = 0; index < std::size(gource_modes); ++index) {
        const auto mode = gource_modes[index];
        auto lines = records(path,false,mode);
        const auto parser = gource_parser(lines);
        std::vector<std::string> files;
        for (RCommit commit; parser->nextCommit(commit);) {
            for (const auto& file : commit.files) {
                check(file.action == "A" && commit.username == "node:snapshot","Invalid native Gource snapshot");
                files.push_back(file.filename);
            }
        }
        check(files == trees.at(index) && lines.empty(),std::string("Unexpected parsed files for Gource / ") + mode);
        std::cout << "Gource / " << mode << ": " << files.size() << " parsed files\n";
    }

    const std::vector<std::string> paths{"/traffic/recorded/incoming/headers","/mempool/admitted/5-10",
        "/chain/connected/812345","/utxo/create/p2wpkh","/observer/resync/fixture"};
    const std::vector<std::string> codes{"RX","ADMIT","BLOCK","CREATE","RESYNC"};
    const std::vector<std::string> paddles{"incoming","mempool","chain","utxo","observer"};
    for (std::size_t index = 0; index < std::size(logstalgia_modes); ++index) {
        const auto mode = logstalgia_modes[index];
        auto lines = records(path,true,mode);
        CustomAccessLog parser;
        std::vector<std::string> received;
        for (auto& record : lines) {
            LogEntry entry;
            check(parser.parseLine(record,entry),"Native Logstalgia parser rejected projected record");
            const auto match = std::find(paths.begin(),paths.end(),entry.path);
            check(match != paths.end(),"Logstalgia parser produced an unexpected path");
            const auto id = std::distance(paths.begin(),match);
            check(entry.response_code == codes[id] && entry.pid == paddles[id] && entry.vhost == "bitcoin-viz" &&
                entry.timestamp == 1700000000,"Logstalgia parser lost event metadata");
            received.push_back(entry.path);
        }
        const auto expected = index == paths.size()-1 ? paths : std::vector<std::string>{paths[index],paths.back()};
        check(received == expected,std::string("Unexpected parsed events for Logstalgia / ") + mode);
        std::cout << "Logstalgia / " << mode << ": " << received.size() << " parsed events\n";
    }
}

void validation() {
    for (const auto* mode : {"overview","track"}) {
        const auto appearance = validate_gource_parameters({{"camera-mode",mode},{"time-scale","2.5"},
            {"background-colour","234567"},{"file-filter","fee-a\n\nfee-b\r\n"}});
        check(appearance.camera == mode && appearance.speed == 2.5f &&
            std::lround(appearance.background.g*255) == 0x45,"Gource appearance did not import native settings");
    }
    gStringHashSeed = 71;
    gGourceSettings.time_scale = 1.75f;
    rejects([] { validate_gource_parameters({{"hash-seed","5"},{"file-filter","good\n["}}); },"file-filter");
    check(gStringHashSeed == 71 && gGourceSettings.time_scale == 1.75f,"Gource invalid draft touched live settings");
    rejects([] { validate_gource_parameters({{"time-scale","2fast"}}); },"time-scale");
    rejects([] { validate_gource_parameters({{"font-size","18.5"}}); },"font-size");
    rejects([] { validate_gource_parameters({{"background-colour","23F4z7"}}); },"background-colour");
    rejects([] { validate_gource_parameters({{"path","-"}}); },"path");
    check(gource_parameters().at("user-idle-time") == "float","Gource registry omitted user-idle-time");

    const auto appearance = validate_logstalgia_parameters({{"background","9A5514"},
        {"simulation-speed","3.5"},{"group","Mempool,URI=^/mempool,50\nBlocks,URI=^/chain,50"}});
    check(appearance.speed == 3.5f && std::lround(appearance.background.b*255) == 0x14,"Logstalgia appearance failed");
    settings.simulation_speed = 1.5f;
    rejects([] { validate_logstalgia_parameters({{"simulation-speed","nan"}}); },"simulation-speed");
    rejects([] { validate_logstalgia_parameters({{"simulation-speed","31"}}); },"simulation speed");
    rejects([] { validate_logstalgia_parameters({{"group","Good,URI=^/,50\nBad,URI=[,50"}}); },"invalid regular expression");
    rejects([] { validate_logstalgia_parameters({{"group","Invalid,URI=^/,MAX=4oops,100"}}); },"invalid numeric field");
    rejects([] { validate_logstalgia_parameters({{"group","Invalid,URI=^/,100,Q12345"}}); },"invalid colour");
    check(settings.simulation_speed == 1.5f,"Logstalgia invalid draft touched live settings");
    check(logstalgia_parameters().count("path-separators") && !logstalgia_parameters().count("group-separators"),
        "Logstalgia catalog does not match the native setting consumed by groups");
}

struct DisplayScope {
    ~DisplayScope() { display.quit(); SDL_Quit(); }
};
struct TestScene {
    Scene scene;
    TestScene(const fs::path& vendor, bool is_logstalgia, Lines& lines, const Parameters& options) {
        const auto path = platform::utf8_path(vendor/(is_logstalgia ? "Logstalgia" : "Gource")/"data") + "/";
        gSDLAppResourceDir = path;
        texturemanager.setDir(path);
        fontmanager.setDir(path+"fonts/");
        shadermanager.setDir(path+"shaders/");
        scene = is_logstalgia ? logstalgia_scene(lines,options) : gource_scene(lines,options);
        scene.app->init();
    }
    ~TestScene() {
        scene.app.reset();
        scene.parser.reset();
        fontmanager.unload();
        fontmanager.purge();
        shadermanager.purge();
        texturemanager.purge();
    }
    void update(int frames) {
        for (int frame = 0; frame < frames; ++frame) scene.app->update(frame/60.0f,1.0f/60);
    }
};

using Colour = std::array<unsigned char,3>;
using Pixels = std::vector<Colour>;
Pixels pixels() {
    Pixels result(static_cast<std::size_t>(display.width)*display.height);
    glPixelStorei(GL_PACK_ALIGNMENT,1);
    glReadPixels(0,0,display.width,display.height,GL_RGB,GL_UNSIGNED_BYTE,result.data());
    check(glGetError() == GL_NO_ERROR,"Could not read renderer pixels");
    return result;
}
std::size_t changed_pixels(const Pixels& before, const Pixels& after) {
    std::size_t changed = 0;
    for (std::size_t i=0; i<before.size(); ++i) changed += before[i] != after[i];
    return changed;
}

void render(const fs::path& database, const fs::path& vendor) {
    SDL_SetMainReady();
    const DisplayScope scope;
    SDLAppInit("Bitcoin Viz scene test","bitcoin-viz-scene-test");
    display.enableVsync(false);
    display.init("Bitcoin Viz scene test",640,400,false);
    std::cout << "OpenGL: " << glGetString(GL_RENDERER) << '\n';

    auto frame = [&](bool is_logstalgia, Lines lines) {
        std::srand(42);
        const Parameters options = is_logstalgia ? Parameters{{"pitch-speed","0.1"},{"disable-glow","true"}}
            : Parameters{{"hide-date","true"},{"hide-mouse","true"}};
        TestScene engine(vendor,is_logstalgia,lines,options);
        engine.update(90);
        return pixels();
    };
    for (bool is_logstalgia : {false,true}) {
        const auto empty = frame(is_logstalgia,{});
        const auto modes = is_logstalgia ? logstalgia_modes : gource_modes;
        const auto size = is_logstalgia ? std::size(logstalgia_modes) : std::size(gource_modes);
        std::vector<Pixels> frames;
        for (std::size_t index=0; index<size; ++index) {
            const auto mode = modes[index];
            auto captured = frame(is_logstalgia,records(database,is_logstalgia,mode));
            const auto changed = changed_pixels(empty,captured);
            const auto name = std::string(is_logstalgia ? "Logstalgia / " : "Gource / ") + mode;
            check(changed > 100,name + " did not render projected data");
            for (const auto& previous : frames)
                check(changed_pixels(previous,captured) > 100,name + " matched another projection");
            std::cout << name << ": " << changed << " pixels changed from empty scene\n";
            frames.push_back(std::move(captured));
        }
    }

    {
        auto lines = records(database,false,"state");
        TestScene gource(vendor,false,lines,{{"file-filter","fee-a\nfee-b"}});
        const auto all_filters = gGourceSettings.file_filters;
        check(all_filters.size()==2 && all_filters[0]->match("fee-a") && all_filters[1]->match("fee-b"),
            "Gource lost repeatable file filters");
        for (const auto* mode : {"track","overview"}) {
            const Parameters changes{{"camera-mode",mode},{"time-scale","2.5"},{"background-colour","234567"}};
            apply_gource_appearance(*gource.scene.app,validate_gource_parameters(changes),changes);
            check(gGourceSettings.camera_mode == mode && gGourceSettings.time_scale == 2.5f,
                "Camera or speed was not applied in place");
        }
        gource.update(90);
        check(pixels().back() == Colour{0x23,0x45,0x67},"Gource did not draw applied background");
        rejects([] { validate_gource_parameters({{"background-colour","wrong"}}); },"background-colour");
        gource.update(1);
        check(pixels().back() == Colour{0x23,0x45,0x67},"Invalid draft changed the working Gource background");
        const Parameters recovery{{"background-colour","316875"}};
        apply_gource_appearance(*gource.scene.app,validate_gource_parameters(recovery),recovery);
        gource.update(1);
        check(pixels().back() == Colour{0x31,0x68,0x75},"Gource did not apply a corrected draft");
        std::cout << "Gource cameras / track and overview: applied; invalid draft retained current scene\n";
    }

    const std::array paddle_options{
        std::pair{"single",PADDLE_SINGLE}, std::pair{"pid",PADDLE_PID},
        std::pair{"vhost",PADDLE_VHOST}, std::pair{"hidden",PADDLE_NONE},
    };
    std::vector<Pixels> paddles;
    for (const auto& [mode,expected] : paddle_options) {
        std::srand(42);
        auto lines = records(database,true,"all");
        TestScene logstalgia(vendor,true,lines,{{"paddle-mode",expected == PADDLE_NONE ? "pid" : mode},
            {"hide-paddle",expected == PADDLE_NONE ? "true" : ""},
            {"path-separators","/"},{"path-max-depth","4"},
            {"group","Traffic,URI=^/traffic,45\nOther,URI=^/,55"}});
        check(settings.paddle_mode == expected,std::string("Wrong native paddle mode for ") + mode);
        check(settings.groups.size() == 2 && settings.groups[0].title == "Traffic" &&
            settings.groups[1].title == "Other" && settings.groups[1].max_depth == 4,
            "Logstalgia lost grouped settings");
        const Parameters changes{{"simulation-speed","3.5"},{"background","9A5514"}};
        apply_logstalgia_appearance(validate_logstalgia_parameters(changes),changes);
        check(settings.simulation_speed == 3.5f,"Logstalgia speed was not applied in place");
        logstalgia.update(90);
        auto captured = pixels();
        check(captured.back() == Colour{0x9a,0x55,0x14},"Logstalgia did not draw applied background");
        for (const auto& previous : paddles)
            check(changed_pixels(previous,captured) > 100,std::string("No visible difference in paddle mode ") + mode);
        paddles.push_back(std::move(captured));
        std::cout << "Logstalgia paddles / " << mode << ": rendered\n";
    }
}
}

int main(int argc, char** argv) {
    try {
        platform::initialize_process();
        check(argc >= 2,"Expected a desktop scene test name");
        const std::string test = argv[1];
        if (test == "projections") {
            Fixture fixture;
            fixture.seed();
            projections(fixture.path);
        } else if (test == "validation") {
            validation();
        } else if (test == "render") {
            check(argc == 3,"Expected vendor asset directory");
            Fixture fixture;
            fixture.seed();
            render(fixture.path,fs::u8path(argv[2]));
        } else {
            throw std::runtime_error("Unknown desktop scene test");
        }
    } catch (const std::exception& error) {
        std::cerr << "Desktop scene: " << error.what() << '\n';
        return 1;
    }
}
