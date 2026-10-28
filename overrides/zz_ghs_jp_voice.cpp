// Japanese voices with the English game (F2 "Japanese voices", jp_voice= in ghs_settings.ini, GHS_JP_VOICE=0/1 wins
// at start). tools\ghs_jp_voice.py copies the Japanese disc's voice pool to game\NGYSTM_V.BIN and writes
// game\ghs_jp_voice.txt: for each EU clip the Japanese recording of the same line, as sectors in that pool.
// The stream channel's "start" step (0x2B7588, inside the state machine that FUN_002b5aa0 runs inline) reads the clip
// start-sector table (u64 at 0x342000) and sends IOP command 8 with 0x2B8B70(8, sp+208, 6): words [channel, disc sector
// = table[clip] + the pool's sector (gp+0x3FF0), bytes = (table[clip + 1] - table[clip]) * 2048, 24, format, ...].
// For a mapped clip (channel s2, clip at +24) that call gets the Japanese clip's sector and length instead. On the
// runner's virtual disc NGYSTM_V.BIN follows NGYSTM_U.BIN directly (files are laid out in name order), so its sectors
// are EU pool size + n. Volume, loop flag and format (0x340350 records) stay the EU clip's: every mapped clip has the
// same format code. "mute" lines (EU lines the JP game shows without a voice) point at a silent clip of the EU pool. tools/install_overrides.ps1 copies this into the runner.

#include "game_overrides.h"
#include "ps2_runtime.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

extern bool g_ghsJpVoice; // ps2_pad.cpp: F2 setting, ghs_settings.ini, GHS_JP_VOICE
extern int g_ghsJpVoiceClips; // ps2_pad.cpp: clips loaded (0: not set up), shown in F2

namespace ghs_jp_voice
{
    constexpr uint32_t kSendCommand = 0x002B8B70u;
    constexpr uint32_t kStreamStartReturn = 0x002B7638u; // after `jal 0x2B8B70` at 0x2B7630
    constexpr uint32_t kTable = 0x00342000u;
    constexpr uint32_t kClips = 989u;

    struct Clip
    {
        uint64_t start = 0, end = 0; // relative to the EU pool's first sector; end 0: keep the EU clip
    };
    std::vector<Clip> g_clips;
    PS2Runtime::RecompiledFunction g_original = nullptr;

    void sendCommand(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_ghsJpVoice && getRegU32(ctx, 31) == kStreamStartReturn && getRegU32(ctx, 4) == 8u && getRegU32(ctx, 6) == 6u)
        {
            uint32_t clip = 0;
            std::memcpy(&clip, rdram + ((getRegU32(ctx, 18) + 24u) & PS2_RAM_MASK), 4); // s2 = channel
            if (clip < g_clips.size() && g_clips[clip].end != 0)
            {
                uint8_t *words = rdram + (getRegU32(ctx, 5) & PS2_RAM_MASK);
                uint64_t euStart = 0;
                std::memcpy(&euStart, rdram + ((kTable + clip * 8u) & PS2_RAM_MASK), 8);
                uint32_t sector = 0;
                std::memcpy(&sector, words + 4, 4);
                sector = static_cast<uint32_t>(sector - euStart + g_clips[clip].start);
                const uint32_t bytes = static_cast<uint32_t>((g_clips[clip].end - g_clips[clip].start) * 2048u);
                std::memcpy(words + 4, &sector, 4);
                std::memcpy(words + 8, &bytes, 4);
                static int s_logged = 0;
                if (s_logged < 5 && ++s_logged)
                    std::fprintf(stderr, "[GHS-JPVOICE] clip %u -> disc sector %u, %u bytes\n", clip, sector, bytes);
            }
        }
        g_original(rdram, ctx, runtime);
    }

    PS2Runtime::RecompiledFunction g_play = nullptr; // GHS-TRACE: 0x2B5230(clip, flag), starts a stream channel
    void tracePlay(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::fprintf(stderr, "[GHS-JPVOICE] play clip=%u flag=%u pc=%x\n", getRegU32(ctx, 4), getRegU32(ctx, 5), ctx->pc);
        g_play(rdram, ctx, runtime);
    }

    std::string isoName(const std::filesystem::directory_entry &e) // iop_cdvd.cpp's sort key
    {
        std::string s = e.path().filename().string();
        for (char &c : s)
            c = static_cast<unsigned char>(c) < 0x80u ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : '_';
        std::error_code ec;
        return e.is_directory(ec) ? s : s + ";1";
    }

    // The virtual disc puts NGYSTM_V.BIN's sectors right after NGYSTM_U.BIN's only if no other entry sorts between them.
    bool poolFollows(uint64_t euSectors, uint64_t jpSectors)
    {
        std::error_code ec;
        std::vector<std::string> names;
        for (const auto &e : std::filesystem::directory_iterator(".", ec))
            names.push_back(isoName(e));
        std::sort(names.begin(), names.end());
        const auto u = std::find(names.begin(), names.end(), "NGYSTM_U.BIN;1");
        return u != names.end() && u + 1 != names.end() && u[1] == "NGYSTM_V.BIN;1" &&
               std::filesystem::file_size("NGYSTM_U.BIN", ec) == euSectors * 2048u &&
               std::filesystem::file_size("NGYSTM_V.BIN", ec) == jpSectors * 2048u;
    }

    void apply(PS2Runtime &runtime)
    {
        FILE *f = std::fopen("ghs_jp_voice.txt", "r");
        if (!f || !runtime.hasFunction(kSendCommand))
        {
            if (f)
                std::fclose(f);
            return;
        }
        char line[128];
        unsigned euClips = 0, jpClips = 0;
        unsigned long long euSectors = 0, jpSectors = 0;
        std::vector<Clip> clips(kClips);
        int count = 0, muted = 0;
        while (std::fgets(line, sizeof(line), f))
        {
            unsigned eu = 0, jp = 0;
            unsigned long long a = 0, b = 0;
            if (std::sscanf(line, "pool %u %llu %u %llu", &euClips, &euSectors, &jpClips, &jpSectors) == 4)
                continue;
            // A line the JP game never voiced: one of the EU pool's silent clips instead of the English recording.
            if (std::sscanf(line, "mute %u %llu %llu", &eu, &a, &b) == 3 && eu < kClips && a < b && b <= euSectors)
            {
                clips[eu] = {a, b};
                ++muted;
                continue;
            }
            if (std::sscanf(line, "%u %u %llu %llu", &eu, &jp, &a, &b) == 4 && eu < kClips && a < b && b <= jpSectors)
            {
                clips[eu] = {euSectors + a, euSectors + b};
                ++count;
            }
        }
        std::fclose(f);
        if (euClips != kClips || !count || !poolFollows(euSectors, jpSectors))
        {
            std::fprintf(stderr, "[GHS-JPVOICE] ghs_jp_voice.txt / NGYSTM_V.BIN don't match this game folder: "
                                 "run tools\\ghs_jp_voice.py again\n");
            return;
        }
        g_clips = std::move(clips);
        g_ghsJpVoiceClips = count;
        g_original = runtime.lookupFunction(kSendCommand);
        runtime.replaceFunction(kSendCommand, &sendCommand);
        if (std::getenv("GHS_JP_VOICE_TRACE") && runtime.hasFunction(0x002B5230u))
        {
            g_play = runtime.lookupFunction(0x002B5230u);
            runtime.replaceFunction(0x002B5230u, &tracePlay);
        }
        std::fprintf(stderr, "[GHS-JPVOICE] %d clips have a Japanese voice, %d none (%s)\n", count, muted, g_ghsJpVoice ? "on" : "off");
    }

    const ps2_game_overrides::Descriptor kDescriptor{"Gregory Horror Show (EU): Japanese voices", "SLES_519.33", 0x00100008u, 0u, apply};
    const ps2_game_overrides::AutoRegister kRegister(kDescriptor);
}
