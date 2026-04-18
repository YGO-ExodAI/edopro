#include "bot_match.h"

#include <chrono>
#include <fstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "fmt.h"
#include "game.h"
#include "netserver.h"
#include "utils.h"

#include <cstdint>

namespace ygo {

namespace {
constexpr auto kReadyLogLine = "[BOT_MATCH] server ready";
constexpr auto kDoneLogLine  = "[BOT_MATCH] server stopped";
constexpr auto kTimeoutLogLine = "[BOT_MATCH] timeout exceeded, exiting";
}  // namespace

bool BotMatch::LoadConfig(epro::path_stringview config_path, BotMatchConfig& out) {
    std::ifstream f(Utils::ToUTF8IfNeeded(config_path));
    if(!f.is_open()) {
        epro::print("[BOT_MATCH] failed to open config: {}\n",
                    Utils::ToUTF8IfNeeded(config_path));
        return false;
    }
    try {
        nlohmann::json j;
        f >> j;
        if(j.contains("port"))         out.port        = j.at("port").get<uint16_t>();
        if(j.contains("timeout_sec"))  out.timeout_sec = j.at("timeout_sec").get<int>();
        if(j.contains("seed") && !j.at("seed").is_null())
            out.seed = j.at("seed").get<uint64_t>();
    } catch(const std::exception& e) {
        epro::print("[BOT_MATCH] json parse error: {}\n", e.what());
        return false;
    }
    if(out.port == 0) {
        epro::print("[BOT_MATCH] invalid port=0\n");
        return false;
    }
    return true;
}

int BotMatch::Run(const BotMatchConfig& cfg) {
    epro::print("[BOT_MATCH] starting NetServer on port {} (timeout={}s)\n",
                cfg.port, cfg.timeout_sec);
    if(cfg.seed) {
        Utils::SeedRandomNumberGenerator(*cfg.seed);
        epro::print("[BOT_MATCH] deterministic seed={} — deck shuffle + ocgcore RNG are reproducible\n",
                    *cfg.seed);
    }
    // Make log output visible to the Python harness immediately — EDOPro's
    // normal stdout path may be line-buffered.
    std::fflush(stdout);

    // GenericDuel::Process eventually calls `mainGame->SetupDuel(...)` +
    // `mainGame->LoadScript(...)` to load card scripts into ocgcore. We can't
    // skip this, but we DON'T need Irrlicht — only the script directory lists.
    // Construct a bare Game object, set mainGame, and populate resource dirs.
    // Do NOT call Game::Initialize() (that creates the Irrlicht device + GUI).
    Game bare_game;
    mainGame = &bare_game;
    mainGame->PopulateResourcesDirectories();
    epro::print("[BOT_MATCH] bare Game initialized (no Irrlicht)\n");
    std::fflush(stdout);

    if(!NetServer::StartServer(cfg.port)) {
        epro::print("[BOT_MATCH] NetServer::StartServer failed (port in use?)\n");
        return 2;
    }

    // StartServer spawned the detached ServerThread. We just have to wait
    // for it to finish. ServerThread clears net_evbase on exit; IsRunning
    // reflects that. generic_duel.cpp calls NetServer::StopServer() when
    // the duel completes, which causes event_base_dispatch to return.
    epro::print("{}\n", kReadyLogLine);
    std::fflush(stdout);

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(cfg.timeout_sec);
    while(NetServer::IsRunning()) {
        if(std::chrono::steady_clock::now() >= deadline) {
            epro::print("{}\n", kTimeoutLogLine);
            std::fflush(stdout);
            std::fflush(stderr);
            // Force-exit the whole process. NetServer::StopServer() alone
            // isn't enough — the detached ServerThread + ocgcore script VM
            // don't unwind cleanly (and even if they did, the Game dtor
            // hits null Irrlicht state). yrpX is already flushed to disk
            // incrementally by generic_duel, so we can't lose the replay.
            std::_Exit(3);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    epro::print("{}\n", kDoneLogLine);
    std::fflush(stdout);
    // Bypass destructor chain — the DataHandler shutdown and Game dtor were
    // hanging / access-violating because we never ran Game::Initialize and
    // downstream cleanup hits null Irrlicht state. We don't need a clean
    // shutdown here; the yrpX has already been written to disk by this
    // point (generic_duel flushes on duel end). Just exit fast so the
    // Python harness unblocks.
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(0);
    return 0;
}

}  // namespace ygo
