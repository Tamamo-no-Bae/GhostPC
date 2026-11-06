# convert_nova_dt.py [out] -- nova_DT.nod (Nova in Dark Templar armour, the old 32-bone skeleton) converted to the
# current Nova's 43-bone skeleton, so it can play nova.nms: writes game\3D\Models\nova_DT43.nod (the original stays).
#
# NOD layout used (header 92 bytes: byte 4 shader count, 5 bone count, 6 vertex sets, 7 mesh groups):
#   shader names 32 bytes each, bones 64 bytes each (parent at +62), vertex data, indices, mesh groups 56 bytes each
#   (cMeshGroup::ReadFile: bone palette aBones[20] at +0x21, numBones +0x35; vertices index the palette), then a
#   marker count and 52-byte markers (bone, type, uint16 index, 4x3 matrix), then 4 zero bytes.
# The old skeleton is the new one without the face bones: spine and head (0-6), arms and legs have the same rest
# positions (checked: identical inverse-root translations); the 7-bone ponytail (7-13) maps onto the new 4-bone
# one (9-12). The bones and markers come from nova.nod (it has the weapon and effect markers the player uses; DT
# has 8 of them), the mesh groups' palettes are renumbered.
import os
import struct
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
MODELS = os.path.join(os.environ.get("GHOST_GAME_DIR", os.path.join(ROOT, "game")), "3D", "Models")
# GhostPC's markers on top of nova.nod's: (bone, type, index, 3x3 axes as rows X, Y, Z, position), in the bone's frame.
# The Psi Blade (src/psiblade.cpp, ghostpc.nfx psibladeNova, markertype 58): the right gauntlet's wrist end (the forearm,
# bone 31: elbow at model x -0.41, hand at -0.69; the gauntlet bulges about 9 cm below the arm at x -0.52 to -0.60),
# its Z (the beam's direction) along the arm toward the hand, so the blade points down past the hand when it hangs.
# User's mockup: out of the wristband's lower edge (user: not its middle, which -0.20 was; -0.27 still a bit high, -0.32 cleared the band) on the outer side of the hand (the arm's top in the rest pose, palms
# down: +z), past the hand, tilted 10 degrees outward (X and Z turned about Y).
EXTRA_MARKERS = [(31, 58, 0, [(0.1736, 0, 0.9848), (0, 1, 0), (-0.9848, 0, 0.1736)], (-0.29, 0.0, 0.045))]
OLD_TO_NEW = {0: 0, 1: 1, 2: 2, 3: 3, 4: 4, 5: 5, 6: 6, 7: 9, 8: 10, 9: 11, 10: 12, 11: 12, 12: 12, 13: 12,
              14: 25, 15: 26, 16: 27, 17: 28, 18: 29, 19: 30, 20: 31, 21: 32, 22: 33, 23: 34, 24: 35, 25: 36, 26: 37,
              27: 38, 28: 39, 29: 40, 30: 41, 31: 42}


def layout(d):
    sc, bc, vsc, mgc = d[4], d[5], d[6], d[7]
    bones = 92 + 32 * sc

    def group_ok(o):
        si, nb, vs = struct.unpack_from("<I", d, o)[0], d[o + 0x35], d[o + 0x36]
        return si < sc and 0 < nb <= 20 and vs < vsc and all(b < bc for b in d[o + 0x21:o + 0x21 + nb])
    # the mesh groups: the run of mgc valid records whose markers then end the file exactly
    for g in range(bones + 64 * bc, len(d) - 56 * mgc):
        if all(group_ok(g + 56 * i) for i in range(mgc)):
            m = g + 56 * mgc
            n = struct.unpack_from("<I", d, m)[0]
            if m + 4 + 52 * n + 4 == len(d):
                return dict(sc=sc, bc=bc, mgc=mgc, bones=bones, groups=g, markers=m, nmarkers=n)
    raise SystemExit("layout not recognised")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(MODELS, "nova_DT43.nod")
    old = open(os.path.join(MODELS, "nova_DT.nod"), "rb").read()
    new = open(os.path.join(MODELS, "nova.nod"), "rb").read()
    lo, ln = layout(old), layout(new)
    assert lo["bc"] == 32 and ln["bc"] == 43, "unexpected skeletons"
    head = bytearray(old[:lo["bones"]])
    head[5] = 43
    bones = new[ln["bones"]:ln["bones"] + 64 * 43]
    body = old[lo["bones"] + 64 * 32:lo["groups"]]          # vertex data and indices, unchanged
    groups = bytearray(old[lo["groups"]:lo["markers"]])
    for g in range(lo["mgc"]):
        o = 56 * g
        for k in range(groups[o + 0x35]):
            groups[o + 0x21 + k] = OLD_TO_NEW[groups[o + 0x21 + k]]
    markers = bytearray(new[ln["markers"]:-4])             # count, Nova's 39 markers (the end added below)
    for bone, typ, idx, axes, pos in EXTRA_MARKERS:
        markers += struct.pack("<BBH12f", bone, typ, idx, *[c for row in axes for c in row], *pos)
    struct.pack_into("<I", markers, 0, ln["nmarkers"] + len(EXTRA_MARKERS))
    markers += bytes(4)
    open(out, "wb").write(bytes(head) + bones + body + bytes(groups) + markers)
    print("wrote %s: %d bytes (%d mesh groups renumbered, %d markers from nova.nod + %d GhostPC)" % (
        out, os.path.getsize(out), lo["mgc"], ln["nmarkers"], len(EXTRA_MARKERS)))


if __name__ == "__main__":
    main()
