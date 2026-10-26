// GHS-TRACE: timing probe for Gregory Horror Show (Europe), SLES_519.33 (docs\TICK_RATE.md). Changes nothing in the
// game. GHS_TIMING_TRACE=1: once a second [GHS-TIMING] wall ms, VBlanks, logic frames (gp+0x240), fps word, in-game
// clock, play time, animation step, video mode, game state, actor 0's position and character 0's animation timer.
// GHS_TIMING_RAMDUMP=<dir>: while <dir>\on exists, EE RAM 0x400000..0x800000 goes to <dir>\ram_<ms>.bin every 250 ms.
// tools/install_overrides.ps1 copies this into the runner.

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace ghs_timing_trace
{
    constexpr uint32_t kGp = 0x0043B770u;

    template <typename T>
    T rd(const uint8_t *rdram, uint32_t addr)
    {
        T v;
        std::memcpy(&v, rdram + (addr & PS2_RAM_MASK), sizeof(v));
        return v;
    }

    void traceThread(PS2Runtime *runtime)
    {
        const uint8_t *rdram = runtime->memory().getRDRAM();
        const char *dumpDir = std::getenv("GHS_TIMING_RAMDUMP");
        const auto t0 = std::chrono::steady_clock::now();
        for (uint64_t n = 1;; ++n)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            if (n % 4u == 0u)
                std::fprintf(stderr, "[GHS-TIMING] ms=%lld vblank=%llu frames=%u fps=%u clock=%u playtime=%u anim=%u mode=%u state=0x%x x=%.2f z=%.2f animtimer=%d\n", ms,
                             static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), rd<uint32_t>(rdram, kGp + 0x240u),
                             rd<uint32_t>(rdram, 0x005E9920u), rd<uint32_t>(rdram, 0x005E9934u), rd<uint32_t>(rdram, 0x004503BCu),
                             rd<uint32_t>(rdram, kGp - 0x427Cu), rd<uint32_t>(rdram, kGp + 0x2880u), rd<uint32_t>(rdram, kGp + 0x23ACu),
                             rd<float>(rdram, 0x00471820u), rd<float>(rdram, 0x00471828u), rd<int16_t>(rdram, 0x004CC0D0u + 0x13Au));
            std::FILE *on = dumpDir ? std::fopen((std::string(dumpDir) + "\\on").c_str(), "rb") : nullptr;
            if (on)
            {
                std::fclose(on);
                if (std::FILE *f = std::fopen((std::string(dumpDir) + "\\ram_" + std::to_string(ms) + ".bin").c_str(), "wb"))
                {
                    std::fwrite(rdram + 0x400000u, 1, 0x400000u, f);
                    std::fclose(f);
                }
            }
        }
    }

    void apply(PS2Runtime &runtime)
    {
        if (std::getenv("GHS_TIMING_TRACE"))
            std::thread(traceThread, &runtime).detach();
    }

    const ps2_game_overrides::Descriptor kDescriptor{"Gregory Horror Show (EU): timing trace", "SLES_519.33", 0x00100008u, 0u, apply};
    const ps2_game_overrides::AutoRegister kRegister(kDescriptor);
}
