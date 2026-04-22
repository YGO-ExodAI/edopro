#ifndef SINGLE_MODE_H
#define SINGLE_MODE_H

#include "epro_thread.h"
#include "dllinterface.h"
#include "replay.h"
#include "mysignal.h"
#include "core_utils.h"
#include "text_types.h"

namespace ygo {

class SingleMode {
private:
	static OCG_Duel pduel;
	static bool is_closing;
	static bool is_continuing;
	static bool is_restarting;
	static epro::thread single_mode_thread;

public:
	struct DuelOptions {
		uint32_t startingLP{ 8000 };
		uint32_t startingDrawCount{ 5 };
		uint32_t drawCountPerTurn{ 1 };
		uint64_t duelFlags{ 0 };
		bool handTestNoOpponent{ true };
		std::string scriptName;
		DuelOptions() {};
		explicit DuelOptions(epro::stringview filename) : scriptName(filename.data(), filename.size()) {};
	};

	static bool StartPlay(DuelOptions&& duelOptions);
	static void StopPlay(bool is_exiting = false);
	static void Restart();
	static void SetResponse(void* resp, size_t len);

	// ExodAI Phase P1 Primitive 1, Chunk 8: in-duel state save hotkey.
	// Calls OCG_DuelSaveState on the current single-mode duel and writes
	// the blob to <positions_dir>/<basename>.bin plus a minimal JSON
	// sidecar at <positions_dir>/<basename>.bin.json. Returns true on
	// success, false on no-active-duel / refuse / I/O error (msg
	// describes which).
	//
	// positions_dir defaults to "./replays/positions/" (relative to the
	// EDOPro working dir); EXODAI_POSITIONS_DIR env var overrides.
	// Caller (event_handler.cpp) surfaces the message via the in-game
	// log tab.
	static bool SaveStateToFile(std::string& out_msg);
	static int SinglePlayThread(DuelOptions&& duelOptions);
	static bool SinglePlayAnalyze(CoreUtils::Packet& packet);
	
	static void SinglePlayRefresh(uint8_t player, uint8_t location, uint32_t flag = 0x2f81fff);
	static void SinglePlayRefresh(uint32_t flag = 0x2f81fff);
	static void SinglePlayRefreshSingle(uint8_t player, uint8_t location, uint8_t sequence, uint32_t flag = 0x2f81fff);
	static void SinglePlayReload();
	static Signal singleSignal;

protected:
	static Replay last_replay;
	static Replay new_replay;
	static ReplayStream replay_stream;
};

}

#endif //SINGLE_MODE_H
