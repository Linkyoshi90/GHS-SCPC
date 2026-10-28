"""Applies a text patch list to the unpacked FILE.STM (tools\\ghsarc.py unpack), in place; repack afterwards.
  python tools\\ghs_textpatch.py textpatch\\pc_english.txt [mods\\FILE.STM]

Patch list: one replacement per line, tab separated: text entry (path in the unpacked folder), old text, new text,
with Python escapes (\\n line break, \\x1b colour codes, \\\\), Latin-1. Lines starting with # are comments. Each
replacement applies to every message of its entry, in list order. Nothing is written unless every replacement
matches (a miss means other game text, or the list is applied already) and every message still fits.
Text entries: messages of 8-bit text, each ended by a 16-bit zero (after one zero byte if the length is odd). The
game finds a message at its fixed byte offset (not in any table in FILE.STM or the ELF; tested: shorter messages
that moved the later ones showed those cut off), so a message keeps its slot: shorter text is zero-filled, longer
text is refused.
"""
import os
import sys


def slots(data):
    """(start, end, text) per message; end includes the terminator and padding."""
    out, i = [], 0
    while i < len(data) - 1:
        j = i
        while j < len(data) - 1 and data[j:j + 2] != b"\0\0":
            j += 2
        s = data[i:j]
        out.append((i, j + 2, s[:-1] if s.endswith(b"\0") else s))
        i = j + 2
    return out


def unescape(text):
    return text.encode("latin-1").decode("unicode_escape").encode("latin-1")


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    root = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "mods", "FILE.STM")
    entries, errors = {}, []
    with open(sys.argv[1], encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\r\n")
            if not line.strip() or line.startswith("#"):
                continue
            entry, old, new = line.split("\t")
            if entry not in entries:
                with open(os.path.join(root, entry), "rb") as e:
                    data = e.read()
                entries[entry] = (data, [s for a, b, s in slots(data)])
            old, new = unescape(old), unescape(new)
            data, texts = entries[entry]
            hits = sum(t.count(old) for t in texts)
            if not hits:
                errors.append("line %d: %r not found in %s" % (n, old.decode("latin-1"), entry))
            entries[entry] = (data, [t.replace(old, new) for t in texts])
            print("line %d: %s, %d x" % (n, entry, hits))
    out = {}
    for entry, (data, texts) in entries.items():
        buf = bytearray(data)
        for (a, b, s), t in zip(slots(data), texts):
            if t != s:
                if len(t) + len(t) % 2 + 2 > b - a:
                    errors.append("%s: %r is %d bytes longer than the original message" % (entry, t.decode("latin-1"), len(t) + len(t) % 2 + 2 - (b - a)))
                    continue
                buf[a:b] = t + bytes(b - a - len(t))
        out[entry] = buf
    if errors:
        raise SystemExit("\n".join(errors) + "\nnothing written")
    for entry, buf in out.items():
        with open(os.path.join(root, entry), "wb") as e:
            e.write(buf)
    print("patched", len(out), "entries in", root, "- now: python tools\\ghsarc.py repack", root, "game\\FILE.STM")
