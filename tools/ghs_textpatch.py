"""Text replacement lists for mods (a mod's *.txt files; tools\\ghsmod.py applies them).

One replacement per line, tab separated: text entry (path in FILE.STM as tools\\ghsarc.py unpack names it), old text,
new text, with Python escapes (\\n line break, \\x1b colour codes, \\\\), Latin-1. Lines starting with # are comments.
Each replacement applies to every message of its entry, in list order; one that matches nothing is an error (other
game text, or the list does not fit this release).
Text entries: messages of 8-bit text, each ended by a 16-bit zero (after one zero byte if the length is odd). The
game finds a message at its fixed byte offset (not in any table in FILE.STM or the ELF; tested: shorter messages
that moved the later ones showed those cut off), so a message keeps its slot: shorter text is zero-filled, longer
text is an error.
"""


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


def parse(text, name):
    """{entry path: [(where, old, new)]} from a replacement list."""
    out = {}
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 3:
            raise ValueError("%s line %d: expected entry<TAB>old<TAB>new" % (name, n))
        out.setdefault(parts[0], []).append(("%s line %d" % (name, n), unescape(parts[1]), unescape(parts[2])))
    return out


def apply(data, replacements):
    """data with the replacements made, each message in its own slot; ValueError lists every problem."""
    texts, errors = [s for a, b, s in slots(data)], []
    for where, old, new in replacements:
        if not any(old in t for t in texts):
            errors.append("%s: %r not found" % (where, old.decode("latin-1")))
        texts = [t.replace(old, new) for t in texts]
    buf = bytearray(data)
    for (a, b, s), t in zip(slots(data), texts):
        if t == s:
            continue
        if len(t) + len(t) % 2 + 2 > b - a:
            errors.append("%r is %d bytes longer than the original message" % (t.decode("latin-1"), len(t) + len(t) % 2 + 2 - (b - a)))
            continue
        buf[a:b] = t + bytes(b - a - len(t))
    if errors:
        raise ValueError("\n".join(errors))
    return bytes(buf)
