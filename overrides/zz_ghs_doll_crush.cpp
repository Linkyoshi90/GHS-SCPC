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

#include <algorithm>
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

    // Her soul: records 0x64E2E0 + slot * 0x90, int16 state at +6; FUN_245700 sets 12 when the player picks up her soul jar
    // (F6 snapshots 2026-11-04: 6 -> 12), FUN_245880 14, FUN_247A50 14 -> 15 when Death gets it. Until then she keeps the
    // game's own behaviour (her soul quest needs it).
    constexpr uint32_t kSoulState = 0x0064E2E6u + kDoll * 0x90u;
    bool soulTaken(const uint8_t *rdram)
    {
        const int16_t s = rd<int16_t>(rdram, kSoulState);
        return s == 12 || s == 14 || s == 15;
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
    void noNewConditions(uint8_t *rdram); // the Doll's show, below
    void vsync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++g_frame;
        scene(rdram);
        watch(rdram);
        noNewConditions(rdram);
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

    // The Doll (user 2026-11-04): caught with the Doll (key item 134) equipped, her horror show starts with "Did you bring
    // that doll for me?", she hugs the player, takes the Doll and the show ends without its mental damage. No Yes / No:
    // Neko's button wait (0xD1 / 0xDF) hung the show on the first line (play-test 2026-11-04). Her show is event script 0x6F (found 2026-11-04): FUN_24FA00(id) loads a script from the table at
    // 0x334910 (pointer, flags) into 0x64FDD8 (id) / 0x64FDDC (pointer); the VM runs it from its current label each
    // frame. Instructions: u16 0xC000 | op << 4 | n, then n int16 arguments; 0x2 L label, 0x3 L goto, 0x7 / 0x8 v k L
    // if var[v] == / != k goto L, 0x0 the end (label searches stop there). Script 0x6F (0x414A60-0x414EC4): L400 fades in
    // and goes to L500, her first line; its only damage is 0x70 (L620, L20500 when skipped); L10500 ends it without.
    // The hook gives the VM a copy at kScript with L400 going to a new L450, the new labels in front of the last one
    // (L32767). Commands: 0xCE w pool id text, 0xDB w 0 0 after it, 0xDC / 0xDE w close; 0x142 v id var[v] = item
    // equipped, 0xEE id takes the item, 0x40 actor x y z a position (-10000 keeps y), 0x4E actor anim mode 0, 0xE / 0xF
    // t c timer reset / wait c / 100 s. Her two new lines (pool 4, kAsk / kThanks) come from the text lookup
    // FUN_2D8D90(pool, id, 0), which returns a pointer to the text.
    constexpr uint32_t kShow = 0x00414A60u, kShowGoto500 = 0x00414B1Au, kShowLast = 0x00414EBEu, kShowEnd = 0x00414EC4u;
    constexpr uint32_t kScriptId = 0x0064FDD8u, kScriptPtr = 0x0064FDDCu, kEquipped = 0x00450710u;
    constexpr uint32_t kScript = 0x000FE000u, kTexts = 0x000FF800u; // free RAM below the program (zero in all snapshots)
    constexpr int16_t kDollItem = 134, kAsk = 9001, kThanks = 9002;
    // Her pool's line header (as in 033.slid\004.bin), then the text.
    const char kAskText[] = "\x1c" "B\x1c" "J\x1c" ":\x1a" "6\x1c" "1\x1c" "K\n Hee hee...\n Did you bring that doll...\n for me...?";
    const char kThanksText[] = "\x1c" "B\x1c" "J\x1c" ":\x1a" "6\x1c" "1\x1c" "K\n My dolly...! Hee hee hee!\n Thank you...my darling...";
    // The show still gave a headache without its 0x70 (play-test 2026-11-04), so the C side of the catch sets it: the
    // conditions (bytes 0x4505D8-0x4505DE, 1 = has it, as in the cheats) the player didn't have at the show's start are
    // cleared each frame for a while. ponytail: a fixed 2 minutes, not the show's end; find the setter if that's too long.
    constexpr uint32_t kConditions = 0x004505D8u, kNoConditions = 2u * 60u * 60u;
    uint8_t g_hadCondition[7] = {};
    uint32_t g_noConditionsUntil = 0;
    void noNewConditions(uint8_t *rdram)
    {
        if (!g_noConditionsUntil)
            return;
        for (uint32_t i = 0; i < sizeof(g_hadCondition); ++i)
            if (!g_hadCondition[i])
                rdram[kConditions + i] = 0u;
        if (static_cast<int32_t>(g_frame - g_noConditionsUntil) >= 0)
            g_noConditionsUntil = 0;
    }
    std::vector<uint8_t> g_code;
    void op(int code, std::initializer_list<int16_t> args)
    {
        const uint16_t h = static_cast<uint16_t>(0xC000u | static_cast<uint32_t>(code) << 4 | args.size());
        g_code.insert(g_code.end(), {static_cast<uint8_t>(h), static_cast<uint8_t>(h >> 8)});
        for (const int16_t a : args)
            g_code.insert(g_code.end(), {static_cast<uint8_t>(a), static_cast<uint8_t>(static_cast<uint16_t>(a) >> 8)});
    }
    bool writeDollShow(uint8_t *rdram)
    {
        if (rd<uint16_t>(rdram, kShowGoto500 - 2u) != 0xC031u || rd<int16_t>(rdram, kShowGoto500) != 500 ||
            rd<uint16_t>(rdram, kShowLast) != 0xC021u || rd<int16_t>(rdram, kShowLast + 2u) != 32767)
            return false; // not the script as found
        g_code.assign(rdram + kShow, rdram + kShowLast);
        g_code[kShowGoto500 - kShow] = 450 & 0xFF;
        g_code[kShowGoto500 - kShow + 1u] = 450 >> 8;
        op(0x2, {450}), op(0x142, {13, kDollItem}), op(0x7, {13, 1, 5000}), op(0x3, {500});
        // Lines as her show runs its own (L500): window 1, closed by a timer, no button wait.
        op(0x2, {5000}), op(0x21, {}), op(0xCE, {1, 4, kAsk}), op(0xDB, {1, 0, 0}), op(0xE, {0, 0}), op(0x3, {5010});
        op(0x2, {5010}), op(0xF, {0, 600}), op(0xDC, {1}), op(0x3, {5020});
        op(0x2, {5020}), op(0xDE, {1}), op(0x3, {5100});
        // Moving her (0x40 4 -130 ...) made her vanish (play-test 2026-11-04): she stays where the show put her.
        op(0x2, {5100}), op(0x4E, {4, 24, 1, 0}), op(0xCE, {1, 4, kThanks}), op(0xDB, {1, 0, 0});
        op(0xEE, {kDollItem}), op(0xE, {0, 0}), op(0x3, {5110});
        op(0x2, {5110}), op(0xF, {0, 600}), op(0xDC, {1}), op(0x3, {5120});
        op(0x2, {5120}), op(0xDE, {1}), op(0x3, {10500});
        g_code.insert(g_code.end(), rdram + kShowLast, rdram + kShowEnd);
        std::vector<int16_t> labels, targets; // self-check: the copy parses to its end and every goto has its label
        size_t at = 0;
        for (uint16_t h = 1; h != 0xC000u && at + 2u <= g_code.size();)
        {
            std::memcpy(&h, &g_code[at], 2);
            const uint32_t code = (h & 0x1FF0u) >> 4, n = h & 0xFu;
            std::vector<int16_t> args(n);
            if (n && at + 2u + 2u * n <= g_code.size())
                std::memcpy(args.data(), &g_code[at + 2u], 2u * n);
            at += 2u + 2u * n;
            if (code == 0x2u && n == 1u)
                labels.push_back(args[0]);
            else if ((code == 0x3u && n == 1u) || ((code == 0x7u || code == 0x8u) && n == 3u))
                targets.push_back(args[n - 1u]);
        }
        for (const int16_t t : targets)
            if (std::find(labels.begin(), labels.end(), t) == labels.end())
                return false;
        if (at != g_code.size())
            return false;
        static_assert(sizeof(kAskText) < 0x80 && sizeof(kThanksText) < 0x80);
        if (g_code.size() > kTexts - kScript)
            return false;
        std::memcpy(rdram + kScript, g_code.data(), g_code.size());
        std::memset(rdram + kTexts, 0, 0x100);
        std::memcpy(rdram + kTexts, kAskText, sizeof(kAskText));
        std::memcpy(rdram + kTexts + 0x80u, kThanksText, sizeof(kThanksText));
        return true;
    }
    PS2Runtime::RecompiledFunction g_loadScript = nullptr, g_text = nullptr;
    void loadScript(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_loadScript(rdram, ctx, runtime);
        if (rd<uint32_t>(rdram, kScriptId) != 0x6Fu || rd<uint32_t>(rdram, kScriptPtr) != kShow)
            return;
        if (!writeDollShow(rdram))
            return (void)std::fprintf(stderr, "[GHS-CRUSH] Lost Doll's horror show isn't the script as found: no Doll question\n");
        std::memcpy(rdram + kScriptPtr, &kScript, 4);
        if (rd<int16_t>(rdram, kEquipped) != kDollItem)
            return;
        std::memcpy(g_hadCondition, rdram + kConditions, sizeof(g_hadCondition));
        g_noConditionsUntil = g_frame + kNoConditions;
        std::fprintf(stderr, "[GHS-CRUSH] Lost Doll's horror show: the Doll is equipped, she asks for it\n");
    }
    void text(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t id = getRegU32(ctx, 5);
        if (getRegU32(ctx, 4) != 4u || getRegU32(ctx, 6) != 0u || (id != static_cast<uint32_t>(kAsk) && id != static_cast<uint32_t>(kThanks)))
            return g_text(rdram, ctx, runtime);
        SET_GPR_U32(ctx, 2, kTexts + (id == static_cast<uint32_t>(kAsk) ? 0u : 0x80u));
        returnToCaller(ctx);
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
        if ((g_loadScript = runtime.lookupFunction(0x0024FA00u)))
            runtime.replaceFunction(0x0024FA00u, &loadScript);
        if ((g_text = runtime.lookupFunction(0x002D8D90u)))
            runtime.replaceFunction(0x002D8D90u, &text);
        std::fprintf(stderr, "[GHS-CRUSH] on: Lost Doll has a crush on the player\n");
    }

    const ps2_game_overrides::Descriptor kDescriptor{"Gregory Horror Show (EU): Lost Doll's crush (mod)", "SLES_519.33", 0x00100008u, 0u, apply};
    const ps2_game_overrides::AutoRegister kRegister(kDescriptor);
}
