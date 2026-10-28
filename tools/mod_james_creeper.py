"""Writes the mod "mods\\james the creeper": James (guest, actor slot 18; FILE.STM 170\\034 textures, 170\\035 model)
as a creeper. His skeleton and animations stay; every mesh keeps its batches and vertex counts (tools\\ghsmesh.py),
the vertices become boxes: the head (mesh 001) a cube with the face, the torso (000 and its 20 bent variants
014-033) the body, each foot (004, 005) a front and a back leg; arms, tail, mouth and eyes draw nothing. All of them
use his first texture, repainted as the creeper's skin. One creeper pixel = 24 model units = 8 texels.
  python tools\\mod_james_creeper.py      (needs game\\FILE.STM.orig or game\\FILE.STM; then build the mods)
"""
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ghsarc
import ghsmesh
import ghstex

U = 24  # model units per creeper pixel
MODEL = ("170", "035.slid", "000")
TEXTURES = ("170", "034.slid.bin")
HEAD, FEET = "001.bin", ("004.bin", "005.bin")
BODY = ["000.bin"] + ["%03d.bin" % i for i in range(14, 34)]
FACE = [  # 8x8, # = dark
    "........",
    ".##..##.",
    ".##..##.",
    "...##...",
    "..####..",
    "..####..",
    "..#..#..",
    "........",
]
GREENS = [(0x0D, 0x6B, 0x14), (0x14, 0x84, 0x1C), (0x1E, 0x9A, 0x26), (0x4C, 0xB8, 0x3C), (0x7C, 0xC8, 0x6C),
          (0x2E, 0x7A, 0x2A), (0xA8, 0xD8, 0xA0), (0x10, 0x55, 0x12)]
DARK = (0x0A, 0x16, 0x0A)
# texture rectangles (texels) for each kind of face; the whole image is camouflage, so they only vary the pattern
RECTS = {"face": (0, 0, 64, 64), "head": (64, 0, 128, 64), "body": (128, 0, 192, 96), "bodyside": (192, 0, 224, 96),
         "top": (64, 64, 128, 96), "leg": (0, 128, 32, 176)}


def skin():
    """256x256 RGBA: 8x8-texel camouflage pixels, the face in RECTS["face"]."""
    rnd = random.Random(8745)
    px = bytearray()
    blocks = [[rnd.choice(GREENS[:5] if rnd.random() < 0.85 else GREENS) for _ in range(32)] for _ in range(32)]
    for y in range(256):
        for x in range(256):
            c = blocks[y // 8][x // 8]
            fx, fy = (x - RECTS["face"][0]) // 8, (y - RECTS["face"][1]) // 8
            if x < 64 and y < 64 and FACE[fy][fx] == "#":
                c = DARK
            px += bytes(c) + b"\xff"
    return bytes(px)


def vertex(pos, nrm, st, shade, morph):
    v = [(pos[0], pos[1], pos[2], 16), (nrm[0], nrm[1], nrm[2], 4096)]
    if morph:
        v += [(0, 0, 0, 16), (0, 0, 0, 4096)]
    return tuple(v + [(shade, shade, shade, 128), (int(st[0] * 16), int(st[1] * 16), 4096, 0)])


def box(x0, x1, y0, y1, z0, z1, rects, morph, faces=("+x", "-x", "-y", "+y", "+z", "-z")):
    """Quads of a box (y grows downwards: y0 < y1, -y is the top); rects: face -> texture rectangle."""
    c = {"+x": [(x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)], "-x": [(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)],
         "+z": [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], "-z": [(x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)],
         "-y": [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)], "+y": [(x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0)]}
    normals = {"+x": (4096, 0, 0), "-x": (-4096, 0, 0), "-y": (0, -4096, 0), "+y": (0, 4096, 0), "+z": (0, 0, 4096), "-z": (0, 0, -4096)}
    shades = {"-y": 116, "+y": 60, "+z": 100, "-z": 100, "+x": 86, "-x": 86}
    quads = []
    for f in faces:
        u0, v0, u1, v1 = rects[f]
        corners = c[f]  # top-left, top-right, bottom-right, bottom-left seen from outside
        uv = [(u0, v0), (u1, v0), (u1, v1), (u0, v1)]
        n = normals[f]
        p0, p1, p3 = corners[0], corners[1], corners[3]
        a, b = [p1[k] - p0[k] for k in range(3)], [p3[k] - p0[k] for k in range(3)]
        cross = (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
        if sum(cross[k] * n[k] for k in range(3)) > 0:  # the game culls faces whose (p1-p0)x(p3-p0) points outwards
            corners, uv = corners[::-1], uv[::-1]
        quads.append([vertex(p, n, t, shades[f], morph) for p, t in zip(corners, uv)])
    return quads


def reshape(data, quads):
    buf = bytearray(data)
    batches = ghsmesh.parse(data)
    morph = ghsmesh.elements(data) == 6
    pad = vertex((0, 0, 0), (0, 0, 0), (0, 0), 0, morph)
    lists, left = ghsmesh.strips([b[2] for b in batches], quads, pad)
    if left:
        raise SystemExit("%d faces do not fit" % left)
    for b, vs in zip(batches, lists):
        ghsmesh.write_vertices(buf, data, b, vs)
    return buf


def main():
    with open(ghsarc.GAME_FILE_STM + ".orig" if os.path.exists(ghsarc.GAME_FILE_STM + ".orig") else ghsarc.GAME_FILE_STM, "rb") as f:
        stm = f.read()
    model = ghsarc.get(stm, MODEL)
    names = ["%03d.bin" % k for k, (off, size, last) in enumerate(ghsarc.as_archive(model))]
    tex0 = ghsmesh.texture(ghsarc.get(stm, MODEL + (BODY[0],)))  # the first image: the shirt, repainted below
    out = os.path.join(ghsarc.ROOT, "mods", "james the creeper", "FILE.STM", *MODEL)
    os.makedirs(out, exist_ok=True)
    r = RECTS
    head = box(-4 * U, 4 * U, -8 * U, 0, -4 * U, 4 * U, {"+z": r["face"], "-z": r["head"], "+x": r["head"], "-x": r["head"], "-y": r["top"], "+y": r["top"]}, False)
    for name in names:
        try:
            data = ghsarc.get(stm, MODEL + (name,))
            ghsmesh.parse(data)
        except ValueError:
            continue  # the end marker
        morph = ghsmesh.elements(data) == 6
        if name == HEAD:
            quads = head
        elif name in BODY:
            quads = box(-4 * U, 4 * U, -18 * U, -6 * U, -2 * U, 2 * U, {"+z": r["body"], "-z": r["body"], "+x": r["bodyside"], "-x": r["bodyside"], "-y": r["top"], "+y": r["top"]}, morph)
        elif name in FEET:
            verts = [v for b in ghsmesh.parse(data) for v in ghsmesh.vertices(data, b)]
            cx = (min(v[0][0] for v in verts) + max(v[0][0] for v in verts)) // 2  # the foot's middle, under the hip
            sides = ("+x", "-x", "+z", "-z")
            quads = [q for z0, z1 in ((0, 4 * U), (-4 * U, 0)) for q in box(cx - 2 * U, cx + 2 * U, -6 * U, 0, z0, z1, {f: r["leg"] for f in sides}, morph, sides)]
        else:
            quads = []
        buf = reshape(data, quads)
        ghsmesh.set_texture(buf, data, tex0)
        with open(os.path.join(out, name), "wb") as f:
            f.write(buf)
    tex = ghstex.images(ghsarc.get(stm, TEXTURES))[0]
    ghstex.write_png(os.path.join(ghsarc.ROOT, "mods", "james the creeper", "FILE.STM", "170", "034.slid.bin.tex0.png"), tex[5], tex[6], skin())
    with open(os.path.join(ghsarc.ROOT, "mods", "james the creeper", "readme.txt"), "w", encoding="utf-8") as f:
        f.write("James as a creeper. Made by tools\\mod_james_creeper.py from your game files (run it again to remake it).\n"
                "Edit FILE.STM\\170\\034.slid.bin.tex0.png to repaint the skin (the face is the top left 64x64).\n")
    print("wrote", os.path.join(ghsarc.ROOT, "mods", "james the creeper"))


if __name__ == "__main__":
    main()
