"""Writes the mod "mods\\miku doll": the Doll item (the one for Lost Doll; bought in the shop) as a sitting chibi
Hatsune Miku, a new model (tools\\ghsmesh.py build) and texture. The doll is in FILE.STM twice, with the same model
and texture: 041\\020.slid (lying in the hotel: 000.bin texture, 001\\000.bin model) and 093.slid (the shop's
shelf: 095.bin texture as a GS upload packet, 096\\000.bin model).
Model units: y grows downwards (the floor is y 0), the doll faces -z. The texture is 64x64, 16 colours: the face
(front half of the head, projected flat) in the top 48 rows, below it one 4x4 swatch per colour for everything else.
  python tools\\mod_miku_doll.py      (needs game\\FILE.STM.orig or game\\FILE.STM; then build the mods)
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ghsarc
import ghsmesh

WORLD = ("041", "020.slid")
SHOP_TEX, SHOP_MODEL = ("093.slid", "095.bin"), ("093.slid", "096", "000.bin")
MAX_VERTICES = 686  # the original doll's; the game may size buffers by it

PALETTE = [  # name: RGB
    ("hair", (0x39, 0xC5, 0xBB)), ("hair_dark", (0x22, 0x8F, 0x8A)), ("skin", (0xFF, 0xE4, 0xD4)),
    ("blush", (0xF6, 0xAE, 0xB0)), ("shirt", (0xCC, 0xD1, 0xD6)), ("grey", (0x5A, 0x60, 0x68)),
    ("black", (0x26, 0x28, 0x2E)), ("teal", (0x14, 0xB0, 0xAC)), ("iris", (0x1A, 0x8C, 0xA6)),
    ("pupil", (0x14, 0x3A, 0x4E)), ("white", (0xFF, 0xFF, 0xFF)), ("pink", (0xE8, 0x45, 0x7F)),
    ("mouth", (0xC0, 0x4C, 0x58)), ("lash", (0x2A, 0x20, 0x30)), ("tie", (0x10, 0x86, 0x84)),
    ("light", (0x9E, 0xEE, 0xE6)),
]
COLOUR = {name: k for k, (name, _) in enumerate(PALETTE)}
FACE_ROWS = 48

HEAD_C, HEAD_R = (0, -255, 0), (100, 95, 90)


def face_colour(fx, fy):
    """Colour of the face texture at (-1..1, -1..1), as seen from the front (fx: -x left, fy: up -1)."""
    def ell(cx, cy, rx, ry):
        return ((fx - cx) / rx) ** 2 + ((fy - cy) / ry) ** 2
    for sx in (-1, 1):
        cx = sx * 0.34
        if ell(cx, 0.30, 0.17, 0.23) < 1:
            if ell(cx - sx * 0.05, 0.20, 0.06, 0.07) < 1:
                return "white"
            if ell(cx, 0.36, 0.08, 0.11) < 1:
                return "pupil"
            return "lash" if fy < 0.16 else "iris"
        if ell(cx, 0.30, 0.20, 0.26) < 1 and fy < 0.30:
            return "lash"  # upper lid
        if ell(sx * 0.55, 0.55, 0.11, 0.06) < 1:
            return "blush"
    if ell(0, 0.62, 0.07, 0.035) < 1:
        return "mouth"
    if abs(fx) > 0.68 or ell(0, 0.28, 0.80, 0.78) >= 1:
        return "hair"
    bangs = -0.02 + 0.16 * abs(math.sin(fx * math.pi * 2.2)) + 0.10 * (abs(fx) > 0.45)  # strand tips
    if fy < bangs:
        return "hair_dark" if fy > bangs - 0.05 else "hair"
    return "skin"


def texture():
    """64x64 colour indices, row by row."""
    px = []
    for y in range(64):
        for x in range(64):
            if y < FACE_ROWS:
                px.append(COLOUR[face_colour((x + 0.5) / 32 - 1, (y + 0.5) / (FACE_ROWS / 2) - 1)])
            else:
                px.append(min(x // 4, 15) if 50 <= y < 58 else COLOUR["black"])
    return px


def swatch(name):
    k = COLOUR[name]
    return ((k * 4 + 2) * 64, 54 * 64)  # texel centre, 4.12 of the texture size


def unit(v):
    n = math.sqrt(sum(x * x for x in v)) or 1
    return [x / n for x in v]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


LIGHT = unit((0.3, -1, -0.7))  # from above, in front


def vertex(p, n, st):
    shade = int(100 + 28 * max(-1, min(1, sum(a * b for a, b in zip(unit(n), LIGHT)))))
    nn = unit(n)
    return ((round(p[0]), round(p[1]), round(p[2]), 16), tuple(round(x * 4096) for x in nn) + (4096,),
            (shade, shade, shade, 128), (int(st[0]), int(st[1]), 4096, 0))


def turn(s):
    """> 0 when the strip's even triangles have (v1-v0)x(v2-v0) along the vertex normals (outwards: culled)."""
    t = 0.0
    for i in range(len(s) - 2):
        p0, p1, p2 = (s[i + k][0] for k in range(3))
        c = cross([p1[k] - p0[k] for k in range(3)], [p2[k] - p0[k] for k in range(3)])
        t += (-1) ** i * sum(c[k] * (s[i][1][k] + s[i + 1][1][k] + s[i + 2][1][k]) for k in range(3))
    return t


def band(a, b):
    """Strip between two rings of (point, normal, st), wound so the outside shows."""
    s = [v for pair in zip(a, b) for v in pair]
    if turn(s) > 0:
        s = [v for pair in zip(b, a) for v in pair]
    return [vertex(*v) for v in s]


def flat(s):
    """Strip of (point, normal, st) as given, wound so the side the normals point to shows."""
    if turn(s) > 0:
        s = s[:1] + s  # a degenerate first triangle swaps every triangle's parity
    return [vertex(*v) for v in s]


def head():
    cx, cy, cz = HEAD_C
    rx, ry, rz = HEAD_R
    lats = [math.pi * k / 6 for k in range(7)]
    strips = []
    for a0, seg in ((-math.pi / 2, "front"), (math.pi / 2, "back")):
        rings = []
        for phi in lats:
            ring = []
            for j in range(7):
                th = a0 + math.pi * j / 6
                x, y, z = rx * math.sin(phi) * math.sin(th), -ry * math.cos(phi), -rz * math.sin(phi) * math.cos(th)
                n = (x / rx ** 2, y / ry ** 2, z / rz ** 2) if phi not in (0, math.pi) else (0, -math.cos(phi), 0)
                st = ((x / rx + 1) / 2 * 4096, (y / ry + 1) / 2 * FACE_ROWS * 64) if seg == "front" else swatch("hair")
                ring.append(((cx + x, cy + y, cz + z), n, st))
            rings.append(ring)
        strips += [band(rings[k], rings[k + 1]) for k in range(6)]
    return strips


def tube(path, colours, sides, ref=(0, 0, 1), cap=None):
    """A tube along path [(point, (radius across, radius along ref))], band k coloured colours[k]; cap: a pole point
    closing the end (coloured like the last band)."""
    rings = []
    for i, (p, (ru, rw)) in enumerate(path):
        t = unit([path[min(i + 1, len(path) - 1)][0][k] - path[max(i - 1, 0)][0][k] for k in range(3)])
        u = unit(cross(t, ref))
        w = cross(t, u)
        rings.append([[p[k] + ru * math.cos(2 * math.pi * j / sides) * u[k] + rw * math.sin(2 * math.pi * j / sides) * w[k] for k in range(3)]
                      for j in range(sides + 1)])
        rings[-1] = (rings[-1], [[math.cos(2 * math.pi * j / sides) * u[k] / max(ru, 1) + math.sin(2 * math.pi * j / sides) * w[k] / max(rw, 1)
                                 for k in range(3)] for j in range(sides + 1)])
    strips = []
    for k, c in enumerate(colours):
        (pa, na), (pb, nb) = rings[k], rings[k + 1]
        strips.append(band([(p, n, swatch(c)) for p, n in zip(pa, na)], [(p, n, swatch(c)) for p, n in zip(pb, nb)]))
    if cap:
        pa, na = rings[-1]
        d = [cap[k] - path[-1][0][k] for k in range(3)]
        strips.append(band([(p, n, swatch(colours[-1])) for p, n in zip(pa, na)], [(cap, d, swatch(colours[-1]))] * (sides + 1)))
    return strips


def miku():
    strips = head()
    for s in (-1, 1):
        tail = [((s * 92, -300, 25), (24, 16)), ((s * 128, -284, 35), (32, 20)), ((s * 165, -215, 45), (34, 22)),
                ((s * 185, -130, 50), (28, 18)), ((s * 186, -55, 45), (17, 11)), ((s * 178, -6, 30), (3, 2))]
        strips += tube(tail, ["hair"] * 5, 5)
        strips += tube([((s * 84, -302, 23), (27, 20)), ((s * 102, -297, 27), (28, 21))], ["black"], 5)  # hair tie
        strips += tube([((s * 44, -173, 5), (12, 12)), ((s * 70, -128, -6), (13, 13)), ((s * 90, -80, -24), (26, 26)),
                        ((s * 91, -76, -25), (27, 27)), ((s * 92, -73, -26), (12, 12))],
                       ["skin", "black", "teal", "black"], 5, cap=(s * 98, -52, -34))  # arm, sleeve, cuff, its end, hand
        strips[-1] = [v[:3] + (swatch("skin") + v[3][2:],) for v in strips[-1]]  # the hand
        strips += tube([((s * 30, -28, -10), (20, 20)), ((s * 33, -26, -100), (20, 20)), ((s * 34, -26, -106), (22, 22)),
                        ((s * 40, -24, -165), (19, 19))], ["skin", "teal", "black"], 5, ref=(0, 1, 0), cap=(s * 40, -40, -176))
    strips += tube([((0, -195, 5), (16, 14)), ((0, -176, 5), (44, 32)), ((0, -105, 5), (38, 30))], ["shirt", "shirt"], 8, ref=(0, 0, 1))
    strips += tube([((0, -110, 5), (40, 32)), ((0, -60, 5), (70, 58)), ((0, -8, 5), (98, 82)), ((0, -2, 5), (100, 84))],
                   ["grey", "black", "teal"], 8)
    tie = [(-6, -184, -34), (6, -184, -34), (-9, -132, -36), (9, -132, -36), (0, -118, -35)]
    strips.append(flat([(p, (0, 0, -1), swatch("tie")) for p in tie]))
    return strips


def write_texture(data, palette_at, pixels_at, px):
    """data with the 16 colours (alpha 0x80: opaque) and the 4-bit pixels (low nibble first) replaced."""
    buf = bytearray(data)
    buf[palette_at:palette_at + 64] = b"".join(bytes(rgb) + b"\x80" for _, rgb in PALETTE)
    buf[pixels_at:pixels_at + 2048] = bytes(px[i] | px[i + 1] << 4 for i in range(0, 4096, 2))
    return bytes(buf)


def main():
    with open(ghsarc.GAME_FILE_STM + ".orig" if os.path.exists(ghsarc.GAME_FILE_STM + ".orig") else ghsarc.GAME_FILE_STM, "rb") as f:
        stm = f.read()
    model = ghsarc.get(stm, WORLD + ("001", "000.bin"))
    if model != ghsarc.get(stm, SHOP_MODEL):
        raise SystemExit("the doll's two models differ: not the expected game files")
    mesh = ghsmesh.build(miku(), ghsmesh.texture(model))
    count, size = int.from_bytes(mesh[4:8], "little"), len(mesh)
    if count > MAX_VERTICES or size > len(model):
        raise SystemExit("the new doll is bigger than the old (%d vertices, %d bytes)" % (count, size))
    px = texture()
    world_tex, shop_tex = ghsarc.get(stm, WORLD + ("000.bin",)), ghsarc.get(stm, SHOP_TEX)
    if len(world_tex) != 2144 or len(shop_tex) != 2464 or world_tex[16:80] != shop_tex[0x90:0xD0]:
        raise SystemExit("the doll's textures are not where expected")
    out = os.path.join(ghsarc.ROOT, "mods", "miku doll")
    files = {WORLD + ("001", "000.bin"): mesh, SHOP_MODEL: mesh,
             WORLD + ("000.bin",): write_texture(world_tex, 16, 96, px), SHOP_TEX: write_texture(shop_tex, 0x90, 0x180, px)}
    for path, data in files.items():
        dst = os.path.join(out, "FILE.STM", *path)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(data)
    with open(os.path.join(out, "readme.txt"), "w", encoding="utf-8") as f:
        f.write("The Doll item (for Lost Doll, sold in the shop) as a chibi Hatsune Miku.\n"
                "Made by tools\\mod_miku_doll.py from your game files (run it again to remake it).\n")
    print("wrote", out, "(%d vertices, %d bytes)" % (count, size))


if __name__ == "__main__":
    main()
