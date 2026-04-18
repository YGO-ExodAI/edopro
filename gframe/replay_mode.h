#ifndef REPLAY_MODE_H
#define REPLAY_MODE_H

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "epro_thread.h"
#include "dllinterface.h"
#include "config.h"
#include "data_manager.h"
#include "deck_manager.h"
#include "replay.h"
#include "core_utils.h"
#include "text_types.h"
#include "RNG/mt19937.h"

namespace ygo {

class ReplayMode {
private:
	static OCG_Duel pduel;
	static bool yrp;
	static bool is_continuing;
	static bool is_closing;
	static bool is_pausing;
	static bool is_paused;
	static bool is_swapping;
	static bool is_restarting;
	static bool undo;
	static bool exit_pending;
	static int skip_turn;
	static int current_step;
	static int skip_step;
	static epro::thread replay_thread;

	// ExodAI thoughts. In Path A, each top-level bot decision emits a
	// MSG_AI_THOUGHT packet embedded in the yrpX stream. On replay, we parse
	// it and display the contents in the Thoughts tab — perfectly pinned to
	// the decision point because the packet was written right before the
	// state-changes from that response.
	static int current_turn;          // from MSG_NEW_TURN (display context only)
	static int current_decision_idx;  // pauseable-step count within turn (display context only)
	// When true, normal per-pauseable pauses are suppressed and the thread
	// runs until it hits a decision marker for the current POV seat. Flag
	// auto-clears on the next qualifying pause.
	static bool stop_at_decision_point;
	// Same mechanism for MSG_NEW_TURN. Two-stage: stop_at_turn_boundary is
	// cleared when MSG_NEW_TURN is seen, which arms armed_post_turn_boundary
	// so we actually pause on the NEXT pauseable step. Without this, the
	// pause lands on MSG_NEW_TURN itself where the board hasn't yet rendered
	// the new turn's state.
	static bool stop_at_turn_boundary;
	static bool armed_post_turn_boundary;
	// Set by ReplayThread before each ReplayAnalyze call so the pauseable
	// decision for MSG_DECISION_POINT can look ahead: if the next packet is
	// MSG_AI_THOUGHT we skip the pause here and let the thought be the
	// pause point.
	static bool peek_next_is_ai_thought;
	// Set by PrevDecisionPoint / Undo before calling Restart so the
	// is_restarting handler in ReplayThread lands on a specific step. -1
	// means "default to current_step - 1" (the normal Undo behavior).
	static int jump_target_step;
	static void ResetThoughtsState();
	static void HandleAiThoughtPacket(const CoreUtils::Packet& p);
	static void HandleDecisionPointPacket(const CoreUtils::Packet& p);
	// Returns the bottom-of-screen seat index (POV). Reads from
	// dInfo.isReplaySwapped so SwapField updates it automatically.
	static int GetPovSeat();
	// Walks packets_stream, replicating the pauseable rules, and returns
	// indices of interesting pauseable steps. Used by the Prev Decision /
	// Prev Turn jump buttons to locate their targets.
	struct ScanResults {
		std::vector<int> pov_decision_steps;  // pov has a real decision here
		std::vector<int> turn_boundary_steps; // MSG_NEW_TURN lands here
	};
	static ScanResults ScanPacketsStream();
	static int GetAiThoughtPlayer(const CoreUtils::Packet& p);
	// Seats known to run ExodAI (emit MSG_AI_THOUGHT at least once in this
	// replay). Computed once at replay start by scanning the full stream.
	// A MSG_DECISION_POINT for a known bot seat that isn't immediately
	// followed by MSG_AI_THOUGHT means the framework auto-resolved it (e.g.
	// chain-with-nothing-chainable, zone-pick, default sort) — the model was
	// never invoked and the viewer treats it as a non-decision for navigation.
	static std::set<int> known_bot_seats;
	static void ScanForKnownBotSeats();

public:
	static Replay cur_replay;
	static Replay* cur_yrp;

public:
	static bool StartReplay(int skipturn, bool is_yrp);
	static void StopReplay(bool is_exiting = false);
	static void SwapField();
	static void Pause(bool is_pause, bool is_step);
	static void StepToNextDecisionPoint();
	static void StepToPrevDecisionPoint();
	static void StepToNextTurn();
	static void StepToPrevTurn();
	static void JumpToStart();
	static bool ReadReplayResponse();
	static int ReplayThread();
	static int OldReplayThread();
	static bool StartDuel();
	static void EndDuel();
	static void Restart(bool refresh);
	static void Undo();
	static bool ReplayAnalyze(const CoreUtils::Packet& packet);
	static bool OldReplayAnalyze(const CoreUtils::Packet& packet);

	static void ReplayRefresh(uint8_t player, uint8_t location, uint32_t flag = 0x2f81fff);
	static void ReplayRefresh(uint32_t flag = 0x2f81fff);
	static void ReplayRefreshSingle(uint8_t player, uint8_t location, uint32_t sequence, uint32_t flag = 0x2f81fff);
	static void ReplayReload();
};

}

#endif //REPLAY_MODE_H
