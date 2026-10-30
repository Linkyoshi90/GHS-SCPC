// Gregory Horror Show: the "lost doll crush" mod (2026-10-29, after the manga GHS: Another World). On while
// mods\lost doll crush is built in (game\FILE.STM.mods lists it; switch it in F2 -> Mods; its lost_doll_love.txt gives
// her the new lines). Lost Doll (actor slot 4) falls in love at first sight (the scene below); once her soul is taken
// she is drawn to the player: twice a second the rest of her night's timetable points at the room he is in (her main room and
// rooms she comes back to keep theirs, as in the cheats' visit shuffle: her quest happens there). Leaving a room she is
// in with him (after 3 s together) is a rejection; a minute together forgives one. At 3 she is angry: her vision test
// FUN_1CB760 (returns 0 / 1) sees him wherever he is, so her own AI hunts him; her next encounter with him
// (FUN_217970(4, ...), a catch) makes up. RAM as found for the cheats (zz_ghs_cheats.cpp): timetables, rooms, actors.
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ghs_cheats.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace ghs_doll_crush
{
    constexpr uint32_t kActors = 0x004717A0u, kActorBytes = 0x6C0u, kDoll = 4u;
    constexpr uint32_t kTimetablesBegin = 0x005E9000u, kTimetablesEnd = 0x005EE000u, kNight = 0x004503B8u;
    constexpr uint32_t kRoomDoors = 0x00326DD0u, kCurrentRoom = 0x0047F178u, kGameState = 0x0043DB1Cu, kGameStatePlay = 0x3Au;
    constexpr uint32_t kStayFrames = 180u, kForgiveFrames = 3600u;
    constexpr int kAngryAt = 3;

    bool g_on = false;
    int g_hurt = 0;
    bool g_angry = false;

    template <typename T> T rd(const uint8_t *rdram, uint32_t addr)
    {
        T v;
        std::memcpy(&v, rdram + addr, sizeof(T));
        return v;
    }
    bool entry(const uint8_t *rdram, uint32_t e) // as the cheats' timetableEntry
    {
        if (e < kTimetablesBegin || e + 16u > kTimetablesEnd)
            return false;
        const int16_t start = rd<int16_t>(rdram, e), end = rd<int16_t>(rdram, e + 2u);
        return start >= 0 && start < 2400 && end >= 0 && end <= 2400 && (start != 0 || end != 0);
    }
    bool isPlace(const uint8_t *rdram, int room)
    {
        if (room < 0 || room > 255)
            return false;
        const uint32_t list = rd<uint32_t>(rdram, kRoomDoors + 4u * static_cast<uint32_t>(room));
        return list >= 0x100000u && list < 0x2000000u;
    }

    int minutes(int hhmm) { return (hhmm / 100) * 60 + hhmm % 100; }

    // Her table: +0xD8 points at the current entry, byte 14 counts up from the table's first one. The entries to
    // redirect are picked once per table, from its own rooms.
    std::vector<uint32_t> g_redirect;
    std::map<uint32_t, uint8_t> g_wrote; // entry -> room written; a different byte there means the table was rebuilt
    uint32_t g_night = 0xFFFFFFFFu;
    void follow(uint8_t *rdram)
    {
        uint32_t p = rd<uint32_t>(rdram, kActors + kDoll * kActorBytes + 0xD8u);
        for (int k = 0; k < 40 && entry(rdram, p - 16u) && rdram[p - 2u] + 1 == rdram[p + 14u]; ++k)
            p -= 16u;
        bool rebuilt = rdram[kNight] != g_night;
        for (const auto &[e, room] : g_wrote)
            rebuilt |= rdram[e + 4u] != room;
        if (rebuilt)
        {
            g_night = rdram[kNight];
            g_wrote.clear();
            g_redirect.clear();
            std::map<int, int> time, visits;
            int last = -1, main = -1;
            for (uint32_t e = p; e < p + 16u * 40u && entry(rdram, e); e += 16u)
            {
                const int room = rdram[e + 4u];
                time[room] += (minutes(rd<int16_t>(rdram, e + 2u)) - minutes(rd<int16_t>(rdram, e)) + 1440) % 1440;
                if (room != last)
                    ++visits[room];
                last = room;
            }
            for (const auto &[room, t] : time) // the real room with the most time, as the cheats' shuffle guards it
                if (isPlace(rdram, room) && (main < 0 || t > time[main]))
                    main = room;
            for (uint32_t e = p; e < p + 16u * 40u && entry(rdram, e); e += 16u)
                if (const int room = rdram[e + 4u]; room != main && visits[room] < 2 && isPlace(rdram, room))
                    g_redirect.push_back(e);
            std::fprintf(stderr, "[GHS-CRUSH] night %u: Lost Doll's table at 0x%x, main room %d, %zu visits follow the player\n", g_night, p,
                         main, g_redirect.size());
        }
        const int32_t here = rd<int32_t>(rdram, kCurrentRoom);
        if (!isPlace(rdram, here))
            return;
        for (const uint32_t e : g_redirect)
            g_wrote[e] = rdram[e + 4u] = static_cast<uint8_t>(here);
    }

    // Her soul: records 0x64E2E0 + slot * 0x90, int16 state at +6; FUN_245880 sets 14 when the player takes it, FUN_247A50
    // 15 when Death gets it. Until then she keeps the game's own behaviour (her soul quest needs it).
    constexpr uint32_t kSoulState = 0x0064E2E6u + kDoll * 0x90u;
    bool soulTaken(const uint8_t *rdram)
    {
        const int16_t s = rd<int16_t>(rdram, kSoulState);
        return s == 14 || s == 15;
    }

    uint32_t g_frame = 0, g_scene = 0; // the frame the scene below began (0: none); following waits while it runs

    // Rejections: the player's room changes while she was in the old one with him for kStayFrames.
    int32_t g_lastRoom = -1;
    uint32_t g_together = 0, g_tick = 0;
    bool g_crush = false;
    void watch(uint8_t *rdram)
    {
        if (rd<uint32_t>(rdram, kGameState) != kGameStatePlay || !entry(rdram, rd<uint32_t>(rdram, kActors + kDoll * kActorBytes + 0xD8u)))
            return;
        if (soulTaken(rdram) != g_crush)
        {
            g_crush = !g_crush;
            std::fprintf(stderr, "[GHS-CRUSH] Lost Doll's soul %s\n", g_crush ? "taken: crush on" : "not taken: crush off");
        }
        if (!g_crush)
            return;
        if (++g_tick % 30u == 0u && !g_scene && !g_ghsCheats.guestShuffle.load(std::memory_order_relaxed)) // the shuffle would undo it
            follow(rdram);
        const int32_t here = rd<int32_t>(rdram, kCurrentRoom);
        const int her = rdram[kActors + kDoll * kActorBytes + 0x220u];
        if (here != g_lastRoom)
        {
            if (g_together >= kStayFrames && her == g_lastRoom && !g_angry && !g_scene)
            {
                g_angry = ++g_hurt >= kAngryAt;
                std::fprintf(stderr, "[GHS-CRUSH] left Lost Doll in room %d: rejection %d of %d%s\n", g_lastRoom, g_hurt, kAngryAt,
                             g_angry ? ", she is angry" : "");
            }
            g_together = 0;
            g_lastRoom = here;
        }
        if (her == here && ++g_together % kForgiveFrames == 0u && g_hurt > 0 && !g_angry)
            std::fprintf(stderr, "[GHS-CRUSH] time with Lost Doll: rejections %d\n", --g_hurt);
    }

    // Love at first sight (before her soul is taken; once per game start): the first time her hunt AI FUN_1BFD60(4, 0)
    // starts, a scene plays instead of a chase. Its spot states (+0x660, int16): 0xC8 says the spot line (FUN_223950(4,
    // 0x29, id, ...), returning to 0x1C011C), sets the 0.5 s timer +0x678 and goes to 0xD2, a pause before the chase.
    // The scene holds her there facing the player (yaw +0xA4 = atan2(dx, dz), as FUN_18FBF0): a stare, a glance away, a
    // stare; then 0xC8 again says line 104 (the mod's text: "It must be love..."), a pause, and she flees: her current
    // timetable entry goes to her own room (code 7) for a minute, and for 20 s she doesn't notice the player.
    // Mulling in front of room 205 (play-test 2026-10-30: actor flags 0x800000, no AI runs) she catches by touch instead:
    // the player's contact check (FUN_14ABF0, FUN_156FB0 = 3) starts FUN_217970(4, 0) from 0x14BCAC / 0x14C8C4. The first
    // such catch is dropped (as the cheats' invulnerability) and the scene is "held": the guest dispatcher hook keeps her
    // in place facing him and says line 104 itself; from the flee on it is the same scene.
    constexpr uint32_t kGlance = 120u, kStareAgain = 150u, kWhisper = 270u, kFlee = 450u, kForget = kFlee + 1200u, kHome = kFlee + 3600u;
    constexpr uint32_t kHeard = 0x0047F13Cu, kSeen = 0x0047F184u;
    bool g_sceneDone = false, g_held = false, g_whispered = false, g_fled = false;
    float g_heldX = 0.0f, g_heldZ = 0.0f;
    uint32_t g_homeEntry = 0;
    uint8_t g_homeRoom = 0;
    uint32_t sceneTime() { return g_scene ? g_frame - g_scene : ~0u; }
    void face(uint8_t *rdram, float turn)
    {
        uint8_t *a = rdram + kActors + kDoll * kActorBytes;
        const float yaw = std::atan2(rd<float>(rdram, kActors + 0x80u) - rd<float>(a, 0x80u), rd<float>(rdram, kActors + 0x88u) - rd<float>(a, 0x88u)) + turn;
        std::memcpy(a + 0xA4u, &yaw, 4);
    }
    void scene(uint8_t *rdram)
    {
        uint8_t *a = rdram + kActors + kDoll * kActorBytes;
        const uint32_t t = sceneTime();
        if (t == kWhisper && !g_held)
        {
            const int16_t spot = 0xC8;
            std::memcpy(a + 0x660u, &spot, 2);
        }
        else if (t >= kFlee && !g_fled && t < kHome)
        {
            g_fled = true;
            g_homeEntry = 0;
            if (!entry(rdram, rd<uint32_t>(a, 0xD8u)))
                return;
            g_homeEntry = rd<uint32_t>(a, 0xD8u);
            g_homeRoom = rdram[g_homeEntry + 4u];
            rdram[g_homeEntry + 4u] = 7u;
            std::fprintf(stderr, "[GHS-CRUSH] Lost Doll flees to her room (timetable 0x%x: room %u -> 7)\n", g_homeEntry, g_homeRoom);
        }
        else if (t >= kHome && t != ~0u)
        {
            if (g_homeEntry && rdram[g_homeEntry + 4u] == 7u) // not rebuilt since
                rdram[g_homeEntry + 4u] = g_homeRoom;
            g_scene = 0;
            std::fprintf(stderr, "[GHS-CRUSH] scene over: Lost Doll back on her timetable\n");
        }
    }

    // Shy (user 2026-10-30: following him into his guest room she hunted him there, so it was no safe place any more):
    // while the crush is on and she isn't angry, spotting or touching him in the room he is in (any followed visit, his
    // guest room among them) runs the scene from its flee on instead of a hunt or a catch. Again later restarts the
    // forgetting; she stays sent home.
    bool shy(const uint8_t *rdram)
    {
        const int32_t here = rd<int32_t>(rdram, kCurrentRoom);
        const uint32_t t = sceneTime();
        return g_crush && !g_angry && (t == ~0u || t >= kForget) && isPlace(rdram, here) &&
               rdram[kActors + kDoll * kActorBytes + 0x220u] == here;
    }
    void shyFlee(const uint8_t *rdram, const char *how)
    {
        g_fled = g_scene && g_fled; // already sent home: keep the saved room
        g_held = false;
        g_scene = g_frame - kFlee;
        std::fprintf(stderr, "[GHS-CRUSH] Lost Doll %s the player in room %d: too shy, she flees\n", how, rd<int32_t>(rdram, kCurrentRoom));
    }

    PS2Runtime::RecompiledFunction g_vsync = nullptr, g_sees = nullptr, g_encounter = nullptr, g_huntAi = nullptr, g_line = nullptr,
                                   g_guests = nullptr;
    void returnToCaller(R5900Context *ctx) { ctx->pc = getRegU32(ctx, 31); }
    void vsync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++g_frame;
        scene(rdram);
        watch(rdram);
        g_vsync(rdram, ctx, runtime);
    }
    void huntAi(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (getRegU32(ctx, 4) != kDoll)
            return g_huntAi(rdram, ctx, runtime);
        uint8_t *a = rdram + kActors + kDoll * kActorBytes;
        const bool start = getRegU32(ctx, 5) == 0u;
        if (start && shy(rdram))
            shyFlee(rdram, "spots");
        else if (start && !g_sceneDone && !soulTaken(rdram))
        {
            g_sceneDone = true;
            g_fled = false;
            g_scene = g_frame;
            std::fprintf(stderr, "[GHS-CRUSH] Lost Doll sees the player for the first time: love at first sight\n");
        }
        const uint32_t t = sceneTime();
        if (t >= kFlee && t < kForget) // she forgets him, as under the cheats' no detection: no new hunt, a pursuit dropped
        {
            if (start)
                return returnToCaller(ctx);
            std::memset(a + 0x688u, 0, 2);
            a[0x690u] = 0;
            const uint32_t heard = rd<uint32_t>(rdram, kHeard), seen = rd<uint32_t>(rdram, kSeen);
            std::memset(rdram + kHeard, 0, 4);
            std::memset(rdram + kSeen, 0, 4);
            g_huntAi(rdram, ctx, runtime);
            std::memcpy(rdram + kHeard, &heard, 4);
            std::memcpy(rdram + kSeen, &seen, 4);
            return;
        }
        const float turn = t >= kGlance && t < kStareAgain ? 1.2f : 0.0f;
        if (t < kWhisper && !start && rd<int16_t>(a, 0x660u) == 0xC8) // stares before the spot line
        {
            face(rdram, turn);
            return returnToCaller(ctx);
        }
        g_huntAi(rdram, ctx, runtime);
        if (t < kFlee && rd<int16_t>(a, 0x660u) == 0xD2)
        {
            const float pause = 0.5f;
            std::memcpy(a + 0x678u, &pause, 4);
            face(rdram, turn);
        }
    }
    void line(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (getRegU32(ctx, 4) == kDoll && getRegU32(ctx, 31) == 0x001C011Cu && sceneTime() < kFlee)
        {
            if (sceneTime() < kWhisper) // no spot line while she stares
            {
                SET_GPR_U32(ctx, 2, 0u);
                return returnToCaller(ctx);
            }
            SET_GPR_U32(ctx, 5, 0x63u); // high priority
            SET_GPR_U32(ctx, 6, 104u);
        }
        g_line(rdram, ctx, runtime);
    }
    void sees(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t t = sceneTime();
        const bool forgets = t >= kFlee && t < kForget;
        if ((!g_angry && !forgets) || getRegU32(ctx, 4) != kDoll || g_ghsCheats.noDetection.load(std::memory_order_relaxed))
            return g_sees(rdram, ctx, runtime);
        SET_GPR_U32(ctx, 2, forgets ? 0u : 1u);
        returnToCaller(ctx);
    }
    // The guest dispatcher FUN_140730 (main thread, once a game frame; the vsync hook is an interrupt callback and
    // mustn't run game code) holds her for the held scene, after her own update.
    void guests(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_guests(rdram, ctx, runtime);
        if (!g_held)
            return;
        const uint32_t t = sceneTime();
        if (t >= kFlee)
        {
            g_held = false;
            return;
        }
        uint8_t *a = rdram + kActors + kDoll * kActorBytes;
        std::memcpy(a + 0x80u, &g_heldX, 4);
        std::memcpy(a + 0x88u, &g_heldZ, 4);
        face(rdram, t >= kGlance && t < kStareAgain ? 1.2f : 0.0f);
        if (t >= kWhisper && !g_whispered)
        {
            g_whispered = true;
            const R5900Context saved = *ctx; // a call of our own: FUN_223950(4, 0x63, 104, 2, 1, -1.0f, 0.0f), as the spot line
            SET_GPR_U32(ctx, 4, kDoll);
            SET_GPR_U32(ctx, 5, 0x63u);
            SET_GPR_U32(ctx, 6, 104u);
            SET_GPR_U32(ctx, 7, 2u);
            SET_GPR_U32(ctx, 8, 1u);
            SET_GPR_U32(ctx, 31, 0x00140730u);
            ctx->f[12] = -1.0f;
            ctx->f[13] = 0.0f;
            g_line(rdram, ctx, runtime);
            *ctx = saved;
            std::fprintf(stderr, "[GHS-CRUSH] Lost Doll whispers: it must be love\n");
        }
    }
    void encounter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        const bool touch = getRegU32(ctx, 4) == kDoll && getRegU32(ctx, 5) == 0u && (ra == 0x0014BCACu || ra == 0x0014C8C4u);
        if (touch && shy(rdram))
            shyFlee(rdram, "touches");
        else if (touch && !g_sceneDone && !soulTaken(rdram))
        {
            const uint8_t *a = rdram + kActors + kDoll * kActorBytes;
            g_fled = false;
            g_sceneDone = g_held = true;
            g_scene = g_frame;
            g_heldX = rd<float>(a, 0x80u);
            g_heldZ = rd<float>(a, 0x88u);
            std::fprintf(stderr, "[GHS-CRUSH] Lost Doll touches the player for the first time: love at first sight\n");
        }
        if (touch && sceneTime() < kForget) // no catch during the scene (as the cheats' invulnerability)
        {
            std::memset(rdram + 0x0043C040u, 0, 4); // the talk partner the caller set (0x96)
            SET_GPR_U32(ctx, 2, 0u);
            return returnToCaller(ctx);
        }
        if (getRegU32(ctx, 4) == kDoll && (g_angry || g_hurt > 0))
        {
            std::fprintf(stderr, "[GHS-CRUSH] Lost Doll got the player: made up\n");
            g_angry = false;
            g_hurt = 0;
        }
        g_encounter(rdram, ctx, runtime);
    }

    void apply(PS2Runtime &runtime)
    {
        std::ifstream built("FILE.STM.mods"); // the working directory is game\ (written by tools\ghsmod.py)
        for (std::string line; std::getline(built, line);)
            g_on |= line.rfind("lost doll crush\t", 0) == 0;
        if (!g_on)
            return;
        // Each hook calls the one before it (the cheats hook the same functions).
        if ((g_vsync = runtime.lookupFunction(0x0012B350u)))
            runtime.replaceFunction(0x0012B350u, &vsync);
        if ((g_sees = runtime.lookupFunction(0x001CB760u)))
            runtime.replaceFunction(0x001CB760u, &sees);
        if ((g_encounter = runtime.lookupFunction(0x00217970u)))
            runtime.replaceFunction(0x00217970u, &encounter);
        if ((g_huntAi = runtime.lookupFunction(0x001BFD60u)))
            runtime.replaceFunction(0x001BFD60u, &huntAi);
        if ((g_line = runtime.lookupFunction(0x00223950u)))
            runtime.replaceFunction(0x00223950u, &line);
        if ((g_guests = runtime.lookupFunction(0x00140730u)))
            runtime.replaceFunction(0x00140730u, &guests);
        std::fprintf(stderr, "[GHS-CRUSH] on: Lost Doll has a crush on the player\n");
    }

    const ps2_game_overrides::Descriptor kDescriptor{"Gregory Horror Show (EU): Lost Doll's crush (mod)", "SLES_519.33", 0x00100008u, 0u, apply};
    const ps2_game_overrides::AutoRegister kRegister(kDescriptor);
}
