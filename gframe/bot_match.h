#ifndef BOT_MATCH_H
#define BOT_MATCH_H

// ExodAI headless bot-vs-bot eval harness.
//
// When EDOPro is launched with `--bot-match <config.json>`, it skips the
// Irrlicht/menu path entirely and just runs NetServer on the configured
// port. Two WindBot clients (spawned by src/eval_harness.py in the ExodAI
// repo) connect, create a game, play a duel, and the server auto-writes a
// yrpX replay with MSG_AI_THOUGHT telemetry to ./replay/. When the duel
// ends (generic_duel calls NetServer::StopServer), this process exits and
// the Python harness picks up the replay.
//
// This path needs DataHandler initialized (for cards/scripts) but NOT
// Game/Irrlicht — branching into bot_match happens between those two
// points in gframe.cpp.

#include <cstdint>
#include <optional>

#include "text_types.h"

namespace ygo {

struct BotMatchConfig {
    uint16_t port = 7999;
    int timeout_sec = 600;  // hard cap — if a duel somehow hangs, exit anyway
    // If set, reseeds Utils::generator before the server starts so deck
    // shuffles + the seed fed into ocgcore are reproducible. Used by the
    // ExodAI eval harness to compare model checkpoints on identical games.
    std::optional<uint64_t> seed;
};

class BotMatch {
public:
    // Parse the JSON config at config_path. Returns false on any error.
    static bool LoadConfig(epro::path_stringview config_path, BotMatchConfig& out);

    // Blocking entry point. Starts NetServer on cfg.port, waits for it to
    // stop (duel end) or for the timeout to fire. Returns the process exit
    // code (0 on clean duel completion, non-zero on timeout/error).
    static int Run(const BotMatchConfig& cfg);
};

}  // namespace ygo

#endif  // BOT_MATCH_H
