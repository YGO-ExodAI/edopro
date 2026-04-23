#ifndef EXODAI_SAVE_STATE_H
#define EXODAI_SAVE_STATE_H

// ExodAI Phase P1 Primitive 1 — shared save-state helper.
// Shared between single-mode's Ctrl+S hotkey (SingleMode::SaveStateToFile)
// and LAN-host mode's Ctrl+S hotkey (event_handler.cpp dispatch against
// NetServer::GetHostDuelHandle()). Writes the OCG save blob to
// <positions_dir>/<basename>.bin and a minimal JSON sidecar to
// <blob_path>.json. The sidecar format mirrors src/state_io.py's schema
// (schema_version 1); callers pass a source_tag that lands in both
// provenance.source and the tags list so downstream mining can tell
// apart "single-mode hotkey" vs "lan+ai hotkey" saves.

#include <string>
#include "dllinterface.h"

namespace ygo {

// Writes the current engine state of `pduel` to a {.bin, .bin.json} pair
// under EXODAI_POSITIONS_DIR (default "./replays/positions/"). Returns
// true on success; on failure, fills out_msg with a human-readable
// reason (caller surfaces via AddLog). `source_tag` is free-form — use
// "edopro-hotkey-single" for SingleMode and "edopro-hotkey-lan" for
// GenericDuel LAN-host saves.
bool ExodAIWriteDuelStateToFile(OCG_Duel pduel, const char* source_tag, std::string& out_msg);

}

#endif // EXODAI_SAVE_STATE_H
