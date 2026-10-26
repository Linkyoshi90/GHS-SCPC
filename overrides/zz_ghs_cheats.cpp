// Gregory Horror Show: cheats (F5 window, ghs_cheats.h). Applied once a frame on the game thread from the game's VBlank
// callback 0x12B350 (frame counter gp+0x274; registered through sceGsSyncVCallback 0x101FC0).
// RAM (found 2026-10-23 with RAM diffs at the Frog Fortune-Teller save, see README):
//   player data 0x4503B0 (0x780 bytes, playerDataInit 0x12C2A0): mental gauge float at +0x220 (0x4505D0), its maximum
//   at +0x224 (3000); inventory 0x4506B0: 16 slots of 6 bytes {u16 id (0xFFFF empty), u8 count, u8 used, u8, u8},
//   slots 0-7 items (ids 0-129), 8-15 key items (ids 130 and up);
//   in-game clock: FUN_1D8DF0(a0 = step) adds a0 to the clock (0x5E9934) and the night timer (0x4506A8); the frame tick
//   FUN_1D8930 calls it with 30 (60 Hz) / 36 (50 Hz) a frame.
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ghs_cheats.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <array>
#include <map>
#include <random>
#include <set>
#include <vector>
#include <mutex>
#include <string>
#include <thread>

namespace ghs_cheats
{
    constexpr uint32_t kGauge = 0x004505D0u, kGaugeMax = 0x004505D4u;
    constexpr uint32_t kInventory = 0x004506B0u, kSlotBytes = 6u, kItemSlots = 8u, kKeySlots = 8u;
    constexpr int kFirstKeyItem = 130;
    constexpr uint32_t kCameraMode = 0x0043D098u;
    // Actors: 0x4717A0 + slot * 0x6C0, slot 0 the player, a guest keeps her slot (6 = Catherine); +0x08 flags (AI
    // mode bits, see the guests below), +0x80 position x, y, z (floats).
    constexpr uint32_t kActors = 0x004717A0u, kActorBytes = 0x6C0u, kGuests = 64u;
    constexpr uint32_t kGuestModes = 0xFE10u; // the dispatcher FUN_140730's AI modes (0x10, 0x200 ... 0x8000)

    struct Slot
    {
        uint16_t id;
        uint8_t count, used, b4, b5;
    };
    Slot readSlot(const uint8_t *rdram, uint32_t i)
    {
        Slot s;
        std::memcpy(&s, rdram + kInventory + i * kSlotBytes, sizeof(s));
        return s;
    }
    void writeSlot(uint8_t *rdram, uint32_t i, const Slot &s) { std::memcpy(rdram + kInventory + i * kSlotBytes, &s, sizeof(s)); }

    // Infinite items: per id, the count summed over a list's slots must not drop below the last snapshot; what was
    // used is added back (to that id's slot, or the first empty one, as the game may close the gap). Many changes at
    // once (a save loaded, a new game) take a new snapshot instead.
    struct ListGuard
    {
        uint32_t first, count;
        std::map<uint16_t, Slot> lastSlot; // a slot as it was, to rebuild a vanished item
        std::map<uint16_t, int> total;
        bool valid = false;
        int seen = 0; // frames a drop has been there: the VSync callback can come while the game moves slots
        uint32_t camera = 0; // camera mode (0x43D098) before the drop

        std::map<uint16_t, int> totals(const uint8_t *rdram) const
        {
            std::map<uint16_t, int> t;
            for (uint32_t i = first; i < first + count; ++i)
                if (const Slot s = readSlot(rdram, i); s.id != 0xFFFFu)
                    t[s.id] += s.count;
            return t;
        }
        void snapshot(const uint8_t *rdram)
        {
            total = totals(rdram);
            lastSlot.clear();
            for (uint32_t i = first; i < first + count; ++i)
                if (const Slot s = readSlot(rdram, i); s.id != 0xFFFFu)
                    lastSlot[s.id] = s;
            valid = true;
        }
        void apply(uint8_t *rdram, bool on)
        {
            if (!on || !valid)
            {
                snapshot(rdram);
                return;
            }
            const auto now = totals(rdram);
            int changed = 0;
            for (const auto &[id, n] : total)
                changed += now.count(id) == 0 || now.at(id) != n;
            for (const auto &[id, n] : now)
                changed += total.count(id) == 0;
            if (changed > 2) // a save loaded / a new game: not items being used
            {
                snapshot(rdram);
                return;
            }
            bool dropped = false;
            for (const auto &[id, n] : total)
                dropped |= (now.count(id) ? now.at(id) : 0) < n;
            uint32_t cameraNow;
            std::memcpy(&cameraNow, rdram + kCameraMode, 4);
            if (!dropped)
            {
                seen = 0;
                camera = cameraNow;
                snapshot(rdram); // new items join the snapshot
                return;
            }
            // Back once the use is over (camera mode as before; a thrown banana peel keeps the camera on its spot until
            // then, and an item back mid-throw left it there for good), or after 10 s.
            if (++seen < 2 || (cameraNow != camera && seen < 600))
                return;
            seen = 0;
            for (const auto &[id, n] : total)
            {
                const int have = now.count(id) ? now.at(id) : 0;
                if (have >= n)
                    continue;
                int missing = n - have;
                for (uint32_t i = first; i < first + count && missing > 0; ++i) // top up the id's slot
                    if (Slot s = readSlot(rdram, i); s.id == id)
                    {
                        const int add = std::min(missing, 99 - static_cast<int>(s.count));
                        s.count = static_cast<uint8_t>(s.count + add);
                        missing -= add;
                        writeSlot(rdram, i, s);
                    }
                for (uint32_t i = first; i < first + count && missing > 0; ++i) // or bring it back
                    if (readSlot(rdram, i).id == 0xFFFFu)
                    {
                        Slot s = lastSlot[id];
                        s.count = static_cast<uint8_t>(std::min(missing, 99));
                        missing -= s.count;
                        writeSlot(rdram, i, s);
                    }
            }
            snapshot(rdram);
        }
    };
    ListGuard g_items{0u, kItemSlots}, g_keyItems{kItemSlots, kKeySlots};

    // Gives one item: one more of it if a slot holds it, else the first empty slot of its list.
    void give(uint8_t *rdram, int id)
    {
        const bool key = id >= kFirstKeyItem;
        const uint32_t first = key ? kItemSlots : 0u, count = key ? kKeySlots : kItemSlots;
        for (uint32_t i = first; i < first + count; ++i)
            if (Slot s = readSlot(rdram, i); s.id == id)
            {
                if (s.count < 99u)
                    ++s.count;
                writeSlot(rdram, i, s);
                return;
            }
        for (uint32_t i = first; i < first + count; ++i)
            if (readSlot(rdram, i).id == 0xFFFFu)
            {
                // The new slot like the game's own: items {id, 1, 0, 0xFF, 0}; key items {id, 1, 0, 0, 0}
                // (ponytail: the game's key items hold 5 or 6 in byte 4, meaning unknown; 0 shows in the menu).
                writeSlot(rdram, i, Slot{static_cast<uint16_t>(id), 1u, 0u, static_cast<uint8_t>(key ? 0u : 0xFFu), 0u});
                return;
            }
        std::fprintf(stderr, "[GHS-CHEAT] give %d: the list is full\n", id);
    }

    // Takes one away: one less in its slot, or the slot emptied and the ones after it moved up (the game's lists have
    // no gaps; an empty slot is {0xFFFF, 0, 0, 0xFF, 0}).
    void remove(uint8_t *rdram, int id)
    {
        const uint32_t first = id >= kFirstKeyItem ? kItemSlots : 0u, count = id >= kFirstKeyItem ? kKeySlots : kItemSlots;
        for (uint32_t i = first; i < first + count; ++i)
            if (Slot s = readSlot(rdram, i); s.id == id)
            {
                if (s.count > 1u)
                {
                    --s.count;
                    writeSlot(rdram, i, s);
                    return;
                }
                for (uint32_t j = i; j + 1u < first + count; ++j)
                    writeSlot(rdram, j, readSlot(rdram, j + 1u));
                writeSlot(rdram, first + count - 1u, Slot{0xFFFFu, 0u, 0u, 0xFFu, 0u});
                return;
            }
        std::fprintf(stderr, "[GHS-CHEAT] remove %d: not in the list\n", id);
    }

    // Item randomizer (placements; found 2026-10-24): the items lying in the hotel are a table at 0x5DB7E0, 0x50 bytes
    // per entry, 110 entries sorted by room: +0x00 position x, y, z; +0x30 room (int16, -1 once taken); +0x32 item id;
    // +0x34 the game's item class (0 drink, 1 food, 2 herb, 3 medicine, 5 book, 6-8 key items, 9 'shrooms); +0x35 a
    // state (1 lying there, 10 hidden in furniture, ...). FUN_1BB1B0(entry) picks one up. The table is part of the save
    // (every slot on the card holds it), so a shuffle in RAM stays once the game is saved. Roll shuffles the item ids
    // (with their class byte) among the untaken entries of the ticked classes: regular items with regular items, key
    // items with key items. Classes by id: bit 0 food 7-27, 1 drinks 0-5 and 28-30, 2 books 78-128 and 143-146, 3 herbs
    // and medicine 6 and 31-43, 4 trading cards 47-62 and 64, 5 trash 44-46, 6 key items 130-138.
    // ponytail: items already shown in the current room keep their old look until the room is entered again.
    constexpr uint32_t kPlacements = 0x005DB7E0u, kPlacementBytes = 0x50u, kPlacementCount = 110u;
    int itemClass(int id)
    {
        if (id >= 7 && id <= 27)
            return 0;
        if ((id >= 0 && id <= 5) || (id >= 28 && id <= 30))
            return 1;
        if ((id >= 78 && id <= 128) || (id >= 143 && id <= 146))
            return 2;
        if (id == 6 || (id >= 31 && id <= 43))
            return 3;
        if ((id >= 47 && id <= 62) || id == 64)
            return 4;
        if (id >= 44 && id <= 46)
            return 5;
        if (id >= 130 && id <= 138)
            return 6;
        return -1;
    }
    // Per-room point lists (0x326DD0 by room number, 0x28-byte records ending with a negative type; type 0 = door with
    // its id at +8, 9 = a floor spot, positions at +0xC / 64). See the warp below.
    constexpr uint32_t kRoomDoors = 0x00326DD0u;

    // "Move items" (randomizer option, 2026-10-25): after the shuffle, the lying items (state 1) of the ticked classes go
    // to random floor spots: the rooms' type-9 points and the lying items' own spots, in any room that holds items (a
    // room never used by an item may belong to another night). Hidden items stay with their furniture. Tested: a
    // herb moved from the hallway to the frog's room shows there and can be picked up.
    struct Spot
    {
        int16_t room;
        float pos[3];
    };
    void moveLyingItems(uint8_t *rdram, uint32_t classes, std::mt19937 &rng)
    {
        std::vector<Spot> spots;
        std::set<int16_t> rooms;
        std::vector<uint32_t> movers;
        for (uint32_t n = 0; n < kPlacementCount; ++n)
        {
            const uint32_t e = kPlacements + n * kPlacementBytes;
            int16_t room, id;
            std::memcpy(&room, rdram + e + 0x30u, 2);
            std::memcpy(&id, rdram + e + 0x32u, 2);
            if (room < 0)
                continue;
            rooms.insert(room);
            if (rdram[e + 0x35u] != 1u)
                continue;
            Spot s{room, {}};
            std::memcpy(s.pos, rdram + e, sizeof(s.pos));
            spots.push_back(s);
            if (const int cls = itemClass(id); cls >= 0 && (classes & (1u << cls)) != 0u)
                movers.push_back(e);
        }
        for (const int16_t room : rooms)
        {
            uint32_t list;
            std::memcpy(&list, rdram + kRoomDoors + 4u * static_cast<uint32_t>(room), 4);
            if (list < 0x100000u || list >= 0x2000000u)
                continue;
            for (uint32_t k = 0; k < 64u; ++k)
            {
                const uint32_t r = list + k * 0x28u;
                int32_t type;
                std::memcpy(&type, rdram + r, 4);
                if (type < 0)
                    break;
                if (type != 9)
                    continue;
                Spot s{room, {}};
                std::memcpy(s.pos, rdram + r + 0xCu, sizeof(s.pos));
                for (float &v : s.pos)
                    v *= 64.0f;
                const bool seen = std::any_of(spots.begin(), spots.end(), [&](const Spot &o)
                                              { return o.room == s.room && std::fabs(o.pos[0] - s.pos[0]) < 32.0f && std::fabs(o.pos[2] - s.pos[2]) < 32.0f; });
                if (!seen)
                    spots.push_back(s);
            }
        }
        std::shuffle(spots.begin(), spots.end(), rng);
        const size_t n = std::min(movers.size(), spots.size());
        for (size_t i = 0; i < n; ++i)
        {
            std::memcpy(rdram + movers[i], spots[i].pos, sizeof(spots[i].pos));
            std::memcpy(rdram + movers[i] + 0x30u, &spots[i].room, 2);
            int16_t id;
            std::memcpy(&id, rdram + movers[i] + 0x32u, 2);
            std::fprintf(stderr, "[GHS-CHEAT] randomizer: item %d -> room %d (%.0f, %.0f)\n", id, spots[i].room, spots[i].pos[0], spots[i].pos[2]);
        }
        std::fprintf(stderr, "[GHS-CHEAT] randomizer: %zu lying items moved among %zu spots in %zu rooms\n", n, spots.size(), rooms.size());
    }

    void shufflePlacements(uint8_t *rdram, uint32_t classes, uint32_t seed, bool move)
    {
        std::vector<uint32_t> spots[2]; // entry addresses: regular items, key items
        for (uint32_t n = 0; n < kPlacementCount; ++n)
        {
            const uint32_t e = kPlacements + n * kPlacementBytes;
            int16_t room, id;
            std::memcpy(&room, rdram + e + 0x30u, 2);
            std::memcpy(&id, rdram + e + 0x32u, 2);
            const int cls = itemClass(id);
            if (room >= 0 && cls >= 0 && (classes & (1u << cls)) != 0u)
                spots[cls == 6 ? 1 : 0].push_back(e);
        }
        std::mt19937 rng(seed);
        for (const std::vector<uint32_t> &pool : spots)
        {
            std::vector<std::array<uint8_t, 3>> items; // id (2 bytes) and class byte, moved together
            for (const uint32_t e : pool)
                items.push_back({rdram[e + 0x32u], rdram[e + 0x33u], rdram[e + 0x34u]});
            std::shuffle(items.begin(), items.end(), rng);
            for (size_t i = 0; i < pool.size(); ++i)
            {
                rdram[pool[i] + 0x32u] = items[i][0];
                rdram[pool[i] + 0x33u] = items[i][1];
                rdram[pool[i] + 0x34u] = items[i][2];
            }
        }
        std::fprintf(stderr, "[GHS-CHEAT] randomizer: seed %u, classes 0x%x: %zu item placements and %zu key item placements shuffled\n", seed,
                     classes, spots[0].size(), spots[1].size());
        if (move)
            moveLyingItems(rdram, classes, rng);
    }

    // Guest timetables (found 2026-10-25): actor +0xD8 points to the guest's entries for the night (16 bytes: int16 start
    // and end as clock times, 1200 = 12:00; byte room; activity bytes); actor +0x220 is the room the guest is in. The game
    // follows an edited room (tested: James 59 -> 53 instead of 66 at 14:00, then 81 as usual). "Shuffle guests' visits"
    // swaps only one-off visits: a guest's main room (most time) and any room a guest comes back to in two separate visits
    // is protected (their own rooms, where soul quests happen), as are rooms without a door list (7, 8: not places).
    // The tables are rebuilt each night and on loads: a changed table set is shuffled again (seed: randomizer seed, night).
    constexpr uint32_t kTimetablesBegin = 0x005E9000u, kTimetablesEnd = 0x005EE000u, kNight = 0x004503B8u;
    struct Visit
    {
        uint32_t entry;
        int room;
    };
    int minutes(int hhmm) { return (hhmm / 100) * 60 + hhmm % 100; }
    // +0xD8 points at the current entry and moves on during the night; the entry's byte 14 is its index in the table,
    // so the table starts where that run of indices does. An entry may end at 0 (midnight); 0-0 ends the table.
    bool timetableEntry(const uint8_t *rdram, uint32_t e)
    {
        if (e < kTimetablesBegin || e + 16u > kTimetablesEnd)
            return false;
        int16_t start, end;
        std::memcpy(&start, rdram + e, 2);
        std::memcpy(&end, rdram + e + 2u, 2);
        return start >= 0 && start < 2400 && end >= 0 && end <= 2400 && (start != 0 || end != 0);
    }
    std::vector<std::vector<Visit>> readTimetables(const uint8_t *rdram)
    {
        std::vector<std::vector<Visit>> tables;
        for (uint32_t slot = 1; slot < kGuests; ++slot)
        {
            uint32_t p;
            std::memcpy(&p, rdram + kActors + slot * kActorBytes + 0xD8u, 4);
            if (!timetableEntry(rdram, p))
                continue;
            for (int k = 0; k < 40 && timetableEntry(rdram, p - 16u) && rdram[p - 2u] + 1 == rdram[p + 14u]; ++k)
                p -= 16u;
            std::vector<Visit> t;
            for (uint32_t k = 0; k < 40u && timetableEntry(rdram, p + 16u * k); ++k)
                t.push_back({p + 16u * k, rdram[p + 16u * k + 4u]});
            tables.push_back(std::move(t));
        }
        return tables;
    }
    bool isPlace(const uint8_t *rdram, int room) // has a door list: a real room (7, 8 are codes, 7 = the guest's own room)
    {
        uint32_t list;
        std::memcpy(&list, rdram + kRoomDoors + 4u * static_cast<uint32_t>(room), 4);
        return list >= 0x100000u && list < 0x2000000u;
    }
    uint64_t timetableHash(const uint8_t *rdram) // times and rooms only (the rest of the area may change while playing)
    {
        uint64_t h = 1469598103934665603ull;
        for (const auto &t : readTimetables(rdram))
            for (const Visit &v : t)
                for (uint32_t i = 0; i < 5u; ++i)
                    h = (h ^ rdram[v.entry + i]) * 1099511628211ull;
        return h;
    }
    uint64_t g_shuffledTables = 0; // hash of the tables after the last shuffle
    void shuffleGuestVisits(uint8_t *rdram)
    {
        const auto tables = readTimetables(rdram);
        std::set<int> guarded;
        for (const auto &t : tables)
        {
            std::map<int, int> time, visits;
            int last = -1, main = -1;
            for (const Visit &v : t)
            {
                int16_t start, end;
                std::memcpy(&start, rdram + v.entry, 2);
                std::memcpy(&end, rdram + v.entry + 2u, 2);
                time[v.room] += (minutes(end) - minutes(start) + 1440) % 1440;
                if (v.room != last)
                    ++visits[v.room];
                last = v.room;
            }
            for (const Visit &v : t) // the real room with the most time; ties: the earlier one
                if (isPlace(rdram, v.room) && (main < 0 || time[v.room] > time[main]))
                    main = v.room;
            if (main >= 0)
                guarded.insert(main);
            for (const auto &[room, n] : visits)
                if (n >= 2)
                    guarded.insert(room);
        }
        std::vector<uint32_t> entries;
        std::vector<uint8_t> rooms;
        for (const auto &t : tables)
            for (const Visit &v : t)
                if (!guarded.count(v.room) && isPlace(rdram, v.room))
                {
                    entries.push_back(v.entry);
                    rooms.push_back(static_cast<uint8_t>(v.room));
                }
        std::mt19937 rng(g_ghsCheats.randomizerSeed.load() ^ (0x9E3779B9u * (rdram[kNight] + 1u)));
        std::shuffle(rooms.begin(), rooms.end(), rng);
        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (rdram[entries[i] + 4u] != rooms[i])
            {
                int16_t start;
                std::memcpy(&start, rdram + entries[i], 2);
                std::fprintf(stderr, "[GHS-CHEAT] guests: visit at %04d room %u -> %u\n", start, rdram[entries[i] + 4u], rooms[i]);
            }
            rdram[entries[i] + 4u] = rooms[i];
        }
        g_shuffledTables = timetableHash(rdram);
        std::fprintf(stderr, "[GHS-CHEAT] guests: night %u, %zu visits shuffled (%zu rooms protected)\n", rdram[kNight], entries.size(), guarded.size());
    }

    // Guest radar: actors with an AI mode, their flags and x/z, and the player's.
    void fillRadar(const uint8_t *rdram)
    {
        std::lock_guard<std::mutex> lock(g_ghsCheats.radarLock);
        int n = 0;
        for (uint32_t slot = 1; slot < kGuests; ++slot)
        {
            const uint8_t *actor = rdram + kActors + slot * kActorBytes;
            GhsGuestView g{static_cast<int>(slot), 0u, 0.0f, 0.0f};
            std::memcpy(&g.flags, actor + 0x08, 4);
            if ((g.flags & kGuestModes) == 0u)
                continue;
            std::memcpy(&g.x, actor + 0x80, 4);
            std::memcpy(&g.z, actor + 0x88, 4);
            g_ghsCheats.guests[static_cast<size_t>(n++)] = g;
        }
        g_ghsCheats.guestCount = n;
        std::memcpy(&g_ghsCheats.playerX, rdram + kActors + 0x80, 4);
        std::memcpy(&g_ghsCheats.playerZ, rdram + kActors + 0x88, 4);
    }

    // Timed effects (found 2026-10-24 by using a Speed Up and RAM diffs): FUN_1A4A50(i) keeps effect i, flag byte
    // 0x4505F6 + i, began at the in-game clock (0x5E9934) stored in 0x450600 + 4 i; an item's effect index is field 1 of
    // the item table (0x317524, 32 bytes per item, read by FUN_1A77D0): 0-6 the cure items' protection (Headache
    // Medicine 0, Laughing 'Shrooms 1, Antacid 2, Sedative 4, Eyedrops 6), 8 Mind Up (FUN_1A50B0: max MG + bonus, at
    // most 6000), 9 Speed Up (FUN_1A5040: flag & 0x7F == 1 and FUN_1A4A50's 30 minutes not over; none while 0x4505D9).
    // The conditions themselves are bytes 0x4505D8-0x4505DE (1 = has it; the menu's faces, FUN_1D7820: tiredness,
    // headache, nervousness, confusion, melancholy ... darkness; checked by setting them).
    constexpr uint32_t kEffectFlags = 0x004505F6u, kEffectStart = 0x00450600u, kClock = 0x005E9934u, kSpeedUp = 9u;
    constexpr uint32_t kConditions = 0x004505D8u, kConditionCount = 7u;
    void applyEffects(uint8_t *rdram)
    {
        if (g_ghsCheats.speedUp.load(std::memory_order_relaxed))
        {
            rdram[kEffectFlags + kSpeedUp] = 1u;
            std::memcpy(rdram + kEffectStart + kSpeedUp * 4u, rdram + kClock, 4); // began just now: never 30 minutes old
        }
        if (g_ghsCheats.noConditions.load(std::memory_order_relaxed))
            std::memset(rdram + kConditions, 0, kConditionCount);
    }

    // Key items whose scenes need the guest to see and chase the player: while one is equipped (int16 0x450710),
    // invulnerability and no detection step aside. Screwdriver: used on Clock Master, he chases you (user, 2026-10-24);
    // Dice: Roulette Boy (user, 2026-10-25). Holding one in the pocket isn't enough: with "held" the cheats were off for
    // the rest of the night.
    constexpr uint32_t kEquippedKeyItem = 0x00450710u;
    bool g_storyItem = false;
    bool storyItemEquipped(const uint8_t *rdram)
    {
        int16_t id;
        std::memcpy(&id, rdram + kEquippedKeyItem, 2);
        return id == 137 || id == 135 || id == 133; // Screwdriver, Dice, Self-help Book (user, 2026-10-26)
    }

    // Warp (found 2026-10-25 with RAM dumps around a door): a door's static definition (16 bytes at 0x36A700-0x36B300:
    // int16 door id, ..., +8 int16 room it leads to) is pointed to by the loaded section's door entries (0x465188, 0x98
    // bytes each: +2 door id, +0x24 definition). The door state machine FUN_159880 (state gp+0xAA0, run while the player
    // mode gp+0x8D0 is 0x1F4) takes entry 0x47F180 in state 0, finds the door in the destination's list (FUN_1D3A60:
    // table 0x326DD0 by room, 0x28-byte records, type 0 = door, +8 door id) and loads that room (current room 0x47F178,
    // previous 0x47F174). A warp points entry 0 at a definition leading into the chosen room and starts the sequence;
    // the game's own transition does the rest (tested: frog room -> room 11). Rooms are numbers: the game keeps no
    // name table (names are in the event text).
    constexpr uint32_t kDoorEntries = 0x00465188u, kDoorDefsBegin = 0x0036A700u, kDoorDefsEnd = 0x0036B300u;
    constexpr uint32_t kDoorIndex = 0x0047F180u, kCurrentRoom = 0x0047F178u, kDoorState = 0x0043C210u, kPlayerMode = 0x0043C040u;
    constexpr uint32_t kGameState = 0x0043DB1Cu, kGameStatePlay = 0x3Au;
    std::map<int, uint32_t> g_warpDefs; // room -> a definition leading into it
    bool roomHasDoor(const uint8_t *rdram, int room, int door)
    {
        uint32_t list;
        std::memcpy(&list, rdram + kRoomDoors + 4u * static_cast<uint32_t>(room), 4);
        if (list < 0x100000u || list >= 0x2000000u)
            return false;
        for (uint32_t k = 0; k < 64u; ++k)
        {
            int32_t type, id;
            std::memcpy(&type, rdram + list + k * 0x28u, 4);
            std::memcpy(&id, rdram + list + k * 0x28u + 8u, 4);
            if (type < 0)
                return false;
            if (type == 0 && id == door)
                return true;
        }
        return false;
    }
    void findWarpDefs(const uint8_t *rdram)
    {
        for (uint32_t a = kDoorDefsBegin; a < kDoorDefsEnd; a += 16u)
        {
            int16_t def[6];
            uint32_t tail;
            std::memcpy(def, rdram + a, sizeof(def));
            std::memcpy(&tail, rdram + a + 12u, 4);
            const int door = def[0], room = def[4];
            // 155 (0x9B) takes another path in FUN_159880; an ordinary door (+4 == 1) is preferred
            if (tail != 0u || door <= 0 || door >= 300 || door == 155 || room < 0 || room >= 256 || !roomHasDoor(rdram, room, door))
                continue;
            if (!g_warpDefs.count(room) || def[2] == 1)
                g_warpDefs[room] = a;
        }
        int n = 0;
        for (const auto &[room, def] : g_warpDefs)
            if (n < static_cast<int>(g_ghsCheats.warpRooms.size()))
                g_ghsCheats.warpRooms[static_cast<size_t>(n++)] = static_cast<int16_t>(room);
        g_ghsCheats.warpRoomCount = n;
        std::fprintf(stderr, "[GHS-CHEAT] warp: %d rooms\n", n);
    }
    void warp(uint8_t *rdram)
    {
        uint32_t state, mode, doorState;
        std::memcpy(&state, rdram + kGameState, 4);
        if (state != kGameStatePlay)
            return;
        if (g_warpDefs.empty())
            findWarpDefs(rdram);
        int32_t room;
        std::memcpy(&room, rdram + kCurrentRoom, 4);
        g_ghsCheats.currentRoom = room;
        const int to = g_ghsCheats.warpTo.load(std::memory_order_relaxed);
        if (to < 0)
            return;
        std::memcpy(&mode, rdram + kPlayerMode, 4);
        std::memcpy(&doorState, rdram + kDoorState, 4);
        if (mode != 0u || doorState != 0u) // talking, a door, a menu: wait until the player is free
            return;
        g_ghsCheats.warpTo = -1;
        const auto it = g_warpDefs.find(to);
        if (it == g_warpDefs.end())
            return;
        int16_t door;
        std::memcpy(&door, rdram + it->second, 2);
        std::memcpy(rdram + kDoorEntries + 2u, &door, 2);   // the section's door 0: its id and where it leads
        std::memcpy(rdram + kDoorEntries + 0x24u, &it->second, 4); // (the section is rebuilt with the new room)
        std::memset(rdram + kDoorIndex, 0, 4);
        const uint32_t doorMode = 0x1F4u;
        std::memcpy(rdram + kPlayerMode, &doorMode, 4);
        std::fprintf(stderr, "[GHS-CHEAT] warp: room %d -> %d (door %d)\n", room, to, door);
    }

    void applyCheats(uint8_t *rdram)
    {
        g_storyItem = storyItemEquipped(rdram);
        warp(rdram);
        // Twice a second: new tables (night start, load) get shuffled again.
        static uint32_t tick = 0;
        if (g_ghsCheats.guestShuffle.load(std::memory_order_relaxed) && ++tick % 30u == 0u)
        {
            uint32_t state;
            std::memcpy(&state, rdram + kGameState, 4);
            if (state == kGameStatePlay && timetableHash(rdram) != g_shuffledTables)
                shuffleGuestVisits(rdram);
        }
        applyEffects(rdram);
        if (g_ghsCheats.radar.load(std::memory_order_relaxed))
            fillRadar(rdram);
        if (g_ghsCheats.infiniteHealth.load(std::memory_order_relaxed))
            std::memcpy(rdram + kGauge, rdram + kGaugeMax, 4);
        g_items.apply(rdram, g_ghsCheats.infiniteItems.load(std::memory_order_relaxed));
        g_keyItems.apply(rdram, g_ghsCheats.infiniteKeyItems.load(std::memory_order_relaxed));
        if (const int id = g_ghsCheats.giveItem.exchange(-1); id >= 0)
        {
            give(rdram, id);
            g_items.snapshot(rdram); // a given item isn't "used up" when it shows
            g_keyItems.snapshot(rdram);
            std::fprintf(stderr, "[GHS-CHEAT] gave item %d\n", id);
        }
        if (const int id = g_ghsCheats.removeItem.exchange(-1); id >= 0)
        {
            remove(rdram, id);
            g_items.snapshot(rdram); // so infinite items doesn't bring it back
            g_keyItems.snapshot(rdram);
            std::fprintf(stderr, "[GHS-CHEAT] removed item %d\n", id);
        }
        if (const uint32_t seed = g_ghsCheats.randomizerRoll.exchange(0u); seed != 0u)
            shufflePlacements(rdram, g_ghsCheats.randomizerClasses.load() & 0x3Fu, seed, // never key items (they broke quests)
                              g_ghsCheats.randomizerMove.load());
    }

    uint32_t g_frame = 0; // VBlanks, for the guests' calm spell
    PS2Runtime::RecompiledFunction g_vsync = nullptr;
    void vsync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        applyCheats(rdram);
        ++g_frame;
        g_vsync(rdram, ctx, runtime);
    }

    // Time speed: the frame tick's step scaled, the remainder carried (x0.25 = every 4th frame's step).
    PS2Runtime::RecompiledFunction g_clockStep = nullptr;
    double g_carry = 0.0;
    void clockStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        const float scale = g_ghsCheats.timeScale.load(std::memory_order_relaxed);
        if (ra >= 0x001D8930u && ra < 0x001D8AF4u && scale != 1.0f) // from the frame tick only (not time jumps)
        {
            const double step = static_cast<int32_t>(getRegU32(ctx, 4)) * static_cast<double>(scale) + g_carry;
            const double whole = std::floor(step);
            g_carry = step - whole;
            SET_GPR_U32(ctx, 4, static_cast<uint32_t>(static_cast<int32_t>(whole)));
        }
        g_clockStep(rdram, ctx, runtime);
    }

    // Guests (found 2026-10-23 with zz_ghs_actor_trace.cpp and RAM dumps, Catherine outside the AAA save room): the guest
    // dispatcher FUN_140730 runs a guest's AI by her flags (actor +0x08): 0x2000 the hunting AI FUN_1BFD60, 0x1000 the
    // calm one FUN_1BE690 (after her catch). The hunting AI spots the player with the vision cone test FUN_1CB760(guest, 0)
    // (from 0x1C033C and, while she looks around, FUN_1CCC50; trace 2026-10-23), hears him through 0x47F13C = 3, also
    // reads 0x47F184; then her sight counter +0x688 = 100 and +0x690 pursue. A catch is FUN_1C1010(guest) from the
    // hunting AI, or the player's contact check (FUN_14ABF0 after
    // FUN_156FB0 = 3, talk partner gp+0x8D0 = 0x96); both start the encounter FUN_217970(guest, 0) (0x4505B0 = 1, guest
    // in 0x4505B4: dialogue, injection, gauge drop).
    // No detection: a guest's vision tests all say "not seen" (FUN_1CB760 is called from several places in her AI), and
    // while her AI runs (hunting FUN_1BFD60, searching FUN_1C56A0 for flags 0x8000) she hears nothing and forgets a
    // pursuit, so she keeps her patrol. Invulnerability: a catch doesn't start, and that guest gets the same for a while
    // instead (like the calm spell after a real catch; without it she circles the player trying again every frame).
    constexpr uint32_t kCalmFrames = 600u;
    constexpr uint32_t kHeard = 0x0047F13Cu, kSeen = 0x0047F184u;
    uint32_t g_calmUntil[kGuests] = {};
    uint32_t g_blocked = 0; // vision tests answered "not seen" (logged now and then)
    void returnToCaller(R5900Context *ctx) { ctx->pc = getRegU32(ctx, 31); }
    bool noDetect() { return g_ghsCheats.noDetection.load(std::memory_order_relaxed) && !g_storyItem; }
    bool noCatch() { return (g_ghsCheats.invulnerable.load(std::memory_order_relaxed) && !g_storyItem) || noDetect(); }
    bool unnoticed(uint32_t guest) // actor 0 is the player
    {
        return guest != 0u && guest < kGuests && (noDetect() || static_cast<int32_t>(g_calmUntil[guest] - g_frame) > 0);
    }
    void calmDown(uint32_t guest)
    {
        if (guest < kGuests)
            g_calmUntil[guest] = g_frame + kCalmFrames;
    }

    PS2Runtime::RecompiledFunction g_guestCatch = nullptr, g_encounter = nullptr, g_huntAi = nullptr, g_searchAi = nullptr, g_calmAi = nullptr, g_sees = nullptr;
    void guestCatch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!noCatch())
            return g_guestCatch(rdram, ctx, runtime);
        calmDown(getRegU32(ctx, 4));
        returnToCaller(ctx);
    }
    void encounter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        if (getRegU32(ctx, 5) != 0u || (ra != 0x0014BCACu && ra != 0x0014C8C4u) || !noCatch()) // scripted encounters run
            return g_encounter(rdram, ctx, runtime);
        calmDown(getRegU32(ctx, 4));
        std::memset(rdram + 0x0043C040u, 0, 4); // the talk partner the caller set (0x96)
        SET_GPR_U32(ctx, 2, 0u);
        returnToCaller(ctx);
    }
    void runUnnoticed(PS2Runtime::RecompiledFunction ai, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t guest = getRegU32(ctx, 4);
        if (!unnoticed(guest))
            return ai(rdram, ctx, runtime);
        uint8_t *actor = rdram + kActors + guest * kActorBytes;
        std::memset(actor + 0x688, 0, 2); // forgets a pursuit
        actor[0x690] = 0;
        uint32_t heard, seen;
        std::memcpy(&heard, rdram + kHeard, 4);
        std::memcpy(&seen, rdram + kSeen, 4);
        std::memset(rdram + kHeard, 0, 4);
        std::memset(rdram + kSeen, 0, 4);
        ai(rdram, ctx, runtime);
        std::memcpy(rdram + kHeard, &heard, 4); // the AI only reads them; others see the real values
        std::memcpy(rdram + kSeen, &seen, 4);
    }
    void huntAi(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { runUnnoticed(g_huntAi, rdram, ctx, runtime); }
    void searchAi(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { runUnnoticed(g_searchAi, rdram, ctx, runtime); }
    // The routine AI too: it hears the player through the same flags (user 2026-10-26: guests still noticed the player;
    // test, night 2 hallway: with every vision test blocked Catherine still turned to hunting; with this she keeps her
    // scheduled search patrol (0x8000, set by the routine AI on arriving: not a detection) and never hunts or catches).
    void calmAi(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { runUnnoticed(g_calmAi, rdram, ctx, runtime); }
    // Touch: the player's state machine finds the actor touching him with FUN_276050 (index in 0x47F128, -1 none),
    // then FUN_156FB0 classifies her; a guest bumped into turns suspicious ("?") and follows. Under no detection a guest
    // in her routine (0x1000), hunting (0x2000) or searching (0x8000) isn't found at the touch check (ra 0x14BB70: the
    // classification, player mode 0xAA / an encounter); the frog (slot 25, flags 0xFFFF) still is, for saving. Calm
    // guests were found until 2026-10-26 (test: Catherine walked into the player and turned to searching).
    PS2Runtime::RecompiledFunction g_contact = nullptr;
    void contact(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        g_contact(rdram, ctx, runtime);
        if ((ra != 0x0014BA44u && ra != 0x0014BB70u) || !noDetect())
            return;
        int32_t guest;
        std::memcpy(&guest, rdram + 0x0047F128u, 4);
        uint32_t flags = 0u;
        if (guest > 0 && static_cast<uint32_t>(guest) < kGuests)
            std::memcpy(&flags, rdram + kActors + static_cast<uint32_t>(guest) * kActorBytes + 0x8u, 4);
        const uint32_t hidden = ra == 0x0014BB70u ? 0xB000u : 0xA000u; // the 0x14BA44 check (Gregory, James) as before
        if (guest != 25 && (flags & hidden) != 0u)
            std::memset(rdram + 0x0047F128u, 0xFF, 4);
    }
    // Every vision test (the routine AI spots the player with it too: limiting it to the hunting/searching AI let guests
    // detect the player, user 2026-10-26). Story scenes that need a guest to see the player go through the equipped
    // story item instead (noDetect() is off while one is equipped).
    void sees(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!unnoticed(getRegU32(ctx, 4)))
            return g_sees(rdram, ctx, runtime);
        if (++g_blocked % 600u == 1u)
            std::fprintf(stderr, "[GHS-CHEAT] guest %u: vision test %u answered 'not seen'\n", getRegU32(ctx, 4), g_blocked);
        SET_GPR_U32(ctx, 2, 0u);
        returnToCaller(ctx);
    }

    // GHS-TRACE (screwdriver quest, 2026-10-24): FUN_2567F0(id) = "key item id is equipped" (0x450710), FUN_256680(id)
    // = "holds id"; the event script VM (FUN_24FF60, opcodes 0xF2 / 0x142) asks the first. Each new (caller, key item,
    // result) is logged once, so one play attempt shows which checks Clock Master's script makes.
    PS2Runtime::RecompiledFunction g_equipped = nullptr, g_holds = nullptr;
    std::set<uint64_t> g_itemChecks;
    void logItemCheck(const char *what, uint32_t ra, uint32_t id, uint32_t result)
    {
        if (id >= 130u && id <= 146u && g_itemChecks.insert((uint64_t{ra} << 32) | (id << 8) | (result & 0xFFu)).second)
            std::fprintf(stderr, "[GHS-ITEMCHECK] frame %u: %s(%u) from 0x%x -> %u\n", g_frame, what, id, ra, result);
    }
    void equipped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31), id = getRegU32(ctx, 4);
        g_equipped(rdram, ctx, runtime);
        logItemCheck("equipped", ra, id, getRegU32(ctx, 2));
    }
    void holds(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31), id = getRegU32(ctx, 4);
        g_holds(rdram, ctx, runtime);
        logItemCheck("holds", ra, id, getRegU32(ctx, 2));
    }

    // Roulette (Roulette Boy's board game; found 2026-10-24 from the user's F6 snapshots): the wheel is FUN_2060B0(obj, ...)
    // with its state in gp+0x2490 (0 wait for START, 1 spin up, 2 spinning "X: STOP", 3 slow down, 4 read, 5 show), the
    // needle angle in 0x60E2F8 (radians), the number under it in gp+0x24A4 / 0x24A8 (trunc(2 + degrees / 22.5), 9 -> 1:
    // 16 segments, 1-8 twice). At the stop (X, or for the computer opponent, obj +0x8C set, rand() % 120 frames) the
    // angle is saved in gp+0x2498; after the slow-down animation the needle is put back there and that is the result.
    // So the cheat changes the saved angle at the 2 -> 3 switch: the needle lands on the chosen number.
    constexpr uint32_t kWheelState = 0x0043DC00u, kWheelStopAngle = 0x0043DC08u;
    PS2Runtime::RecompiledFunction g_wheel = nullptr;
    void wheel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t obj = getRegU32(ctx, 4) & 0x01FFFFFFu;
        uint32_t before, after, opponent;
        std::memcpy(&before, rdram + kWheelState, 4);
        g_wheel(rdram, ctx, runtime);
        std::memcpy(&after, rdram + kWheelState, 4);
        if (before != 2u || after != 3u)
            return;
        std::memcpy(&opponent, rdram + obj + 0x8Cu, 4);
        const int want = (opponent != 0u ? g_ghsCheats.spinTheirs : g_ghsCheats.spinMine).load(std::memory_order_relaxed);
        if (want < 1 || want > 8)
            return;
        // The middle of the number's segment: 2-8 at (n - 2) * 22.5 + 11.25 degrees, 1 at 168.75 (157.5-180 wraps to 1).
        const float degrees = want == 1 ? 168.75f : static_cast<float>(want - 2) * 22.5f + 11.25f;
        const float radians = degrees * 3.14159265f / 180.0f;
        std::memcpy(rdram + kWheelStopAngle, &radians, 4);
        std::fprintf(stderr, "[GHS-CHEAT] roulette: %s spin set to %d\n", opponent != 0u ? "opponent's" : "your", want);
    }

    // GHS-TRACE: GHS_CHEATFILE=<path>: lines "health 0|1", "items 0|1", "keyitems 0|1", "time <x>", "give <id>" set the
    // F5 state when the file appears; the file is then deleted (tests without clicking the window).
    void cheatFileThread(std::string path)
    {
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            FILE *f = std::fopen(path.c_str(), "r");
            if (!f)
                continue;
            char word[16] = {};
            float value = 0.0f;
            while (std::fscanf(f, " %15s %f", word, &value) == 2)
            {
                const std::string w = word;
                if (w == "health")
                    g_ghsCheats.infiniteHealth = value != 0.0f;
                else if (w == "items")
                    g_ghsCheats.infiniteItems = value != 0.0f;
                else if (w == "keyitems")
                    g_ghsCheats.infiniteKeyItems = value != 0.0f;
                else if (w == "invuln")
                    g_ghsCheats.invulnerable = value != 0.0f;
                else if (w == "nodetect")
                    g_ghsCheats.noDetection = value != 0.0f;
                else if (w == "rand") // roll with this seed
                    g_ghsCheats.randomizerRoll = static_cast<uint32_t>(value);
                else if (w == "randclasses")
                    g_ghsCheats.randomizerClasses = static_cast<uint32_t>(value);
                else if (w == "randmove")
                    g_ghsCheats.randomizerMove = value != 0.0f;
                else if (w == "guests")
                    g_ghsCheats.guestShuffle = value != 0.0f;
                else if (w == "speedup")
                    g_ghsCheats.speedUp = value != 0.0f;
                else if (w == "nocond")
                    g_ghsCheats.noConditions = value != 0.0f;
                else if (w == "spinme")
                    g_ghsCheats.spinMine = static_cast<int>(value);
                else if (w == "spinhim")
                    g_ghsCheats.spinTheirs = static_cast<int>(value);
                else if (w == "remove")
                    g_ghsCheats.removeItem = static_cast<int>(value);
                else if (w == "radar")
                    g_ghsCheats.radar = value != 0.0f;
                else if (w == "time")
                    g_ghsCheats.timeScale = value;
                else if (w == "warp")
                    g_ghsCheats.warpTo = static_cast<int>(value);
                else if (w == "give")
                    g_ghsCheats.giveItem = static_cast<int>(value);
                std::fprintf(stderr, "[GHS-CHEAT] file: %s %g\n", word, value);
                std::this_thread::sleep_for(std::chrono::milliseconds(100)); // one give per frame or so
            }
            std::fclose(f);
            std::remove(path.c_str());
        }
    }

    void apply(PS2Runtime &runtime)
    {
        // The randomizer's classes and last seed (ghs_randomizer.ini next to the game, written by the F5 window).
        if (FILE *f = std::fopen("ghs_randomizer.ini", "r"))
        {
            unsigned classes = 0x3Fu, seed = 0, move = 0, guests = 0;
            if (std::fscanf(f, "classes=%u seed=%u move=%u guests=%u", &classes, &seed, &move, &guests) >= 2) // "move", "guests" are newer
            {
                g_ghsCheats.randomizerClasses = classes;
                g_ghsCheats.randomizerSeed = seed;
                g_ghsCheats.randomizerMove = move != 0u;
                g_ghsCheats.guestShuffle = guests != 0u;
            }
            std::fclose(f);
        }
        if (const char *file = std::getenv("GHS_CHEATFILE")) // GHS-TRACE
            std::thread(cheatFileThread, std::string(file)).detach();
        g_vsync = runtime.lookupFunction(0x0012B350u);
        if (g_vsync)
            runtime.replaceFunction(0x0012B350u, &vsync);
        g_clockStep = runtime.lookupFunction(0x001D8DF0u);
        if (g_clockStep)
            runtime.replaceFunction(0x001D8DF0u, &clockStep);
        if ((g_guestCatch = runtime.lookupFunction(0x001C1010u)))
            runtime.replaceFunction(0x001C1010u, &guestCatch);
        if ((g_encounter = runtime.lookupFunction(0x00217970u)))
            runtime.replaceFunction(0x00217970u, &encounter);
        if ((g_huntAi = runtime.lookupFunction(0x001BFD60u)))
            runtime.replaceFunction(0x001BFD60u, &huntAi);
        if ((g_searchAi = runtime.lookupFunction(0x001C56A0u)))
            runtime.replaceFunction(0x001C56A0u, &searchAi);
        if ((g_calmAi = runtime.lookupFunction(0x001BE690u)))
            runtime.replaceFunction(0x001BE690u, &calmAi);
        if ((g_wheel = runtime.lookupFunction(0x002060B0u)))
            runtime.replaceFunction(0x002060B0u, &wheel);
        if ((g_contact = runtime.lookupFunction(0x00276050u)))
            runtime.replaceFunction(0x00276050u, &contact);
        if ((g_sees = runtime.lookupFunction(0x001CB760u)))
            runtime.replaceFunction(0x001CB760u, &sees);
        if ((g_equipped = runtime.lookupFunction(0x002567F0u)))
            runtime.replaceFunction(0x002567F0u, &equipped);
        if ((g_holds = runtime.lookupFunction(0x00256680u)))
            runtime.replaceFunction(0x00256680u, &holds);
    }

    const ps2_game_overrides::Descriptor kDescriptor{"Gregory Horror Show (EU): cheats (F5)", "SLES_519.33", 0x00100008u, 0u, apply};
    const ps2_game_overrides::AutoRegister kRegister(kDescriptor);
}
