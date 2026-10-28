// Gregory Horror Show: online two-player Hell's Roulette (the title screen's mode), client of tools\roulette_server.py.
// GHS_NET=<host>:<port> and GHS_ROOM=<name> (tools\launch.ps1 -Online / -Room, tools/launch.sh --online / --room) turn it on; the first player in a room
// plays blue, the second red. Both run the game's own two-player mode; a background thread watches EE RAM (README:
// Two-player Hell's Roulette):
//  - own turn (turn flag 0x60E2F0 = own seat): the player plays; each spin's number goes to the server, and when the turn
//    passes on, both player structs (0x6D3640, 2 x 0xB0 bytes) as they are then.
//  - the other's turn: the controller is taken over. Cross is pressed (menu: Roulette, START, STOP, messages), but not
//    while the wheel spins before their number has come; while it slows down the saved stop angle (gp+0x2498) is set so
//    the needle lands on their number, as the roulette cheat does. When the turn comes back here, their structs are
//    written over ours (random squares and cards played out differently here), then the player gets the controller.
// tools/install_overrides.ps1 copies this into the runner.
#include "game_overrides.h"
#include "ps2_runtime.h"

#ifdef _WIN32
#define NOMINMAX // after the runtime's headers, so windows.h's macros only meet this file
#define WIN32_LEAN_AND_MEAN
#define NOGDI  // the unity build puts raylib.h after this file: wingdi's Rectangle and winuser's CloseWindow/ShowCursor clash
#define NOUSER
#include <winsock2.h>
#include <ws2tcpip.h>
#define MSG_NOSIGNAL 0
#else
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace ps2_stubs // Kernel/Stubs/Pad.h (test hooks): what scePadRead returns instead of the controller
{
    void setPadOverrideState(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
    void clearPadOverrideState();
}

namespace ghs_roulette_net
{
#ifndef _WIN32 // POSIX sockets under Winsock's names (in this namespace: the unity build shares the global one)
    using SOCKET = int;
    constexpr SOCKET INVALID_SOCKET = -1;
    inline int closesocket(SOCKET s) { return close(s); }
#endif
    constexpr uint32_t kPlayers = 0x006D3640u, kPlayerBytes = 0xB0u; // blue, then red
    constexpr uint32_t kSquare = 0x50u, kHealthMax = 0x8Cu;
    constexpr uint32_t kTurn = 0x0060E2F0u;           // wheel object +0x8C: 0 blue's turn, 1 red's
    constexpr uint32_t kWheelState = 0x0043DC00u;     // gp+0x2490: 0 wait for START, 1 spin up, 2 spinning, 3 slow down, 4 read, 5 show
    constexpr uint32_t kWheelStopAngle = 0x0043DC08u; // gp+0x2498
    constexpr uint32_t kWheelNumber = 0x0043DC14u;    // gp+0x24A4
    constexpr int kGoal = 34;
    constexpr uint16_t kNoButtons = 0xFFFFu, kCross = 0x4000u; // pad bits, 0 = pressed
    constexpr uint8_t kStickCenter = 0x80u;
    // Taken from the other side's structs: position and facing (+0x00..+0x17), +0x40..+0x77 (direction, square, the
    // square before, item slots and their values, last item) and health (+0x88). Animation ids (+0x20/+0x24) stay ours.
    constexpr std::pair<uint32_t, uint32_t> kSynced[] = {{0x00u, 0x18u}, {0x40u, 0x78u}, {0x88u, 0x8Cu}};

    std::mutex g_lock;
    SOCKET g_sock = INVALID_SOCKET;
    int g_seat = -1;          // 0 blue, 1 red; -1 not in a room
    bool g_peer = false;      // the other player is in the room
    std::deque<int> g_rolls;  // the other player's numbers not used here yet
    std::string g_state;      // the other side's last structs (hex), not written here yet
    unsigned g_rollsHeard = 0;

    template <typename T>
    T rd(const uint8_t *rdram, uint32_t addr)
    {
        T v;
        std::memcpy(&v, rdram + addr, sizeof(v));
        return v;
    }

    void sendLine(const std::string &line)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_sock != INVALID_SOCKET)
            send(g_sock, (line + "\n").c_str(), static_cast<int>(line.size() + 1), MSG_NOSIGNAL); // lost server: no SIGPIPE
    }

    void heard(const std::string &line)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (line.rfind("seat ", 0) == 0)
            g_seat = std::atoi(line.c_str() + 5);
        else if (line.rfind("peer ", 0) == 0)
            g_peer = line[5] == '1';
        else if (line.rfind("roll ", 0) == 0)
        {
            g_rolls.push_back(std::atoi(line.c_str() + 5));
            ++g_rollsHeard;
        }
        else if (line.rfind("state ", 0) == 0)
            g_state = line.substr(6);
        std::fprintf(stderr, "[GHS-NET] %s\n", line.rfind("state ", 0) == 0 ? "their structs came" : line.c_str());
    }

    void netThread(std::string host, std::string port, std::string room)
    {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        for (;; std::this_thread::sleep_for(std::chrono::seconds(3)))
        {
            addrinfo hints{}, *found = nullptr;
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            SOCKET s = INVALID_SOCKET;
            if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) == 0)
            {
                for (addrinfo *a = found; a && s == INVALID_SOCKET; a = a->ai_next)
                    if ((s = socket(a->ai_family, a->ai_socktype, a->ai_protocol)) != INVALID_SOCKET &&
                        connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) != 0)
                    {
                        closesocket(s);
                        s = INVALID_SOCKET;
                    }
                freeaddrinfo(found);
            }
            if (s == INVALID_SOCKET)
            {
                std::fprintf(stderr, "[GHS-NET] can't reach %s:%s, trying again\n", host.c_str(), port.c_str());
                continue;
            }
            std::fprintf(stderr, "[GHS-NET] connected to %s:%s, room %s\n", host.c_str(), port.c_str(), room.c_str());
            {
                std::lock_guard<std::mutex> lock(g_lock);
                g_sock = s;
            }
            sendLine("join " + room);
            std::string buffer;
            char chunk[4096];
            for (int n; (n = recv(s, chunk, sizeof(chunk), 0)) > 0;)
            {
                buffer.append(chunk, static_cast<size_t>(n));
                for (size_t nl; (nl = buffer.find('\n')) != std::string::npos; buffer.erase(0, nl + 1))
                    heard(buffer.substr(0, nl));
            }
            {
                std::lock_guard<std::mutex> lock(g_lock);
                closesocket(s);
                g_sock = INVALID_SOCKET;
                g_seat = -1;
                g_peer = false;
                g_rolls.clear();
                g_state.clear();
            }
            std::fprintf(stderr, "[GHS-NET] connection lost, reconnecting\n");
        }
    }

    std::string structsHex(const uint8_t *rdram)
    {
        static const char kDigits[] = "0123456789abcdef";
        std::string out;
        for (uint32_t p = 0; p < 2u; ++p)
        {
            out += p ? " " : "";
            for (uint32_t i = 0; i < kPlayerBytes; ++i)
            {
                const uint8_t b = rdram[kPlayers + p * kPlayerBytes + i];
                out += kDigits[b >> 4];
                out += kDigits[b & 15];
            }
        }
        return out;
    }

    // Their structs over ours (kSynced ranges); false if the line isn't two 0xB0-byte structs.
    bool writeStructs(uint8_t *rdram, const std::string &hex)
    {
        if (hex.size() != kPlayerBytes * 4u + 1u || hex[kPlayerBytes * 2u] != ' ')
            return false;
        uint8_t bytes[2 * kPlayerBytes];
        for (uint32_t i = 0; i < 2u * kPlayerBytes; ++i)
        {
            const size_t at = i * 2u + (i >= kPlayerBytes ? 1u : 0u);
            char pair[3] = {hex[at], hex[at + 1], 0}, *end = nullptr;
            bytes[i] = static_cast<uint8_t>(std::strtoul(pair, &end, 16));
            if (end != pair + 2)
                return false;
        }
        for (uint32_t p = 0; p < 2u; ++p)
            for (const auto &[from, to] : kSynced)
                std::memcpy(rdram + kPlayers + p * kPlayerBytes + from, bytes + p * kPlayerBytes + from, to - from);
        return true;
    }

    // ponytail: RAM is read and written from this thread while the game thread runs (a few words, between frames most
    // of the time); move it into the VBlank hook if a write ever lands mid-update.
    void watchThread(PS2Runtime *runtime)
    {
        uint8_t *rdram = runtime->memory().getRDRAM();
        using Clock = std::chrono::steady_clock;
        bool controlling = false;  // the pad override is ours
        bool playing = false;      // a game is on: from both players before the first square to a player at the goal
        bool sendNumber = false;   // our spin has stopped: send its number once it shows
        bool waitState = false;    // their turn is over here: hold the controller until their structs came
        int skipRolls = 0;         // their spins that stopped here before the number came: drop that many numbers
        int lastTurn = -1;
        uint32_t lastWheel = 0;
        unsigned lastRollsHeard = 0;
        auto lastActivity = Clock::now();
        for (;; std::this_thread::sleep_for(std::chrono::milliseconds(5)))
        {
            int seat;
            bool peer;
            unsigned rollsHeard;
            {
                std::lock_guard<std::mutex> lock(g_lock);
                seat = g_seat;
                peer = g_peer;
                rollsHeard = g_rollsHeard;
            }
            const int turn = rd<int32_t>(rdram, kTurn);
            const uint32_t wheel = rd<uint32_t>(rdram, kWheelState);
            int square[2];
            bool sane = turn == 0 || turn == 1;
            for (uint32_t p = 0; p < 2u; ++p)
            {
                square[p] = rd<int32_t>(rdram, kPlayers + p * kPlayerBytes + kSquare);
                sane = sane && square[p] >= -1 && square[p] <= kGoal && rd<int32_t>(rdram, kPlayers + p * kPlayerBytes + kHealthMax) == 3000;
            }
            if (!sane || square[0] == kGoal || square[1] == kGoal)
                playing = false;
            else if (square[0] == -1 && square[1] == -1 && !playing)
            {
                playing = true;
                waitState = false;
                std::fprintf(stderr, "[GHS-NET] game started\n");
            }
            const auto now = Clock::now();
            if (turn != lastTurn || wheel != lastWheel || rollsHeard != lastRollsHeard)
                lastActivity = now;
            // Nothing moved for 3 minutes: the game was probably left (Give Up); the player gets the controller back.
            const bool online = seat >= 0 && peer && playing && now - lastActivity < std::chrono::minutes(3);

            if (online && turn != lastTurn)
                std::fprintf(stderr, "[GHS-NET] %s's turn (%s)\n", turn == 0 ? "blue" : "red", turn == seat ? "you" : "them");
            if (online && lastTurn == seat && turn != seat) // our turn is over: they get both structs as they are now
                sendLine("state " + structsHex(rdram));
            if (online && lastTurn == 1 - seat && turn == seat)
                waitState = true;

            if (!online || (turn == seat && !waitState))
            {
                if (controlling)
                {
                    ps2_stubs::clearPadOverrideState();
                    controlling = false;
                }
                if (online && lastWheel == 3u && wheel != 3u)
                    sendNumber = true;
                if (online && sendNumber && (wheel == 5u || wheel == 0u))
                {
                    sendLine("roll " + std::to_string(rd<int32_t>(rdram, kWheelNumber)));
                    sendNumber = false;
                }
            }
            else
            {
                bool haveRoll;
                {
                    std::lock_guard<std::mutex> lock(g_lock);
                    if (turn != seat && lastWheel == 3u && wheel != 3u) // one of their spins is over here
                    {
                        if (g_rolls.empty())
                        {
                            ++skipRolls;
                            std::fprintf(stderr, "[GHS-NET] their spin stopped before the number came\n");
                        }
                        else
                            g_rolls.pop_front();
                    }
                    for (; skipRolls > 0 && !g_rolls.empty(); --skipRolls)
                        g_rolls.pop_front();
                    haveRoll = !g_rolls.empty();
                    if (turn != seat && wheel == 3u && haveRoll)
                    {
                        // The middle of the number's segment, as in zz_ghs_cheats.cpp wheel().
                        const int want = g_rolls.front();
                        const float degrees = want == 1 ? 168.75f : static_cast<float>(want - 2) * 22.5f + 11.25f;
                        const float radians = degrees * 3.14159265f / 180.0f;
                        std::memcpy(rdram + kWheelStopAngle, &radians, 4);
                    }
                    if (waitState && !g_state.empty())
                    {
                        std::fprintf(stderr, "[GHS-NET] %s\n", writeStructs(rdram, g_state) ? "their structs written" : "their structs were malformed");
                        g_state.clear();
                        waitState = false;
                    }
                }
                // Their turn: Cross on and off every 60 ms, except while the wheel spins up or waits for their number.
                const bool press = turn != seat && !(wheel == 1u || (wheel == 2u && !haveRoll)) &&
                                   (std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() / 60) % 2 == 0;
                ps2_stubs::setPadOverrideState(press ? static_cast<uint16_t>(kNoButtons & ~kCross) : kNoButtons, kStickCenter,
                                               kStickCenter, kStickCenter, kStickCenter);
                controlling = true;
            }
            lastTurn = turn;
            lastWheel = wheel;
            lastRollsHeard = rollsHeard;
        }
    }

    void apply(PS2Runtime &runtime)
    {
        const char *net = std::getenv("GHS_NET");
        if (!net || !*net)
            return;
        std::string host = net, port = "7790";
        if (const size_t colon = host.rfind(':'); colon != std::string::npos)
        {
            port = host.substr(colon + 1);
            host.erase(colon);
        }
        const char *room = std::getenv("GHS_ROOM");
        std::thread(netThread, host, port, std::string(room && *room ? room : "default")).detach();
        std::thread(watchThread, &runtime).detach();
    }

    const ps2_game_overrides::Descriptor kDescriptor{"Gregory Horror Show (EU): online Hell's Roulette", "SLES_519.33", 0x00100008u, 0u, apply};
    const ps2_game_overrides::AutoRegister kRegister(kDescriptor);
}
