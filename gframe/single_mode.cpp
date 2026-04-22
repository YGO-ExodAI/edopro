#include "single_mode.h"
#include "game_config.h"
#include "duelclient.h"
#include "game.h"
#include "core_utils.h"
#include "sound_manager.h"
#include "fmt.h"
#include "localtime.h"
#include "CGUIFileSelectListBox/CGUIFileSelectListBox.h"
// Chunk 8: SaveStateToFile uses the OCG_Duel* C API directly + minimal
// JSON sidecar generation. No nlohmann/json dep — keeps the EDOPro
// vcxproj surface unchanged.
#include <chrono>
#include <cstdlib>  // std::getenv
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <IrrlichtDevice.h>
#include <IGUIWindow.h>
#include <IGUIStaticText.h>
#include <IGUIEditBox.h>
#include <IGUIButton.h>
#include <IGUIContextMenu.h>
#include <IGUITabControl.h>

namespace ygo {

OCG_Duel SingleMode::pduel = 0;
bool SingleMode::is_closing = false;
bool SingleMode::is_continuing = false;
bool SingleMode::is_restarting = false;
Replay SingleMode::last_replay;
Replay SingleMode::new_replay;
ReplayStream SingleMode::replay_stream;
Signal SingleMode::singleSignal;
epro::thread SingleMode::single_mode_thread;

// Chunk 8 helpers: SHA256 from-scratch (avoid pulling openssl), POSIX
// directory create, ISO timestamp.
namespace {
// Tiny SHA256 implementation. Standard FIPS 180-4. Compact rather than
// fast — used once per save call, not in any hot path.
struct Sha256 {
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint8_t buf[64]; size_t buflen=0; uint64_t total=0;
    static uint32_t rot(uint32_t x,int n){return (x>>n)|(x<<(32-n));}
    void block(const uint8_t* p) {
        uint32_t w[64];
        for(int i=0;i<16;i++) w[i]=(p[i*4]<<24)|(p[i*4+1]<<16)|(p[i*4+2]<<8)|p[i*4+3];
        for(int i=16;i<64;i++) {
            uint32_t s0=rot(w[i-15],7)^rot(w[i-15],18)^(w[i-15]>>3);
            uint32_t s1=rot(w[i-2],17)^rot(w[i-2],19)^(w[i-2]>>10);
            w[i]=w[i-16]+s0+w[i-7]+s1;
        }
        static const uint32_t K[64]={
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for(int i=0;i<64;i++) {
            uint32_t S1=rot(e,6)^rot(e,11)^rot(e,25);
            uint32_t ch=(e&f)^((~e)&g);
            uint32_t t1=hh+S1+ch+K[i]+w[i];
            uint32_t S0=rot(a,2)^rot(a,13)^rot(a,22);
            uint32_t mj=(a&b)^(a&c)^(b&c);
            uint32_t t2=S0+mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    void update(const void* data, size_t len) {
        const uint8_t* p=(const uint8_t*)data; total+=len;
        while(len) {
            size_t take = std::min<size_t>(64-buflen, len);
            memcpy(buf+buflen, p, take); buflen+=take; p+=take; len-=take;
            if(buflen==64) { block(buf); buflen=0; }
        }
    }
    std::string finalize() {
        uint64_t bits=total*8;
        update("\x80",1);
        while(buflen!=56) update("\x00",1);
        uint8_t lb[8]; for(int i=0;i<8;i++) lb[i]=(bits>>(56-i*8))&0xff;
        update(lb,8);
        std::ostringstream o; o<<std::hex<<std::setfill('0');
        for(int i=0;i<8;i++) o<<std::setw(8)<<h[i];
        return o.str();
    }
};

std::string sha256_hex(const void* data, size_t len) {
    Sha256 s; s.update(data,len); return s.finalize();
}

std::string iso_utc_now() {
    auto t = std::chrono::system_clock::now();
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
    std::time_t tt = static_cast<std::time_t>(sec);
    std::tm tm_buf;
#ifdef _WIN32
    gmtime_s(&tm_buf, &tt);
#else
    gmtime_r(&tt, &tm_buf);
#endif
    char buf[40];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return std::string(buf);
}

std::string positions_dir() {
    if (const char* env = std::getenv("EXODAI_POSITIONS_DIR")) return env;
    return "./replays/positions/";
}

bool ensure_dir(const std::string& path) {
    // Cheap mkdir-with-mkdir-parents. Only needed for the EXODAI_POSITIONS_DIR;
    // if it doesn't exist, save fails with a clear message. Tolerate
    // already-exists.
#ifdef _WIN32
    std::string cmd = "if not exist \"" + path + "\" mkdir \"" + path + "\"";
#else
    std::string cmd = "mkdir -p \"" + path + "\"";
#endif
    return std::system(cmd.c_str()) == 0;
}

std::string make_basename() {
    // YYYYMMDD-HHMMSS-<random4>.bin
    auto t = std::chrono::system_clock::now();
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
    std::time_t tt = static_cast<std::time_t>(sec);
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &tt);
#else
    localtime_r(&tt, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm_buf);
    static std::mt19937 rng{std::random_device{}()};
    char rb[6];
    std::snprintf(rb, sizeof(rb), "-%04x", rng() & 0xffff);
    return std::string(buf) + rb + ".bin";
}
}  // namespace

bool SingleMode::SaveStateToFile(std::string& out_msg) {
    if (pduel == 0) {
        out_msg = "Save: no active single-mode duel.";
        return false;
    }
    void* blob = nullptr;
    uint32_t size = 0;
    int status = OCG_DuelSaveState(pduel, &blob, &size);
    if (status != 0 /*OCG_SAVE_OK*/) {
        out_msg = fmt::format("Save refused (status={}). See "
            "phase_p1_primitive_1_unsupported_cards.md for refuse classes; "
            "this may be a tpchain/ntpchain/select_chains state — "
            "Tier 2 deferred them.", status);
        if (blob) OCG_FreeSaveBuffer(blob);
        return false;
    }
    std::string dir = positions_dir();
    if (!ensure_dir(dir)) {
        out_msg = fmt::format("Save: failed to create positions dir '{}'", dir);
        OCG_FreeSaveBuffer(blob);
        return false;
    }
    std::string base = make_basename();
    std::string blob_path = dir + (dir.back()=='/' || dir.back()=='\\' ? "" : "/") + base;
    std::string sidecar_path = blob_path + ".json";

    // Write blob
    {
        std::ofstream f(blob_path, std::ios::binary);
        if (!f) {
            out_msg = fmt::format("Save: cannot open '{}' for write", blob_path);
            OCG_FreeSaveBuffer(blob);
            return false;
        }
        f.write(static_cast<const char*>(blob), size);
    }

    // Compute blob hash for sidecar
    std::string blob_hash = sha256_hex(blob, size);
    OCG_FreeSaveBuffer(blob);

    // Write minimal JSON sidecar. Format mirrors src/state_io.py's schema
    // (subset — engine_build_hash and script_corpus_hash left empty for
    // the Python tools to populate post-hoc if needed). Manual JSON
    // formatting to avoid pulling nlohmann/json into the EDOPro deps.
    {
        std::ofstream f(sidecar_path);
        if (!f) {
            out_msg = fmt::format("Save: blob written to '{}' but sidecar "
                                  "write to '{}' failed", blob_path, sidecar_path);
            return false;
        }
        f << "{\n";
        f << "  \"schema_version\": 1,\n";
        f << "  \"save_timestamp_utc\": \"" << iso_utc_now() << "\",\n";
        f << "  \"save_path\": \"" << blob_path << "\",\n";
        f << "  \"save_safety\": \"OK\",\n";
        f << "  \"refuse_reason\": \"\",\n";
        f << "  \"provenance\": {\"source\": \"edopro-hotkey\"},\n";
        f << "  \"script_corpus_hash\": {},\n";
        f << "  \"engine_build_hash\": \"\",\n";
        f << "  \"blob_sha256\": \"" << blob_hash << "\",\n";
        f << "  \"tags\": [\"edopro-hotkey\"],\n";
        f << "  \"notes\": \"\",\n";
        f << "  \"correct_action\": null,\n";
        f << "  \"card_codes\": []\n";
        f << "}\n";
    }
    out_msg = fmt::format("Saved {} bytes to {} (+sidecar)", size, blob_path);
    return true;
}

bool SingleMode::StartPlay(DuelOptions&& duelOptions) {
	if(mainGame->dInfo.isSingleMode)
		return false;
	if(single_mode_thread.joinable())
		single_mode_thread.join();
	single_mode_thread = epro::thread(SinglePlayThread, std::move(duelOptions));
	return true;
}
void SingleMode::StopPlay(bool is_exiting) {
	is_closing = is_exiting;
	is_continuing = false;
	is_restarting = false;
	mainGame->actionSignal.Set();
	if(is_closing) {
		singleSignal.SetNoWait(true);
		if(single_mode_thread.joinable())
			single_mode_thread.join();
	} else
		singleSignal.Set();
}
void SingleMode::Restart() {
	StopPlay();
	is_restarting = true;
}
void SingleMode::SetResponse(void* resp, size_t len) {
	if(!pduel)
		return;
	last_replay.Write<uint8_t>(static_cast<uint8_t>(len), false);
	last_replay.WriteData(resp, len);
	OCG_DuelSetResponse(pduel, resp, static_cast<uint32_t>(len));
}
int SingleMode::SinglePlayThread(DuelOptions&& duelOptions) {
	Utils::SetThreadName("SinglePlay");
	uint64_t opt = duelOptions.duelFlags;
	std::string script_name = "";
	auto InitReplay = [&]() {
		uint16_t buffer[20];
		BufferIO::EncodeUTF16(mainGame->dInfo.selfnames[0].data(), buffer, 20);
		last_replay.WriteData(buffer, 40, false);
		new_replay.WriteData(buffer, 40, false);
		BufferIO::EncodeUTF16(mainGame->dInfo.opponames[0].data(), buffer, 20);
		last_replay.WriteData(buffer, 40, false);
		new_replay.WriteData(buffer, 40, false);
		last_replay.Write<uint32_t>(duelOptions.startingLP, false);
		last_replay.Write<uint32_t>(duelOptions.startingDrawCount, false);
		last_replay.Write<uint32_t>(duelOptions.drawCountPerTurn, false);
		last_replay.Write<uint64_t>(opt, false);
		last_replay.Write<uint16_t>((uint16_t)script_name.size(), false);
		last_replay.WriteData(script_name.data(), script_name.size(), false);
		last_replay.Flush();
		new_replay.Write<uint64_t>(opt);
	};
	mainGame->btnLeaveGame->setRelativePosition(mainGame->Resize(205, 5, 295, 45));
	is_continuing = false;
	is_restarting = false;
	auto rnd = Utils::GetRandomNumberGenerator();
restart:
	mainGame->dInfo.isSingleMode = true;
	OCG_Player team = { duelOptions.startingLP, duelOptions.startingDrawCount, duelOptions.drawCountPerTurn };
	bool hand_test = mainGame->dInfo.isHandTest = (duelOptions.scriptName == "hand-test-mode");
	if(hand_test)
		opt |= DUEL_ATTACK_FIRST_TURN;
	const auto seed = Utils::GetRandomNumberGeneratorSeed();
	pduel = mainGame->SetupDuel({ { seed[0], seed[1], seed[2], seed[3] }, opt, team, team });
	mainGame->dInfo.duel_params = opt;
	mainGame->dInfo.duel_field = mainGame->GetMasterRule(mainGame->dInfo.duel_params);
	matManager.SetActiveVertices(mainGame->dInfo.HasFieldFlag(DUEL_3_COLUMNS_FIELD),
								 !mainGame->dInfo.HasFieldFlag(DUEL_SEPARATE_PZONE));
	mainGame->dInfo.compat_mode = false;
	mainGame->dInfo.legacy_race_size = false;
	mainGame->dInfo.startlp = mainGame->dInfo.lp[0] = mainGame->dInfo.lp[1] = duelOptions.startingLP;
	mainGame->dInfo.strLP[0] = mainGame->dInfo.strLP[1] = epro::to_wstring(mainGame->dInfo.lp[0]);
	mainGame->dInfo.selfnames = { mainGame->ebNickName->getText() };
	mainGame->dInfo.opponames = { L"" };
	mainGame->dInfo.player_type = 0;
	mainGame->dInfo.turn = 0;
	bool loaded = true;
	bool saveReplay = !hand_test || gGameConfig->saveHandTest;
	if(saveReplay) {
		auto replay_header = ExtendedReplayHeader::CreateDefaultHeader(REPLAY_YRP1, static_cast<uint32_t>(time(nullptr)));
		replay_header.SetSeed(seed);
		replay_header.base.flag |= REPLAY_SINGLE_MODE;
		if(hand_test)
			replay_header.base.flag |= REPLAY_HAND_TEST;
		last_replay.BeginRecord(true, Replay::GetReplayFilePath(EPRO_TEXT("_LastReplay.yrp")));
		last_replay.WriteHeader(replay_header);
		//records the replay with the new system
		new_replay.BeginRecord();
		replay_header.base.id = REPLAY_YRPX;
		new_replay.WriteHeader(replay_header);
		replay_stream.clear();
	}
	if(hand_test) {
		script_name = "hand-test-mode";
		InitReplay();
		Deck playerdeck(mainGame->deckBuilder.GetCurrentDeck());
		if ((duelOptions.duelFlags & DUEL_PSEUDO_SHUFFLE) == 0)
			std::shuffle(playerdeck.main.begin(), playerdeck.main.end(), rnd);
		auto LoadDeck = [&](uint8_t team) {
			OCG_NewCardInfo card_info = { team, 0, 0, team, 0, 0, POS_FACEDOWN_DEFENSE };
			card_info.loc = LOCATION_DECK;
			last_replay.Write<uint32_t>(static_cast<uint32_t>(playerdeck.main.size()), false);
			for (int32_t i = (int32_t)playerdeck.main.size() - 1; i >= 0; --i) {
				card_info.code = playerdeck.main[i]->code;
				OCG_DuelNewCard(pduel, &card_info);
				last_replay.Write<uint32_t>(playerdeck.main[i]->code, false);
			}
			card_info.loc = LOCATION_EXTRA;
			last_replay.Write<uint32_t>(static_cast<uint32_t>(playerdeck.extra.size()), false);
			for (int32_t i = (int32_t)playerdeck.extra.size() - 1; i >= 0; --i) {
				card_info.code = playerdeck.extra[i]->code;
				OCG_DuelNewCard(pduel, &card_info);
				last_replay.Write<uint32_t>(playerdeck.extra[i]->code, false);
			}
		};
		LoadDeck(0);
		if (duelOptions.handTestNoOpponent) {
			last_replay.Write<uint32_t>(0, false);
			last_replay.Write<uint32_t>(0, false);
		} else {
			LoadDeck(1);
		}
		last_replay.Flush();
		const char cmd[] = "Debug.ReloadFieldEnd()";
		loaded = OCG_LoadScript(pduel, cmd, sizeof(cmd) - 1, " ");
	} else {
		if(open_file) {
			script_name = Utils::ToUTF8IfNeeded(open_file_name);
			if(!mainGame->LoadScript(pduel, script_name)) {
				script_name = epro::format("./puzzles/{}", script_name);
				loaded = mainGame->LoadScript(pduel, script_name);
			}
		} else {
			script_name = duelOptions.scriptName;
			loaded = mainGame->LoadScript(pduel, script_name);
		}
		InitReplay();
	}
	if(!loaded) {
		OCG_DestroyDuel(pduel);
		pduel = nullptr;
		mainGame->dInfo.isSingleMode = false;
		mainGame->dInfo.isHandTest = false;
		open_file = false;
		last_replay.EndRecord();
		new_replay.EndRecord();
		std::unique_lock<epro::mutex> lock(mainGame->gMutex);
		if(is_restarting) {
			mainGame->dInfo.isInDuel = false;
			mainGame->dInfo.isStarted = false;
			mainGame->dInfo.isSingleMode = false;
			mainGame->dInfo.isHandTest = false;
			if(!hand_test) {
				mainGame->closeDuelWindow = true;
				mainGame->closeDoneSignal.Wait(lock);
			}
			mainGame->btnLeaveGame->setRelativePosition(mainGame->Resize(205, 5, 295, 80));
			if(!hand_test) {
				mainGame->ShowElement(mainGame->wSinglePlay);
				mainGame->stTip->setVisible(false);
			}
			mainGame->SetMessageWindow();
			mainGame->device->setEventReceiver(&mainGame->menuHandler);
			if(hand_test) {
				mainGame->btnChainIgnore->setVisible(false);
				mainGame->btnChainAlways->setVisible(false);
				mainGame->btnChainWhenAvail->setVisible(false);
				mainGame->btnCancelOrFinish->setVisible(false);
				mainGame->btnShuffle->setVisible(false);
				mainGame->wChat->setVisible(false);
				mainGame->btnRestartSingle->setVisible(false);
				mainGame->wPhase->setVisible(false);
				mainGame->deckBuilder.Initialize(false);
			}
		} else
			mainGame->btnLeaveGame->setRelativePosition(mainGame->Resize(205, 5, 295, 80));
		is_restarting = false;
		return 0;
	}
	mainGame->gMutex.lock();
	if(!hand_test && !is_restarting) {
		mainGame->HideElement(mainGame->wSinglePlay);
		mainGame->ClearCardInfo();
	}
	is_restarting = false;
	mainGame->mTopMenu->setVisible(false);
	mainGame->wCardImg->setVisible(true);
	mainGame->wInfos->setVisible(true);
	mainGame->btnLeaveGame->setVisible(true);
	mainGame->btnLeaveGame->setText(gDataManager->GetSysString(1210).data());
	mainGame->btnRestartSingle->setVisible(true);
	mainGame->wPhase->setVisible(true);
	mainGame->dField.Clear();
	mainGame->dInfo.isFirst = true;
	mainGame->dInfo.isTeam1 = true;
	mainGame->dInfo.isInDuel = true;
	mainGame->dInfo.isStarted = true;
	mainGame->dInfo.isCatchingUp = false;
	mainGame->dInfo.checkRematch = false;
	mainGame->SetMessageWindow();
	mainGame->device->setEventReceiver(&mainGame->dField);
	mainGame->gMutex.unlock();
	is_closing = false;
	is_continuing = true;
	int engFlag = 0;
	for(auto& message : CoreUtils::ParseMessages(pduel))
		is_continuing = SinglePlayAnalyze(message) && is_continuing;
	if(is_continuing) {
		OCG_StartDuel(pduel);
		do {
			engFlag = OCG_DuelProcess(pduel);
			for(auto& message : CoreUtils::ParseMessages(pduel)) {
				if(message.message == MSG_WIN && hand_test)
					continue;
				is_continuing = SinglePlayAnalyze(message) && is_continuing;
			}
		} while(is_continuing && engFlag && mainGame->dInfo.curMsg != MSG_WIN);
	}
	OCG_DestroyDuel(pduel);
	pduel = nullptr;
	if(saveReplay && !is_restarting) {
		last_replay.EndRecord(0x1000);
		auto oldbuffer = last_replay.GetSerializedBuffer();
		CoreUtils::Packet tmp{};
		tmp.message = OLD_REPLAY_MODE;
		tmp.buffer.swap(oldbuffer);
		new_replay.WritePacket(tmp);
		new_replay.EndRecord();
	}
	if(is_closing) {
		open_file = false;
		is_restarting = false;
		mainGame->gMutex.lock();
		mainGame->btnLeaveGame->setRelativePosition(mainGame->Resize(205, 5, 295, 80));
		mainGame->gMutex.unlock();
		return 0;
	}
	gSoundManager->StopSounds();
	bool was_restarting = is_restarting;
	if(saveReplay && !was_restarting) {
		auto now = std::time(nullptr);
		std::unique_lock<epro::mutex> lock(mainGame->gMutex);
		mainGame->PopupSaveWindow(gDataManager->GetSysString(1340), epro::format(L"{:%Y-%m-%d %H-%M-%S}", epro::localtime(now)), gDataManager->GetSysString(1342));
		mainGame->replaySignal.Wait(lock);
		if(mainGame->saveReplay)
			new_replay.SaveReplay(Utils::ToPathString(mainGame->ebFileSaveName->getText()));
	}
	new_replay.Reset();
	last_replay.Reset();
	mainGame->gMutex.lock();
	mainGame->dField.Clear();
	mainGame->gMutex.unlock();
	if(!is_closing) {
		if(was_restarting || hand_test) {
			std::lock_guard<epro::mutex> lock(mainGame->gMutex);
			for(auto wit = mainGame->fadingList.begin(); wit != mainGame->fadingList.end(); ++wit) {
				if(wit->isFadein)
					wit->autoFadeoutFrame = 1;
			}
			mainGame->wACMessage->setVisible(false);
			mainGame->wANAttribute->setVisible(false);
			mainGame->wANCard->setVisible(false);
			mainGame->wANNumber->setVisible(false);
			mainGame->wANRace->setVisible(false);
			mainGame->wCardSelect->setVisible(false);
			mainGame->wCardDisplay->setVisible(false);
			mainGame->wCmdMenu->setVisible(false);
			mainGame->wMessage->setVisible(false);
			mainGame->wOptions->setVisible(false);
			mainGame->wPosSelect->setVisible(false);
			mainGame->wQuery->setVisible(false);
			mainGame->stHintMsg->setVisible(false);
			if(was_restarting)
				goto restart;
		}
		std::unique_lock<epro::mutex> lock(mainGame->gMutex);
		mainGame->dInfo.isInDuel = false;
		mainGame->dInfo.isStarted = false;
		mainGame->dInfo.isSingleMode = false;
		mainGame->dInfo.isHandTest = false;
		if(!hand_test) {
			mainGame->closeDuelWindow = true;
			mainGame->closeDoneSignal.Wait(lock);
		}
		mainGame->btnLeaveGame->setRelativePosition(mainGame->Resize(205, 5, 295, 80));
		if(!hand_test) {
			mainGame->ShowElement(mainGame->wSinglePlay);
			mainGame->stTip->setVisible(false);
		}
		mainGame->SetMessageWindow();
		mainGame->device->setEventReceiver(&mainGame->menuHandler);
		if(hand_test) {
			mainGame->btnChainIgnore->setVisible(false);
			mainGame->btnChainAlways->setVisible(false);
			mainGame->btnChainWhenAvail->setVisible(false);
			mainGame->btnCancelOrFinish->setVisible(false);
			mainGame->btnShuffle->setVisible(false);
			mainGame->wChat->setVisible(false);
			mainGame->btnRestartSingle->setVisible(false);
			mainGame->wPhase->setVisible(false);
			mainGame->deckBuilder.Initialize(false);
		}
	}
	open_file = false;
	return 0;
}

bool SingleMode::SinglePlayAnalyze(CoreUtils::Packet& packet) {
	auto Analyze = [&packet]()->bool {
		DuelClient::answered = false;
		return DuelClient::ClientAnalyze(packet);
	};
	replay_stream.clear();
	if(is_closing || !is_continuing)
		return false;
	mainGame->dInfo.curMsg = packet.message;
	bool record = true;
	bool record_last = false;
	switch(mainGame->dInfo.curMsg) {
		case MSG_RETRY:	{
			std::unique_lock<epro::mutex> lock(mainGame->gMutex);
			mainGame->stMessage->setText(gDataManager->GetSysString(1434).data());
			mainGame->PopupElement(mainGame->wMessage);
			mainGame->actionSignal.Wait(lock);
			return false;
		}
		case MSG_HINT: {
			const auto* pbuf = packet.data();
			int type = BufferIO::Read<uint8_t>(pbuf);
			auto player = BufferIO::Read<uint8_t>(pbuf);
			/*uint64_t data = BufferIO::Read<uint64_t>(pbuf);*/
			bool analyze = false;
			switch (type) {
			case 1:
			case 2:
			case 3:
			case 5: {
				analyze = player == 0;
				break;
			}
			case 4:
			case 6:
			case 7:
			case 8:
			case 9:
			case 11: {
				analyze = player != 0;
				break;
			}
			case 10:
			case 200:
			case 201:
			case 202:
			case 203: {
				analyze = true;
				break;

			}
			}
			if(analyze)
				Analyze();
			if(type > 0 && type < 6 && type != 4)
				record = false;
			break;
		}
		case MSG_AI_NAME:
		case MSG_SHOW_HINT: {
			auto* pbuf = packet.data();
			auto len = BufferIO::Read<uint16_t>(pbuf);
			if((len + 1u) != packet.buff_size() - (sizeof(uint16_t)))
				break;
			pbuf[len] = 0;
			if(packet.message == MSG_AI_NAME) {
				mainGame->dInfo.opponames[0] = BufferIO::DecodeUTF8({ reinterpret_cast<char*>(pbuf), len });
			} else {
				std::unique_lock<epro::mutex> lock(mainGame->gMutex);
				mainGame->stMessage->setText(BufferIO::DecodeUTF8({ reinterpret_cast<char*>(pbuf), len }).data());
				mainGame->PopupElement(mainGame->wMessage);
				mainGame->actionSignal.Wait(lock);
			}
			break;
		}
		case MSG_SELECT_BATTLECMD:
		case MSG_SELECT_IDLECMD: {
			record = false;
			SinglePlayRefresh();
			if(!Analyze())
				singleSignal.Wait();
			break;
		}
		case MSG_SELECT_EFFECTYN:
		case MSG_SELECT_YESNO:
		case MSG_SELECT_OPTION:
		case MSG_SELECT_CARD:
		case MSG_SELECT_TRIBUTE:
		case MSG_SELECT_UNSELECT_CARD:
		case MSG_SELECT_CHAIN:
		case MSG_SELECT_PLACE:
		case MSG_SELECT_DISFIELD:
		case MSG_SELECT_POSITION:
		case MSG_SELECT_COUNTER:
		case MSG_SELECT_SUM:
		case MSG_SORT_CARD:
		case MSG_SORT_CHAIN:
		case MSG_ROCK_PAPER_SCISSORS:
		case MSG_ANNOUNCE_RACE:
		case MSG_ANNOUNCE_ATTRIB:
		case MSG_ANNOUNCE_CARD:
		case MSG_ANNOUNCE_NUMBER: {
			record = false;
			if(mainGame->dInfo.curMsg == MSG_SELECT_CHAIN || mainGame->dInfo.curMsg == MSG_NEW_TURN) {
				SinglePlayRefresh(0, LOCATION_MZONE);
				SinglePlayRefresh(1, LOCATION_MZONE);
				SinglePlayRefresh(0, LOCATION_SZONE);
				SinglePlayRefresh(1, LOCATION_SZONE);
				record_last = true;
			}
			if(!Analyze())
				singleSignal.Wait();
			break;
		}
		default: {
			Analyze();
			break;
		}
	}
	auto* pbuf = packet.data();
	switch(mainGame->dInfo.curMsg) {
		case MSG_SHUFFLE_DECK: {
			auto player = BufferIO::Read<uint8_t>(pbuf);
			SinglePlayRefresh(player, LOCATION_DECK, 0x2181fff);
			break;
		}
		case MSG_SWAP_GRAVE_DECK: {
			auto player = BufferIO::Read<uint8_t>(pbuf);
			SinglePlayRefresh(player, LOCATION_GRAVE, 0x2181fff);
			break;
		}
		case MSG_REVERSE_DECK: {
			SinglePlayRefresh(0, LOCATION_DECK, 0x2181fff);
			SinglePlayRefresh(1, LOCATION_DECK, 0x2181fff);
			break;
		}
		case MSG_MOVE: {
			pbuf += 4;
			auto previous = CoreUtils::ReadLocInfo(pbuf, false);
			auto current = CoreUtils::ReadLocInfo(pbuf,false);
			if(previous.location && !(current.location & 0x80) && (previous.location != current.location || previous.controler != current.controler))
				SinglePlayRefreshSingle(current.controler, current.location, current.sequence);
			break;
		}
		case MSG_TAG_SWAP: {
			auto player = BufferIO::Read<uint8_t>(pbuf);
			SinglePlayRefresh(player, LOCATION_DECK, 0x181fff);
			SinglePlayRefresh(player, LOCATION_EXTRA, 0x181fff);
			break;
		}
		case MSG_NEW_PHASE:
		case MSG_SUMMONED:
		case MSG_SPSUMMONED:
		case MSG_FLIPSUMMONED:
		case MSG_CHAINED:
		case MSG_CHAIN_SOLVED:
		case MSG_DAMAGE_STEP_START:
		case MSG_DAMAGE_STEP_END: {
			SinglePlayRefresh();
			break;
		}
		case MSG_CHAIN_END:	{
			SinglePlayRefresh();
			SinglePlayRefresh(0, LOCATION_DECK);
			SinglePlayRefresh(1, LOCATION_DECK);
			break;
		}
		case MSG_RELOAD_FIELD: {
			SinglePlayReload();
			std::lock_guard<epro::mutex> lock(mainGame->gMutex);
			mainGame->dField.RefreshAllCards();
			break;
		}
	}
	if(record)
		replay_stream.insert(record_last ? replay_stream.end() : replay_stream.begin(), std::move(packet));
	new_replay.WriteStream(replay_stream);
	new_replay.Flush();
	return is_continuing;
}
void SingleMode::SinglePlayRefresh(uint8_t player, uint8_t location, uint32_t flag) {
	std::vector<uint8_t> buffer;
	uint32_t len = 0;
	OCG_QueryInfo info{ flag, player, location };
	auto buff = OCG_DuelQueryLocation(pduel, &len, &info);
	if(len == 0)
		return;
	buffer.resize(buffer.size() + len);
	memcpy(buffer.data(), buff, len);
	mainGame->gMutex.lock();
	mainGame->dField.UpdateFieldCard(mainGame->LocalPlayer(player), location, buffer.data());
	mainGame->gMutex.unlock();
	buffer.insert(buffer.begin(), location);
	buffer.insert(buffer.begin(), player);
	replay_stream.emplace_back(MSG_UPDATE_DATA, buffer.data(), buffer.size());
}
void SingleMode::SinglePlayRefreshSingle(uint8_t player, uint8_t location, uint8_t sequence, uint32_t flag) {
	std::vector<uint8_t> buffer;
	uint32_t len = 0;
	OCG_QueryInfo info{ flag, player, location, sequence };
	auto buff = OCG_DuelQuery(pduel, &len, &info);
	if(buff == nullptr)
		return;
	buffer.resize(buffer.size() + len);
	memcpy(buffer.data(), buff, len);
	mainGame->gMutex.lock();
	mainGame->dField.UpdateCard(mainGame->LocalPlayer(player), location, sequence, buffer.data());
	mainGame->gMutex.unlock();
	buffer.insert(buffer.begin(), sequence);
	buffer.insert(buffer.begin(), location);
	buffer.insert(buffer.begin(), player);
	replay_stream.emplace_back(MSG_UPDATE_CARD, buffer.data(), buffer.size());
}
void SingleMode::SinglePlayRefresh(uint32_t flag) {
	for(int p = 0; p < 2; p++)
		for(int loc = LOCATION_HAND; loc != LOCATION_GRAVE; loc *= 2)
			SinglePlayRefresh(p, loc, flag);
}
void SingleMode::SinglePlayReload() {
	for(int p = 0; p < 2; p++)
		for(int loc = LOCATION_DECK; loc != LOCATION_OVERLAY; loc *= 2)
			SinglePlayRefresh(p, loc, 0x2ffdfff);
}

}
