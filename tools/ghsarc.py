"""Reader for the game's FILE.STM resource archive (structure only; nothing is written to disk).

FILE.STM / nested archive layout (little endian):
  table of (offset:u32, size:u32) pairs; the first offset equals the table size in bytes;
  the last pair has bit 31 set in size (end marker, the real size is size & 0x7FFFFFFF), the
  entries are 16-byte aligned. Entries are either raw data, nested archives, or 'SLID' blocks.
SLID block: 'SLID', u32 total size (incl. this 16-byte header), u32 uncompressed size,
  u32 payload size, then Okumura LZSS (4096-byte ring initialised to 0, start 0xFEE, flags LSB first,
  bit set = literal, match = 12-bit ring position + 4-bit (length-3)).
"""
import os
import struct


def lzss(src, start, end, out_size):
    ring = bytearray(4096)
    r = 0xFEE
    out = bytearray()
    i = start
    flags = 0
    while i < end and len(out) < out_size:
        flags >>= 1
        if not (flags & 0x100):
            flags = src[i] | 0xFF00
            i += 1
            if i >= end:
                break
        if flags & 1:
            c = src[i]
            i += 1
            out.append(c)
            ring[r] = c
            r = (r + 1) & 0xFFF
        else:
            if i + 1 >= end:
                break
            b0 = src[i]
            b1 = src[i + 1]
            i += 2
            pos = b0 | ((b1 & 0xF0) << 4)
            length = (b1 & 0x0F) + 3
            for k in range(length):
                c = ring[(pos + k) & 0xFFF]
                out.append(c)
                ring[r] = c
                r = (r + 1) & 0xFFF
    return bytes(out)


def slid(data):
    if len(data) < 16 or data[:4] != b"SLID":
        return None
    total, usize, psize = struct.unpack_from("<III", data, 4)
    return lzss(data, 16, min(len(data), 16 + psize), usize), usize


def as_archive(data):
    """Return list of (offset, size, last) or None if data does not look like an archive."""
    n = len(data)
    if n < 16:
        return None
    first = struct.unpack_from("<I", data, 0)[0]
    if first < 8 or first % 8 or first > n or first > 0x10000:
        return None
    cnt = first // 8
    ents = []
    for k in range(cnt):
        off, size = struct.unpack_from("<II", data, k * 8)
        last = bool(size & 0x80000000)
        size &= 0x7FFFFFFF
        ents.append((off, size, last))
        if last:
            break
    else:
        return None
    for off, size, last in ents:
        if off < first or off + size > n:
            if not (off == 0 and size == 0):
                return None
    if not ents[-1][2]:
        return None
    return ents


def top_level(path):
    with open(path, "rb") as f:
        raw = f.read()
    ents = as_archive(raw)
    return raw, ents


def load_file_stm(path=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "game", "FILE.STM")):
    with open(path, "rb") as f:
        raw = f.read()
    first = struct.unpack_from("<I", raw, 0)[0]
    top = []
    for k in range(first // 8):
        off, size = struct.unpack_from("<II", raw, k * 8)
        top.append((off, size & 0x7FFFFFFF))
    return raw, top


def walk_leaves(data, path=(), depth=0):
    """Yield (path, bytes) for every leaf of a resource (SLID blocks are decompressed, archives entered)."""
    if data[:4] == b"SLID":
        dec = slid(data)
        if dec is None:
            return
        data = dec[0]
    ents = as_archive(data)
    if ents is not None and depth < 6:
        for k, (off, size, last) in enumerate(ents):
            if off == 0 and size == 0:
                continue
            yield from walk_leaves(data[off:off + size], path + (k,), depth + 1)
    else:
        yield path, data
