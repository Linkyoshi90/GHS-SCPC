"""The game's textures (FILE.STM entries starting with 08 00 00 00 or 09 00 00 00) as PNG, for mods.

An entry holds one or more images back to back, each: u32 type (8: 4-bit, 9: 8-bit), u32 palette bytes, u32 0,
u16 x u16 palette layout (8x2 / 16x16), palette (RGBA, alpha 0x80 = opaque; 8-bit palettes in the PS2's CSM1 order:
entries 8-15 and 16-23 of each 32 swapped), then u32 flags (bit 31: last image), u32 pixel bytes, u32 0,
u16 width, u16 height, then the pixels, row by row (4-bit: low nibble first).
"""
import struct
import zlib
from collections import Counter


def images(data):
    """[(start, end, type, palette start, pixel start, width, height)] or None if data is not a texture entry."""
    out, q = [], 0
    while q + 16 <= len(data):
        kind, palsize = struct.unpack_from("<II", data, q)
        if kind not in (8, 9) or palsize != (64 if kind == 8 else 1024) or q + 32 + palsize > len(data):
            return None
        flags, nbytes, _, w, h = struct.unpack_from("<IIIHH", data, q + 16 + palsize)
        px = q + 32 + palsize
        if nbytes != w * h * (kind - 7) // 2 or px + nbytes > len(data):
            return None
        out.append((q, px + nbytes, kind, q + 16, px, w, h))
        q = px + nbytes
        if flags & 0x80000000:
            return out if q == len(data) else None
    return None


def _csm1(k):
    return (k & 0xE7) | ((k & 8) << 1) | ((k & 16) >> 1)


def palette(data, img):
    q, end, kind, pal = img[:4]
    n = 16 if kind == 8 else 256
    raw = [tuple(data[pal + k * 4:pal + k * 4 + 4]) for k in range(n)]
    return raw if n == 16 else [raw[_csm1(k)] for k in range(n)]


def decode(data, img):
    """RGBA bytes of one image (PS2 alpha 0x80 -> 255)."""
    cols = [bytes((r, g, b, min(255, a * 2))) for r, g, b, a in palette(data, img)]
    px = data[img[4]:img[1]]
    if img[2] == 8:
        return b"".join(cols[c & 15] + cols[c >> 4] for c in px)
    return b"".join(cols[c] for c in px)


def _quantize(colors, n):
    """Median cut: up to n palette colours and an index per colour in colors (a Counter of RGBA tuples)."""
    boxes = [list(colors)]
    while len(boxes) < n:
        spans = [(max(max(c[i] for c in b) - min(c[i] for c in b) for i in range(4)), k) for k, b in enumerate(boxes) if len(b) > 1]
        if not spans:
            break
        _, k = max(spans)
        box = boxes.pop(k)
        axis = max(range(4), key=lambda i: max(c[i] for c in box) - min(c[i] for c in box))
        box.sort(key=lambda c: c[axis])
        half, acc, cut = sum(colors[c] for c in box) / 2, 0, 1
        for cut, c in enumerate(box, 1):
            acc += colors[c]
            if acc >= half:
                break
        cut = min(max(cut, 1), len(box) - 1)
        boxes += [box[:cut], box[cut:]]
    pal, index = [], {}
    for k, box in enumerate(boxes):
        total = sum(colors[c] for c in box)
        pal.append(tuple(round(sum(c[i] * colors[c] for c in box) / total) for i in range(4)))
        for c in box:
            index[c] = k
    return pal, index


def encode(data, img, rgba):
    """data with one image replaced by rgba (width x height RGBA bytes): the original palette when every colour is in
    it, else a new one (median cut to 16 or 256 colours)."""
    q, end, kind, pal_at, px_at, w, h = img
    n = 16 if kind == 8 else 256
    pixels = [tuple(rgba[i:i + 4]) for i in range(0, len(rgba), 4)]
    old = [(r, g, b, min(255, a * 2)) for r, g, b, a in palette(data, img)]
    index = {}
    for k, c in enumerate(old):
        index.setdefault(c, k)
    clear = next((k for k, c in enumerate(old) if c[3] == 0), None)  # editors may change the colour of clear pixels
    out = bytearray(data)
    if all(c in index or (c[3] == 0 and clear is not None) for c in set(pixels)):
        idx = [index.get(c, clear) for c in pixels]
    else:
        pixels = [c if c[3] else (0, 0, 0, 0) for c in pixels]
        pal, index = _quantize(Counter(pixels), n)
        pal = [(r, g, b, (a + 1) // 2) for r, g, b, a in pal] + [(0, 0, 0, 0)] * (n - len(pal))  # 255 -> 0x80
        idx = [index[c] for c in pixels]
        raw = pal if n == 16 else [pal[_csm1(k)] for k in range(n)]
        out[pal_at:pal_at + n * 4] = b"".join(bytes(c) for c in raw)
    if kind == 8:
        out[px_at:end] = bytes(idx[i] | (idx[i + 1] << 4) for i in range(0, len(idx), 2))
    else:
        out[px_at:end] = bytes(idx)
    return bytes(out)


def write_png(path, w, h, rgba):
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def read_png(blob):
    """(width, height, RGBA bytes) of a PNG: 8-bit grey/RGB/RGBA/grey+alpha or 1-8 bit palette, not interlaced."""
    if blob[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    i, idat, plte, trns = 8, b"", b"", b""
    while i < len(blob):
        n = struct.unpack_from(">I", blob, i)[0]
        t, d = blob[i + 4:i + 8], blob[i + 8:i + 8 + n]
        if t == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", d)
        elif t == b"IDAT":
            idat += d
        elif t == b"PLTE":
            plte = d
        elif t == b"tRNS":
            trns = d
        i += 12 + n
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    if interlace or (depth != 8 and ctype != 3) or depth > 8:
        raise ValueError("unsupported PNG (save it as 8-bit RGBA, not interlaced)")
    raw = zlib.decompress(idat)
    stride, bpp = (w * channels * depth + 7) // 8, max(1, channels * depth // 8)
    rows, prev, p = [], bytearray(stride), 0
    for _ in range(h):
        ft, line = raw[p], bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if ft == 1:
                line[x] = (line[x] + a) & 255
            elif ft == 2:
                line[x] = (line[x] + b) & 255
            elif ft == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif ft == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    out = bytearray()
    for line in rows:
        if ctype == 3:
            vals = [(line[x * depth // 8] >> (8 - depth - x * depth % 8)) & ((1 << depth) - 1) for x in range(w)]
            for v in vals:
                out += plte[v * 3:v * 3 + 3] + bytes((trns[v] if v < len(trns) else 255,))
        elif ctype == 6:
            out += line
        elif ctype == 2:
            for x in range(w):
                out += line[x * 3:x * 3 + 3] + b"\xff"
        elif ctype == 4:
            for x in range(w):
                out += bytes((line[x * 2],) * 3 + (line[x * 2 + 1],))
        else:
            for x in range(w):
                out += bytes((line[x],) * 3 + (255,))
    return w, h, bytes(out)
