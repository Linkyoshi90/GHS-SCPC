"""Reader and writer for the game's FILE.STM resource archive (for mods).

FILE.STM / nested archive layout (little endian):
  table of (offset:u32, size:u32) pairs; the first offset equals the table size in bytes (padded with
  zeros to 16); the last pair has bit 31 set in size (end marker, the real size is size & 0x7FFFFFFF), the
  entries are 16-byte aligned (zero padding). Entries are either raw data, nested archives, or 'SLID' blocks.
SLID block: 'SLID', u32 total size (incl. this 16-byte header), u32 uncompressed size,
  u32 payload size, then Okumura LZSS (4096-byte ring initialised to 0, start 0xFEE, flags LSB first,
  bit set = literal, match = 12-bit ring position + 4-bit (length-3)).

Unpacked folder (one name per entry, numbered in table order): NNN.bin raw data, NNN\\ nested archive,
NNN.slid.bin / NNN.slid\\ the same SLID-compressed. Archives that would not rebuild byte for byte stay one file.
  python tools\\ghsarc.py unpack [FILE.STM] [folder]       default: game\\FILE.STM -> unpacked\\FILE.STM
  python tools\\ghsarc.py repack <folder> <out> [base]     base (default game\\FILE.STM): untouched SLID blocks are
                                                          copied from it instead of compressed again
Repacking over game\\FILE.STM keeps the original as game\\FILE.STM.orig (copy it back to undo the mod).
To install mods, use tools\\ghsmod.py instead: it builds game\\FILE.STM from the original and every mod in mods\\.
"""
import os
import shutil
import struct
import sys


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


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME_FILE_STM = os.path.join(ROOT, "game", "FILE.STM")


def lzss_compress(data):
    """Greedy LZSS for lzss() above: matches of 3..18 bytes up to 4078 bytes back, found through 3-byte hash chains."""
    out = bytearray()
    chains = {}
    n = len(data)
    i = 0
    while i < n:
        flag_at = len(out)
        out.append(0)
        for bit in range(8):
            if i >= n:
                break
            best, best_pos = 0, 0
            cands = chains.get(data[i:i + 3]) if i + 3 <= n else None
            if cands:
                longest = min(18, n - i)
                for p in reversed(cands[-64:]):  # ponytail: 64 tries per byte; raise for smaller output, not needed so far
                    if i - p > 4096 - 18:
                        break
                    m = 3
                    while m < longest and data[p + m] == data[i + m]:
                        m += 1
                    if m > best:
                        best, best_pos = m, p
                        if m == longest:
                            break
            step = best if best >= 3 else 1
            if best >= 3:
                pos = (0xFEE + best_pos) & 0xFFF
                out += bytes((pos & 0xFF, ((pos >> 4) & 0xF0) | (best - 3)))
            else:
                out[flag_at] |= 1 << bit
                out.append(data[i])
            for j in range(i, min(i + step, n - 2)):
                chains.setdefault(data[j:j + 3], []).append(j)
            i += step
    return bytes(out)


def slid_compress(data):
    payload = lzss_compress(data)
    block = b"SLID" + struct.pack("<III", 16 + len(payload), len(data), len(payload)) + payload
    assert slid(block)[0] == data, "SLID compressor produced a block that does not decompress to its input"
    return block


def build_archive(entries):
    """Inverse of as_archive(): table padded to 16, entries 16-byte aligned, end marker on the last one."""
    out = bytearray((8 * len(entries) + 15) & ~15)
    for k, e in enumerate(entries):
        out += bytes(-len(out) % 16)
        struct.pack_into("<II", out, 8 * k, len(out), len(e) | (0x80000000 if k == len(entries) - 1 else 0))
        out += e
    return bytes(out)


def unpack(data, dst, depth=0):
    """Write an entry to dst (+ '.slid', + '.bin' for a file, or a folder for an archive)."""
    if data[:4] == b"SLID":
        data = slid(data)[0]
        dst += ".slid"
    ents = as_archive(data) if depth < 6 else None
    kids = [data[off:off + size] for off, size, last in ents] if ents else None
    if kids and build_archive(kids) == data:
        os.makedirs(dst)
        for k, kid in enumerate(kids):
            unpack(kid, os.path.join(dst, "%03d" % k), depth + 1)
    else:
        with open(dst + ".bin", "wb") as f:
            f.write(data)


def pack(path, base=None):
    """Bytes of the entry at path (a name unpack() wrote); base: the original entry, reused when nothing changed."""
    compressed = os.path.basename(path).split(".")[1:2] == ["slid"]
    inner_base = (slid(base) or (None,))[0] if compressed and base else base
    if os.path.isdir(path):
        names = sorted(os.listdir(path), key=lambda s: int(s.split(".")[0]) if s.split(".")[0].isdigit() else -1)
        for k, s in enumerate(names):
            if s.split(".")[0] != "%03d" % k or s[len("%03d" % k):] not in ("", ".bin", ".slid", ".slid.bin"):
                raise SystemExit("%s: expected entry %03d (.bin, .slid, .slid.bin or a folder), found %s" % (path, k, s))
        base_ents = as_archive(inner_base) if inner_base else None
        base_kids = [inner_base[off:off + size] for off, size, last in base_ents] if base_ents else []
        data = build_archive([pack(os.path.join(path, s), base_kids[k] if k < len(base_kids) else None)
                              for k, s in enumerate(names)])
    else:
        with open(path, "rb") as f:
            data = f.read()
    if not compressed:
        return data
    if data == inner_base:
        return base
    print("compressing", path, len(data), "bytes")
    return slid_compress(data)


def _child(data, name, path):
    """(entries, index, compressed) of the entry called name (as unpack() names it) in archive data."""
    num, _, rest = name.partition(".")
    ents = as_archive(data)
    if not num.isdigit() or rest not in ("", "bin", "slid", "slid.bin") or ents is None or int(num) >= len(ents):
        raise KeyError("/".join(path))
    kids = [data[off:off + size] for off, size, last in ents]
    compressed = rest.startswith("slid")
    if compressed != (kids[int(num)][:4] == b"SLID"):
        raise KeyError("/".join(path))
    return kids, int(num), compressed


def get(data, path):
    """The entry at path (names as unpack() writes them, e.g. ("033.slid", "070.bin")), decompressed."""
    for name in path:
        kids, k, compressed = _child(data, name, path)
        data = slid(kids[k])[0] if compressed else kids[k]
    return data


def edit(data, edits, prefix=()):
    """data with each entry of edits ({path: fn}) replaced by fn(entry); only the archives on the way are rebuilt,
    each (and each SLID block) once. KeyError(path) for a path that is not in data."""
    if () in edits:
        data = edits[()](data)
    groups = {}
    for path, fn in edits.items():
        if path:
            groups.setdefault(path[0], {})[path[1:]] = fn
    if not groups:
        return data
    kids, changed = None, False
    for name, sub in groups.items():
        kids_, k, compressed = _child(data, name, prefix + (name,))
        kids = kids or kids_
        inner = slid(kids[k])[0] if compressed else kids[k]
        new = edit(inner, sub, prefix + (name,))
        if new != inner:
            kids[k] = slid_compress(new) if compressed else new
            changed = True
    return build_archive(kids) if changed else data


if __name__ == "__main__":
    cmd, args = (sys.argv[1], sys.argv[2:]) if len(sys.argv) > 1 else ("", [])
    if cmd == "unpack" and len(args) <= 2:
        src = args[0] if args else GAME_FILE_STM
        dst = args[1] if len(args) > 1 else os.path.join(ROOT, "unpacked", "FILE.STM")
        if os.path.exists(dst) and os.listdir(dst):
            raise SystemExit(dst + " is not empty: move or delete it first (it may hold your changes)")
        os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
        if os.path.isdir(dst):
            os.rmdir(dst)
        with open(src, "rb") as f:
            unpack(f.read(), dst)
        print("unpacked", src, "->", dst)
    elif cmd == "repack" and len(args) in (2, 3):
        src, out = args[0], args[1]
        base_path = args[2] if len(args) > 2 else GAME_FILE_STM
        with open(base_path, "rb") as f:
            base = f.read()
        data = pack(src, base)
        if os.path.exists(out) and os.path.samefile(out, base_path) and not os.path.exists(out + ".orig"):
            shutil.copyfile(out, out + ".orig")
        with open(out + ".tmp", "wb") as f:
            f.write(data)
        os.replace(out + ".tmp", out)
        print("repacked", src, "->", out, "(identical to base)" if data == base else "(%d bytes)" % len(data))
    else:
        raise SystemExit(__doc__)
