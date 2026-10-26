"""Room server for online two-player Hell's Roulette (client: overrides/zz_ghs_roulette_net.cpp).

Usage: python tools/roulette_server.py [port]     (default 7790; open that TCP port in the firewall)
       python tools/roulette_server.py --selftest

One text line per message. Client -> server: "join <room>", "roll <n>", "state <hex0> <hex1>" (the two 0xB0-byte player
structs from 0x6D3640, blue then red). Server -> client: "seat <0|1>", "full", "peer <0|1>", and the other player's
"roll" / "state" lines as they were sent. The first player in a room is blue (seat 0), the second red (seat 1).
The server keeps, per player, the square, the last roll, the items and the health, and prints the room on each change.
"""
import asyncio
import struct
import sys

ITEM_NAMES = {30: "Red Herb Tea", 31: "Green Herb", 67: "Reverse", 68: "Change", 69: "W-Up", 70: "W-Down",
              71: "Break", 72: "Steal"}
rooms = {}  # name -> Room


class Room:
    def __init__(self, name):
        self.name = name
        self.seats = [None, None]
        self.players = [{"square": -1, "roll": None, "items": [], "health": 3000} for _ in range(2)]

    def other(self, seat):
        return self.seats[1 - seat]

    def show(self):
        for seat, p in enumerate(self.players):
            items = ", ".join(ITEM_NAMES.get(i, str(i)) for i in p["items"]) or "-"
            print(f"[{self.name}] {'blue' if seat == 0 else 'red '} {'on ' if self.seats[seat] else 'off'}"
                  f" square {p['square']:>2} roll {p['roll']} health {p['health']} items {items}", flush=True)


def decode(hexstr):
    """square, items, health from a 0xB0-byte player struct (README: Two-player Hell's Roulette)."""
    raw = bytes.fromhex(hexstr)
    if len(raw) != 0xB0:
        raise ValueError("player struct is not 0xB0 bytes")
    square, = struct.unpack_from("<i", raw, 0x50)
    items = [i for i in struct.unpack_from("<3i", raw, 0x5C) if i >= 0]
    health, = struct.unpack_from("<i", raw, 0x88)
    return square, items, health


async def send(writer, line):
    if writer:
        writer.write((line + "\n").encode())
        await writer.drain()


async def handle(reader, writer):
    room, seat = None, None
    peer = writer.get_extra_info("peername")
    try:
        while line := (await reader.readline()).decode(errors="replace").strip():
            op, _, rest = line.partition(" ")
            if op == "join" and room is None:
                room = rooms.setdefault(rest or "default", Room(rest or "default"))
                if None not in room.seats:
                    room = None
                    await send(writer, "full")
                    continue
                seat = room.seats.index(None)
                room.seats[seat] = writer
                print(f"{peer} joined room {room.name} as {'blue' if seat == 0 else 'red'}", flush=True)
                await send(writer, f"seat {seat}")
                await send(writer, f"peer {int(room.other(seat) is not None)}")
                await send(room.other(seat), "peer 1")
                room.show()
            elif room is None:
                continue
            elif op == "roll" and rest.isdigit() and 1 <= int(rest) <= 8:
                room.players[seat]["roll"] = int(rest)
                await send(room.other(seat), line)
                room.show()
            elif op == "state":
                parts = rest.split()
                if len(parts) != 2:
                    continue
                try:
                    decoded = [decode(h) for h in parts]
                except ValueError:
                    continue  # not from our client: don't pass it on
                for p, (square, items, health) in zip(room.players, decoded):
                    p.update(square=square, items=items, health=health)
                await send(room.other(seat), line)
                room.show()
    except (ConnectionError, asyncio.IncompleteReadError):
        pass
    finally:
        if room is not None:
            room.seats[seat] = None
            print(f"{peer} left room {room.name}", flush=True)
            if room.other(seat) is None:
                rooms.pop(room.name, None)
            else:
                try:
                    await send(room.other(seat), "peer 0")
                except ConnectionError:
                    pass
        writer.close()


async def main(port):
    server = await asyncio.start_server(handle, "0.0.0.0", port)
    print(f"Hell's Roulette room server on port {port}", flush=True)
    async with server:
        await server.serve_forever()


async def selftest():
    server = await asyncio.start_server(handle, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]

    async def client():
        r, w = await asyncio.open_connection("127.0.0.1", port)
        return r, w

    async def say(w, line):
        w.write((line + "\n").encode())
        await w.drain()

    async def hear(r):
        return (await asyncio.wait_for(r.readline(), 2)).decode().strip()

    blue_r, blue_w = await client()
    await say(blue_w, "join test")
    assert await hear(blue_r) == "seat 0"
    assert await hear(blue_r) == "peer 0"
    red_r, red_w = await client()
    await say(red_w, "join test")
    assert await hear(red_r) == "seat 1"
    assert await hear(red_r) == "peer 1"
    assert await hear(blue_r) == "peer 1"
    third_r, third_w = await client()
    await say(third_w, "join test")
    assert await hear(third_r) == "full"

    await say(blue_w, "roll 5")
    assert await hear(red_r) == "roll 5"
    blue = bytearray(0xB0)
    struct.pack_into("<i", blue, 0x50, 4)
    struct.pack_into("<3i", blue, 0x5C, 31, 69, -1)
    struct.pack_into("<i", blue, 0x88, 2250)
    red = bytearray(0xB0)
    struct.pack_into("<i", red, 0x50, -1)
    struct.pack_into("<3i", red, 0x5C, 31, -1, -1)
    struct.pack_into("<i", red, 0x88, 3000)
    line = f"state {blue.hex()} {red.hex()}"
    await say(blue_w, line)
    assert await hear(red_r) == line
    await say(red_w, "state 00 11")  # malformed: dropped
    room = rooms["test"]
    assert room.players[0] == {"square": 4, "roll": 5, "items": [31, 69], "health": 2250}
    assert room.players[1]["items"] == [31]

    red_w.close()
    assert await hear(blue_r) == "peer 0"
    blue_w.close()
    third_w.close()
    await asyncio.sleep(0.1)
    assert "test" not in rooms
    server.close()
    print("selftest ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]:
        asyncio.run(selftest())
    else:
        asyncio.run(main(int(sys.argv[1]) if len(sys.argv) > 1 else 7790))
