#ifndef COMMON_H
#define COMMON_H
#include "ocgapi_constants.h"

#define OLD_REPLAY_MODE          231
// ExodAI extension (see network.h::CTOS_AI_THOUGHT). Written into the yrpX
// stream on each top-level bot decision; payload is uint16 length + UTF-8 JSON.
#define MSG_AI_THOUGHT           232
// ExodAI extension. Written into the yrpX stream by the server at every
// engine-emitted MSG_SELECT_*, before refreshes flush. Payload: 2 bytes
// [player_byte, select_msg_type]. Marks the exact location of every decision
// point by either player, regardless of whether the bot provides a thought.
// For bot decisions this is immediately followed by MSG_AI_THOUGHT.
#define MSG_DECISION_POINT       233

#endif //COMMON_H
