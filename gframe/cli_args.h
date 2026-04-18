#ifndef CLI_ARGS_H
#define CLI_ARGS_H

#include <array>
#include "text_types.h"

enum LAUNCH_PARAM {
	WORK_DIR,
	MUTE,
	CHANGELOG,
	DISCORD,
	OVERRIDE_UPDATE_URL,
	WANTS_TO_RUN_AS_ADMIN,
	REPOS_READ_ONLY,
	ONLY_CLONE_REPOS,
	USER_STORAGE_DIRECTORY,
	// ExodAI: headless bot-vs-bot evaluation mode. Argument is a path to a JSON
	// config file with server port, seed, deck names, replay output path.
	// See src/eval_harness.py in the ExodAI repo and gframe/bot_match.cpp.
	BOT_MATCH,
	COUNT,
};


struct Option {
	bool enabled{ false };
	epro::path_stringview argument;
};

using args_t = std::array<Option, LAUNCH_PARAM::COUNT>;

extern args_t cli_args;

#endif //CLI_ARGS_H
