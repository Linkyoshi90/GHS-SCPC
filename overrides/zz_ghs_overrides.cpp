// Game-specific runtime fixes for Gregory Horror Show (Europe), SLES_519.33.
// tools/install_generated.ps1 copies this into PS2Recomp/ps2xRuntime/src/runner/.
// Named zz_ so it sorts last: the runner is a unity build (32 files per batch, alphabetical),
// so a file at the end only invalidates the last batch instead of shifting all of them.

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ps2_stubs
{
    bool ghsMpegFramesPending(uint32_t mpegAddr); // Kernel/Stubs/MPEG.cpp: decoded pictures not shown yet
}
#include <utility>

namespace
{
    // The game's printf (FUN_0010c820) builds a DECI2 packet at this buffer:
    // u16 total length at +0, text at +12.
    constexpr uint32_t kTtyPacketAddr = 0x00440D80u;

    // Deci2Call (syscall wrapper at 0x10B7D0), as a retail PS2 with no debugger attached.
    //
    // The game's printf calls ReqSend (3), then spins on a busy flag that only the DECI2
    // completion handler clears. PS2Recomp's Deci2Call is compiled out in Release builds,
    // and in Debug it reports ReqSend as successful without ever calling the handler, so
    // both builds hang in that loop at 0x10C928. Failing ReqSend makes printf take its
    // own error path (clear flag, return -1). The text is still printed to stderr here.
    // Every replacement must set ctx->pc to the return address like PS2Recomp's generated stub
    // wrappers do; if the dispatcher resumes the thread, it continues from ctx->pc.
    void returnToCaller(R5900Context *ctx)
    {
        ctx->pc = getRegU32(ctx, 31);
    }

    void ghsDeci2Call(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        returnToCaller(ctx);
        const int32_t code = static_cast<int32_t>(getRegU32(ctx, 4));

        switch (code)
        {
        case 1: // sceDeci2Open: hand out a socket so init code is happy
            setReturnS32(ctx, 1);
            return;
        case 2: // sceDeci2Close
        case 4: // sceDeci2Poll
            setReturnS32(ctx, 0);
            return;
        case 3: // sceDeci2ReqSend: no host to send to
        {
            const uint8_t *packet = rdram + (kTtyPacketAddr & PS2_RAM_MASK);
            const uint32_t total = packet[0] | (packet[1] << 8);
            if (total > 12u && total <= 12u + 0x100u)
            {
                std::string text(reinterpret_cast<const char *>(packet + 12), total - 12u);
                for (char &ch : text)
                {
                    if (ch == '\r')
                        ch = ' ';
                }
                std::cerr << "[GHS tty] " << text;
                if (text.empty() || text.back() != '\n')
                    std::cerr << '\n';
            }
            setReturnS32(ctx, -1);
            return;
        }
        default:
            std::cerr << "[GHS Deci2Call] unhandled code " << code << '\n';
            setReturnS32(ctx, -1);
            return;
        }
    }

    std::string readGuestString(const uint8_t *rdram, uint32_t addr, size_t maxLen = 256)
    {
        std::string out;
        for (size_t i = 0; addr != 0u && i < maxLen; ++i)
        {
            const char ch = static_cast<char>(rdram[(addr + i) & PS2_RAM_MASK]);
            if (ch == '\0')
                break;
            out.push_back(ch);
        }
        return out;
    }

    // Sony fileio functions PS2Recomp has no handler for. The game's own copies send an
    // RPC to the IOP fileio server, which nothing answers, so the caller waits forever.
    // sceDevctl("cdrom0:", 0x438A / 0x4324, ...) is called during disc setup at 0x2B3B04
    // and the result is ignored; a retail drive accepts both, so it returns 0.
    // The rest return -1 (error) and log, so we see it if the game ever needs one.
    struct FileioOverride
    {
        uint32_t address;
        const char *name;
        int32_t result;
    };

    constexpr FileioOverride kFileio[] = {
        {0x00110328u, "sceIoctl2", -1},
        {0x001106D0u, "sceMkdir", -1},
        {0x001108A0u, "sceFormat", -1},
        {0x00110B10u, "sceAddDrv", -1},
        {0x00110C50u, "sceDopen", -1},
        {0x00110D18u, "sceDclose", -1},
        {0x00110E80u, "sceDread", -1},
        {0x00110FD8u, "sceGetstat", -1},
        {0x00111178u, "sceChstat", -1},
        {0x001113B8u, "sceRename", -1},
        {0x001115C8u, "sceSync", 0},
        {0x00111760u, "sceMount", -1},
        {0x001119F0u, "sceLseek64", -1},
        {0x00111C28u, "sceDevctl", 0},
        {0x00111E60u, "sceSymlink", -1},
        {0x00112040u, "sceReadlink", -1},
    };

    template <size_t Index>
    void ghsFileio(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        returnToCaller(ctx);
        const FileioOverride &f = kFileio[Index];
        std::cerr << "[GHS fileio] " << f.name << "(\"" << readGuestString(rdram, getRegU32(ctx, 4))
                  << "\", 0x" << std::hex << getRegU32(ctx, 5) << std::dec << ") -> " << f.result << '\n';
        setReturnS32(ctx, f.result);
    }

    template <size_t... I>
    void installFileio(PS2Runtime &runtime, std::index_sequence<I...>)
    {
        (runtime.replaceFunction(kFileio[I].address, &ghsFileio<I>), ...);
    }

    // _sceSifLoadModuleBuffer at 0x112920 (every IRX load in the game goes through it).
    //
    // On hardware loading an IRX is an RPC to the IOP loader: the EE thread blocks until the
    // module's start entry returns, and other EE threads run meanwhile. The game relies on that:
    // before loading ngycdvd it starts a priority-0 thread that registers EE RPC server 0x12348,
    // and ngycdvd's start entry binds to that server before returning. PS2Recomp loads modules
    // synchronously inside the call, so the server thread never got to run first.
    // Here the calling thread yields once to equal-priority threads, then re-enters this
    // function (ctx->pc points back at it) and does the actual load.
    constexpr uint32_t kLoadModuleBufferAddr = 0x00112920u;

    void ghsLoadModuleBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int s_yieldedThread = -1;
        static uint32_t s_yieldedReturn = 0u;

        EeScheduler &ee = runtime->eeScheduler();
        const int self = ee.currentThreadId();
        const uint32_t returnAddress = getRegU32(ctx, 31);
        ctx->pc = returnAddress;

        if (s_yieldedThread != self || s_yieldedReturn != returnAddress)
        {
            s_yieldedThread = self;
            s_yieldedReturn = returnAddress;
            ctx->pc = kLoadModuleBufferAddr;
            (void)ee.rotateReadyQueue(0, false); // 0 = own priority
            ee.transferIfRequested(false);       // throws to the dispatcher; we re-enter later
            ctx->pc = returnAddress;             // nothing to switch to: load right away
        }
        s_yieldedThread = -1;
        s_yieldedReturn = 0u;
        ps2_stubs::sceSifLoadModuleBuffer(rdram, ctx, runtime);
    }

    // GHS-TRACE (temporary): wrap a game function, call the original, log (a0, a1) -> v0 on change.
    struct TracedFunction
    {
        uint32_t address;
        const char *name;
        PS2Runtime::RecompiledFunction original;
    };
    TracedFunction g_traced[] = {
        {0x002B4A10u, "soundStatus(ch)", nullptr},
        {0x002B7CD0u, "soundStream(ch,a1)", nullptr},
        {0x002B4FA0u, "soundStatus2(id)", nullptr},
        {0x002B7FE0u, "streamBusy(ch)", nullptr},
        {0x002B59B0u, "soundBusy()", nullptr},
        {0x002B14F8u, "soundAck(result)", nullptr},
        {0x002B8D70u, "soundDrain()", nullptr},
        {0x002B91E0u, "soundPump()", nullptr},
        {0x002B1520u, "soundPoll(a0)", nullptr},
        {0x002B8B30u, "int10Handler(cause)", nullptr},
        {0x0010C250u, "iWakeupThread(id)", nullptr},
        {0x002BD3B0u, "gsSetBitBltBuf(pkt,sbp,sbw,spsm,dbp,dbw,dpsm)", nullptr},
        {0x002BF0C0u, "movieTick(a0,a1)", nullptr},
        {0x002BFB00u, "movieFrameHook(state,a1)", nullptr},
        {0x002BA610u, "movieStream610(a0)", nullptr},
        {0x002BA710u, "movieStream710(a0)", nullptr},
        {0x0010B1A0u, "CreateThread(param)", nullptr},
        {0x00117670u, "sceCdStRead(sectors,buf,mode,err)", nullptr},
        {0x00108768u, "sceMpegDemuxPssRing(mp,addr,size)", nullptr},
        {0x00200170u, "titleTick()", nullptr},
        {0x00239610u, "requestTitleRes()", nullptr},
        {0x002396D0u, "res2396d0()", nullptr},
        {0x002395C0u, "releaseTitleRes()", nullptr},
        {0x00234D60u, "objUpdate234d60()", nullptr},
        {0x00239670u, "titleResReady()", nullptr},
        {0x00192EB0u, "resLoaderTick()", nullptr},
        {0x001178A0u, "sceCdStStat()", nullptr},
        {0x00117328u, "sceCdGetError()", nullptr},
        {0x001177F8u, "sceCdStPause()", nullptr},
        {0x002BB6C0u, "audioMonoMix(l,lenL,r,lenR)", nullptr},
        {0x002BA530u, "movieAudioConsumed(s)", nullptr},
        {0x002B9490u, "movieAudioRing(q,..)", nullptr},
        {0x002BE740u, "movieStop()", nullptr},
        {0x002BDB90u, "moviePlay(a0..t0)", nullptr},
        {0x002BEA00u, "movieReaderThread()", nullptr}, // tail-jumps to the reader 0x2BDCB0(0x6D2E80, gp+0x42C0, gp+0x4250)
        {0x002BA8A0u, "movieAbort(mp)", nullptr},      // sets mp+0xA8 = 1: the decode loop 0x2BAA90 quits
    };
    std::atomic<int64_t> g_ghs234Enter{0}; // GHS-TRACE: steady_clock ms when func_234D60 was entered, 0 = not inside
    std::atomic<uint32_t> g_ghs234Calls{0};
    std::atomic<int64_t> g_ghsLastDemuxMs{0}; // when the game last fed sceMpegDemuxPssRing (0 = no movie running)

    // TEMPORARY BYPASS (sound): at boot, FUN_0020dc10 stops sound channels 0/1 (and 0x1E) and
    // waits for each status to become 1. Channel 0 goes 0 -> 2 and stays: the stop finishes only
    // after a sound-chip event PS2Recomp's SPU2 emulation doesn't produce yet, and the driver
    // doesn't take channel 1's command until then. Inside that wait loop only, report "stopped" (1).
    // Remove once SPU2 voice/status emulation exists.
    bool inBootSoundWait(const R5900Context *ctx)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        return ra >= 0x0020DC10u && ra < 0x0020DD40u;
    }

    // GHS-TRACE: what the movie reader (0x2BDCB0) tests to skip a movie: (halfword at 0x59DC8C) & (halfword at
    // gp+0x41A0) != 0 sets the movie state byte gp+0x4194 to 3 (stop). Logs those three values when they change
    // (always when force is set).
    void ghsLogMovieSkipInputs(uint8_t *rdram, const R5900Context *ctx, const char *where, bool force)
    {
        const uint32_t gp = getRegU32(ctx, 28);
        uint16_t pad = 0, mask = 0;
        std::memcpy(&pad, rdram + (0x59DC8Cu & PS2_RAM_MASK), 2);
        std::memcpy(&mask, rdram + ((gp + 0x41A0u) & PS2_RAM_MASK), 2);
        const uint8_t state = rdram[(gp + 0x4194u) & PS2_RAM_MASK];
        static uint64_t last = ~0ull;
        const uint64_t key = (uint64_t(pad) << 24) | (uint64_t(mask) << 8) | state;
        if (key == last && !force)
            return;
        last = key;
        std::cerr << "[GHS-SKIP] " << where << " padword=0x" << std::hex << pad << " skipmask=0x" << mask << " movieState=0x"
                  << unsigned(state) << " pad&mask=0x" << (pad & mask) << std::dec << std::endl;
    }

    template <size_t Index>
    void ghsTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::unordered_map<uint64_t, uint32_t> last;
        TracedFunction &f = g_traced[Index];
        const uint32_t a0 = getRegU32(ctx, 4);
        const uint32_t a1 = getRegU32(ctx, 5);
        const bool bootWait = inBootSoundWait(ctx);
        if (Index == 31) // func_2B9490(q, &a, &b, &c, &d): ring counters the end-of-movie flush waits on
        {
            const auto rd = [&](uint32_t addr) { uint32_t v; std::memcpy(&v, rdram + (addr & PS2_RAM_MASK), 4); return v; };
            const uint32_t p1 = getRegU32(ctx, 5), p2 = getRegU32(ctx, 6), p3 = getRegU32(ctx, 7), p4 = getRegU32(ctx, 8);
            const uint32_t caller = getRegU32(ctx, 31);
            f.original(rdram, ctx, runtime);
            static uint64_t n = 0;
            if ((++n & (n - 1u)) == 0u || (n % 4096u) == 0u)
                std::cerr << "[GHS-ARING] #" << n << " caller=0x" << std::hex << caller << " out=" << rd(p1) << "," << rd(p2) << "," << rd(p3) << "," << rd(p4)
                          << std::dec << std::endl;
            return;
        }
        if (Index == 35) // movie abort: who asks the decode loop to quit
        {
            std::cerr << "[GHS-MOVIEEND] movieAbort(mp=0x" << std::hex << a0 << ") ra=0x" << getRegU32(ctx, 31) << std::dec << std::endl;
            f.original(rdram, ctx, runtime);
            return;
        }
        if (Index == 33 || Index == 34) // movie play wrapper / reader loop: args, descriptor, status words, result
        {
            const auto rd = [&](uint32_t addr) { uint32_t v; std::memcpy(&v, rdram + (addr & PS2_RAM_MASK), 4); return v; };
            const uint32_t gp = getRegU32(ctx, 28);
            const uint32_t a2 = getRegU32(ctx, 6), a3 = getRegU32(ctx, 7), t0 = getRegU32(ctx, 8);
            const uint32_t ra = getRegU32(ctx, 31);
            const auto status = [&](const char *when)
            {
                std::cerr << "[GHS-MOVIEEND] " << f.name << ' ' << when << std::hex << " 4188=" << rd(gp + 0x4188u) << " 4194=" << unsigned(rdram[(gp + 0x4194u) & PS2_RAM_MASK])
                          << " 418C=" << unsigned(rdram[(gp + 0x418Cu) & PS2_RAM_MASK]) << " 4170=" << rd(gp + 0x4170u) << " 4174=" << rd(gp + 0x4174u)
                          << " 4178=" << rd(gp + 0x4178u) << " 4190=" << rd(gp + 0x4190u) << " 4198=" << unsigned(rdram[(gp + 0x4198u) & PS2_RAM_MASK]);
                if (Index == 34)
                {
                    const uint32_t desc = gp + 0x4250u, ring = gp + 0x42C0u; // the reader's a2 / a1
                    std::cerr << " desc@" << desc << "=" << rd(desc) << ' ' << rd(desc + 4u) << ' ' << rd(desc + 8u) << ' ' << rd(desc + 12u)
                              << " a1@" << ring << "=" << rd(ring) << ' ' << rd(ring + 4u) << ' ' << rd(ring + 8u) << ' ' << rd(ring + 12u);
                }
                std::cerr << std::dec << std::endl;
            };
            std::cerr << "[GHS-MOVIEEND] " << f.name << " enter a0=0x" << std::hex << a0 << " a1=0x" << a1 << " a2=0x" << a2 << " a3=0x" << a3
                      << " t0=0x" << t0 << " ra=0x" << ra << std::dec << std::endl;
            status("enter");
            const auto t = std::chrono::steady_clock::now();
            f.original(rdram, ctx, runtime);
            std::cerr << "[GHS-MOVIEEND] " << f.name << " -> 0x" << std::hex << getRegU32(ctx, 2) << std::dec << " after "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count() << " ms" << std::endl;
            status("exit");
            return;
        }
        if (Index == 32) // movie stop: log the skip inputs, then stop
        {
            ghsLogMovieSkipInputs(rdram, ctx, "movieStop", true);
            f.original(rdram, ctx, runtime);
            return;
        }
        if (Index == 13) // movie frame hook: wall time per call
        {
            ghsLogMovieSkipInputs(rdram, ctx, "frame", false);
            const auto t0 = std::chrono::steady_clock::now();
            f.original(rdram, ctx, runtime);
            static double total = 0.0, worst = 0.0;
            static uint64_t n = 0;
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            total += ms; worst = std::max(worst, ms); ++n;
            if ((n % 64u) == 0u)
            {
                std::cerr << "[GHS-HOOK] frame hook calls=" << n << " avg=" << total / 64.0 << " ms worst=" << worst << " ms" << std::endl;
                total = 0.0; worst = 0.0;
            }
            return;
        }
        if (Index == 24 || Index == 25) // title waits for resource 0xAB: dump the load queue a few times
        {
            const auto rd = [&](uint32_t addr) { uint32_t v; std::memcpy(&v, rdram + (addr & PS2_RAM_MASK), 4); return v; };
            f.original(rdram, ctx, runtime);
            static uint64_t n = 0;
            ++n;
            if (n == 5u || n == 200u || n == 2000u || (Index == 25 && (n % 512u) == 1u))
            {
                std::cerr << "[GHS-RES] " << f.name << " call #" << n << " -> " << getRegU32(ctx, 2) << " queue@0x5CA000:" << std::hex;
                for (uint32_t i = 0; i < 0x90u; i += 4u)
                    std::cerr << ((i % 16u) == 0u ? " |" : " ") << rd(0x5CA000u + i);
                std::cerr << " | 0xAA:";
                for (uint32_t i = 0; i < 16u; i += 4u) std::cerr << ' ' << rd(0x5CAC78u + 0xAAu * 16u + i);
                std::cerr << " 0xAB:";
                for (uint32_t i = 0; i < 16u; i += 4u) std::cerr << ' ' << rd(0x5CAC78u + 0xABu * 16u + i);
                std::cerr << " flag=" << rd(0x641E44u) << std::dec << std::endl;
            }
            return;
        }
        if (Index == 23) // func_234D60 watchdog (see ghsWatchdog)
        {
            g_ghs234Calls.fetch_add(1);
            g_ghs234Enter.store(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            f.original(rdram, ctx, runtime);
            g_ghs234Enter.store(0);
            return;
        }
        if (Index == 19) // game state words (gp = 0x43B770): log when they change
        {
            const auto rd = [&](uint32_t addr) { uint32_t v; std::memcpy(&v, rdram + (addr & PS2_RAM_MASK), 4); return v; };
            f.original(rdram, ctx, runtime);
            static uint32_t last[4] = {~0u, ~0u, ~0u, ~0u};
            const uint32_t now[4] = {rd(0x43B770u + 0x23B4u), rd(0x43B770u + 0x23ACu), rd(0x43B770u + 0x2324u), rd(0x43B770u + 0x23B0u)};
            static int stuckFrames = 0;
            if (now[1] == 0x11u && now[3] == 4u && ++stuckFrames == 5)
            {
                std::cerr << "[GHS-RES] queue@0x5CA000:" << std::hex;
                for (uint32_t i = 0; i < 0x90u; i += 4u)
                    std::cerr << ((i % 16u) == 0u ? " |" : " ") << rd(0x5CA000u + i);
                std::cerr << "\n[GHS-RES] table 0xAA:";
                for (uint32_t i = 0; i < 16u; i += 4u) std::cerr << ' ' << rd(0x5CAC78u + 0xAAu * 16u + i);
                std::cerr << " 0xAB:";
                for (uint32_t i = 0; i < 16u; i += 4u) std::cerr << ' ' << rd(0x5CAC78u + 0xABu * 16u + i);
                std::cerr << " flag=" << rd(0x641E44u) << std::dec << '\n';
            }
            if (std::memcmp(now, last, sizeof(now)) != 0)
            {
                std::memcpy(last, now, sizeof(now));
                std::cerr << "[GHS-STATE] 23B4=0x" << std::hex << now[0] << " 23AC=0x" << now[1] << " 2324=0x" << now[2] << " 23B0=0x" << now[3] << std::dec << '\n';
            }
            return;
        }
        if (Index == 16) // CreateThread: entry, stack, size, priority -> id
        {
            const auto rd = [&](uint32_t addr) { uint32_t v; std::memcpy(&v, rdram + (addr & PS2_RAM_MASK), 4); return v; };
            f.original(rdram, ctx, runtime);
            std::cerr << "[GHS-THREAD] CreateThread entry=0x" << std::hex << rd(a0 + 4) << " stack=0x" << rd(a0 + 8) << " size=0x" << rd(a0 + 12)
                      << " prio=" << std::dec << rd(a0 + 20) << " -> " << static_cast<int32_t>(getRegU32(ctx, 2)) << std::endl;
            return;
        }
        if ((Index == 12 || Index == 14 || Index == 15) || Index == 17 || Index == 18 || Index == 20 || Index == 21 || Index == 22 || Index == 26 || Index == 27 || Index == 28 || Index == 29 || Index == 30 || Index == 31) // movie loop: count calls, log the first few and every power of two
        {
            static uint64_t nCalls = 0; // one counter per instantiation
            const uint64_t c = ++nCalls;
            const uint32_t ghsA2 = getRegU32(ctx, 6), ghsA3 = getRegU32(ctx, 7);
            if (Index == 18)
                g_ghsLastDemuxMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            f.original(rdram, ctx, runtime);
            if (c <= 8u || (c & (c - 1u)) == 0u || ((Index == 17 || Index == 18) && (c % 4096u) == 0u)) // demux spins ~40,000 calls/s
                std::cerr << "[GHS-MOVIE] " << f.name << " #" << c << " a0=0x" << std::hex << a0 << " a1=0x" << a1 << " a2=0x" << ghsA2 << " a3=0x" << ghsA3 << " -> 0x" << getRegU32(ctx, 2)
                          << " ra=0x" << getRegU32(ctx, 31) << std::dec << '\n';
            return;
        }
        if (Index == 11) // upload packets: log destination + caller chain, once per distinct combination
        {
            static std::unordered_map<uint64_t, uint32_t> seen;
            const uint32_t dbp = getRegU32(ctx, 8), dbw = getRegU32(ctx, 9), dpsm = getRegU32(ctx, 10);
            const uint32_t ra = getRegU32(ctx, 31);
            const uint64_t key = (static_cast<uint64_t>(ra) << 32) | (dbp << 12) | (dbw << 6) | dpsm;
            if (seen.size() < 400u && seen.emplace(key, 0u).second)
            {
                const auto rd = [&](uint32_t addr) { uint32_t v; std::memcpy(&v, rdram + (addr & PS2_RAM_MASK), 4); return v; };
                const uint32_t sp = getRegU32(ctx, 29);
                std::cerr << "[GHS-BLT] dbp=0x" << std::hex << dbp << " dbw=" << std::dec << dbw << " dpsm=0x" << std::hex << dpsm
                          << " ra=0x" << ra << " s0=0x" << getRegU32(ctx, 16) << " s1=0x" << getRegU32(ctx, 17) << " s2=0x" << getRegU32(ctx, 18)
                          << " s3=0x" << getRegU32(ctx, 19) << " stack:";
                for (uint32_t i = 0; i < 24u; ++i)
                    std::cerr << ' ' << rd(sp + 4u * i);
                std::cerr << std::dec << '\n';
            }
            f.original(rdram, ctx, runtime);
            return;
        }
        if (Index == 10 && a0 >= 6u) // movie threads: who wakes them, and how often
        {
            static uint64_t n = 0;
            if (++n <= 30u || (n & (n - 1u)) == 0u)
                std::cerr << "[GHS-WAKE] iWakeupThread(" << a0 << ") #" << n << " ra=0x" << std::hex << getRegU32(ctx, 31) << std::dec
                          << " cur=" << runtime->eeScheduler().currentThreadId() << '\n';
        }
        if (Index == 9 || Index == 10) // timer-1 interrupt path: log every call (first 60)
        {
            static uint32_t n[2] = {};
            if (n[Index - 9]++ < 60u)
                std::cerr << "[GHS-TRACE] " << f.name << " a0=" << a0 << " a1=" << a1 << " ra=0x" << std::hex << getRegU32(ctx, 31)
                          << " sp=0x" << getRegU32(ctx, 29) << std::dec << " thread=" << runtime->eeScheduler().currentThreadId() << '\n';
        }
        if (Index == 5) // soundAck: log where the result goes (flag at +4, pointer at +0x14 of 0x6CB6A0)
        {
            const auto rd = [&](uint32_t addr) { uint32_t v; std::memcpy(&v, rdram + (addr & PS2_RAM_MASK), 4); return v; };
            static uint32_t n = 0;
            if (n++ < 40u)
                std::cerr << "[GHS-TRACE] soundAck result=0x" << std::hex << a0 << " wantResult=" << rd(0x006CB6A4u)
                          << " resultPtr=0x" << rd(0x006CB6B4u) << " caller=0x" << getRegU32(ctx, 31) << std::dec << '\n';
        }
        if (Index == 8) { static uint64_t n = 0; ++n; if (n <= 20u || (n & (n - 1u)) == 0u) std::cerr << "[GHS-TRACE] soundPoll call #" << n << " a0=" << a0 << " ra=0x" << std::hex << getRegU32(ctx, 31) << std::dec << std::endl; }
        if (Index == 6 || Index == 7) // call counters for the sound queue drain / pump
        {
            const auto rd = [&](uint32_t addr) { uint32_t v; std::memcpy(&v, rdram + (addr & PS2_RAM_MASK), 4); return v; };
            static uint64_t calls[2] = {};
            const uint64_t n = ++calls[Index - 6];
            if (n <= 5u || (n & (n - 1u)) == 0u)
                std::cerr << "[GHS-TRACE] " << f.name << " call #" << n << " queue w=" << rd(0x006D2510u) << " r=" << rd(0x006D2514u)
                          << " busy=" << rd(0x006CB6B0u) << '\n';
        }
        f.original(rdram, ctx, runtime);
        uint32_t v0 = getRegU32(ctx, 2);
        if (Index == 3 && v0 != 0u && std::getenv("GHS_BYPASS")) // TEMPORARY EXPERIMENT (sound): streams never finish stopping, see inBootSoundWait
        {
            static bool logged = false;
            if (!logged)
                std::cerr << "[GHS bypass] streamBusy forced to 0\n";
            logged = true;
            v0 = 0u;
            setReturnS32(ctx, 0);
        }
        if ((Index == 0 || Index == 2) && bootWait && v0 != 1u && std::getenv("GHS_BYPASS")) // TEMPORARY BYPASS (sound), see inBootSoundWait
        {
            static bool logged[2] = {};
            if (!logged[Index / 2])
                std::cerr << "[GHS bypass] boot sound wait: reporting " << f.name << " " << a0 << " as stopped (was " << v0 << ")\n";
            logged[Index / 2] = true;
            v0 = 1u;
            setReturnS32(ctx, 1);
        }
        const uint64_t key = (static_cast<uint64_t>(a0) << 32) | a1;
        const auto it = last.find(key);
        if (it == last.end() || it->second != v0)
        {
            last[key] = v0;
            std::cerr << "[GHS-TRACE] " << f.name << " a0=" << a0 << " a1=" << a1 << " -> " << static_cast<int32_t>(v0) << '\n';
        }
    }

    template <size_t... I>
    void installTraces(PS2Runtime &runtime, std::index_sequence<I...>)
    {
        ((g_traced[I].original = runtime.lookupFunction(g_traced[I].address),
          g_traced[I].original ? (void)runtime.replaceFunction(g_traced[I].address, &ghsTraced<I>) : (void)0),
         ...);
    }

    // GHS-TRACE (temporary): same watch list and log format as tools/pine_trace.py, so the
    // recompiled port can be diffed against the real game running in PCSX2.
    struct Watch
    {
        const char *name;
        uint32_t address;
        int words;
    };
    constexpr Watch kWatch[] = {
        {"ch0 status", 0x6CDEAC, 1},
        {"ch1 status", 0x6CDEAC + 1 * 0x84, 1},
        {"ch2 status", 0x6CDEAC + 2 * 0x84, 1},
        {"ch30 status", 0x6CDEAC + 30 * 0x84, 1},
        {"ch0 cmdq r/w", 0x6CDE50 + 0x50, 2},
        {"ch0 +0x58..0x7C", 0x6CDE50 + 0x58, 10},
        {"track0", 0x6CD6D0, 8},
        {"track1", 0x6CD6F0, 8},
        {"sound cmd ring w/r", 0x6D2510, 2},
        {"sound ack block", 0x6CB6A0, 6},
        {"sound send buf", 0x6CB700, 8},
        {"0x12370 inbox", 0x6CC180, 4},
        {"0x12370 flag", 0x6CC1C0, 1},
        {"0x12380 inbox", 0x6CCA40, 4},
        {"disc queue st/h/t", 0x6CD530, 3},
        {"disc send buf", 0x6CAC80, 12},
        {"disc inbox 0x12348", 0x6CB660, 4},
        {"stream queue h/t", 0x43B770 + 0x4050, 2},
        {"movie ring desc", 0x43B770 + 0x42C0, 4},    // reader ring {base, ?, ?, size}: the audio callback's a2
        {"movie audio stage", 0x43B770 + 0x41F0, 8}, // audio staging ring the callback copies into
        // Memory card UI (dispatcher 0x1F6FE0: mode gp+0x2308, 0x230C, countdown 0x2314) and the card
        // command machine (0x1F62F0: results gp+0x2214/0x2220, state 0x2234..0x2290, queue r/w 0x2284/0x2288).
        {"mc ui", 0x43B770 + 0x2300, 8},
        {"mc results", 0x43B770 + 0x2210, 8},
        {"mc machine", 0x43B770 + 0x2234, 24},
        // Save menu freeze (2026-10-18): game state gp+0x23AC (0x26/0x29 = save menu), screen fade (0x12A370/0x12A5E0:
        // gp+0x1FC level, 0x200 speed, 0x208 type, 0x20C active; the menu's state 27 waits for 0x20C != 1) and the
        // in-game save menu 0x1EC300 (state gp+0x21A8).
        {"game state", 0x43B770 + 0x23A8, 4},
        {"fade", 0x43B770 + 0x1F0, 8},
        {"save menu", 0x43B770 + 0x21A8, 12},
    };

    // Text fix: Gregory's voiced line (English text set, message 186 of resource 106) says "garbage" but the
    // text read "rubbish". Same length, so the loaded text is patched in place; the search repeats every 2 s
    // because the text resource is reloaded with each scene.
    void textFixThread(uint8_t *rdram)
    {
        static const char kFind[] = "all this rubbish around?";
        const uint8_t *const begin = rdram, *const end = rdram + PS2_RAM_SIZE;
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            for (const uint8_t *p = begin; (p = std::search(p, end, kFind, kFind + sizeof(kFind) - 1)) != end; ++p)
                std::memcpy(const_cast<uint8_t *>(p) + 9, "garbage", 7);
        }
    }

    // GHS-TRACE: GHS_POKEFILE=<path>: lines "address value" (hex) are written to EE RAM as 32-bit words, then the
    // file is deleted. For tests, e.g. opening the save menu from a room: "43DB1C 24" (game state gp+0x23AC).
    // A line "save" opens the save menu the way the Frog Fortune-Teller's event script does (0x1A7910, command
    // step 4: fade words cleared, then 0x1F9BD0(0x24)), from the next fade tick on the game thread.
    std::atomic<bool> g_ghsOpenSave{false};
    std::atomic<uint32_t> g_ghsOpenSaveState{0x2Au}; // "save [state]": 0x2A save, 0x24 load (frog script 0x1A7910)
    PS2Runtime::RecompiledFunction g_ghsFadeTick = nullptr;
    void ghsFadeTick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_ghsOpenSave.exchange(false))
        {
            // Tail call instead of the fade tick (one skipped tick): the menu runs as guest code, so its waits
            // go through the scheduler, and it returns to the fade tick's caller (ra unchanged).
            std::memset(rdram + 0x43B770u + 0x20Cu, 0, 4);
            std::memset(rdram + 0x43B770u + 0x208u, 0, 4);
            SET_GPR_U32(ctx, 4, g_ghsOpenSaveState.load());
            ctx->pc = 0x001F9BD0u;
            std::cerr << "[GHS-POKE] opening the save menu" << std::endl;
            return;
        }
        g_ghsFadeTick(rdram, ctx, runtime);
    }

    void pokeThread(uint8_t *rdram, std::string path)
    {
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            FILE *f = std::fopen(path.c_str(), "r");
            if (!f)
                continue;
            char word[16] = {};
            if (std::fscanf(f, " %15s", word) == 1 && std::strcmp(word, "save") == 0)
            {
                unsigned state = 0x2Au;
                (void)std::fscanf(f, "%x", &state);
                g_ghsOpenSaveState = state;
                g_ghsOpenSave = true;
            }
            else
                std::rewind(f);
            unsigned addr = 0, value = 0;
            while (std::fscanf(f, "%x %x", &addr, &value) == 2)
            {
                std::memcpy(rdram + (addr & PS2_RAM_MASK & ~3u), &value, 4);
                std::cerr << "[GHS-POKE] 0x" << std::hex << addr << " = 0x" << value << std::dec << std::endl;
            }
            std::fclose(f);
            std::remove(path.c_str());
        }
    }

    void portTraceThread(PS2Runtime *runtime)
    {
        uint8_t *rdram = runtime->memory().getRDRAM();
        FILE *log = std::fopen("D:\\docs\\ghs-recomp\\recomp\\port_trace.log", "w");
        if (!rdram || !log)
            return;
        std::unordered_map<std::string, std::vector<uint32_t>> last;
        const auto start = std::chrono::steady_clock::now();
        std::cerr << "[GHS-TRACE] port_trace t0=" << std::chrono::duration_cast<std::chrono::milliseconds>(start.time_since_epoch()).count() << std::endl;
        for (;;)
        {
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            for (const Watch &w : kWatch)
            {
                std::vector<uint32_t> cur(static_cast<size_t>(w.words));
                std::memcpy(cur.data(), rdram + (w.address & PS2_RAM_MASK), cur.size() * 4);
                auto &prev = last[w.name];
                if (prev != cur)
                {
                    prev = cur;
                    std::fprintf(log, "%9.4f  %-20s ", now, w.name);
                    for (uint32_t v : cur)
                        std::fprintf(log, " %x", v);
                    std::fprintf(log, "\n");
                    std::fflush(log);
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }

    // sceCdDiskReady (0x117068) was not recognised as a Sony function, so its game copy ran: it
    // binds the BIOS CD/DVD server 0x8000059A, which PS2Recomp does not provide, and retries
    // forever (hang before the intro movie). Use PS2Recomp's handler, like sceCdInit already does.
    // (Also listed in recomp\ghs.toml, so a full recompile stubs it directly.)
    void ghsCdDiskReady(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        returnToCaller(ctx);
        ps2_stubs::sceCdDiskReady(rdram, ctx, runtime);
    }

    // sceSifFreeIopHeap (0x112380; 0x1123F8 is sceSifFreeSysMemory, which calls it) was not
    // recognised as a Sony function, while sceSifInitIopHeap/sceSifAllocIopHeap were. The game's
    // copy only frees when its own heap RPC client is initialised, which PS2Recomp's
    // sceSifInitIopHeap never does, so every free was a silent no-op: each movie leaked ~164 KB
    // of IOP memory until the movie after the gender question could not allocate its buffers.
    void ghsFreeIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        returnToCaller(ctx);
        ps2_stubs::sceSifFreeIopHeap(rdram, ctx, runtime);
    }

    // sceCdSearchFile (public wrapper 0x1168E0, worker 0x1165C8) has the same problem: it binds
    // the BIOS server 0x80000597. Used to locate the movie files before the intro.
    void ghsCdSearchFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        returnToCaller(ctx);
        ps2_stubs::sceCdSearchFile(rdram, ctx, runtime);
        static int logged = 0;
        if (logged++ < 20)
        {
            const char *name = reinterpret_cast<const char *>(rdram + (getRegU32(ctx, 5) & PS2_RAM_MASK));
            std::cerr << "[GHS cd] sceCdSearchFile(\"" << name << "\") -> " << static_cast<int32_t>(getRegU32(ctx, 2)) << '\n';
        }
    }

    // EXPERIMENT (movie): the movie's frame thread (entry 0x2BF0C0, priority 1) loops
    // "update frame; SleepThread()" and is woken by an interrupt handler (0x2BD74C). In the port
    // each loop iteration is slower than the wake rate, so wakeups pile up, SleepThread never
    // blocks and the lower-priority stream/demux threads (0x2BEA00, 0x2BAA30) starve: the movie
    // stops after one frame. Drop the backlog before that one SleepThread call (ra 0x2BF148).
    PS2Runtime::RecompiledFunction g_ghsSleepThread = nullptr;
    void ghsSleepThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (getRegU32(ctx, 31) == 0x002BF148u)
        {
            const uint32_t a0 = getRegU32(ctx, 4);
            setReturnS32(ctx, 0);
            ctx->r[4] = _mm_setzero_si128(); // CancelWakeupThread(TH_SELF)
            ps2_syscalls::CancelWakeupThread(rdram, ctx, runtime);
            ctx->r[4] = _mm_set_epi64x(0, a0);
        }
        g_ghsSleepThread(rdram, ctx, runtime);
    }

    // FIX (movie end): moviePlay (0x2BDB90) starts the stream reader (gp+0x4290, entry 0x2BEA00) and
    // the decoder (gp+0x428C, entry 0x2BAA30) at priority 2, drops itself to priority 3, calls
    // RotateThreadReadyQueue(2) (ra 0x2BDC30) and then stops the movie (0x2BE740). On hardware the
    // two workers never block (they spin with RotateThreadReadyQueue), so the priority-3 thread only
    // gets there once both have exited. PS2Recomp's sceMpegGetPicture / sceCdStRead park them in
    // scheduler waits, the priority-3 thread runs early and every movie stops after a frame or two.
    // Here it waits one VSync at a time until both workers are dormant.
    PS2Runtime::RecompiledFunction g_ghsRotateReadyQueue = nullptr;
    bool ghsMovieWorkersRunning(uint8_t *rdram, PS2Runtime *runtime, uint32_t gp)
    {
        for (const uint32_t offset : {0x4290u, 0x428Cu})
        {
            int32_t id = 0;
            std::memcpy(&id, rdram + ((gp + offset) & PS2_RAM_MASK), 4);
            const GuestThread *t = id > 0 ? runtime->eeScheduler().thread(id) : nullptr;
            if (t != nullptr && t->status != EeThreadStatus::Dormant)
                return true;
        }
        return false;
    }
    void ghsWaitForMovieWorkers(uint8_t *rdram, R5900Context &ctx, PS2Runtime *runtime)
    {
        if (!ghsMovieWorkersRunning(rdram, runtime, getRegU32(&ctx, 28)))
        {
            std::cerr << "[GHS movie] workers done, stopping the movie" << std::endl;
            return;
        }
        runtime->eeScheduler().waitVSync(runtime->eeScheduler().currentVSyncTick(), 0,
                                         [rdram, runtime](R5900Context &resumeContext)
                                         { ghsWaitForMovieWorkers(rdram, resumeContext, runtime); });
    }
    void ghsRotateReadyQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool moviePlayWait = getRegU32(ctx, 31) == 0x002BDC30u;
        g_ghsRotateReadyQueue(rdram, ctx, runtime); // sets ctx->pc = ra
        if (moviePlayWait)
        {
            std::cerr << "[GHS movie] moviePlay waits for the reader/decoder threads" << std::endl;
            ghsWaitForMovieWorkers(rdram, *ctx, runtime);
        }
    }

    // GHS-TRACE (temporary): wall-time profile of the movie frame hook's callees
    struct TimedFunction { uint32_t address; PS2Runtime::RecompiledFunction original; std::atomic<uint64_t> ns{0}; std::atomic<uint64_t> calls{0}; };
    TimedFunction g_timed[] = {{0x002BB930u, nullptr}, {0x0010B620u, nullptr}, {0x0010B760u, nullptr}, {0x0010B740u, nullptr}};
    template <size_t I>
    void ghsTimed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const auto t0 = std::chrono::steady_clock::now();
        g_timed[I].original(rdram, ctx, runtime);
        g_timed[I].ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        ++g_timed[I].calls;
    }
    template <size_t... I>
    void installTimed(PS2Runtime &runtime, std::index_sequence<I...>)
    {
        ((g_timed[I].original = runtime.lookupFunction(g_timed[I].address),
          g_timed[I].original ? (void)runtime.replaceFunction(g_timed[I].address, &ghsTimed<I>) : (void)0), ...);
    }

    void ghsWatchdog(PS2Runtime *rt) // GHS-TRACE: report a func_234D60 call that does not return; PC sampling profiler
    {
        bool reported = false;
        std::unordered_map<uint64_t, uint32_t> hist;
        uint32_t s_idle = 0, s_guest = 0, s_host = 0; // no EE thread / inside a recompiled function / dispatcher, syscalls, stubs
        for (;;)
        {
            for (int ms = 0; ms < 1000; ++ms)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                const int tid = rt->eeScheduler().currentThreadId();
                const R5900Context *c = rt->eeScheduler().currentContext();
                ++(tid == 0 ? s_idle : rt->eeScheduler().isExecutingGuest() ? s_guest : s_host);
                if (c)
                    ++hist[(static_cast<uint64_t>(static_cast<uint32_t>(tid)) << 32) | (c->pc & ~0x3Fu)];
            }
            static int sec = 0;
            if (++sec % 5 == 0 && !hist.empty())
            {
                std::vector<std::pair<uint32_t, uint64_t>> top;
                for (const auto &[k, v] : hist)
                    top.emplace_back(v, k);
                std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
                std::cerr << "[GHS-PCPROF] idle=" << s_idle << " guest=" << s_guest << " host=" << s_host;
                s_idle = s_guest = s_host = 0;
                for (size_t i = 0; i < top.size() && i < 12; ++i)
                    std::cerr << " t" << (top[i].second >> 32) << ":0x" << std::hex << static_cast<uint32_t>(top[i].second) << std::dec << "=" << top[i].first;
                std::cerr << std::endl;
                hist.clear();
            }
            const int64_t enter = g_ghs234Enter.load();
            const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            static int tick = 0;
            if (++tick % 5 == 0)
            {
                std::cerr << "[GHS-PROF]";
                for (auto &t : g_timed)
                    std::cerr << " 0x" << std::hex << t.address << std::dec << "=" << t.ns.exchange(0) / 1000000 << "ms/" << t.calls.exchange(0);
                std::cerr << std::endl;
            }
            if (enter != 0 && now - enter > 3000 && !reported)
            {
                reported = true;
                std::cerr << "[GHS-WATCHDOG] func_234D60 call #" << g_ghs234Calls.load() << " has not returned for " << (now - enter) << " ms" << std::endl;
            }
        }
    }

    // TEMPORARY (movie end): 0x1096E0 is sceMpegIsEnd (returns mp->sys->isEnd), which wasn't
    // recognised as a Sony function, so the game's copy reads a field PS2Recomp's MPEG stubs never
    // set and the movie's decode thread loops forever. Use PS2Recomp's handler (which now also ends
    // a stream at a drained sequence_end_code, as GHS stores its movies back to back in MOVIEPL.BIN
    // and file EOF never comes). Fallback, logged: no demuxer input for 4 s (the clip in state 0x1F
    // stops feeding without a decoded sequence end; a 1.5 s limit fired
    // mid-movie during a disc-load hiccup and left the movie threads waiting for audio).
    void ghsMpegIsEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        returnToCaller(ctx);
        const uint32_t mpegAddr = getRegU32(ctx, 4);
        ps2_stubs::sceMpegIsEnd(rdram, ctx, runtime);
        if (getRegU32(ctx, 2) != 0u)
        {
            if (g_ghsLastDemuxMs.exchange(0) != 0)
                std::cerr << "[GHS movie] sceMpegIsEnd: end of movie (sequence end)" << std::endl;
            return;
        }
        const int64_t last = g_ghsLastDemuxMs.load();
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        // Only while the game keeps polling: the first poll of a new movie must not see the
        // previous movie's (game-ended) timestamp as a stall.
        static int64_t s_lastPollMs = 0;
        const bool polling = now - s_lastPollMs < 1000;
        s_lastPollMs = now;
        // The movie's stream reader (0x2BDCB0) sets gp+0x4188 = 2 once it has fed the whole movie (0x2BE288):
        // after that, the movie ends as soon as demuxing has stopped and every decoded picture was shown,
        // instead of after the 4 s fallback below (every movie ended ~4 s late). GHS_MOVIE_EARLYEND=0: off.
        static const bool earlyEnd = [] { const char *v = std::getenv("GHS_MOVIE_EARLYEND"); return !v || v[0] != '0'; }();
        uint32_t readerState = 0;
        std::memcpy(&readerState, rdram + ((getRegU32(ctx, 28) + 0x4188u) & PS2_RAM_MASK), 4);
        if (earlyEnd && polling && last != 0 && readerState == 2u && now - last > 300 && !ps2_stubs::ghsMpegFramesPending(mpegAddr))
        {
            g_ghsLastDemuxMs.store(0);
            std::cerr << "[GHS movie] reader finished, nothing left to show: end of movie" << std::endl;
            setReturnS32(ctx, 1);
            return;
        }
        if (polling && last != 0 && now - last > 4000)
        {
            g_ghsLastDemuxMs.store(0);
            std::cerr << "[GHS movie] no demuxer input for " << (now - last) << " ms: reporting end of movie" << std::endl;
            setReturnS32(ctx, 1);
        }
    }

    // Movie audio mono mix (0x2BB6C0, called by the movie stream thread 0x2BEA00 for each batch of
    // SPU2 blocks): for every 0x400-byte block (256 left + 256 right samples) of a two-segment
    // ring (a0/a1 samples, then a2/a3), set both channels to (L + R) / 2. Native version: the
    // recompiled loop was the bottleneck of movie playback (with it skipped, the decoder never
    // starved). Same register-level logic as the original.
    void ghsAudioMonoMix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        auto s16 = [rdram](uint32_t addr) { return reinterpret_cast<int16_t *>(rdram + (addr & 0x01FFFFFEu)); };
        uint32_t a0 = getRegU32(ctx, 4), a2 = getRegU32(ctx, 6);
        int32_t a1 = static_cast<int32_t>(getRegU32(ctx, 5)), a3 = static_cast<int32_t>(getRegU32(ctx, 7));
        bool wrapL = false, wrapR = false; // t5, t3: pointer already moved to the second segment
        do
        {
            uint32_t pl = a1 > 0 ? a0 : a2;
            uint32_t pr;
            if (a1 < 0x101)
            {
                pr = a2 + static_cast<uint32_t>((0x100 - a1) << 1);
                wrapR = true;
            }
            else
                pr = a0 + 0x200u;
            const uint32_t end = a0 + static_cast<uint32_t>(a1 << 1);
            for (int i = 0; i < 256; ++i)
            {
                const int32_t sum = *s16(pl) + *s16(pr);
                const int16_t mono = static_cast<int16_t>(sum / 2); // MIPS: (sum + (sum < 0)) >> 1
                *s16(pl) = mono;
                *s16(pr) = mono;
                pl += 2;
                pr += 2;
                if (pl >= end && !wrapL)
                {
                    pl = a2;
                    wrapL = true;
                }
                if (pr >= end && !wrapR)
                {
                    pr = a2;
                    wrapR = true;
                }
            }
            if (a1 != 0)
            {
                a1 -= 0x200;
                a0 += 0x400u;
                if (a1 < 0)
                {
                    a3 += a1;
                    a1 = 0;
                }
            }
            else
            {
                a3 -= 0x200;
                a2 += 0x400u;
            }
        } while (a1 + a3 >= 0x200);
        returnToCaller(ctx);
    }

    // GHS-TRACE (temporary): the movie's audio stream callback (registered with
    // sceMpegAddStrCallback, type 2): data/len from the callback data, PSS ring base/size, result.
    // GHS-TRACE (temporary): room object visibility. func_182670(room, index, flag, set) sets/clears a flag
    // (0x40000000 = draw) in the room's object table; the event script's commands 0x5A-0x5D call it.
    PS2Runtime::RecompiledFunction g_ghsObjFlagOriginal = nullptr;
    void ghsObjFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t n = 0;
        if (n++ < 400u)
            std::fprintf(stderr, "[GHS-OBJFLAG] room=0x%x index=%d flag=0x%x set=%d ra=0x%x\n", getRegU32(ctx, 4), static_cast<int32_t>(getRegU32(ctx, 5)),
                         getRegU32(ctx, 6), static_cast<int32_t>(getRegU32(ctx, 7)), getRegU32(ctx, 31));
        g_ghsObjFlagOriginal(rdram, ctx, runtime);
    }

    // GHS-TRACE (temporary): who appends DMA CALL tags (room model parts) to the frame's VIF1 chain
    struct GhsChainFn
    {
        uint32_t address;
        PS2Runtime::RecompiledFunction original;
    };
    GhsChainFn g_ghsChainFns[] = {{0x00164BE0u, nullptr}, {0x00164CE0u, nullptr}, {0x00164F50u, nullptr}, {0x00165830u, nullptr}, {0x00165AB0u, nullptr}};
    template <size_t I>
    void ghsChainFn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t n = 0;
        const uint32_t pkt = getRegU32(ctx, 5);
        if (pkt >= 0x01600000u && pkt < 0x01620000u && n++ < 80u) // the room's MAP2 model parts
            std::fprintf(stderr, "[GHS-CHAINFN] %#x a0=%#x a1=%#x a2=%#x a3=%#x ra=%#x\n", g_ghsChainFns[I].address, getRegU32(ctx, 4), getRegU32(ctx, 5),
                         getRegU32(ctx, 6), getRegU32(ctx, 7), getRegU32(ctx, 31));
        g_ghsChainFns[I].original(rdram, ctx, runtime);
    }
    template <size_t... I>
    void installChainTraces(PS2Runtime &runtime, std::index_sequence<I...>)
    {
        ((g_ghsChainFns[I].original = runtime.lookupFunction(g_ghsChainFns[I].address),
          g_ghsChainFns[I].original ? (void)runtime.replaceFunction(g_ghsChainFns[I].address, &ghsChainFn<I>) : (void)0),
         ...);
    }

    PS2Runtime::RecompiledFunction g_ghsAudioCbOriginal = nullptr;
    void ghsAudioCb(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        auto rd32 = [rdram](uint32_t addr) { uint32_t v; std::memcpy(&v, rdram + (addr & 0x01FFFFFCu), 4); return v; };
        const uint32_t cb = getRegU32(ctx, 5), ring = getRegU32(ctx, 6);
        const uint32_t data = rd32(cb + 8), len = rd32(cb + 12), base = rd32(ring), size = rd32(ring + 12);
        const uint32_t mpeg = getRegU32(ctx, 4), w1 = rd32(ring + 4), w2 = rd32(ring + 8), callerPc = ctx->pc;
        g_ghsAudioCbOriginal(rdram, ctx, runtime);
        static uint32_t n = 0;
        ++n;
        // The MPEG stub passes a private copy of the packet below the ring (not the ring address itself), so only a
        // changed ring descriptor, a huge length or a copy reaching past the ring end are odd.
        static uint32_t lastBase = base, lastSize = size;
        const bool odd = base != lastBase || size != lastSize || len > 0x10000u || data + len > base + size;
        lastBase = base;
        lastSize = size;
        // Returning 0 is the normal case (~130,000 calls a session); a flushed line for each was needless log I/O on
        // the audio path, so those only count, with one line per 4096.
        static uint32_t zeros = 0;
        const bool zero = getRegU32(ctx, 2) == 0u;
        zeros += zero ? 1u : 0u;
        if (n <= 4 || odd || (!zero && (n % 256u) == 0u) || (zero && (zeros % 4096u) == 1u))
            std::cerr << "[GHS-ACB] #" << n << " data=0x" << std::hex << data << " len=0x" << len << " ring=0x" << base << "+0x" << size
                      << " -> " << getRegU32(ctx, 2) << std::dec << (odd ? " (odd)" : "") << " (" << zeros << " returned 0 so far)\n";
        static uint32_t oddLines = 0;
        if (odd && oddLines++ < 64) // GHS-TRACE: which descriptor/MPEG object a bad call used
            std::cerr << "[GHS-ACB] odd: mpeg=0x" << std::hex << mpeg << " desc@0x" << ring << " = " << base << ' ' << w1 << ' ' << w2 << ' ' << size
                      << " cb@0x" << cb << " pc=0x" << callerPc << std::dec << '\n';
    }

    // sceGsSyncVCallback is HLE, but the game's sceGsGetGParam (0x1003E0) is its own code returning
    // the static sceGsGParam at 0x2E9A90. The movie player saves sceGsVSCfunc (+8) from there and
    // restores it afterwards; nothing wrote the field, so it restored 0 and the VBlank callback
    // 0x12B350 (frame counter gp+0x274) stopped after the intro movie. The stuck counter made the
    // name-entry clock play its tick on every logic frame (scrambled ticking).
    PS2Runtime::RecompiledFunction g_ghsSyncVCallback = nullptr;
    void ghsSyncVCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t callback = getRegU32(ctx, 4);
        std::memcpy(rdram + 0x002E9A98u, &callback, 4);
        g_ghsSyncVCallback(rdram, ctx, runtime);
    }

    // Texture lists. FUN_1939F0(list, state, ...) uploads a list of image records ({flags, size, ...} then size bytes;
    // flags bit 3 starts an image, a negative flags word ends the list: returns 0; 64 images: returns -1). Handed data
    // that isn't a list it never ends: the user's freeze after night 4's guest introductions (2026-10-23) spun here,
    // re-uploading for minutes, right after the movie and the reload of resource bank 0xAA (FUN_2396D0). The list is
    // walked first by the same rules; one that leaves RAM, has a negative or huge size, or doesn't end gets -1 (as a
    // full list) and a log line instead of the freeze.
    PS2Runtime::RecompiledFunction g_ghsTexList = nullptr;

    // GHS-TRACE: the guard above didn't fire in the user's second freeze (2026-10-24), so: what each call got (list,
    // caller, the game's call chain), and a watcher that prints it if a call doesn't return for 3 s or the calls come
    // over 2000 a second. Guest call chain: words on the game's stack that point just past a jal (return addresses).
    // Plain data (no constructors before main: a std::mutex/std::string pair here crashed the exe at start, 2026-10-24).
    std::atomic_flag g_ghsTexInfoBusy = ATOMIC_FLAG_INIT;
    char g_ghsTexInfo[1024] = {};
    void ghsSetTexInfo(const std::string &text)
    {
        while (g_ghsTexInfoBusy.test_and_set(std::memory_order_acquire)) {}
        std::snprintf(g_ghsTexInfo, sizeof(g_ghsTexInfo), "%s", text.c_str());
        g_ghsTexInfoBusy.clear(std::memory_order_release);
    }
    std::string ghsGetTexInfo()
    {
        while (g_ghsTexInfoBusy.test_and_set(std::memory_order_acquire)) {}
        std::string text = g_ghsTexInfo;
        g_ghsTexInfoBusy.clear(std::memory_order_release);
        return text;
    }
    std::atomic<int64_t> g_ghsTexEnteredMs{0}; // steady_clock ms of the call running now, 0 = none
    std::atomic<uint32_t> g_ghsTexCalls{0};
    int64_t ghsNowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    std::string ghsGuestBacktrace(const uint8_t *rdram, const R5900Context *ctx)
    {
        std::string out;
        const uint32_t sp = getRegU32(ctx, 29) & 0x01FFFFF0u;
        int found = 0;
        for (uint32_t a = sp; a + 4u <= PS2_RAM_SIZE && a < sp + 0x1000u && found < 16; a += 4u)
        {
            uint32_t v, ins;
            std::memcpy(&v, rdram + a, 4);
            if (v < 0x00100008u || v >= 0x002D0000u || (v & 3u) != 0u)
                continue;
            std::memcpy(&ins, rdram + v - 8u, 4);
            if ((ins >> 26) != 3u) // jal
                continue;
            char buf[48];
            std::snprintf(buf, sizeof(buf), " 0x%x(jal 0x%x)", v, (ins & 0x03FFFFFFu) << 2);
            out += buf;
            ++found;
        }
        return out;
    }
    void ghsTexWatch()
    {
        int64_t reportedEntry = 0, lastRateReport = 0;
        uint32_t lastCalls = 0;
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const int64_t now = ghsNowMs(), entered = g_ghsTexEnteredMs.load();
            const uint32_t calls = g_ghsTexCalls.load(), perSecond = calls - lastCalls;
            lastCalls = calls;
            const std::string info = ghsGetTexInfo();
            if (entered != 0 && now - entered > 3000 && entered != reportedEntry)
            {
                reportedEntry = entered;
                std::fprintf(stderr, "[GHS-TEXLIST] FUN_1939F0 hasn't returned for %lld ms: %s\n", static_cast<long long>(now - entered), info.c_str());
            }
            if (perSecond > 2000u && now - lastRateReport > 10000)
            {
                lastRateReport = now;
                std::fprintf(stderr, "[GHS-TEXLIST] %u calls in 1 s; last: %s\n", perSecond, info.c_str());
            }
        }
    }

    void ghsTexList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t list = getRegU32(ctx, 4) & 0x1FFFFFFFu; // KSEG0/1 and uncached aliases of main RAM
        g_ghsTexCalls.fetch_add(1u, std::memory_order_relaxed);
        const auto run = [&](const char *note)
        {
            char head[160];
            std::snprintf(head, sizeof(head), "list 0x%x state 0x%x ra 0x%x (%s); call chain:", list, getRegU32(ctx, 5), getRegU32(ctx, 31), note);
            ghsSetTexInfo(head + ghsGuestBacktrace(rdram, ctx));
            g_ghsTexEnteredMs = ghsNowMs();
            g_ghsTexList(rdram, ctx, runtime);
            g_ghsTexEnteredMs = 0;
        };
        if (list >= PS2_RAM_SIZE) // not main RAM (scratchpad): not checked
            return run("not main RAM");
        const auto rd = [&](uint32_t addr) { int32_t v; std::memcpy(&v, rdram + addr, 4); return v; };
        uint32_t p = list, images = 0u, steps = 0u;
        bool ok = false;
        while (p + 16u <= PS2_RAM_SIZE && steps++ < 65536u)
        {
            if (images >= 64u || rd(p) < 0)
            {
                ok = true;
                break;
            }
            const int32_t size = rd(p + 4u);
            if (size < 0 || size > 0x01000000)
                break;
            p += static_cast<uint32_t>(size / 4 * 4) + 16u;
            if (p + 4u <= PS2_RAM_SIZE && (rd(p) & 8) != 0)
                ++images;
        }
        if (ok)
        {
            char note[64];
            std::snprintf(note, sizeof(note), "%u records, %u images", steps, images);
            return run(note);
        }
        std::fprintf(stderr, "[GHS-TEXLIST] bad texture list at 0x%x (ra 0x%x): %08x %08x %08x %08x, stopped at 0x%x after %u records; upload skipped\n",
                     list, getRegU32(ctx, 31), rd(list), rd(list + 4u), rd(list + 8u), rd(list + 12u), p, steps);
        setReturnS32(ctx, -1);
        ctx->pc = getRegU32(ctx, 31);
    }

    void applyGhsOverrides(PS2Runtime &runtime)
    {
        if ((g_ghsTexList = runtime.lookupFunction(0x001939F0u)))
        {
            runtime.replaceFunction(0x001939F0u, &ghsTexList);
            std::thread(ghsTexWatch).detach(); // GHS-TRACE
        }
        installChainTraces(runtime, std::make_index_sequence<std::size(g_ghsChainFns)>{}); // GHS-TRACE
        g_ghsObjFlagOriginal = runtime.lookupFunction(0x00182670u); // GHS-TRACE
        if (g_ghsObjFlagOriginal)
            runtime.replaceFunction(0x00182670u, &ghsObjFlag);
        g_ghsAudioCbOriginal = runtime.lookupFunction(0x002BED50u);
        if (g_ghsAudioCbOriginal)
            runtime.replaceFunction(0x002BED50u, &ghsAudioCb);
        runtime.replaceFunction(0x002BB6C0u, &ghsAudioMonoMix);
        runtime.replaceFunction(0x001096E0u, &ghsMpegIsEnd);
        std::thread(ghsWatchdog, &runtime).detach(); // GHS-TRACE
        installTimed(runtime, std::make_index_sequence<std::size(g_timed)>{}); // GHS-TRACE
        g_ghsSleepThread = runtime.lookupFunction(0x0010B2C0u);
        if (g_ghsSleepThread)
            runtime.replaceFunction(0x0010B2C0u, &ghsSleepThread);
        g_ghsRotateReadyQueue = runtime.lookupFunction(0x0010B250u);
        if (g_ghsRotateReadyQueue)
            runtime.replaceFunction(0x0010B250u, &ghsRotateReadyQueue);
        runtime.replaceFunction(0x00117068u, &ghsCdDiskReady);
        runtime.replaceFunction(0x00112380u, &ghsFreeIopHeap);
        runtime.replaceFunction(0x001168E0u, &ghsCdSearchFile);
        runtime.replaceFunction(0x001165C8u, &ghsCdSearchFile);
        std::thread(portTraceThread, &runtime).detach(); // GHS-TRACE
        installTraces(runtime, std::make_index_sequence<std::size(g_traced)>{}); // GHS-TRACE
        runtime.replaceFunction(0x0010B7D0u, &ghsDeci2Call);
        g_ghsSyncVCallback = runtime.lookupFunction(0x00101FC0u);
        if (g_ghsSyncVCallback)
            runtime.replaceFunction(0x00101FC0u, &ghsSyncVCallback);
        installFileio(runtime, std::make_index_sequence<std::size(kFileio)>{});
        runtime.replaceFunction(kLoadModuleBufferAddr, &ghsLoadModuleBuffer);
        std::thread(textFixThread, runtime.memory().getRDRAM()).detach();
        if (const char *poke = std::getenv("GHS_POKEFILE")) // GHS-TRACE
        {
            std::thread(pokeThread, runtime.memory().getRDRAM(), std::string(poke)).detach();
            g_ghsFadeTick = runtime.lookupFunction(0x0012A5E0u);
            if (g_ghsFadeTick)
                runtime.replaceFunction(0x0012A5E0u, &ghsFadeTick);
        }
    }
}

PS2_REGISTER_GAME_OVERRIDE("Gregory Horror Show (EU)", "SLES_519.33", 0x00100008u, 0u, applyGhsOverrides)
