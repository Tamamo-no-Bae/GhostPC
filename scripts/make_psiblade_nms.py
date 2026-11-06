# make_psiblade_nms.py -- writes game\3D\Motions\nova_psiblade.nms: Nova's motion set (nova.nms, which stays) with her
# melee swapped for the Psi Blade's, the old-skeleton "K" set converted by convert_nova_k_anims.py (nova43_K_*.nad).
# GhostPC switches a player to it when she picks up the Psi Blade (src/psiblade.cpp).
#
# nova.nms: "mode  file.nad  [flags]" lines (a mode listed twice is picked between, as the shock glove's left / right
# jab), then overlays (crouch pistol, rifle, grenade) with their own entries. Swapped in the base section:
#   attack2 / attack3 (added; attack1, the jabs, stays) -> K_attack1_1 (wrist swipe) / K_attack1_3 (spin), hitting through
#                                        cPlayerControl::MeleeCallback's tags 0x1F / 0x20)
# (gunmelee, the melee with a gun, stays Nova's own: user, the blade swings only with the Psi Blade weapon.) Crouched
# melee (the kick) stays.
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
MOTIONS = os.path.join(os.environ.get("GHOST_GAME_DIR", os.path.join(ROOT, "game")), "3D", "Motions")

# (user: no kick. attack1_1 is the wrist swipe, attack1_2 the kick, attack1_3 the spin, user; my reading of the keys
# had the first two swapped)
COMBO = ["nova43_K_attack1_1.nad", "nova43_K_attack1_3.nad"]
# The blade is its own weapon, forked from melee (user): attack1 stays Nova's jabs; the blade's swings are modes
# attack2 / attack3 (unused by nova.nms), which psiblade.cpp plays in place of attack1 while the blade is chosen
BLADE_MODES = ["attack2", "attack3"]
# The plunge (psiblade.cpp, user: a mid-air attack): the game's unused motion modes attackdown (0x1F) and
# attackdownland (0x20), named for these two
# (attackdown: the dive from frame 10, in place; trim_from writes it)
PLUNGE = [("attackdown", "nova43_K_JumpDownAttack_air.nad", "endpause"), ("attackdownland", "nova43_K_JumpDownAttack_land_strike.nad", "endpause")]
THRUST = "nova43_K_Thrustattack.nad"
# Stealth kills (user: unlocked with the Psi Blade): cPlayerControl::SilentStun (the action button behind an unaware
# enemy) plays mode silentkill, silentkill2 or silentkill3, which nova.nms only has commented out (so Nova never does
# them). Her cut one, nova_M_shockpunch, hits on tag 0x1F (MeleeHitCheck) at frame 26; the K thrusts only carry the
# 0x18 / 0x19 window tags, so copies get a 0x1F hit inside their window.
TAKEDOWNS = [("silentkill", "nova43_K_Thrustattack.nad", "nova43_K_ThrustKill.nad", 10.0),
             ("silentkill2", "nova43_K_SThrustattack.nad", "nova43_K_SThrustKill.nad", 8.0),
             ("silentkill3", "nova43_K_Thrustattack.nad", "nova43_K_ThrustKill.nad", 10.0)]
ANIMS = os.path.join(os.path.dirname(MOTIONS), "Anims")


def trim_from(src, dst, start, still_root=True):
    """a copy of an animation from frame <start> on (keys before it dropped, the last one before it moved to 0; times
    shifted), its root (bone 0) translation zeroed: the plunge's dive (user: Nova "dived up" -- JumpDownAttack is a
    0.93 m leap over frames 0-10, then the root plunges 8.9 m by frame 22; in the air physics moves her, so only the
    strike from frame 10 on, in place). .nad v4: u32 4, u32 tracks, u32 flags, f32 length; per track u32 keys, bone,
    type (1 = translation), keys of 52 bytes (time + 12 floats); then the event tags"""
    import struct
    d = open(os.path.join(ANIMS, src), "rb").read()
    ver, n, flags, length = struct.unpack_from("<IIIf", d, 0)
    o, out = 16, []
    for _ in range(n):
        k, bone, typ = struct.unpack_from("<III", d, o)
        keys = [list(struct.unpack_from("<13f", d, o + 12 + 52 * i)) for i in range(k)]
        o += 12 + 52 * k
        before = [q for q in keys if q[0] <= start]
        kept = ([before[-1]] if before else []) + [q for q in keys if q[0] > start]
        for q in kept:
            q[0] = max(0.0, q[0] - start)
            if still_root and bone == 0 and typ == 1:
                q[1:] = [0.0] * 12
        if typ == 0 and kept:   # (angles brought into -180..180 by whole turns: the landing's hips were at -340 to -399
            # degrees after the somersault, and she showed turned around, user; the terms after the angles are tangents)
            for c in range(1, 4):
                shift = -360.0 * round(kept[0][c] / 360.0)
                for q in kept:
                    q[c] += shift
        out.append(struct.pack("<III", len(kept), bone, typ) + b"".join(struct.pack("<13f", *q) for q in kept))
    tags = d[o:]   # (no event tags in these: copied as they are)
    open(os.path.join(ANIMS, dst), "wb").write(struct.pack("<IIIf", ver, n, flags, length - start) + b"".join(out) + tags)


def add_hit_tag(src, dst, frame, tag=0x1F):
    """a copy of an animation with one more event tag (.nad v4: tracks, then u32 count, (f32 frame, u32 tag)..., u32 0)"""
    import struct
    d = open(os.path.join(ANIMS, src), "rb").read()
    o = 16
    for _ in range(struct.unpack_from("<I", d, 4)[0]):
        o += 12 + 52 * struct.unpack_from("<I", d, o)[0]
    n = struct.unpack_from("<I", d, o)[0]
    tags = sorted([struct.unpack_from("<fI", d, o + 4 + 8 * i) for i in range(n)] + [(frame, tag)])
    out = d[:o] + struct.pack("<I", len(tags)) + b"".join(struct.pack("<fI", f, t) for f, t in tags) + bytes(4)
    open(os.path.join(ANIMS, dst), "wb").write(out)


def main():
    trim_from("nova43_K_JumpDownAttack.nad", "nova43_K_JumpDownAttack_air.nad", 10.0)
    # (the landing from frame 14: its first 14 frames are a somersault from the dive's head-down pose back upright, hips
    # pitched -116 to -340 degrees and lifted 1 m; on the ground it looked like she turned around, user)
    trim_from("nova43_K_JumpDownAttack_land.nad", "nova43_K_JumpDownAttack_land_strike.nad", 14.0)
    lines = open(os.path.join(MOTIONS, "nova.nms"), encoding="latin1", newline="").read().split("\r\n")
    out, overlay, did_attack, swaps = [], None, False, []
    for ln in lines:
        m = re.match(r"\s*overlay\s+(\S+)", ln)
        if m:
            overlay = m.group(1)
        mode = re.match(r"\s*(\w+)\s+(\S+\.nad)(.*)$", ln, re.I)
        if mode and overlay is None and mode.group(1) == "attack1":
            out.append(ln)   # (Nova's own jabs stay)
            if not did_attack:   # the blade's swings after them
                out += ["%s\t\t\t\t%s\t\tendpause" % (m, c) for m, c in zip(BLADE_MODES, COMBO)]
                out += ["%s\t\t\t%s\t\t%s" % m for m in PLUNGE]
                did_attack = True
                swaps.append(", ".join("%s = %s" % mc for mc in zip(BLADE_MODES, COMBO)))
            continue
        if False and mode and mode.group(1) == "gunmelee" and overlay in (None, "StandRifle"):   # (user: melee with a gun
            # stays the normal one; only the Psi Blade weapon swings the blade)
            out.append("gunmelee\t\t\t\t%s\t\t\tnoloop" % THRUST)
            swaps.append("gunmelee (%s) %s -> %s" % (overlay or "base", mode.group(2), THRUST))
            continue
        out.append(ln)
        if ln.strip().startswith(";silentkill") and overlay is None and not any(o.startswith("silentkill	") for o in out):
            for mode, src, dst, frame in TAKEDOWNS:   # (after the base section's commented-out silentkill line)
                add_hit_tag(src, dst, frame)
                out.append("%s			%s				noloop" % (mode, dst))
            swaps.append("silentkill / 2 / 3 -> " + ", ".join(t[2] for t in TAKEDOWNS))
    head = ["; nova_psiblade.nms -- GhostPC: nova.nms with the Psi Blade's melee (scripts/make_psiblade_nms.py)"]
    dst = os.path.join(MOTIONS, "nova_psiblade.nms")
    open(dst, "w", encoding="latin1", newline="").write("\r\n".join(head + out))
    print("wrote %s:\n  %s" % (dst, "\n  ".join(swaps)))


if __name__ == "__main__":
    sys.exit(main())
