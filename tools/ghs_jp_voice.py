# Japanese voices with the English game: reads the Japanese disc image (game_jp\*.iso), copies its voice pool
# NGYSTM.BIN to game\NGYSTM_V.BIN and writes game\ghs_jp_voice.txt, the EU clip -> JP clip map that
# overrides\zz_ghs_jp_voice.cpp plays from (F2 -> "Japanese voices"). Game data: both files stay local.
#   python tools\ghs_jp_voice.py [path\to\japan.iso]
#
# How clips are paired (EU has 989 clips, 260 of them silent placeholders; JP has 870, the last 20 = the same music):
#  1. dialogue database: JP keeps it in the program (SLPM_653.24) as (id, pointer) records with the same message
#     ids as the EU text resource (33, slots 101-119); a voice cue `1F xxxx` on the same message pairs the clips.
#  2. event text (resource 33, slots 0-99): plain string pools in EU, (id, pointer) tables in JP; aligned line by
#     line (Needleman-Wunsch) using the pairs from 1. plus the clips in between that keep the same id offset.
#  3. clips between two pairs: aligned by spoken length; silent EU placeholders stay silent.
import glob, math, os, re, shutil, struct, sys
from collections import Counter, defaultdict
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ghsarc

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.join(ROOT, 'game')
SECTOR = 2048
EU_TABLE, EU_FORMATS, EU_CLIPS = 0x342000, 0x340350, 989  # SLES_519.33 (0x2B7598 reads both)
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
    raw, top = ghsarc.load_file_stm(os.path.join(GAME, 'FILE.STM'))
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
    votes = defaultdict(Counter)
    for k, msgs in eudb.items():
        for mid, t in msgs.items():
            a, b = cues(t), cues(jmerged[k].get(mid, b''))
            if a and len(a) == len(b):
                for x, y in zip(a, b):
                    if x < EU_CLIPS and y < nj:
                        votes[x][y] += 1
    pairs = increasing([(e, c.most_common(1)[0][0]) for e, c in votes.items()])
    print(f'dialogue database: {len(pairs)} clip pairs')

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
        if len(et) != len(jt):
            return -1
        if all(e in sure for e in et):
            return 3 if tuple(sure[e] for e in et) == jt else -1.5
        return 0.6

    jevents = [[cues(t) for _, t in tab] for tab in tabs if tab is not db]
    for k in range(100):
        el = [cues(s) for s in leaves.get(k, b'').split(b'\0') if s]
        known = sum(1 for t in el if t and all(e in sure for e in t))
        if known < 3:
            continue
        best = None
        for jl in jevents:
            if not any(t and t[0] in sure.values() for t in jl):
                continue
            al = nw(el, jl, score)
            hits = sum(1 for a, b in al if el[a] and all(e in sure for e in el[a]) and tuple(sure[e] for e in el[a]) == jl[b])
            if not best or hits > best[0]:
                best = hits, al, jl
        if not best or best[0] < max(3, known // 3):
            continue
        for a, b in best[1]:
            if el[a] and len(el[a]) == len(best[2][b]):
                for x, y in zip(el[a], best[2][b]):
                    if x < EU_CLIPS and y < nj:
                        votes[x][y] += 1
    for e in range(EU_CLIPS - 20, EU_CLIPS):  # the music at the end: byte-identical in both pools
        votes[e][e - EU_CLIPS + nj] += 100
    pairs = increasing([(e, c.most_common(1)[0][0]) for e, c in votes.items() if not silent(e)])
    print(f'with event text: {len(pairs)} clip pairs')

    # 3. between pairs: align by spoken length (JP lines run about 9 % longer); silent EU slots are free to skip.
    clip_map = dict(pairs)
    anchors = [(-1, -1)] + sorted(pairs.items()) + [(EU_CLIPS, nj)]
    for (e0, j0), (e1, j1) in zip(anchors, anchors[1:]):
        es, js = list(range(e0 + 1, e1)), list(range(j0 + 1, j1))
        if not es or not js:
            continue
        cost = lambda e, j: 0.7 if silent(e) else min(abs(math.log(le[e] / max(lj[j], 1)) - 0.09), 3.0)
        n, m = len(es), len(js)
        D = np.full((n + 1, m + 1), 1e9)
        B = np.zeros((n + 1, m + 1), np.int8)
        D[0, 0] = 0
        for a in range(n + 1):
            for b in range(m + 1):
                if a and b and D[a - 1, b - 1] + cost(es[a - 1], js[b - 1]) < D[a, b]:
                    D[a, b], B[a, b] = D[a - 1, b - 1] + cost(es[a - 1], js[b - 1]), 0
                if a and D[a - 1, b] + (0 if silent(es[a - 1]) else 1.2) < D[a, b]:
                    D[a, b], B[a, b] = D[a - 1, b] + (0 if silent(es[a - 1]) else 1.2), 1
                if b and D[a, b - 1] + 1.0 < D[a, b]:
                    D[a, b], B[a, b] = D[a, b - 1] + 1.0, 2
        a, b = n, m
        while a or b:
            if a and b and B[a, b] == 0:
                if not silent(es[a - 1]):
                    clip_map[es[a - 1]] = js[b - 1]
                a, b = a - 1, b - 1
            elif a and B[a, b] == 1:
                a -= 1
            else:
                b -= 1

    real = [e for e in range(EU_CLIPS) if not silent(e)]
    mapped = sorted(e for e in real if e in clip_map and efmt[6 * e] == jfmt[6 * clip_map[e]])
    assert all(clip_map[a] < clip_map[b] for a, b in zip(mapped, mapped[1:])), 'map is not in order'
    print(f'{len(mapped)} of {len(real)} spoken EU clips get a Japanese voice ({len(pairs)} from text)')

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
        f.write(f'# EU clip, JP clip, JP start sector, JP end sector (in NGYSTM_V.BIN); tools\\ghs_jp_voice.py\n')
        f.write(f'pool {len(E) - 1} {E[-1]} {nj} {J[-1]}\n')
        for e in mapped:
            j = clip_map[e]
            f.write(f'{e} {j} {J[j]} {J[j + 1]}\n')
    print(f'-> {os.path.join(GAME, "ghs_jp_voice.txt")}')


if __name__ == '__main__':
    main()
