// 16:9 widescreen for Gregory Horror Show (Europe), SLES_519.33, the way PCSX2's widescreen patch does it
// (patch=1,EE,00166078,word,3c023f40: ax = 0.75 for the main camera). FUN_00166410 builds the camera's view-screen,
// clip and clip-to-screen matrices from Sony's ViewScreenMatrix arguments (f12 scrz, f13 ax, f14 ay, f15/f16 cx/cy, ...).
// Its main-camera call (0x1660C8, every camera except #1) passes ax = 1.0; 0.75 fits a third more width into the same
// 640-wide frame, which the presenter (ps2_runtime.cpp) stretches to 16:9. The clip matrix doesn't use ax, so VU1 still
// clips at +-1200 * 0.75 = 900 px around the centre (the screen edge is at 320): nothing at the new edges gets dropped.
// With GHS_WIDESCREEN=1 or the F2 setting; off, the original runs with its arguments untouched.
// tools/install_overrides.ps1 copies this into the runner.

#include "game_overrides.h"
#include "ps2_runtime.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern bool g_ghsWidescreen;                     // ps2_pad.cpp: F2 setting, ghs_settings.ini, GHS_WIDESCREEN
extern std::atomic<uint32_t> g_ghsWideCamera;    // ps2_pad.cpp: read by the presenter

namespace ghs_widescreen
{
    constexpr uint32_t kViewScreen = 0x00166410u;
    constexpr uint32_t kMainCameraReturn = 0x001660D0u; // after `jal 0x166410` at 0x1660C8

    constexpr uint32_t kHudCameraReturn = 0x0016606Cu;  // camera #1 (`jal 0x166410` at 0x166064)

    PS2Runtime::RecompiledFunction g_original = nullptr;
    // GHS-TRACE: GHS_WIDESCREEN_HUD=1 also narrows camera #1 (PCSX2's commented-out "hud fix").
    const bool g_hud = std::getenv("GHS_WIDESCREEN_HUD") && std::atoi(std::getenv("GHS_WIDESCREEN_HUD")) != 0;

    void viewScreen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_ghsWidescreen && g_hud && getRegU32(ctx, 31) == kHudCameraReturn)
        {
            static bool s_logged = false;
            if (!s_logged)
                std::fprintf(stderr, "[GHS-WIDE] HUD camera narrowed\n");
            s_logged = true;
            ctx->f[13] *= 0.75f;
        }
        if (g_ghsWidescreen && getRegU32(ctx, 31) == kMainCameraReturn)
        {
            ctx->f[13] *= 0.75f; // ax; f17 (zmin) got its own copy of the 1.0 before the call
            if (g_ghsWideCamera.fetch_add(1u, std::memory_order_relaxed) == 0u)
                std::fprintf(stderr, "[GHS-WIDE] main camera widened (ax %.3f)\n", ctx->f[13]);
        }
        g_original(rdram, ctx, runtime);
    }

    void apply(PS2Runtime &runtime)
    {
        if (!runtime.hasFunction(kViewScreen))
            return;
        g_original = runtime.lookupFunction(kViewScreen);
        runtime.replaceFunction(kViewScreen, &viewScreen);
    }

    const ps2_game_overrides::Descriptor kDescriptor{"Gregory Horror Show (EU): widescreen", "SLES_519.33", 0x00100008u, 0u, apply};
    const ps2_game_overrides::AutoRegister kRegister(kDescriptor);
}
