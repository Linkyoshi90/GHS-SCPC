"""The game's character meshes (PM22 rigid, PM23 morphing; FILE.STM model archives, e.g. 170\\035.slid\\000\\*.bin),
for mods that reshape a model without changing its layout (same batches, same vertex counts, same VU memory use).

A mesh: 'PM22'/'PM23', u32 vertex count, u32 byte size, u32 group count, 8 zero bytes, then a VIF stream (little
endian). Per group: STCYCL, UNPACK V4-32 x3 (word 1 of qword 1 + qword 1 word 1: the GS TEX0 of the group's texture,
then the GIF tag), UNPACK V4-16 (the vertices), more batches as UNPACK V4-32 x1 (GIF tag: NLOOP vertices, triangle
strip, bit 15 = end) + UNPACK V4-16, then MSCNT. A vertex is 4 (PM22) or 6 (PM23) int16 quads: position (w 16, or 128
on PM23), normal (4.12 fixed, w 4096), [PM23: position delta, normal delta], colour (0x80 = 1.0), texture s t q
(4.12; 0x1000 = the texture's far edge). Strips are counter-clockwise around the normal on even triangles.
Texture TEX0: TBP0 in the low 14 bits, counted in 256-byte blocks from the character's first image (tools\\ghstex.py
images in order), CBP = image index in bits 5+ of the high word.
"""
import struct


def _vif(data):
    q = 0x18
    while q + 4 <= len(data):
        code = struct.unpack_from("<I", data, q)[0]
        cmd, num, imm = code >> 24, (code >> 16) & 0xFF, code & 0xFFFF
        p, plen = q + 4, 0
        if cmd & 0x60 == 0x60:
            plen = ((32 >> (cmd & 3)) * (((cmd >> 2) & 3) + 1) * (num or 256) + 31) // 32 * 4
        yield cmd, num, p
        q = p + plen


def parse(data):
    """[(tex0 offset or None, vertex offset, vertex count)] per batch; vertices are int16 quads at vertex offset."""
    if data[:4] not in (b"PM22", b"PM23"):
        raise ValueError("not a PM22/PM23 mesh")
    per = 6 if data[:4] == b"PM23" else 4
    out, tex0 = [], None
    for cmd, num, p in _vif(data):
        if cmd == 0x6C:
            tex0 = p + 16 if num == 3 else None
        elif cmd == 0x6D:
            out.append((tex0, p, num // per))
    return out


def elements(data):
    return 6 if data[:4] == b"PM23" else 4


def vertices(data, batch):
    """[(element quads...)] of one batch."""
    per = elements(data)
    _, p, n = batch
    vals = struct.unpack_from("<%dh" % (n * per * 4), data, p)
    return [tuple(tuple(vals[(i * per + e) * 4:(i * per + e) * 4 + 4]) for e in range(per)) for i in range(n)]


def write_vertices(buf, data, batch, verts):
    per = elements(data)
    _, p, n = batch
    if len(verts) != n or any(len(v) != per for v in verts):
        raise ValueError("a batch keeps its vertex count and layout")
    struct.pack_into("<%dh" % (n * per * 4), buf, p, *[x for v in verts for e in v for x in e])


def set_texture(buf, data, tex0):
    """Point every group at the texture of tex0 = (word 0, word 1) of a TEX0 from the same character."""
    for t, _, _ in parse(data):
        if t is not None:
            struct.pack_into("<II", buf, t, *tex0)


def texture(data):
    """(word 0, word 1) of the first group's TEX0."""
    t = parse(data)[0][0]
    return struct.unpack_from("<II", data, t)


def strips(batches, quads, pad):
    """Vertex lists for batches of the given sizes, holding quads (each 4 vertices: counter-clockwise corners seen
    from the outside) as strips joined by zero-area triangles; a 3-vertex batch takes half a quad. Batches left over
    get pad (a vertex) everywhere, so they draw nothing. Returns (lists, quads that did not fit)."""
    todo = [(q[0], q[1], q[3], q[2]) for q in quads]  # strip order: p0 p1 p3 p2
    half = None
    out = []
    for n in batches:
        vs = []
        if half is not None:  # a lone triangle, the rest of the batch repeats its last vertex
            vs, half = list(half), None
        elif n == 3 and todo:
            a, b, c, d = todo.pop(0)
            vs, half = [a, b, c], (c, b, d)  # the strip's second triangle, turned to even winding
        else:
            while todo and len(vs) + (4 if not vs else 6) <= n:
                q = todo.pop(0)
                vs += ([vs[-1], q[0]] if vs else []) + list(q)
        vs += [vs[-1] if vs else pad] * (n - len(vs))
        out.append(vs)
    return out, len(todo) + (half is not None)
