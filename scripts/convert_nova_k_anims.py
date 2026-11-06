# convert_nova_k_anims.py -- the old-skeleton "K" melee animations (nova_K_*.nad, 32 bones: Nova in Dark Templar armour,
# the Psi Blade's) converted to the current 43-bone Nova skeleton: writes game\3D\Anims\nova43_K_*.nad next to the
# originals (which stay). Use with nova_DT43.nod (scripts/convert_nova_dt.py) or nova.nod.
#
# .nad v4 (tRenderKeyTrack::LoadData): u32 version 4, u32 track count, u32 flags, f32 length (frames, 30/s); per track
# u32 key count, u32 bone, u32 type (0 rotation, 1 translation), then 52-byte keys (time + 12 floats); then the event
# tags: u32 count, count x (f32 time, u32 eRenderKeyTag), u32 0.
#
# The keys are relative to the bone's parent, and under convert_nova_dt's OLD_TO_NEW every old bone keeps its parent
# (spine/head 0-6, arms and legs 14-31 -> 25-42, identical rest positions), except the ponytail: the old 7-bone one
# (7-13) is the new 4-bone one (9-12) plus three more, so old 7-10 drive new 9-12 and old 11-13's tracks are dropped.
# The new face bones (7, 8, 13-24) get no tracks and keep their rest pose.
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from convert_nova_dt import ROOT, OLD_TO_NEW   # noqa: E402

ANIMS = os.path.join(os.environ.get("GHOST_GAME_DIR", os.path.join(ROOT, "game")), "3D", "Anims")
DROP = {11, 12, 13}   # the old ponytail's last three bones (no new bone of their own)
K_ANIMS = ["attack1_1", "attack1_2", "attack1_3", "uppercut", "Suppercut", "Thrustattack", "SThrustattack",
           "SChargeup", "SChargeDown", "JumpDownAttack", "JumpDownAttack_land", "ThrowAttack"]


def convert(src, dst):
    d = open(src, "rb").read()
    ver, n, flags = struct.unpack_from("<III", d, 0)
    assert ver == 4, "%s: version %d" % (src, ver)
    o, tracks, dropped = 16, [], 0
    for _ in range(n):
        k, bone, typ = struct.unpack_from("<III", d, o)
        assert bone < 32, "%s: bone %d is not on the old skeleton" % (src, bone)
        keys = d[o + 12:o + 12 + 52 * k]
        o += 12 + 52 * k
        if bone in DROP:
            dropped += 1
            continue
        tracks.append(struct.pack("<III", k, OLD_TO_NEW[bone], typ) + keys)
    tags = d[o:]
    nt = struct.unpack_from("<I", tags, 0)[0]
    assert len(tags) == 4 + 8 * nt + 4, "%s: unexpected tail (%d bytes, %d tags)" % (src, len(tags), nt)
    out = d[:4] + struct.pack("<I", len(tracks)) + d[8:16] + b"".join(tracks) + tags
    open(dst, "wb").write(out)
    return n, len(tracks), dropped, nt


def main():
    for name in K_ANIMS:
        src = os.path.join(ANIMS, "nova_K_%s.nad" % name)
        dst = os.path.join(ANIMS, "nova43_K_%s.nad" % name)
        n, kept, dropped, nt = convert(src, dst)
        print("%-28s %2d tracks -> %2d (%d ponytail dropped), %d event tags" % (os.path.basename(dst), n, kept, dropped, nt))


if __name__ == "__main__":
    main()
