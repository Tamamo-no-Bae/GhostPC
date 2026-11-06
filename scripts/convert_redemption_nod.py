# convert_redemption_nod.py <in.nod> <out.nod> [scale] -- a Vampire: The Masquerade - Redemption model (.nod version 7,
# Nihilistic's earlier engine) rewritten as Ghost's .nod version 10 (renderModel_Load refuses versions below 10).
# Made for the Szlachta (Global.not testEnemy_old: szlachta.nod), from the freely distributed Redemption demo.
#
# Version 7 (Szlachta.nod, worked out from the file):
#   u32 version 7, u32 material count, material names (32 bytes each);
#   u16 bones, u16 mesh parts, u32 vertices, u32 triangles (all groups), u16 groups, u16 ?, u16 0;
#   6 floats bounds (min xyz, max xyz);
#   bones, 66 bytes each: local position (3 f), local rotation (3x3 f), inverse bind position (3 f), int16 next
#     sibling, first child, parent (-1 none) -- Ghost's bone record with byte links;
#   mesh part names, 32 bytes each;
#   vertices, 40 bytes each: position, normal, uv, f32 weight (1.0), u32 bone (one bone per vertex);
#   u16 per-group vertex maps (one entry per vertex; not needed), u16 triangle lists (3 per triangle, group by group,
#     indices local to the group's vertices, which are consecutive in group order), 8 spare u16;
#   groups, 28 bytes each (the last cut short by the end of the file): u16 triangles, vertices, ?; flags; byte +9 part;
#     byte +12 material.
# Version 10 (Ghost; see nod_uvmap.py / convert_nova_dt.py): a 0x5C header (version, byte counts: materials, bones,
# vertex sets, groups; bounds at +0xC; vertex sets (type, count) at +0x24; index count at +0x44), material names,
# 64-byte bones, vertices by set (type 1: 36 bytes, position, normal, uv, u32 bone as an index into the group's
# palette), u16 indices group by group and LOD by LOD, 56-byte groups (u32 material; 4 LODs of (strip, list, vertex
# count) and the group's vertex count; +0x20 part; +0x21 a palette of up to 20 bones, +0x35 its size, +0x36 vertex
# set), then u32 marker count, markers, u32 0.
# Redemption units: Christof, a grown man, is 128 tall: one unit is about 1.4 cm (scale 0.014 by default).
import struct
import sys

PALETTE = 20


def read_v7(d):
    ver, nmat = struct.unpack_from("<II", d, 0)
    assert ver == 7, "not a version 7 model (%d)" % ver
    mats = [d[8 + 32 * i:8 + 32 * i + 32].split(b"\0")[0].decode("latin1") for i in range(nmat)]
    o = 8 + 32 * nmat
    nb, nparts, nv, ntri, ngroups = struct.unpack_from("<HHIIH", d, o)
    o += 18
    bounds = struct.unpack_from("<6f", d, o)
    o += 24
    bones = []
    for b in range(nb):
        pos = struct.unpack_from("<3f", d, o)
        rot = struct.unpack_from("<9f", d, o + 12)
        inv = struct.unpack_from("<3f", d, o + 48)
        sib, child, parent = struct.unpack_from("<3h", d, o + 60)
        bones.append((pos, rot, inv, sib, child, parent))
        o += 66
    parts = [d[o + 32 * i:o + 32 * i + 32].split(b"\0")[0].decode("latin1") for i in range(nparts)]
    o += 32 * nparts
    verts = []
    for v in range(nv):
        p = struct.unpack_from("<8f", d, o)
        w, bone = struct.unpack_from("<fI", d, o + 32)
        verts.append((p[0:3], p[3:6], p[6:8], bone))
        o += 40
    idx_start = o
    gend = len(d) - (28 * ngroups - 16)   # (the last record is 16 bytes short)
    groups = []
    for g in range(ngroups):
        r = d[gend + 28 * g:gend + 28 * g + 28].ljust(28, b"\0")
        tris, gv, _ = struct.unpack_from("<3H", r, 0)
        groups.append(dict(tris=tris, verts=gv, part=r[9], mat=r[12] if r[12] < nmat else 0))
    assert sum(g["verts"] for g in groups) == nv and sum(g["tris"] for g in groups) == ntri, "group counts do not match"
    o = idx_start + 2 * nv   # (the vertex maps)
    base = 0
    for g in groups:
        g["idx"] = [base + i for i in struct.unpack_from("<%dH" % (3 * g["tris"]), d, o)]
        g["base"] = base
        o += 6 * g["tris"]
        base += g["verts"]
    return mats, bones, parts, verts, groups, bounds


def split(tris, verts):
    """triangles (global vertex indices) into runs whose bones fit one palette"""
    out, cur, bones = [], [], set()
    for t in tris:
        tb = {verts[v][3] for v in t}
        if len(bones | tb) > PALETTE and cur:
            out.append(cur)
            cur, bones = [], set()
        cur.append(t)
        bones |= tb
    if cur:
        out.append(cur)
    return out


def write_v10(mats, bones, verts, groups, bounds, s):
    outg = []   # (material, part, palette, local vertex list (global ids), local indices)
    for g in groups:
        tris = [tuple(g["idx"][i:i + 3]) for i in range(0, len(g["idx"]), 3)]
        for run in split(tris, verts):
            used, local = [], {}
            for t in run:
                for v in t:
                    if v not in local:
                        local[v] = len(used)
                        used.append(v)
            pal = sorted({verts[v][3] for v in used})
            outg.append((g["mat"], g["part"], pal, used, [local[v] for t in run for v in t]))
    nverts = sum(len(g[3]) for g in outg)
    nidx = sum(4 * len(g[4]) for g in outg)
    head = bytearray(0x5c)
    struct.pack_into("<I", head, 0, 10)
    head[4], head[5], head[6], head[7] = len(mats), len(bones), 1, len(outg)
    struct.pack_into("<6f", head, 0xc, *[x * s for x in bounds])
    struct.pack_into("<II", head, 0x24, 1, nverts)
    struct.pack_into("<I", head, 0x44, nidx)
    body = bytearray(head)
    for m in mats:
        body += m.encode("latin1").ljust(32, b"\0")
    for pos, rot, inv, sib, child, parent in bones:
        body += struct.pack("<3f9f3f", *[x * s for x in pos], *rot, *[x * s for x in inv])
        body += struct.pack("<bbbb", sib, child, parent, 0)
    for mat, part, pal, used, idx in outg:
        for v in used:
            p, n, uv, bone = verts[v]
            # (V flipped: Redemption counts it from the other edge; a UV plot shows the islands on the texture's parts
            # only flipped)
            body += struct.pack("<8fI", *[x * s for x in p], *n, uv[0], 1.0 - uv[1], pal.index(bone))
    for mat, part, pal, used, idx in outg:
        for lod in range(4):
            body += struct.pack("<%dH" % len(idx), *idx)
    for mat, part, pal, used, idx in outg:
        r = bytearray(56)
        struct.pack_into("<I", r, 0, mat)
        struct.pack_into("<13H", r, 4, *([0, len(idx), len(used)] * 4), len(used))
        r[0x20] = part
        r[0x21:0x21 + len(pal)] = bytes(pal)
        r[0x35], r[0x36] = len(pal), 0
        body += r
    body += struct.pack("<II", 0, 0)   # (no markers)
    return bytes(body), outg


def main():
    src, dst = sys.argv[1], sys.argv[2]
    s = float(sys.argv[3]) if len(sys.argv) > 3 else 0.014
    mats, bones, parts, verts, groups, bounds = read_v7(open(src, "rb").read())
    data, outg = write_v10(mats, bones, verts, groups, bounds, s)
    open(dst, "wb").write(data)
    print("%s: materials %s, %d bones, parts %s, %d vertices, %d groups -> %d groups (bones per group %s), scale %g, %d bytes" % (
        dst, mats, len(bones), parts, len(verts), len(groups), len(outg), [len(g[2]) for g in outg], s, len(data)))


if __name__ == "__main__":
    main()
