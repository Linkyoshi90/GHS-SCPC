// Gregory Horror Show (Europe), SLES_519.33: the 60 Hz mode plays at the 50 Hz game speed (docs\TICK_RATE.md).
// Most of the game times itself by the fps word 0x5E9920 (50 / 60) and runs at the same speed in both modes; these
// parts count raw frames instead, so in the 60 Hz mode they ran 1.2x as fast. Here they get 50/60 of their step
// while the fps word reads 60; in the 50 Hz mode nothing changes. tools/install_overrides.ps1 copies this into
// the runner.

#include "game_overrides.h"
#include "ps2_runtime.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <unordered_map>

namespace ghs_60hz_speed
{
    constexpr uint32_t kFps = 0x005E9920u;

    uint32_t rd32(const uint8_t *rdram, uint32_t addr)
    {
        uint32_t v;
        std::memcpy(&v, rdram + (addr & PS2_RAM_MASK), 4);
        return v;
    }
    bool sixtyHz(const uint8_t *rdram) { return rd32(rdram, kFps) == 60u; }

    // Walking and turning: the 2D-style controls (and the same code at 0x1609D8 / 0x2962A0..0x296C40) make the
    // per-frame step with a literal 60.0, not the fps word, and pass it in $f13 to FUN_18FD80 (move along the
    // heading) or FUN_1859D0 (turn). Measured: 3.9 units a frame in both modes = 200 units/s at 50 Hz, 232 at 60.
    constexpr uint32_t kMoveRa[] = {0x00150238u, 0x00150B20u, 0x00151108u, 0x00160A20u, 0x0029677Cu, 0x00296C68u};
    constexpr uint32_t kTurnRa[] = {0x00150398u, 0x00150688u, 0x002962F8u, 0x00296530u};
    PS2Runtime::RecompiledFunction g_move, g_turn;
    template <size_t K>
    bool calledFrom(const uint32_t (&list)[K], uint32_t ra)
    {
        return std::find(std::begin(list), std::end(list), ra) != std::end(list);
    }
    void move(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (calledFrom(kMoveRa, getRegU32(ctx, 31)) && sixtyHz(rdram))
            ctx->f[13] *= 50.0f / 60.0f;
        g_move(rdram, ctx, runtime);
    }
    void turn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (calledFrom(kTurnRa, getRegU32(ctx, 31)) && sixtyHz(rdram))
            ctx->f[13] *= 50.0f / 60.0f;
        g_turn(rdram, ctx, runtime);
    }

    // Pad auto-repeat (FUN_190F90, once a frame per pad; pad block 0x59DC40 + pad x 0xAC): the counter at +0x9E runs
    // from 0xF2 (first repeat after 13 frames) or 0xFC (then every 3) to 0xFF in raw frames. At 60 Hz every 6th plain
    // step is undone, so it repeats after the same time as at 50 Hz (260 ms, then every 60 ms).
    // Recompiled branches return to the dispatcher, which re-enters the function at the target, so the wrapper sits on
    // every entry of the function and checks the counter when the function really returns (pc = ra, outside it).
    constexpr uint32_t kPadRead = 0x00190F90u, kPadReadEnd = 0x00191680u, kPadBlock = 0x0059DC40u;
    std::unordered_map<uint32_t, PS2Runtime::RecompiledFunction> g_padRead;
    uint32_t g_pad = 0u, g_padSteps[2] = {};
    uint16_t g_padCounter = 0u;
    void padRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pc = ctx->pc;
        if (pc == kPadRead)
        {
            g_pad = getRegU32(ctx, 4) & 1u;
            std::memcpy(&g_padCounter, rdram + kPadBlock + g_pad * 0xACu + 0x9Eu, 2);
        }
        g_padRead[pc](rdram, ctx, runtime);
        if ((ctx->pc < kPadRead || ctx->pc >= kPadReadEnd) && ctx->pc == getRegU32(ctx, 31) && sixtyHz(rdram))
        {
            uint8_t *counter = rdram + kPadBlock + g_pad * 0xACu + 0x9Eu;
            uint16_t now;
            std::memcpy(&now, counter, 2);
            if (now == static_cast<uint16_t>(g_padCounter + 1u) && ++g_padSteps[g_pad] % 6u == 0u)
                std::memcpy(counter, &g_padCounter, 2); // a plain step (not a repeat or a release)
        }
    }

    void apply(PS2Runtime &runtime)
    {
        g_move = runtime.lookupFunction(0x0018FD80u);
        runtime.replaceFunction(0x0018FD80u, &move);
        g_turn = runtime.lookupFunction(0x001859D0u);
        runtime.replaceFunction(0x001859D0u, &turn);
        for (uint32_t pc = kPadRead; pc < kPadReadEnd; pc += 4u)
            if (runtime.hasFunction(pc))
            {
                g_padRead[pc] = runtime.lookupFunction(pc);
                runtime.replaceFunction(pc, &padRead);
            }
    }

    const ps2_game_overrides::Descriptor kDescriptor{"Gregory Horror Show (EU): 60 Hz at 50 Hz game speed", "SLES_519.33", 0x00100008u, 0u, apply};
    const ps2_game_overrides::AutoRegister kRegister(kDescriptor);
}
