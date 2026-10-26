// GHS-TRACE: guest AI probe for Gregory Horror Show (Europe), SLES_519.33. Changes nothing in the game.
// GHS_ACTORTRACE=1: wraps the functions that store an actor's action word (+0xC0, from the asm) and logs
// [GHS-ACT] when one of them changes an actor's +0xC0 or state +0x68 (actor array 0x4717A0, 0x6C0 bytes each),
// with the caller; also the encounter start FUN_217970(actor, 0) and FUN_156FB0's non-zero results.
// Found with it (2026-10-23, Catherine at the AAA save): see zz_ghs_cheats.cpp.
// tools/install_overrides.ps1 copies this into the runner.

#include "game_overrides.h"
#include "ps2_runtime.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <tuple>
#include <utility>

namespace ghs_actor_trace
{
    constexpr uint32_t kActors = 0x004717A0u, kActorBytes = 0x6C0u, kCount = 48u;
    constexpr std::array<uint32_t, 63> kFns = {
        0x107690, 0x1077a0, 0x128c10, 0x129130, 0x132930, 0x13f930, 0x140110, 0x1765a0, 0x17c2e0, 0x17e3c0, 0x17ecd0,
        0x181060, 0x196fc0, 0x198430, 0x198920, 0x198cc0, 0x199000, 0x199620, 0x1be690, 0x1c11b0, 0x1c2b60, 0x1c3530,
        0x1cdbd0, 0x1cdd90, 0x1cfa70, 0x1d1c40, 0x1d9380, 0x213d50, 0x217970, 0x21b310, 0x2217a0, 0x2383c0, 0x2410e0,
        0x242bc0, 0x242ef0, 0x2430e0, 0x249850, 0x24ff60, 0x259db0, 0x25bb00, 0x25f340, 0x260bf0, 0x262300, 0x2673e0,
        0x267830, 0x26bcb0, 0x26ebd0, 0x26f6d0, 0x27c6f0, 0x297120, 0x29deb0, 0x29ff90, 0x2a08b0, 0x2a25f0, 0x2a8310,
        0x2a8bd0, 0x2a9d10, 0x2aa530, 0x2b4290, 0x2b5aa0, 0x2bdcb0, 0x2c0020, 0x2c73c0};
    std::array<PS2Runtime::RecompiledFunction, kFns.size()> g_orig{};
    PS2Runtime::RecompiledFunction g_check = nullptr;

    uint32_t rd(const uint8_t *rdram, uint32_t addr)
    {
        uint32_t v;
        std::memcpy(&v, rdram + addr, 4);
        return v;
    }

    template <size_t I>
    void wrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31), a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5);
        uint32_t before[kCount][2];
        for (uint32_t i = 0; i < kCount; ++i)
        {
            before[i][0] = rd(rdram, kActors + i * kActorBytes + 0xC0u);
            before[i][1] = rd(rdram, kActors + i * kActorBytes + 0x68u);
        }
        if (kFns[I] == 0x217970u && a1 == 0u)
            std::fprintf(stderr, "[GHS-ACT] encounter start FUN_217970(%u, 0) from ra 0x%x\n", a0, ra);
        g_orig[I](rdram, ctx, runtime);
        for (uint32_t i = 0; i < kCount; ++i)
        {
            const uint32_t c0 = rd(rdram, kActors + i * kActorBytes + 0xC0u), s = rd(rdram, kActors + i * kActorBytes + 0x68u);
            if (c0 != before[i][0] || s != before[i][1])
                std::fprintf(stderr, "[GHS-ACT] FUN_%x(a0 %x, a1 %x) ra 0x%x: actor %u action %x -> %x, state %x -> %x\n", kFns[I], a0, a1, ra, i,
                             before[i][0], c0, before[i][1], s);
        }
    }

    void check(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31), a0 = getRegU32(ctx, 4);
        g_check(rdram, ctx, runtime);
        if (const uint32_t v0 = getRegU32(ctx, 2); v0 != 0u)
            std::fprintf(stderr, "[GHS-ACT] FUN_156FB0(%x) = %u from ra 0x%x, contact actor %d\n", a0, v0, ra,
                         static_cast<int32_t>(rd(rdram, 0x0047F128u)));
    }

    // Who runs a guest's hunting/vision code: each new (function, caller, actor) once, and the vision test's hits.
    constexpr std::array<uint32_t, 6> kWho = {0x1bfd60, 0x1c1b50, 0x1cb760, 0x1c1010, 0x1c56a0, 0x1c5430};
    std::array<PS2Runtime::RecompiledFunction, kWho.size()> g_whoOrig{};
    std::set<std::tuple<uint32_t, uint32_t, uint32_t>> g_seenCalls;
    template <size_t I>
    void who(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31), a0 = getRegU32(ctx, 4);
        if (g_seenCalls.insert({kWho[I], ra, a0}).second)
            std::fprintf(stderr, "[GHS-ACT] call FUN_%x(a0 %x, a1 %x) from ra 0x%x\n", kWho[I], a0, getRegU32(ctx, 5), ra);
        g_whoOrig[I](rdram, ctx, runtime);
        if (kWho[I] == 0x1cb760u && getRegU32(ctx, 2) != 0u)
            std::fprintf(stderr, "[GHS-ACT] FUN_1cb760(%x) sees: %x, ra 0x%x\n", a0, getRegU32(ctx, 2), ra);
    }
    template <size_t... I>
    void hookWho(PS2Runtime &runtime, std::index_sequence<I...>)
    {
        ((g_whoOrig[I] = runtime.lookupFunction(kWho[I]), g_whoOrig[I] ? (runtime.replaceFunction(kWho[I], &who<I>), 0) : 0), ...);
    }

    template <size_t... I>
    void hookAll(PS2Runtime &runtime, std::index_sequence<I...>)
    {
        ((g_orig[I] = runtime.lookupFunction(kFns[I]), g_orig[I] ? (runtime.replaceFunction(kFns[I], &wrap<I>), 0) : 0), ...);
    }

    void apply(PS2Runtime &runtime)
    {
        if (!std::getenv("GHS_ACTORTRACE"))
            return;
        hookAll(runtime, std::make_index_sequence<kFns.size()>{});
        hookWho(runtime, std::make_index_sequence<kWho.size()>{});
        if ((g_check = runtime.lookupFunction(0x00156FB0u)))
            runtime.replaceFunction(0x00156FB0u, &check);
        std::fprintf(stderr, "[GHS-ACT] tracing %zu functions\n", kFns.size());
    }

    const ps2_game_overrides::Descriptor kDescriptor{"Gregory Horror Show (EU): guest AI trace", "SLES_519.33", 0x00100008u, 0u, apply};
    const ps2_game_overrides::AutoRegister kRegister(kDescriptor);
}
