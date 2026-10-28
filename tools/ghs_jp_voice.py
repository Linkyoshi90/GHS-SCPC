# Japanese voices with the English game: reads the Japanese disc image (game_jp\*.iso), copies its voice pool
# NGYSTM.BIN to game\NGYSTM_V.BIN and writes game\ghs_jp_voice.txt, the EU clip -> JP clip map that
# overrides\zz_ghs_jp_voice.cpp plays from (F2 -> "Japanese voices"). Game data: both files stay local.
#   python tools\ghs_jp_voice.py [path\to\japan.iso]
#
# How clips are paired (EU has 989 clips, 260 of them silent placeholders; JP has 870, the last 20 = the same music):
#  1. dialogue database: JP keeps it in the program (SLPM_653.24) as (id, pointer) records with the same message
#     ids as the EU text resource (33, slots 101-119); a voice cue `1F xxxx` on the same message pairs the clips.
#     Game code that starts a clip by number pairs too (same call, found by the code around it).
#  2. event text (resource 33, slots 0-99): plain string pools in EU, (id, pointer) tables in JP; aligned line by
#     line (Needleman-Wunsch) using the pairs from 1. plus the clips in between that keep the same id offset.
#  3. clips between two pairs: EU only inserts clips, so the pairs' id offset continues while the lengths fit; a
#     line that JP voices with a reused clip gets that clip; EU lines JP never voiced (Death's dream talk, Judgment
#     Boy's quiz) play silence instead of English.
import glob, math, os, re, shutil, struct, sys
from collections import Counter, defaultdict
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ghsarc

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.join(ROOT, 'game')
SECTOR = 2048
EU_TABLE, EU_FORMATS, EU_CLIPS = 0x342000, 0x340350, 989  # SLES_519.33 (0x2B7598 reads both)
EU_VOICE_CALLS = (0x20E240, 0x20E2B0)  # SLES_519.33: start voice clip a0 (both lead to 0x2B5230)
CUE = re.compile(rb'\x1f([0-9a-fA-F]{4})')


def iso_files(path):
    """{NAME: (lba, size)} of the image's root directory (ISO 9660)."""
    f = open(path, 'rb')
    f.seek(16 * SECTOR)
    pvd = f.read(SECTOR)
    if pvd[1:6] != b'CD001':
        sys.exit(f'{path}: not an ISO 9660 image')
    lba, size = struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]
    f.seek(lba * SECTOR)
    d = f.read(size)
    out, i = {}, 0
    while i < len(d):
        n = d[i]
        if n == 0:
            i = (i // SECTOR + 1) * SECTOR
            continue
        r = d[i:i + n]
        name = r[33:33 + r[32]].decode('ascii', 'replace').split(';')[0]
        out[name] = struct.unpack_from('<I', r, 2)[0], struct.unpack_from('<I', r, 10)[0]
        i += n
    return out


def u64_table(elf, file_off, end_sector):
    """Stream start sectors up to and including the end sentinel (= pool size in sectors)."""
    t = []
    while not t or t[-1] != end_sector:
        t.append(struct.unpack_from('<Q', elf, file_off + 8 * len(t))[0])
        if len(t) > 4096:
            sys.exit('stream table not found')
    return t


def cues(s):
    return tuple(int(x, 16) for x in CUE.findall(s))


def voiced_lengths(f, base, table):
    """Spoken length of each stream in 50 ms steps (0 = silent placeholder)."""
    out = []
    for a, b in zip(table, table[1:]):
        f.seek(base + a * SECTOR)
        x = np.abs(np.frombuffer(f.read((b - a) * SECTOR), '<i2').astype(np.float32))
        on = np.nonzero(x[:len(x) // 2400 * 2400].reshape(-1, 2400).mean(1) > 300)[0]
        out.append(int(on[-1] - on[0] + 1) if len(on) else 0)
    return out


def words(elf):
    return struct.unpack_from(f'<{len(elf) // 4}I', elf)


def shape(w):
    """An instruction without its immediate or jump target, to compare code between the two programs."""
    op = w >> 26
    if op in (2, 3):
        return w & 0xFC000000
    return w if op in (0, 0x10, 0x11, 0x12, 0x1C) else w & 0xFFFF0000


def code_pairs(eu, jp):
    """[(EU clip, JP clip)] from the calls that start a clip by number: the JP program has the same functions and
    calls at other addresses, found by comparing the code around them."""
    we, wj = words(eu), words(jp)
    se, sj = [shape(w) for w in we], [shape(w) for w in wj]

    def calls(w, s, fn):
        jal = 0x0C000000 | (fn >> 2)
        for i, x in enumerate(w):
            if x == jal:
                li = [w[k] & 0xFFFF for k in range(i - 8, i + 2) if w[k] >> 16 in (0x2404, 0x3404)]  # li a0, clip
                if li:
                    yield li[-1], s[i - 24:i + 8]

    out = []
    for fn in EU_VOICE_CALLS:
        k = (fn - 0x100000 + 0x80) // 4
        pat = se[k:k + 24]
        eu_calls = list(calls(we, se, fn))
        for i in range(len(sj) - 24):  # JP has some of these functions twice: the code around the calls decides
            if sj[i] == pat[0] and sj[i:i + 24] == pat:
                for clip, ctx in calls(wj, sj, i * 4 - 0x80 + 0x100000):
                    e, ectx = max(eu_calls, key=lambda c: sum(a == b for a, b in zip(ctx, c[1])))
                    if sum(a == b for a, b in zip(ctx, ectx)) >= 26:  # of 32 instructions
                        out.append((e, clip))
    return out


def jp_tables(jp):
    """(id, pointer) record tables in the JP program that hold voiced text: list of [(id, bytes)]."""
    lo, hi = 0x100000, 0x100000 + len(jp) - 0x80
    text = lambda va: jp[va - 0x100000 + 0x80:jp.index(b'\0', va - 0x100000 + 0x80)]
    tabs, i = [], 0
    while i < len(jp) - 16:
        mid, p = struct.unpack_from('<II', jp, i)
        if mid == 1 and lo <= p < hi:
            recs, k = [], i
            while True:
                mid, p = struct.unpack_from('<II', jp, k)
                if not lo <= p < hi or mid > 100000:
                    break
                recs.append((mid, text(p)))
                k += 8
            if len(recs) >= 5 and any(CUE.search(t) for _, t in recs):
                tabs.append(recs)
                i = k
                continue
        i += 4
    return tabs


def eu_text():
    """English text resource 33: {slot: leaf bytes}."""
    orig = os.path.join(GAME, 'FILE.STM.orig')  # the game's own text even when mods change it (tools\ghsmod.py)
    raw, top = ghsarc.load_file_stm(orig if os.path.exists(orig) else os.path.join(GAME, 'FILE.STM'))
    off, size = top[33]
    data = raw[off:off + size]
    if data[:4] == b'SLID':
        data = ghsarc.slid(data)[0]
    out = {}
    for k, (o, s, _) in enumerate(ghsarc.as_archive(data)):
        leaf = data[o:o + s]
        out[k] = ghsarc.slid(leaf)[0] if leaf[:4] == b'SLID' else leaf
    return out


def dialogue(leaf):
    """EU dialogue leaf: {message id: text}."""
    out, q = {}, 0
    while q + 8 <= len(leaf):
        mid, off = struct.unpack_from('<II', leaf, q)
        if mid == 0xFFFFFFFF:
            break
        out.setdefault(mid, leaf[off:leaf.index(b'\0', off)])
        q += 8
    return out


def nw(E, J, score, gap=-0.6):
    """Needleman-Wunsch: list of aligned (i, j) index pairs."""
    n, m = len(E), len(J)
    D = np.zeros((n + 1, m + 1))
    B = np.zeros((n + 1, m + 1), np.int8)
    D[:, 0] = np.arange(n + 1) * gap
    D[0, :] = np.arange(m + 1) * gap
    B[1:, 0], B[0, 1:] = 1, 2
    for a in range(1, n + 1):
        for b in range(1, m + 1):
            D[a, b], B[a, b] = max((D[a - 1, b - 1] + score(E[a - 1], J[b - 1]), 0), (D[a - 1, b] + gap, 1), (D[a, b - 1] + gap, 2))
    pairs, a, b = [], n, m
    while a and b:
        if B[a, b] == 0:
            pairs.append((a - 1, b - 1))
            a, b = a - 1, b - 1
        elif B[a, b] == 1:
            a -= 1
        else:
            b -= 1
    return pairs


def increasing(pairs):
    """Longest strictly increasing run of (eu, jp) pairs sorted by eu: drops the few contradicting pairs."""
    pairs = sorted(pairs)
    best, prev = [1] * len(pairs), [-1] * len(pairs)
    for i in range(len(pairs)):
        for k in range(i):
            if pairs[k][1] < pairs[i][1] and pairs[k][0] < pairs[i][0] and best[k] + 1 > best[i]:
                best[i], prev[i] = best[k] + 1, k
    i, out = max(range(len(pairs)), key=best.__getitem__), []
    while i >= 0:
        out.append(pairs[i])
        i = prev[i]
    return dict(reversed(out))


def main():
    isos = sys.argv[1:] or glob.glob(os.path.join(ROOT, 'game_jp', '*.iso'))
    if not isos:
        sys.exit('put the Japanese disc image into game_jp\\ (or pass its path)')
    iso = isos[0]
    files = iso_files(iso)
    exe = next((n for n in files if n.startswith('SLPM_')), None)
    if not exe or 'NGYSTM.BIN' not in files:
        sys.exit(f'{iso}: no SLPM_* / NGYSTM.BIN, not the Japanese Gregory Horror Show disc?')
    with open(iso, 'rb') as f:
        f.seek(files[exe][0] * SECTOR)
        jp = f.read(files[exe][1])
    eu = open(os.path.join(GAME, 'SLES_519.33'), 'rb').read()
    eu_pool = os.path.join(GAME, 'NGYSTM_U.BIN')

    # Stream tables: EU at its known address, JP found by its end sentinel (the pool size in sectors).
    E = u64_table(eu, EU_TABLE - 0x100000 + 0x80, os.path.getsize(eu_pool) // SECTOR)
    jp_end = files['NGYSTM.BIN'][1] // SECTOR
    end_off = next(i for i in range(0, len(jp) - 8, 8) if struct.unpack_from('<Q', jp, i)[0] == jp_end)
    start = end_off
    while struct.unpack_from('<Q', jp, start)[0] != 0:
        start -= 8
    J = u64_table(jp, start, jp_end)
    if len(E) != EU_CLIPS + 1:
        sys.exit(f'EU stream table has {len(E) - 1} entries, expected {EU_CLIPS}')
    nj = len(J) - 1
    # 6-byte format records (u8 format: channels/rate, ...); JP's end with the same 20 music records as EU's.
    efmt = eu[EU_FORMATS - 0x100000 + 0x80:][:6 * EU_CLIPS]
    jfmt_end = jp.find(efmt[-120:]) + 120
    jfmt = jp[jfmt_end - 6 * nj:jfmt_end]
    print(f'{exe}: {nj} JP clips, EU {EU_CLIPS}')

    print('measuring clip lengths (both pools, about a minute)...')
    with open(eu_pool, 'rb') as f:
        le = voiced_lengths(f, 0, E)
    with open(iso, 'rb') as f:
        lj = voiced_lengths(f, files['NGYSTM.BIN'][0] * SECTOR, J)
    silent = lambda e: le[e] == 0

    # 1. dialogue database.
    leaves = eu_text()
    tabs = jp_tables(jp)
    db = max(tabs, key=len)
    jdb, cur, prev = [], None, None
    for mid, t in db:
        if prev is None or mid < prev:
            cur = {}
            jdb.append(cur)
        cur.setdefault(mid, t)
        prev = mid
    eudb = {k: dialogue(leaves[k]) for k in range(101, 120) if len(leaves.get(k, b'')) > 64}
    jmerged = defaultdict(dict)
    for part in jdb:  # JP splits some leaves in two or three: each part joins the EU leaf it shares most ids with
        k = max(eudb, key=lambda k: len(eudb[k].keys() & part.keys()) / len(eudb[k].keys() | part.keys()))
        jmerged[k].update(part)
    lines = []  # (EU clips, JP clips) of the same line in both games
    for k, msgs in eudb.items():
        for mid, t in msgs.items():
            a, b = cues(t), cues(jmerged[k].get(mid, b''))
            if a and b:
                lines.append((a, b))
    code = code_pairs(eu, jp)
    votes = defaultdict(Counter)
    for a, b in lines:
        if len(a) == len(b):
            for x, y in zip(a, b):
                votes[x][y] += 1
    for x, y in code:
        votes[x][y] += 100
    pairs = increasing([(e, c.most_common(1)[0][0]) for e, c in votes.items() if e < EU_CLIPS and c.most_common(1)[0][0] < nj])
    print(f'dialogue database and game code: {len(pairs)} clip pairs ({len(code)} from code)')

    # 2. event text; "confident" = paired above or inside a run of pairs with the same id offset.
    sure = dict(pairs)
    keys = sorted(pairs)
    for e0, e1 in zip(keys, keys[1:]):
        if e1 - e0 == pairs[e1] - pairs[e0]:
            for e in range(e0 + 1, e1):
                sure[e] = pairs[e0] + e - e0

    def score(et, jt):
        if not et and not jt:
            return 0.3
        if not et or not jt:
            return -0.7
        known = [e for e in et if e in sure]
        if known:
            return 3 if all(sure[e] in jt for e in known) else -1.5
        return 0.6 if len(et) == len(jt) else -0.3

    jevents = [[cues(t) for _, t in tab] for tab in tabs if tab is not db]
    for k in range(100):
        el = [cues(s) for s in leaves.get(k, b'').split(b'\0') if s]
        known = sum(1 for t in el if any(e in sure for e in t))
        if known < 3:
            continue
        best = None
        for jl in jevents:
            if not any(t and t[0] in sure.values() for t in jl):
                continue
            al = nw(el, jl, score)
            hits = sum(1 for a, b in al if score(el[a], jl[b]) == 3)
            if not best or hits > best[0]:
                best = hits, al, jl
        if not best or best[0] < max(3, known // 3):
            continue
        lines += [(el[a], best[2][b]) for a, b in best[1] if el[a] and best[2][b]]

    def pair_cues(et, jt):
        """Clip pairs of one line: clips that pair for sure split both games' lists, the rest pair in order between
        them (EU often splits a JP clip into several text boxes: the extra EU clips get none)."""
        out, i0, k0 = [], 0, 0
        for i, e in enumerate(et + (None,)):
            if e is None:
                k = len(jt)
            elif sure.get(e) in jt[k0:]:
                k = jt.index(sure[e], k0)
            else:
                continue
            out += zip(et[i0:i], jt[k0:k])
            if e is not None:
                out.append((e, jt[k]))
            i0, k0 = i + 1, k + 1
        return out

    votes = defaultdict(Counter)
    for a, b in lines:
        for x, y in pair_cues(a, b):
            votes[x][y] += 1
    for x, y in code:
        votes[x][y] += 100
    for e in range(EU_CLIPS - 20, EU_CLIPS):  # the music at the end: byte-identical in both pools
        votes[e][e - EU_CLIPS + nj] += 100
    votes = {e: c.most_common(1)[0][0] for e, c in votes.items() if e < EU_CLIPS and not silent(e) and c.most_common(1)[0][0] < nj}
    pairs = increasing(votes.items())
    print(f'with event text: {len(pairs)} clip pairs')

    # 3. Between pairs: EU inserts clips (silent placeholders, new English lines) but keeps the order, so a gap whose
    # two ends have the same id offset keeps it throughout; otherwise each end's offset continues into the gap while
    # the clips still fit. The JP game reuses old clips for some lines that EU recorded anew: the text pairs those.
    # A spoken EU clip left over has no Japanese recording (the JP game shows that line without a voice): silent.
    fits = lambda e, j: efmt[6 * e] == jfmt[6 * j] and abs(math.log(le[e] / max(lj[j], 1))) < 0.7
    # a clip EU starts from its text can only be one that JP starts from its text too (JP voices lines the same way)
    eu_said = {c for leaf in leaves.values() for c in cues(leaf)} | set(cues(eu))
    jp_said = set(cues(jp))
    fits_text = lambda e, j: e not in eu_said or j in jp_said
    clip_map = dict(pairs)
    taken = set(pairs.values())
    anchors = [(-1, -1)] + sorted(pairs.items()) + [(EU_CLIPS, nj)]
    for (e0, j0), (e1, j1) in zip(anchors, anchors[1:]):
        if e1 - e0 == j1 - j0:
            clip_map.update((e, e + j0 - e0) for e in range(e0 + 1, e1) if not silent(e))
            continue
        for es, off in ((range(e0 + 1, e1), j0 - e0), (range(e1 - 1, e0, -1), j1 - e1)):
            for e in es:
                if not j0 < e + off < j1 or e in clip_map:
                    break
                if not silent(e):
                    if e in votes or not fits(e, e + off) or not fits_text(e, e + off) or e + off in taken:
                        break
                    clip_map[e] = e + off
                    taken.add(e + off)
    for e, j in votes.items():
        clip_map.setdefault(e, j)

    real = [e for e in range(EU_CLIPS) if not silent(e)]
    mapped = sorted(e for e in real if e in clip_map and efmt[6 * e] == jfmt[6 * clip_map[e]])
    mute = [e for e in real if e not in mapped]
    print(f'{len(mapped)} of {len(real)} spoken EU clips get a Japanese voice ({len(pairs)} from text and code), '
          f'{len(mute)} have none (silent)')

    # Voice pool: game\NGYSTM_V.BIN sorts right after NGYSTM_U.BIN, so on the runner's virtual disc its sectors
    # follow the EU pool's directly (the override relies on that and checks it).
    dst = os.path.join(GAME, 'NGYSTM_V.BIN')
    if not os.path.exists(dst) or os.path.getsize(dst) != files['NGYSTM.BIN'][1]:
        print(f'copying NGYSTM.BIN ({files["NGYSTM.BIN"][1] >> 20} MB) -> {dst}')
        with open(iso, 'rb') as src, open(dst, 'wb') as out:
            src.seek(files['NGYSTM.BIN'][0] * SECTOR)
            left = files['NGYSTM.BIN'][1]
            while left:
                chunk = src.read(min(left, 16 << 20))
                out.write(chunk)
                left -= len(chunk)
    with open(os.path.join(GAME, 'ghs_jp_voice.txt'), 'w', newline='\n') as f:
        f.write(f'# EU clip, JP clip, JP start sector, JP end sector (in NGYSTM_V.BIN); "mute" EU clip, start and end\n'
                f'# sector of a silent EU clip (in NGYSTM_U.BIN); tools\\ghs_jp_voice.py\n')
        f.write(f'pool {len(E) - 1} {E[-1]} {nj} {J[-1]}\n')
        for e in mapped:
            j = clip_map[e]
            f.write(f'{e} {j} {J[j]} {J[j + 1]}\n')
        quiet = min((e for e in range(EU_CLIPS) if silent(e)), key=lambda e: E[e + 1] - E[e])
        for e in mute:
            f.write(f'mute {e} {E[quiet]} {E[quiet + 1]}\n')
    print(f'-> {os.path.join(GAME, "ghs_jp_voice.txt")}')


if __name__ == '__main__':
    main()
