"""The game's text font (FILE.STM 000\\000.slid.bin large, 000\\001.slid.bin small: the same design in two sizes).

A font: u32 offset of the image (0x1D0), u32 glyph count (224), 8 zero bytes, one u16 character code per glyph
(Latin-1 0x20-0xFF; some codes draw the game's own symbols: $ note, ^ circle, _ triangle, { square, } cross,
| (R), @ e acute, " bullet, ` closing quote), then a 4-bit image as tools\\ghstex.py reads them, but its pixels are
the glyphs one after another, each padded to count / pixel bytes: rows of 22 (large) or 20 (small) pixels, low
nibble first, 22 or 20 rows. The 16 colours are a light fill with a dark, half transparent edge (alpha 0x80 =
opaque); the game swaps the palette for coloured text, so a glyph is its colour indices.
  python tools\\ghsfont.py export [folder]          both fonts as PNG sheets (16 glyphs a row, from code 0x20)
  python tools\\ghsfont.py text <out.png> <text> [small]   the text drawn with the font
  python tools\\ghsfont.py ttf [folder]             the large font as TrueType (needs: py -m pip install fonttools)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ghsarc
import ghstex

FONTS = {"large": ("000", "000.slid.bin"), "small": ("000", "001.slid.bin")}
SIZES = {256: (22, 22), 208: (20, 20)}  # bytes per glyph: (width, rows); not in the file
# Spacing, measured on dialogue screenshots (large font; 640-wide screen): from one glyph's first inked column to the
# next one's. The game keeps a width per character that no formula of the glyph's pixels gives; inked width + 4 is
# within 1-2 pixels of it. ponytail: characters not in ADVANCE use that until someone measures them (F6 snapshots).
ADVANCE = {"a": 11, "b": 9, "c": 11, "e": 10.5, "f": 9.5, "g": 12, "h": 11, "i": 6, "j": 8, "l": 6, "m": 16, "o": 10,
           "r": 10, "s": 10.5, "t": 9, "u": 11, "v": 10.5, "y": 10, "H": 13, "T": 12.5, ".": 9, ",": 9, "'": 8}
GAP, SPACE = 4, 7


def load(data):
    """{code: glyph} with glyph = rows of colour indices, and the palette [(r, g, b, a)]."""
    img_at, count = struct.unpack_from("<II", data, 0)
    codes = struct.unpack_from("<%dH" % count, data, 16)
    pal = [tuple(data[img_at + 16 + k * 4:img_at + 20 + k * 4]) for k in range(16)]
    nbytes = struct.unpack_from("<I", data, img_at + 84)[0]
    px = data[img_at + 96:img_at + 96 + nbytes]
    per = nbytes // count
    w, h = SIZES[per]
    glyphs = {}
    for g, code in enumerate(codes):
        raw = px[g * per:g * per + w * h // 2]
        idx = [(b >> s) & 15 for b in raw for s in (0, 4)]
        glyphs[code] = [idx[y * w:(y + 1) * w] for y in range(h)]
    return glyphs, pal


def ink(glyph):
    """(first, last) column with any pixel, or None for an empty glyph."""
    cols = [x for x in range(len(glyph[0])) if any(row[x] for row in glyph)]
    return (cols[0], cols[-1]) if cols else None


def rgba(pal, k):
    r, g, b, a = pal[k]
    return bytes((r, g, b, min(255, a * 2)))


def font(name):
    with open(ghsarc.GAME_FILE_STM + ".orig" if os.path.exists(ghsarc.GAME_FILE_STM + ".orig") else ghsarc.GAME_FILE_STM, "rb") as f:
        return load(ghsarc.get(f.read(), FONTS[name]))


def export(folder):
    os.makedirs(folder, exist_ok=True)
    for name in FONTS:
        glyphs, pal = font(name)
        h, w = len(glyphs[0x20]), len(glyphs[0x20][0])
        cw, ch, cols = w + 2, h + 2, 16
        rows = (len(glyphs) + cols - 1) // cols
        W, H = cols * cw, rows * ch
        out = bytearray(W * H * 4)
        for n, code in enumerate(sorted(glyphs)):
            ox, oy = (n % cols) * cw + 1, (n // cols) * ch + 1
            for y, row in enumerate(glyphs[code]):
                for x, k in enumerate(row):
                    p = ((oy + y) * W + ox + x) * 4
                    out[p:p + 4] = rgba(pal, k)
        path = os.path.join(folder, "font_%s.png" % name)
        ghstex.write_png(path, W, H, bytes(out))
        print("wrote", path, "(%dx%d glyphs from 0x20)" % (w, h))


def advance(name, code, e):
    """Pixels from this glyph's first inked column to the next glyph's (e: ink(glyph))."""
    return (ADVANCE if name == "large" else {}).get(chr(code), e[1] - e[0] + 1 + GAP)


def text(path, s, name="large"):
    glyphs, pal = font(name)
    h = len(glyphs[0x20])
    placed, x = [], 0.0
    for ch in s.encode("latin-1"):
        g = glyphs.get(ch, glyphs[0x20])
        e = ink(g)
        if e is None:
            x += SPACE
            continue
        placed.append((round(x) - e[0], g))
        x += advance(name, ch, e)
    W = max(round(x), 1)
    out = bytearray(W * h * 4)
    for ox, g in placed:
        for y, row in enumerate(g):
            for gx, k in enumerate(row):
                if k and 0 <= ox + gx < W:
                    p = (y * W + ox + gx) * 4
                    out[p:p + 4] = rgba(pal, k)
    ghstex.write_png(path, W, h, bytes(out))
    print("wrote", path)


UNICODE = {0x24: 0x266A, 0x5E: 0x25CB, 0x5F: 0x25B3, 0x7B: 0x25A1, 0x2B: 0x201C, 0x60: 0x201D, 0x27: 0x2019}
BASELINE = 18  # glyph row under the letters' feet (large font)


def outline(pixels):
    """Closed contours (TrueType: outer ones clockwise, y up) around a set of (x, row) pixels, in pixel units."""
    edges = {}
    for x, y in pixels:
        top, bottom = BASELINE - y, BASELINE - y - 1
        sq = [(x, top), (x + 1, top), (x + 1, bottom), (x, bottom)]
        for a, b in zip(sq, sq[1:] + sq[:1]):
            if b in edges.get(a, ()) or a in edges.get(b, ()):
                edges[b].remove(a)  # an edge between two inked pixels
            else:
                edges.setdefault(a, []).append(b)
    contours = []
    while any(edges.values()):
        start = next(a for a, bs in edges.items() if bs)
        loop, a = [start], start
        while True:
            b = edges[a].pop()
            if b == start:
                break
            loop.append(b)
            a = b
        pts = [p for i, p in enumerate(loop)  # drop points in the middle of a straight line
               if (loop[i - 1][0] - p[0]) * (loop[(i + 1) % len(loop)][1] - p[1]) != (loop[i - 1][1] - p[1]) * (loop[(i + 1) % len(loop)][0] - p[0])]
        contours.append(pts)
    return contours


def ttf(folder):
    """GHS Dialogue.ttf: the large font as the dialogue looks in the game (pixels from alpha 0x3C, the half
    transparent edge included; the opaque core alone breaks up), 100 units a pixel and 2200 an em, so at 22 px
    it draws the game's pixels.
    Characters keep the game's codes (& draws #, + and ` the curly quotes, $ ^ _ { } | the symbols); the symbols
    are also at their Unicode places."""
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    glyphs, pal = font("large")
    os.makedirs(folder, exist_ok=True)
    order, cmap, outlines, metrics = [".notdef"], {}, {}, {}
    pen = TTGlyphPen(None)
    outlines[".notdef"], metrics[".notdef"] = pen.glyph(), (1100, 0)
    for code, g in sorted(glyphs.items()):
        e = ink(g)
        if e is None and code not in (0x20, 0xA0):
            continue  # no glyph in the game
        name = "uni%04X" % code
        pen = TTGlyphPen(None)
        if e:
            inked = {(x - e[0], y) for y, row in enumerate(g) for x, k in enumerate(row) if k and pal[k][3] >= 0x3C}
            contours = outline(inked)
            assert -sum(sum(c[i - 1][0] * p[1] - p[0] * c[i - 1][1] for i, p in enumerate(c)) for c in contours) == 2 * len(inked)
            for c in contours:
                pen.moveTo((c[0][0] * 100, c[0][1] * 100))
                for x, y in c[1:]:
                    pen.lineTo((x * 100, y * 100))
                pen.closePath()
        order.append(name)
        outlines[name] = pen.glyph()
        metrics[name] = (round((advance("large", code, e) if e else SPACE) * 100), 0)
        cmap[code] = name
        if code in UNICODE:
            cmap[UNICODE[code]] = name
    fb = FontBuilder(2200, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap(cmap)
    fb.setupGlyf(outlines)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=BASELINE * 100, descent=-(22 - BASELINE) * 100)
    fb.setupNameTable({"familyName": "GHS Dialogue", "styleName": "Regular", "fullName": "GHS Dialogue", "psName": "GHSDialogue-Regular"})
    fb.setupOS2(sTypoAscender=BASELINE * 100, sTypoDescender=-(22 - BASELINE) * 100, sTypoLineGap=0,
                usWinAscent=BASELINE * 100, usWinDescent=(22 - BASELINE) * 100, fsSelection=0x40)
    fb.setupPost()
    path = os.path.join(folder, "GHS Dialogue.ttf")
    fb.save(path)
    print("wrote", path, "(%d glyphs)" % (len(order) - 1))


if __name__ == "__main__":
    cmd, args = (sys.argv[1], sys.argv[2:]) if len(sys.argv) > 1 else ("", [])
    if cmd == "export" and len(args) <= 1:
        export(args[0] if args else os.path.join(ghsarc.ROOT, "unpacked", "font"))
    elif cmd == "ttf" and len(args) <= 1:
        ttf(args[0] if args else os.path.join(ghsarc.ROOT, "unpacked", "font"))
    elif cmd == "text" and len(args) in (2, 3):
        text(args[0], args[1], "small" if len(args) == 3 else "large")
    else:
        raise SystemExit(__doc__)
