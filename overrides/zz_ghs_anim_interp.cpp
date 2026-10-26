// Smooth character animation for Gregory Horror Show (Europe), SLES_519.33 (docs/FORMATS.md 5f).
// The bone sampler FUN_00213d50 poses every bone at keyframe timer / 60 (30 keyframes per second) and
// ignores the remainder. With GHS_ANIM_INTERP=1 or the F2 setting, this lets the original run and then
// rewrites the bone slots it wrote with the blend of that keyframe and the next one at timer / 60.
// Off: the original runs untouched. tools/install_overrides.ps1 copies this into the runner.

#include "game_overrides.h"
#include "ps2_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern bool g_ghsAnimInterp; // ps2_pad.cpp: F2 setting, ghs_settings.ini, GHS_ANIM_INTERP

namespace ghs_anim_interp
{
    constexpr uint32_t kSampler = 0x00213D50u, kSamplerEnd = 0x00214458u;
    constexpr uint32_t kCharState = 0x004CC0D0u; // + char * 0x2D0
    constexpr uint32_t kBoneSlots = 0x004ED730u; // + slot * 0x130: +0x40 translation, +0x50 rotation
    constexpr float kPi = 3.14159274f, kTwoPi = 6.28318548f; // FUN_00185a40's constants

    // Every function-table slot inside the sampler: the function itself and the blocks a scheduler
    // checkpoint resumes it at, so the blend also runs when the sampler finishes after a resume.
    PS2Runtime::RecompiledFunction g_original[(kSamplerEnd - kSampler) / 4u] = {};

    template <typename T>
    T rd(const uint8_t *rdram, uint32_t addr)
    {
        T v;
        std::memcpy(&v, rdram + (addr & PS2_RAM_MASK), sizeof(v));
        return v;
    }

    void wr(uint8_t *rdram, uint32_t addr, float v)
    {
        std::memcpy(rdram + (addr & PS2_RAM_MASK), &v, sizeof(v));
    }

    float wrapPi(float a) // FUN_00185a40
    {
        while (a > kPi)
            a -= kTwoPi;
        while (a < -kPi)
            a += kTwoPi;
        return a;
    }

    // One keyframe as the sampler stores it: translation in engine units, rotation (x, y, z) in radians.
    struct Key
    {
        float t[3], r[3];
    };

    Key readKey(const uint8_t *rdram, uint32_t rec, bool isFloat) // rec: (tx, ty, tz, ry, rx, rz)
    {
        Key k;
        for (int i = 0; i < 3; ++i)
            k.t[i] = isFloat ? 64.0f * rd<float>(rdram, rec + 4u * i) : (1.0f / 64.0f) * rd<int16_t>(rdram, rec + 2u * i);
        static constexpr int kRotIndex[3] = {4, 3, 5};
        for (int i = 0; i < 3; ++i)
            k.r[i] = isFloat ? rd<float>(rdram, rec + 4u * kRotIndex[i]) : rd<int16_t>(rdram, rec + 2u * kRotIndex[i]) / 4096.0f;
        return k;
    }

    void blend(uint8_t *rdram, uint32_t charOffset)
    {
        const uint32_t st = kCharState + charOffset;
        const int8_t mode = rd<int8_t>(rdram, st + 0x135u), anim = rd<int8_t>(rdram, st + 0x137u), queued = rd<int8_t>(rdram, st + 0x138u);
        const int16_t start = rd<int16_t>(rdram, st + 0x13Cu), end = rd<int16_t>(rdram, st + 0x13Eu), step = rd<int16_t>(rdram, st + 0x142u);
        // The sampler advanced the timer after sampling (backwards in mode 4); undo that for the sampled time.
        const int timer = static_cast<int16_t>(rd<int16_t>(rdram, st + 0x13Au) + (mode == 4 ? step : -step));
        if (timer <= 0 || anim < 0 || timer % 60 == 0)
            return; // on a keyframe the original values are exact
        const int frame = timer / 60;
        const float f = (timer % 60) / 60.0f;
        const uint32_t list = rd<uint32_t>(rdram, st + 0x74u + 4u * anim);
        const uint32_t count = list ? rd<uint32_t>(rdram, list) : 0u;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t track = list + rd<uint32_t>(rdram, list + 4u + 4u * i);
            const int frames = rd<uint16_t>(rdram, track);
            if (frame >= frames)
                continue; // the original left this bone alone
            // Next keyframe; past the end of a looping animation (mode 2, nothing queued) the one it restarts at.
            // Otherwise hold, as the original does on its last keyframe.
            int next = frame + 1;
            if (next >= frames || next * 60 >= end)
                next = (mode == 2 && queued < 0) ? start / 60 : -1;
            if (next < 0 || next >= frames || next == frame)
                continue;

            const bool isFloat = rd<uint8_t>(rdram, track + 3u) != 0u;
            const uint32_t size = isFloat ? 24u : 12u;
            const Key a = readKey(rdram, track + 4u + size * frame, isFloat), b = readKey(rdram, track + 4u + size * next, isFloat);
            const int16_t slot = rd<int16_t>(rdram, st + 0x22u + 2u * rd<uint8_t>(rdram, track + 2u));
            const uint32_t out = kBoneSlots + static_cast<uint32_t>(slot * 0x130);
            for (int j = 0; j < 3; ++j)
                wr(rdram, out + 0x40u + 4u * j, a.t[j] + (b.t[j] - a.t[j]) * f);

            // Shortest way per axis; a jump over 90 degrees between keyframes is an Euler flip or a cut,
            // so keep this keyframe's rotation rather than spin the limb through it.
            float d[3];
            bool jump = false;
            for (int j = 0; j < 3; ++j)
            {
                d[j] = wrapPi(b.r[j] - a.r[j]);
                jump |= std::fabs(d[j]) > 0.5f * kPi;
            }
            if (jump)
                continue;
            for (int j = 0; j < 3; ++j)
            {
                const float r = a.r[j] + d[j] * f;
                wr(rdram, out + 0x50u + 4u * j, isFloat ? wrapPi(r) : r);
            }
        }
    }

    // GHS-TRACE: GHS_ANIM_CHECK=1: after every original sample, readKey() at the sampled keyframe must equal
    // what the sampler wrote, bit for bit (proves the decoding the blend relies on); also logs every character's
    // first track per call ([GHS-ANIM]) to compare the motion with the setting off and on.
    const bool g_check = std::getenv("GHS_ANIM_CHECK") != nullptr;
    uint64_t g_checkCalls = 0, g_checkTracks = 0, g_checkBad = 0;
    void verify(const uint8_t *rdram, uint32_t charOffset)
    {
        const uint32_t st = kCharState + charOffset;
        const int8_t mode = rd<int8_t>(rdram, st + 0x135u), anim = rd<int8_t>(rdram, st + 0x137u);
        const int timer = static_cast<int16_t>(rd<int16_t>(rdram, st + 0x13Au) + (mode == 4 ? 1 : -1) * rd<int16_t>(rdram, st + 0x142u));
        const uint32_t list = anim >= 0 ? rd<uint32_t>(rdram, st + 0x74u + 4u * anim) : 0u;
        if (timer < 0 || !list)
            return;
        ++g_checkCalls;
        for (uint32_t i = 0, count = rd<uint32_t>(rdram, list); i < count; ++i)
        {
            const uint32_t track = list + rd<uint32_t>(rdram, list + 4u + 4u * i);
            if (timer / 60 >= rd<uint16_t>(rdram, track))
                continue;
            const bool isFloat = rd<uint8_t>(rdram, track + 3u) != 0u;
            Key k = readKey(rdram, track + 4u + (isFloat ? 24u : 12u) * (timer / 60), isFloat);
            for (float &r : k.r)
                r = isFloat ? wrapPi(r) : r;
            const uint32_t out = kBoneSlots + static_cast<uint32_t>(rd<int16_t>(rdram, st + 0x22u + 2u * rd<uint8_t>(rdram, track + 2u)) * 0x130);
            ++g_checkTracks;
            if (std::memcmp(k.t, rdram + (out + 0x40u & PS2_RAM_MASK), 12) || std::memcmp(k.r, rdram + (out + 0x50u & PS2_RAM_MASK), 12))
                ++g_checkBad;
        }
    }
    // After the blend: largest per-call rotation change of any animated bone (shortest way) and how often a
    // stored angle crossed +-pi, i.e. a limb flip would show up here as a step near pi.
    float g_prevRot[4096][3];
    uint8_t g_prevAnim[4096];
    float g_maxStep = 0.0f;
    uint64_t g_wraps = 0;
    void motion(const uint8_t *rdram, uint32_t charOffset)
    {
        const uint32_t st = kCharState + charOffset;
        const int8_t anim = rd<int8_t>(rdram, st + 0x137u);
        const uint32_t list = anim >= 0 ? rd<uint32_t>(rdram, st + 0x74u + 4u * anim) : 0u;
        for (uint32_t i = 0, count = list ? rd<uint32_t>(rdram, list) : 0u; i < count; ++i)
        {
            const uint32_t track = list + rd<uint32_t>(rdram, list + 4u + 4u * i);
            const uint32_t slot = static_cast<uint16_t>(rd<int16_t>(rdram, st + 0x22u + 2u * rd<uint8_t>(rdram, track + 2u))) & 4095u;
            for (int j = 0; j < 3; ++j)
            {
                const float r = rd<float>(rdram, kBoneSlots + slot * 0x130u + 0x50u + 4u * j), d = r - g_prevRot[slot][j];
                if (g_prevAnim[slot] == static_cast<uint8_t>(anim + 1))
                {
                    g_wraps += std::fabs(d) > kPi;
                    g_maxStep = std::max(g_maxStep, std::fabs(wrapPi(d)));
                }
                g_prevRot[slot][j] = r;
            }
            g_prevAnim[slot] = static_cast<uint8_t>(anim + 1);
        }
        if (g_checkCalls % 2000u == 0u)
            std::fprintf(stderr, "[GHS-ANIM-CHECK] %llu samples, %llu tracks compared, %llu differ; largest rotation step %.4f rad, %llu angle wraps\n",
                         (unsigned long long)g_checkCalls, (unsigned long long)g_checkTracks, (unsigned long long)g_checkBad, g_maxStep,
                         (unsigned long long)g_wraps);
    }
    void logChar(const uint8_t *rdram, uint32_t charOffset)
    {
        static uint32_t n = 0;
        const uint32_t st = kCharState + charOffset, list = rd<uint32_t>(rdram, st + 0x74u + 4u * rd<int8_t>(rdram, st + 0x137u));
        if (n++ >= 20000u || !list || !rd<uint32_t>(rdram, list))
            return;
        const uint32_t track = list + rd<uint32_t>(rdram, list + 4u);
        const uint32_t out = kBoneSlots + static_cast<uint32_t>(rd<int16_t>(rdram, st + 0x22u + 2u * rd<uint8_t>(rdram, track + 2u)) * 0x130);
        std::fprintf(stderr, "[GHS-ANIM] c=%u anim=%d timer=%d t=%.3f %.3f %.3f r=%.4f %.4f %.4f\n", charOffset / 0x2D0u, rd<int8_t>(rdram, st + 0x137u), rd<int16_t>(rdram, st + 0x13Au),
                     rd<float>(rdram, out + 0x40u), rd<float>(rdram, out + 0x44u), rd<float>(rdram, out + 0x48u), rd<float>(rdram, out + 0x50u),
                     rd<float>(rdram, out + 0x54u), rd<float>(rdram, out + 0x58u));
    }

    void sampler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pc = ctx->pc;
        // s1 = char * 0x2D0 from 0x213d94 until the epilogue; at the entry only a0 holds the character.
        const uint32_t charOffset = pc == kSampler ? getRegU32(ctx, 4) * 0x2D0u : getRegU32(ctx, 17);
        g_original[(pc - kSampler) / 4u](rdram, ctx, runtime);
        // Finished (returned to its caller) and sampled (v0 = 0; -1 = animation over, nothing sampled)?
        const uint32_t ret = ctx->pc;
        if ((ret < kSampler || ret >= kSamplerEnd) && ret == getRegU32(ctx, 31) && getRegU32(ctx, 2) == 0u)
        {
            if (g_check)
                verify(rdram, charOffset);
            if (g_ghsAnimInterp)
                blend(rdram, charOffset);
            if (g_check)
            {
                motion(rdram, charOffset);
                logChar(rdram, charOffset);
            }
        }
    }

    void apply(PS2Runtime &runtime)
    {
        for (uint32_t pc = kSampler; pc < kSamplerEnd; pc += 4u)
        {
            if (!runtime.hasFunction(pc))
                continue;
            g_original[(pc - kSampler) / 4u] = runtime.lookupFunction(pc);
            runtime.replaceFunction(pc, &sampler);
        }
    }

    const ps2_game_overrides::Descriptor kDescriptor{"Gregory Horror Show (EU): animation interpolation", "SLES_519.33", 0x00100008u, 0u, apply};
    const ps2_game_overrides::AutoRegister kRegister(kDescriptor);
}
