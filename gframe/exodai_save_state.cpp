#include "exodai_save_state.h"

#include "fmt.h"
#include "game.h"
#include "file_stream.h"
#include "utils.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// Helpers duplicated from the original single_mode.cpp implementation
// (SHA256 / iso_utc_now / positions_dir / ensure_dir / make_basename).
// They were moved here so the LAN-host Ctrl+S path can reuse them
// without pulling single_mode as a dep; single_mode.cpp now calls
// ExodAIWriteDuelStateToFile.

namespace ygo {
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
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec && ec != std::make_error_code(std::errc::file_exists)) {
        // Log real failures but don't crash — mirrors the original behavior.
        fmt::print(stderr, "Save: create_directories('{}') failed: {}\n", path, ec.message());
        return false;
    }
    return true;
}

std::string make_basename() {
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

// SHA-256 over the script corpus reachable via Game::script_dirs.
//
// Algorithm:
//   1. Walk mainGame->script_dirs in order. Skip the "archives" sentinel
//      — files packaged in .ypk archives are not enumerated here. The
//      ExodAI deployment loads from ./script/ on disk, so plain-file
//      enumeration is sufficient. If we later move to archive-packaged
//      corpora, extend this to include archive contents.
//   2. For each directory, list *.lua at depth 0 — script_dirs already
//      has subdirectories appended as their own entries (see
//      Game::PopulateResourcesDirectories), so recursion is not needed.
//   3. Dedup by basename: first-match-wins, mirroring Game::FindScript's
//      script_dirs walk. This ensures the hash reflects the bytes the
//      engine would actually load, not every .lua file that happens to
//      exist on disk.
//   4. Sort entries by basename (ascending, UTF-8 byte order) for
//      stability across filesystem enumeration order.
//   5. For each (basename, full_path), SHA-256 update with:
//        u32_le(name_len) | name_bytes | u32_le(content_len) | content_bytes
//      Length-prefix framing prevents collisions where distinct
//      (name, content) pairs could share a concatenated representation.
//   6. Hex digest is returned. Cached in get_script_corpus_hash() via
//      std::call_once so the cost — ~14k Lua files, a few hundred ms —
//      is paid at most once per process.
//
// Returns "" if mainGame is null or script_dirs is empty (defensive;
// in practice script_dirs is populated very early at startup).
std::string compute_script_corpus_hash_impl() {
    if (mainGame == nullptr || mainGame->script_dirs.empty())
        return std::string();

    std::vector<std::pair<std::string, epro::path_string>> entries;
    std::unordered_set<std::string> seen;
    static const std::vector<epro::path_stringview> lua_ext{ EPRO_TEXT("lua") };
    for (const auto& dir : mainGame->script_dirs) {
        if (dir == EPRO_TEXT("archives")) continue;
        auto files = Utils::FindFiles(dir, lua_ext, 0);
        for (auto& name : files) {
            std::string basename_u8 = Utils::ToUTF8IfNeeded(epro::path_stringview{ name });
            if (seen.insert(basename_u8).second)
                entries.emplace_back(std::move(basename_u8), dir + name);
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    Sha256 sha;
    auto put_u32_le = [&sha](uint32_t v) {
        uint8_t b[4] = {
            static_cast<uint8_t>(v & 0xff),
            static_cast<uint8_t>((v >> 8) & 0xff),
            static_cast<uint8_t>((v >> 16) & 0xff),
            static_cast<uint8_t>((v >> 24) & 0xff)
        };
        sha.update(b, 4);
    };
    for (const auto& entry : entries) {
        const std::string& bn = entry.first;
        const epro::path_string& path = entry.second;
        FileStream f(path.data(), FileStream::in | FileStream::binary);
        if (f.fail()) continue;  // missing/unreadable entry: skip rather than poison
        std::string content{
            std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>()
        };
        put_u32_le(static_cast<uint32_t>(bn.size()));
        sha.update(bn.data(), bn.size());
        put_u32_le(static_cast<uint32_t>(content.size()));
        sha.update(content.data(), content.size());
    }
    return sha.finalize();
}

std::string get_script_corpus_hash() {
    static std::once_flag once;
    static std::string cached;
    std::call_once(once, [] { cached = compute_script_corpus_hash_impl(); });
    return cached;
}

}  // namespace

bool ExodAIWriteDuelStateToFile(OCG_Duel pduel, const char* source_tag, std::string& out_msg) {
    if (pduel == 0) {
        out_msg = "Save: no active duel handle.";
        return false;
    }
    if (source_tag == nullptr) source_tag = "edopro-hotkey";

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

    {
        std::ofstream f(blob_path, std::ios::binary);
        if (!f) {
            out_msg = fmt::format("Save: cannot open '{}' for write", blob_path);
            OCG_FreeSaveBuffer(blob);
            return false;
        }
        f.write(static_cast<const char*>(blob), size);
    }

    std::string blob_hash = sha256_hex(blob, size);
    OCG_FreeSaveBuffer(blob);

    // Manual JSON formatting — avoids pulling nlohmann/json into the
    // EDOPro dep tree. Schema mirrors src/state_io.py (schema_version 1).
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
        f << "  \"provenance\": {\"source\": \"" << source_tag << "\"},\n";
        f << "  \"script_corpus_hash\": \"" << get_script_corpus_hash() << "\",\n";
        f << "  \"engine_build_hash\": \"\",\n";
        f << "  \"blob_sha256\": \"" << blob_hash << "\",\n";
        f << "  \"tags\": [\"" << source_tag << "\"],\n";
        f << "  \"notes\": \"\",\n";
        f << "  \"correct_action\": null,\n";
        f << "  \"card_codes\": []\n";
        f << "}\n";
    }
    out_msg = fmt::format("Saved {} bytes to {} (+sidecar)", size, blob_path);
    return true;
}

}  // namespace ygo
