// GHS-TRACE: guest roster probe for Gregory Horror Show (Europe), SLES_519.33. Changes nothing in the game.
// GHS_ROSTER=1: whenever the night or a timetable's times/rooms change, logs [GHS-ROSTER] lines: each guest slot's
// flags, room and timetable, then the rooms (with a door list, as the cheats' isPlace) no timetable visits that night.
// Each slot's table sits at a fixed place (found 2026-10-30 in RAM snapshots: slot k at 0x5E9BC0 + (k - 1) * 0x280,
// 40 entries of 16 bytes), so slots with no guest that night show too. Purpose: room and slot for a new guest.

#include "game_overrides.h"
#include "ps2_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

namespace ghs_roster_trace
{
    constexpr uint32_t kActors = 0x004717A0u, kActorBytes = 0x6C0u, kSlots = 27u;
    constexpr uint32_t kTables = 0x005E9BC0u, kTableBytes = 0x280u, kNight = 0x004503B8u, kRoomDoors = 0x00326DD0u;
    constexpr uint32_t kGameState = 0x0043DB1Cu, kGameStatePlay = 0x3Au;

    template <typename T> T rd(const uint8_t *rdram, uint32_t addr)
    {
        T v;
        std::memcpy(&v, rdram + addr, sizeof(T));
        return v;
    }
    bool entry(const uint8_t *rdram, uint32_t e) // as the cheats' timetableEntry
    {
        const int16_t start = rd<int16_t>(rdram, e), end = rd<int16_t>(rdram, e + 2u);
        return start >= 0 && start < 2400 && end >= 0 && end <= 2400 && (start != 0 || end != 0);
    }
    bool isPlace(const uint8_t *rdram, int room)
    {
        const uint32_t list = rd<uint32_t>(rdram, kRoomDoors + 4u * static_cast<uint32_t>(room));
        return list >= 0x100000u && list < 0x2000000u;
    }
    uint32_t table(uint32_t slot) { return kTables + (slot - 1u) * kTableBytes; }

    uint64_t hash(const uint8_t *rdram) // night, times and rooms
    {
        uint64_t h = rdram[kNight];
        for (uint32_t s = 1; s <= kSlots; ++s)
            for (uint32_t e = table(s); e < table(s) + kTableBytes && entry(rdram, e); e += 16u)
                h = h * 1099511628211ull ^ (rd<uint32_t>(rdram, e) ^ static_cast<uint64_t>(rdram[e + 4u]) << 32);
        return h;
    }

    void log(const uint8_t *rdram)
    {
        std::fprintf(stderr, "[GHS-ROSTER] night %u\n", rdram[kNight]);
        std::set<int> visited;
        for (uint32_t s = 1; s <= kSlots; ++s)
        {
            const uint32_t a = kActors + s * kActorBytes, flags = rd<uint32_t>(rdram, a + 0x08u);
            std::string t;
            for (uint32_t e = table(s); e < table(s) + kTableBytes && entry(rdram, e); e += 16u)
            {
                char buf[24];
                std::snprintf(buf, sizeof(buf), " %04d-%04d:%u", rd<int16_t>(rdram, e), rd<int16_t>(rdram, e + 2u), rdram[e + 4u]);
                t += buf;
                visited.insert(rdram[e + 4u]);
            }
            if (flags != 0u || !t.empty())
                std::fprintf(stderr, "[GHS-ROSTER]   slot %2u flags %08x room %3u ptr %08x%s%s\n", s, flags, rdram[a + 0x220u],
                             rd<uint32_t>(rdram, a + 0xD8u), t.empty() ? " (no timetable)" : " table:", t.c_str());
            else
                std::fprintf(stderr, "[GHS-ROSTER]   slot %2u empty\n", s);
        }
        std::string unused;
        for (int room = 0; room < 256; ++room)
            if (isPlace(rdram, room) && !visited.count(room))
                unused += " " + std::to_string(room);
        std::fprintf(stderr, "[GHS-ROSTER]   rooms no timetable visits:%s\n", unused.c_str());
    }

    PS2Runtime::RecompiledFunction g_vsync = nullptr;
    uint64_t g_last = 0;
    void vsync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (rd<uint32_t>(rdram, kGameState) == kGameStatePlay)
            if (const uint64_t h = hash(rdram); h != g_last)
            {
                g_last = h;
                log(rdram);
            }
        g_vsync(rdram, ctx, runtime);
    }

    void apply(PS2Runtime &runtime)
    {
        if (!std::getenv("GHS_ROSTER"))
            return;
        if ((g_vsync = runtime.lookupFunction(0x0012B350u))) // calls the hook before it (cheats, crush)
            runtime.replaceFunction(0x0012B350u, &vsync);
        std::fprintf(stderr, "[GHS-ROSTER] on\n");
    }

    const ps2_game_overrides::Descriptor kDescriptor{"Gregory Horror Show (EU): guest roster trace", "SLES_519.33", 0x00100008u, 0u, apply};
    const ps2_game_overrides::AutoRegister kRegister(kDescriptor);
}
