#include <IrrlichtDevice.h>
#include <IGUIWindow.h>
#include <IGUIStaticText.h>
#include <algorithm>
#include <nlohmann/json.hpp>
#include "replay_mode.h"
#include "duelclient.h"
#include "game.h"
#include "single_mode.h"
#include "sound_manager.h"
#include "file_stream.h"
#include "bufferio.h"
#include "logging.h"

namespace ygo {

void* ReplayMode::pduel = 0;
bool ReplayMode::yrp = false;
Replay ReplayMode::cur_replay{};
Replay* ReplayMode::cur_yrp = nullptr;
bool ReplayMode::is_continuing = true;
bool ReplayMode::is_closing = false;
bool ReplayMode::is_pausing = false;
bool ReplayMode::is_paused = false;
bool ReplayMode::is_swapping = false;
bool ReplayMode::is_restarting = false;
bool ReplayMode::exit_pending = false;
int ReplayMode::skip_turn = 0;
int ReplayMode::current_step = 0;
int ReplayMode::skip_step = 0;
epro::thread ReplayMode::replay_thread;

int ReplayMode::current_turn = 0;
int ReplayMode::current_decision_idx = 0;
bool ReplayMode::stop_at_decision_point = false;
bool ReplayMode::stop_at_turn_boundary = false;
bool ReplayMode::armed_post_turn_boundary = false;
bool ReplayMode::peek_next_is_ai_thought = false;
int ReplayMode::jump_target_step = -1;
std::set<int> ReplayMode::known_bot_seats;

void ReplayMode::ScanForKnownBotSeats() {
	known_bot_seats.clear();
	for(const auto& p : cur_replay.packets_stream) {
		if(p.message != MSG_AI_THOUGHT)
			continue;
		int player = GetAiThoughtPlayer(p);
		if(player >= 0)
			known_bot_seats.insert(player);
	}
}

int ReplayMode::GetPovSeat() {
	// The bottom-of-screen player. Starts at seat 0, flips each SwapField.
	return mainGame && mainGame->dInfo.isReplaySwapped ? 1 : 0;
}

int ReplayMode::GetAiThoughtPlayer(const CoreUtils::Packet& p) {
	// Payload: uint16 length + UTF-8 JSON. JSON must contain {"player": int}.
	if(p.buff_size() < 2)
		return -1;
	const auto* pbuf = p.data();
	uint16_t len = BufferIO::Read<uint16_t>(pbuf);
	if(len + sizeof(uint16_t) > p.buff_size())
		return -1;
	try {
		auto j = nlohmann::json::parse(std::string(reinterpret_cast<const char*>(pbuf), len));
		return j.value("player", -1);
	} catch(const std::exception&) {
		return -1;
	}
}

void ReplayMode::ResetThoughtsState() {
	current_turn = 0;
	current_decision_idx = 0;
	if(mainGame && mainGame->stThoughts)
		mainGame->stThoughts->setText(L"");
}

static const wchar_t* SelectMsgTypeName(uint8_t msg) {
	// Names the MSG_SELECT_* code stored in MSG_DECISION_POINT's payload
	// byte 1. Covers every variant emitted by generic_duel.cpp Sending.
	switch(msg) {
	case MSG_SELECT_BATTLECMD:      return L"SelectBattleCmd";
	case MSG_SELECT_IDLECMD:        return L"SelectIdleCmd";
	case MSG_SELECT_EFFECTYN:       return L"SelectEffectYN";
	case MSG_SELECT_YESNO:          return L"SelectYesNo";
	case MSG_SELECT_OPTION:         return L"SelectOption";
	case MSG_SELECT_CARD:           return L"SelectCard";
	case MSG_SELECT_CHAIN:          return L"SelectChain";
	case MSG_SELECT_PLACE:          return L"SelectPlace";
	case MSG_SELECT_POSITION:       return L"SelectPosition";
	case MSG_SELECT_TRIBUTE:        return L"SelectTribute";
	case MSG_SORT_CHAIN:            return L"SortChain";
	case MSG_SELECT_COUNTER:        return L"SelectCounter";
	case MSG_SELECT_SUM:            return L"SelectSum";
	case MSG_SELECT_DISFIELD:       return L"SelectDisfield";
	case MSG_SORT_CARD:             return L"SortCard";
	case MSG_SELECT_UNSELECT_CARD:  return L"SelectUnselectCard";
	case MSG_ROCK_PAPER_SCISSORS:   return L"RockPaperScissors";
	case MSG_ANNOUNCE_RACE:         return L"AnnounceRace";
	case MSG_ANNOUNCE_ATTRIB:       return L"AnnounceAttrib";
	case MSG_ANNOUNCE_CARD:         return L"AnnounceCard";
	case MSG_ANNOUNCE_NUMBER:       return L"AnnounceNumber";
	default:                        return L"Unknown";
	}
}

void ReplayMode::HandleDecisionPointPacket(const CoreUtils::Packet& p) {
	// Payload: [player_byte, select_msg_type_byte]. Only touch the Thoughts
	// tab when this decision is for the POV player; non-POV decisions aren't
	// relevant to the tab. If an MSG_AI_THOUGHT follows (peek), this text
	// will be immediately overwritten with the real thought.
	if(p.buff_size() < 2)
		return;
	if(!mainGame || !mainGame->stThoughts)
		return;
	int decision_player = p.data()[0];
	if(decision_player != GetPovSeat())
		return;
	uint8_t select_type = p.data()[1];
	// current_step is incremented AFTER this handler, so + 1 labels the step
	// the pause will actually land on.
	auto text = epro::format(
		L"Turn {} · {} · Player {} · Step {}\n\nNo decision point data",
		current_turn, SelectMsgTypeName(select_type), decision_player, current_step + 1);
	mainGame->stThoughts->setText(text.c_str());
}

void ReplayMode::HandleAiThoughtPacket(const CoreUtils::Packet& p) {
	if(!mainGame || !mainGame->stThoughts)
		return;
	// Payload layout: uint16 length + UTF-8 JSON bytes.
	if(p.buff_size() < 2)
		return;
	const auto* pbuf = p.data();
	uint16_t len = BufferIO::Read<uint16_t>(pbuf);
	if(len + sizeof(uint16_t) > p.buff_size())
		return;
	std::string raw(reinterpret_cast<const char*>(pbuf), len);
	nlohmann::json j;
	try {
		j = nlohmann::json::parse(raw);
	} catch(const std::exception&) {
		return;
	}
	// Filter by POV — ignore thoughts from the other side of the board.
	int player = j.value("player", -1);
	if(player != GetPovSeat())
		return;
	int turn = j.value("turn", -1);
	int thought_step = j.value("step", -1);
	std::wstring decision_type = BufferIO::DecodeUTF8(j.value("decision_type", std::string{"?"}));
	std::wstring phase = BufferIO::DecodeUTF8(j.value("phase", std::string{""}));
	std::wstring move = BufferIO::DecodeUTF8(j.value("move", std::string{""}));
	std::wstring move_desc = BufferIO::DecodeUTF8(j.value("move_description", std::string{""}));

	// Chosen-action probability + predicted state value. Both optional — fall
	// back gracefully when the bot is an older build that doesn't send them.
	auto fmt_pct = [](double v) {
		// One-decimal percent without locale pulling in a thousands separator.
		int x = static_cast<int>(v * 1000.0 + 0.5);
		return epro::format(L"{}.{}%", x / 10, x % 10);
	};
	auto fmt_value = [](double v) {
		int x = static_cast<int>((v >= 0 ? v : -v) * 1000.0 + 0.5);
		const wchar_t* sign = v >= 0 ? L"+" : L"-";
		return epro::format(L"{}{}.{:03}", sign, x / 1000, x % 1000);
	};

	double confidence = j.value("confidence", -1.0);
	double value = j.value("value", 999.0);  // sentinel: "not present"
	int valid_count = j.value("valid_count", -1);

	std::wstring text = epro::format(
		L"Turn {} · {} · Player {} · Step {}",
		turn, decision_type, player, current_step + 1);
	if(!phase.empty())
		text += epro::format(L"  ({})", phase);
	text += L"\n\n";

	// The chosen move gets the prominent line. Show the readable description
	// with the raw action code in parentheses so both are searchable.
	if(!move_desc.empty() && move_desc != move)
		text += epro::format(L"Move: {}  ({})\n", move_desc, move);
	else
		text += epro::format(L"Move: {}\n", move);

	if(confidence >= 0.0)
		text += epro::format(L"Confidence: {}\n", fmt_pct(confidence));
	if(value < 900.0)
		text += epro::format(L"Predicted value: {}\n", fmt_value(value));
	if(valid_count > 0)
		text += epro::format(L"Valid actions: {}\n", valid_count);

	// Top-K alternatives, ranked. Silently skipped when the payload doesn't
	// include them (older WindBot builds).
	if(j.contains("top_actions") && j["top_actions"].is_array()) {
		const auto& arr = j["top_actions"];
		if(!arr.empty()) {
			text += L"\nTop actions:\n";
			int rank = 0;
			for(const auto& t : arr) {
				++rank;
				std::wstring a_code = BufferIO::DecodeUTF8(t.value("action", std::string{""}));
				std::wstring a_desc = BufferIO::DecodeUTF8(t.value("description", std::string{""}));
				double a_prob = t.value("prob", 0.0);
				if(a_desc.empty() || a_desc == a_code)
					text += epro::format(L"  {}. {}  ({})\n", rank, a_code, fmt_pct(a_prob));
				else
					text += epro::format(L"  {}. {}  ({}, {})\n", rank, a_desc, a_code, fmt_pct(a_prob));
			}
		}
	}

	// Match/step metadata on the last lines — useful when correlating with
	// server-side logs.
	if(thought_step >= 0)
		text += epro::format(L"\nAction #{}", thought_step);

	mainGame->stThoughts->setText(text.c_str());
}

bool ReplayMode::StartReplay(int skipturn, bool is_yrp) {
	if(mainGame->dInfo.isReplay)
		return false;
	skip_turn = skipturn;
	if(skip_turn < 0)
		skip_turn = 0;
	yrp = is_yrp;
	is_swapping = false;
	is_pausing = false;
	is_paused = false;
	is_restarting = false;
	if(replay_thread.joinable())
		replay_thread.join();
	if(is_yrp) {
		if(cur_replay.IsOldReplayMode())
			cur_yrp = &cur_replay;
		else
			cur_yrp = cur_replay.yrp.get();
		if(!cur_yrp)
			return false;
		replay_thread = epro::thread(OldReplayThread);
	} else
		replay_thread = epro::thread(ReplayThread);
	return true;
}
void ReplayMode::StopReplay(bool is_exiting) {
	is_pausing = false;
	is_continuing = false;
	is_closing = is_exiting;
	exit_pending = true;
	mainGame->actionSignal.Set();
	if(is_exiting && replay_thread.joinable())
		replay_thread.join();
}
void ReplayMode::SwapField() {
	if(is_paused)
		mainGame->dField.ReplaySwap();
	else
		is_swapping = true;
}
void ReplayMode::Pause(bool is_pause, bool is_step) {
	if(is_pause)
		is_pausing = true;
	else {
		if(!is_step)
			is_pausing = false;
		mainGame->actionSignal.Set();
	}
}
void ReplayMode::StepToNextDecisionPoint() {
	// Arm the flag, resume play. The pauseable block in ReplayAnalyze flips
	// is_pausing back on the next pauseable decision marker that belongs to
	// the POV player. Non-POV decisions are non-pauseable and are skipped.
	stop_at_decision_point = true;
	is_pausing = false;
	mainGame->actionSignal.Set();
}

ReplayMode::ScanResults ReplayMode::ScanPacketsStream() {
	// Walks packets_stream once, replicating ReplayAnalyze's pauseable rules,
	// to find pauseable-step indices for POV decisions and turn boundaries.
	// Used by the Prev Decision / Prev Turn buttons to locate targets.
	// O(stream_length); stream is at most a few thousand packets.
	ScanResults out;
	int pov = GetPovSeat();
	int step = 0;
	const auto& packets = cur_replay.packets_stream;
	for(size_t i = 0; i < packets.size(); ++i) {
		const auto& p = packets[i];
		bool pauseable = true;
		bool decision_marker_pov = false;
		bool turn_boundary = false;
		if(p.message == MSG_AI_THOUGHT) {
			int player = GetAiThoughtPlayer(p);
			if(player != pov)
				pauseable = false;
			else
				decision_marker_pov = true;
		} else if(p.message == MSG_DECISION_POINT) {
			int dp_player = (p.buff_size() >= 1) ? p.data()[0] : -1;
			bool next_is_thought = (i + 1 < packets.size()
				&& packets[i + 1].message == MSG_AI_THOUGHT);
			if(dp_player != pov)
				pauseable = false;
			else if(next_is_thought)
				pauseable = false;  // defer to the thought's pause
			else if(known_bot_seats.count(dp_player))
				pauseable = false;  // bot seat, no thought → framework auto-resolved
			else
				decision_marker_pov = true;
		} else {
			switch(p.message) {
			case MSG_START: case MSG_UPDATE_DATA: case MSG_UPDATE_CARD:
			case MSG_SET: case MSG_SWAP: case MSG_FIELD_DISABLED:
			case MSG_SUMMONING: case MSG_SPSUMMONING: case MSG_FLIPSUMMONING:
			case MSG_CHAIN_SOLVING: case MSG_CHAIN_SOLVED: case MSG_CHAIN_END:
			case MSG_RANDOM_SELECTED: case MSG_EQUIP: case MSG_UNEQUIP:
			case MSG_CARD_TARGET: case MSG_CANCEL_TARGET: case MSG_BATTLE:
			case MSG_ATTACK_DISABLED: case MSG_DAMAGE_STEP_START:
			case MSG_DAMAGE_STEP_END: case MSG_TAG_SWAP: case MSG_RELOAD_FIELD:
			case MSG_AI_NAME: case OLD_REPLAY_MODE:
				pauseable = false;
				break;
			case MSG_NEW_TURN:
				turn_boundary = true;
				break;
			default:
				break;
			}
		}
		if(pauseable) {
			step++;
			if(decision_marker_pov)
				out.pov_decision_steps.push_back(step);
			if(turn_boundary)
				out.turn_boundary_steps.push_back(step);
		}
	}
	return out;
}

static int FindLargestLessThan(const std::vector<int>& xs, int bound) {
	int target = -1;
	for(int s : xs) {
		if(s < bound)
			target = s;
		else
			break;  // xs is ascending
	}
	return target;
}

void ReplayMode::StepToPrevDecisionPoint() {
	if(mainGame->dInfo.isCatchingUp || current_step == 0)
		return;
	int target = FindLargestLessThan(ScanPacketsStream().pov_decision_steps, current_step);
	if(target < 0)
		target = 0;  // no prior POV decision — jump to start
	jump_target_step = target;
	mainGame->dInfo.isCatchingUp = true;
	Restart(false);
	Pause(false, false);
}

void ReplayMode::StepToNextTurn() {
	// Arm the flag, resume play. The pauseable block in ReplayAnalyze flips
	// is_pausing back on the next MSG_NEW_TURN packet.
	stop_at_turn_boundary = true;
	is_pausing = false;
	mainGame->actionSignal.Set();
}

void ReplayMode::StepToPrevTurn() {
	if(mainGame->dInfo.isCatchingUp || current_step == 0)
		return;
	// Mirror Next Turn: target the step AFTER the boundary so the board
	// renders the new turn's state. Also use (current_step - 1) as the
	// upper bound so we skip past the current turn's start on first press,
	// giving "restart this turn" semantics on the first click.
	int boundary = FindLargestLessThan(ScanPacketsStream().turn_boundary_steps, current_step - 1);
	int target = (boundary < 0) ? 0 : boundary + 1;
	jump_target_step = target;
	mainGame->dInfo.isCatchingUp = true;
	Restart(false);
	Pause(false, false);
}

void ReplayMode::JumpToStart() {
	if(mainGame->dInfo.isCatchingUp)
		return;
	jump_target_step = 0;
	mainGame->dInfo.isCatchingUp = true;
	Restart(false);
	Pause(false, false);
}
int ReplayMode::ReplayThread() {
	Utils::SetThreadName("ReplayMode");
	mainGame->dInfo.isReplay = true;
	const auto& replay_header = cur_replay.pheader;
	mainGame->dInfo.isFirst = true;
	mainGame->dInfo.isTeam1 = true;
	mainGame->dInfo.isRelay = !!(cur_replay.params.duel_flags & DUEL_RELAY);
	mainGame->dInfo.isSingleMode = !!(replay_header.base.flag & REPLAY_SINGLE_MODE);
	mainGame->dInfo.isHandTest = !!(replay_header.base.flag & REPLAY_HAND_TEST);
	mainGame->dInfo.compat_mode = !(replay_header.base.flag & REPLAY_LUA64);
	mainGame->dInfo.legacy_race_size = GET_CORE_VERSION_MAJOR(replay_header.base.version) < 10;
	mainGame->dInfo.team1 = cur_replay.GetPlayersCount(0);
	mainGame->dInfo.team2 = cur_replay.GetPlayersCount(1);
	mainGame->dInfo.current_player[0] = 0;
	mainGame->dInfo.current_player[1] = 0;
	if(!mainGame->dInfo.isRelay)
		mainGame->dInfo.current_player[1] = mainGame->dInfo.team2 - 1;
	const auto& names = cur_replay.GetPlayerNames();
	const auto first_oppo_player = names.begin() + mainGame->dInfo.team1;
	mainGame->dInfo.selfnames.assign(names.begin(), first_oppo_player);
	mainGame->dInfo.opponames.assign(first_oppo_player, names.end());
	mainGame->dInfo.duel_params = cur_replay.params.duel_flags;
	mainGame->dInfo.duel_field = mainGame->GetMasterRule(mainGame->dInfo.duel_params);
	matManager.SetActiveVertices(mainGame->dInfo.HasFieldFlag(DUEL_3_COLUMNS_FIELD),
								 !mainGame->dInfo.HasFieldFlag(DUEL_SEPARATE_PZONE));
	mainGame->SetPhaseButtons();
	auto& current_stream = cur_replay.packets_stream;
	if(!current_stream.size()) {
		EndDuel();
		return 0;
	}
	mainGame->dInfo.isInDuel = true;
	mainGame->dInfo.isStarted = true;
	mainGame->dInfo.checkRematch = false;
	mainGame->SetMessageWindow();
	mainGame->dInfo.turn = 0;
	mainGame->dInfo.isCatchingUp = (skip_turn > 0);
	is_continuing = true;
	skip_step = 0;
	exit_pending = false;
	current_step = 0;
	ScanForKnownBotSeats();
	ResetThoughtsState();
	if(mainGame->dInfo.isCatchingUp)
		mainGame->gMutex.lock();
	for(auto it = current_stream.begin(); is_continuing && !exit_pending && it != current_stream.end();) {
		// Peek the next packet so MSG_DECISION_POINT can decide pauseability.
		auto next_it = it + 1;
		peek_next_is_ai_thought = (next_it != current_stream.end()
			&& next_it->message == MSG_AI_THOUGHT);
		is_continuing = ReplayAnalyze((*it));
		if(is_restarting) {
			mainGame->gMutex.lock();
			it = current_stream.begin();
			is_restarting = false;
			int step;
			if(jump_target_step >= 0) {
				step = jump_target_step;
				jump_target_step = -1;
			} else {
				step = current_step - 1;
			}
			if (step < 0)
				step = 0;
			if (step == 0) {
				Pause(true, false);
				mainGame->dInfo.isInDuel = true;
				mainGame->dInfo.isStarted = true;
				mainGame->dInfo.isCatchingUp = false;
				mainGame->dField.RefreshAllCards();
				mainGame->SetMessageWindow();
				mainGame->gMutex.unlock();
			}
			skip_step = step;
			current_step = 0;
			// MSG_NEW_TURN on the re-play will rebuild current_turn from 0,
			// and MSG_AI_THOUGHT packets will re-populate the Thoughts tab
			// as they stream through again.
			current_turn = 0;
			current_decision_idx = 0;
			stop_at_decision_point = false;
			stop_at_turn_boundary = false;
			armed_post_turn_boundary = false;
			ResetThoughtsState();
		} else
			it++;
	}
	if(mainGame->dInfo.isCatchingUp) {
		mainGame->dInfo.isCatchingUp = false;
		mainGame->dField.RefreshAllCards();
		mainGame->gMutex.unlock();
	}
	EndDuel();
	return 0;
}
void ReplayMode::EndDuel() {
	if(pduel) {
		OCG_DestroyDuel(pduel);
		pduel = nullptr;
	}
	if(!is_closing) {
		std::unique_lock<epro::mutex> lock(mainGame->gMutex);
		mainGame->stMessage->setText(gDataManager->GetSysString(1501).data());
		if(mainGame->wCardSelect->isVisible())
			mainGame->HideElement(mainGame->wCardSelect);
		mainGame->PopupElement(mainGame->wMessage);
		mainGame->actionSignal.Wait(lock);
		mainGame->dInfo.isInDuel = false;
		mainGame->dInfo.isStarted = false;
		mainGame->dInfo.isReplay = false;
		mainGame->dInfo.isSingleMode = false;
		mainGame->dInfo.isHandTest = false;
		mainGame->dInfo.isOldReplay = false;
		mainGame->closeDuelWindow = true;
		mainGame->closeDoneSignal.Wait(lock);
		mainGame->ShowElement(mainGame->wReplay);
		mainGame->SetMessageWindow();
		mainGame->stTip->setVisible(false);
		gSoundManager->StopSounds();
		mainGame->device->setEventReceiver(&mainGame->menuHandler);
	}
}
void ReplayMode::Restart(bool refresh) {
	if(pduel) {
		OCG_DestroyDuel(pduel);
		pduel = nullptr;
		//end_duel(pduel);
		cur_replay.Rewind();
	}
	mainGame->dInfo.isInDuel = false;
	mainGame->dInfo.isStarted = false;
	mainGame->dInfo.turn = 0;
	mainGame->dField.Clear();
	mainGame->dInfo.current_player[0] = 0;
	mainGame->dInfo.current_player[1] = 0;
	if(!mainGame->dInfo.isRelay) {
		if(mainGame->dInfo.isFirst)
			mainGame->dInfo.current_player[1] = mainGame->dInfo.team2 - 1;
		else
			mainGame->dInfo.current_player[0] = mainGame->dInfo.team1 - 1;
	}
	if (yrp && !StartDuel()) {
		EndDuel();
	}
	if(refresh) {
		mainGame->dField.RefreshAllCards();
		mainGame->dInfo.isInDuel = true;
		mainGame->dInfo.isStarted = true;
	}
	skip_turn = 0;
	is_restarting = true;
}
void ReplayMode::Undo() {
	if(mainGame->dInfo.isCatchingUp || current_step == 0)
		return;
	mainGame->dInfo.isCatchingUp = true;
	Restart(false);
	Pause(false, false);
}
bool ReplayMode::ReplayAnalyze(const CoreUtils::Packet& p) {
	is_restarting = false;
	{
		if(is_closing)
			return false;
		if(is_restarting)
			return true;
		if(is_swapping) {
			std::lock_guard<epro::mutex> lock(mainGame->gMutex);
			mainGame->dField.ReplaySwap();
			is_swapping = false;
		}
		bool pauseable = true;
		bool skip_client_analyze = false;
		bool is_decision_marker = false;
		mainGame->dInfo.curMsg = p.message;
		if(mainGame->dInfo.curMsg == MSG_NEW_TURN) {
			current_turn++;
			current_decision_idx = 0;
		}
		// ExodAI markers. Neither is a real game-engine message, so we skip
		// ClientAnalyze for both. POV filtering: only the POV player's
		// decisions are marked as pauseable / is_decision_marker; the
		// opponent's decisions slip through as non-pauseable so Step and
		// Next Decision don't stop on them.
		if(mainGame->dInfo.curMsg == MSG_AI_THOUGHT) {
			HandleAiThoughtPacket(p);
			skip_client_analyze = true;
			int player = GetAiThoughtPlayer(p);
			if(player == GetPovSeat())
				is_decision_marker = true;
			else
				pauseable = false;
		} else if(mainGame->dInfo.curMsg == MSG_DECISION_POINT) {
			skip_client_analyze = true;
			int dp_player = (p.buff_size() >= 1) ? p.data()[0] : -1;
			if(dp_player != GetPovSeat()) {
				pauseable = false;  // opponent's decision — just slide past
			} else if(peek_next_is_ai_thought) {
				pauseable = false;  // thought follows, pause there instead
			} else if(known_bot_seats.count(dp_player)) {
				// Known bot seat with no thought packet: the WindBot framework
				// auto-resolved this (e.g. chain-with-nothing-chainable, zone
				// pick, default sort). The model wasn't invoked, so this isn't
				// a real decision to navigate to.
				pauseable = false;
			} else {
				// Human (or non-ExodAI) seat — every engine select is a real
				// decision from their perspective. Pause and show the "no
				// thought data" telemetry.
				HandleDecisionPointPacket(p);
				is_decision_marker = true;
			}
		}
		switch (mainGame->dInfo.curMsg) {
		case MSG_RETRY: {
			if(mainGame->dInfo.isCatchingUp) {
				mainGame->dInfo.isCatchingUp = false;
				mainGame->dField.RefreshAllCards();
				mainGame->gMutex.unlock();
			}
			std::unique_lock<epro::mutex> lock(mainGame->gMutex);
			mainGame->stMessage->setText(gDataManager->GetSysString(1434).data());
			mainGame->PopupElement(mainGame->wMessage);
			mainGame->actionSignal.Wait(lock);
			return false;
		}
		case MSG_WIN: {
			if(!yrp || !cur_yrp || !(cur_yrp->pheader.base.flag & REPLAY_HAND_TEST)) {
				if (mainGame->dInfo.isCatchingUp) {
					mainGame->dInfo.isCatchingUp = false;
					mainGame->dField.RefreshAllCards();
					mainGame->gMutex.unlock();
				}
				DuelClient::ClientAnalyze(p);
				return false;
			}
			return true;
		}
		case MSG_START:
		case MSG_UPDATE_DATA:
		case MSG_UPDATE_CARD:
		case MSG_SET:
		case MSG_SWAP:
		case MSG_FIELD_DISABLED:
		case MSG_SUMMONING:
		case MSG_SPSUMMONING:
		case MSG_FLIPSUMMONING:
		case MSG_CHAIN_SOLVING:
		case MSG_CHAIN_SOLVED:
		case MSG_CHAIN_END:
		case MSG_RANDOM_SELECTED:
		case MSG_EQUIP:
		case MSG_UNEQUIP:
		case MSG_CARD_TARGET:
		case MSG_CANCEL_TARGET:
		case MSG_BATTLE:
		case MSG_ATTACK_DISABLED:
		case MSG_DAMAGE_STEP_START:
		case MSG_DAMAGE_STEP_END:
		case MSG_TAG_SWAP:
		case MSG_RELOAD_FIELD: {
			pauseable = false;
			break;
		}
		case MSG_NEW_TURN: {
			if(skip_turn) {
				skip_turn--;
				if(skip_turn == 0) {
					mainGame->dInfo.isCatchingUp = false;
					mainGame->dField.RefreshAllCards();
					mainGame->gMutex.unlock();
				}
			}
			break;
		}
		case MSG_AI_NAME: {
			const auto* pbuf = p.data();
			auto len = BufferIO::Read<uint16_t>(pbuf);
			if((len + 1u) != p.buff_size() - (sizeof(uint16_t)))
				break;
			mainGame->dInfo.opponames[0] = BufferIO::DecodeUTF8({ reinterpret_cast<const char*>(pbuf), len });
			return true;
		}
		case OLD_REPLAY_MODE:
			return true;
		}
		if(!skip_client_analyze)
			DuelClient::ClientAnalyze(p);
		if(pauseable) {
			current_step++;
			current_decision_idx++;
			// Thoughts tab rules:
			//   • MSG_AI_THOUGHT already set the intent text.
			//   • MSG_DECISION_POINT (non-bot) already set the "No data" text.
			//   • Everything else is a plain state-change — clear the box.
			if(!is_decision_marker && mainGame && mainGame->stThoughts)
				mainGame->stThoughts->setText(L"");
			// "Next Decision" button: flip back to pause mode when the next
			// decision marker passes through.
			if(stop_at_decision_point && is_decision_marker) {
				stop_at_decision_point = false;
				is_pausing = true;
			}
			// "Next Turn" button: two-stage trip. MSG_NEW_TURN arms the flag;
			// we actually pause on the NEXT pauseable step so the board has
			// time to render the new turn's initial state.
			if(stop_at_turn_boundary && mainGame->dInfo.curMsg == MSG_NEW_TURN) {
				stop_at_turn_boundary = false;
				armed_post_turn_boundary = true;
			} else if(armed_post_turn_boundary) {
				armed_post_turn_boundary = false;
				is_pausing = true;
			}
			if(skip_step) {
				skip_step--;
				if(skip_step == 0) {
					Pause(true, false);
					mainGame->dInfo.isInDuel = true;
					mainGame->dInfo.isStarted = true;
					mainGame->dInfo.isCatchingUp = false;
					mainGame->dField.RefreshAllCards();
					mainGame->gMutex.unlock();
				}
			}
			if(is_pausing) {
				is_paused = true;
				std::unique_lock<epro::mutex> lock(mainGame->gMutex);
				mainGame->actionSignal.Wait(lock);
				is_paused = false;
			}
		}
	}
	return true;
}

}
