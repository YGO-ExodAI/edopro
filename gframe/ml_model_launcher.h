#ifndef ML_MODEL_LAUNCHER_H
#define ML_MODEL_LAUNCHER_H

#include <string>

namespace ygo {

struct MLModelLaunchResult {
	bool ok = false;
	bool cancelled = false;  // true when user dismissed the file-picker
	std::wstring modelName;
	std::wstring errorMessage;
};

// Spawn the ExodAI inference server (serve_model.py) and the deploy
// WindBot.exe so the bot connects to the locally-hosted room at `port`
// using room password `pass`.
MLModelLaunchResult LaunchMLModelBot(int port, const std::wstring& pass);

// Terminate any serve_model.py / WindBot processes spawned via
// LaunchMLModelBot. Safe to call when nothing was launched (no-op).
// Called on leave-game / surrender; the underlying Job Object also
// kills the children automatically when EDOPro exits or crashes.
void ShutdownMLModelBot();

}

#endif
