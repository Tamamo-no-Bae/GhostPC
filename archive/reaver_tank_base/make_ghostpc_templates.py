# make_ghostpc_templates.py -- write game/Misc/Templates/ghostpc.not, GhostPC's own object templates (the original
# Global.not stays untouched). GhostPC opens it at startup next to "global" (vehicles.cpp: gameTemplate_OpenFile), and
# levels may place its templates with nod_template_archive "ghostpc.not".
#
# Format (as Global.not, see notdump.py): u32 'NOT\x10', u32 1, u32 n, n x (name[32], u32 record offset); record:
# u32 count, count x (u32 modifier id, 32-byte value: string, int, float or 3 floats).
import struct, os, sys
H = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(H, '..', 'game', 'Misc', 'Templates', 'ghostpc.not')

MODS = {}   # name -> (id, type): 0 string, 2 float, 3 int, 4 vector
for l in open(os.path.join(H, '..', 're', 'modifiers.txt')):
    s = l.split()
    MODS[' '.join(s[2:-2])] = (int(s[0]), int(s[-1][3:]))

# The Reaver: the Siege Tank's player template (Arclite_player; GhostPC gives reaver_vehicle_player its cTankControl) with the
# Reaver's model and motion set (reaver_prop's; stand / walk / turnleft / turnright / recoilstand). vehicles.cpp turns
# it into a slow crawler that launches Scarabs (no siege mode, no turret, no shells).
# The Scarab: proj_scarab (a copy of the photon cannon's shot: the tossTurretProj glow) slowed down, homing
# (PROJECT_FLAGS as the Goliath's missiles; 0x100 lets the shooter set the target, +0x37C) and hitting for 1200, with a trail of fading smaller glows
# (scarabTrail, Misc/ghostpc.nfx).
TEMPLATES = [
    ("reaver_vehicle_player", [
        ("basedon", "Arclite_player"),
        ("DESCRIPTIONID", "REAVER"),
        ("model", "reaver.nod"),
        ("motionset", "reaver.nms"),
        ("foley", "ptssDragoon"),   # the Dragoon's sounds (no Reaver bank in the build)
        ("MODELSCALE", 0.75),       # 9 x 12.75 x 8.5 m (native, reaver_prop's: 12 x 17 x 11 m); vehicles.cpp REAVER_SCALE
        ("radius", 4.5),
        ("height", 3.75),
        ("EFFECT1", "none"),
        ("EFFECT3", "none"),
        ("ACTOR_HEALTH", 600),
        ("ACTOR_MAXHEALTH", 600),
    ]),
    ("proj_reaverScarab", [
        ("basedon", "proj_scarab"),
        ("TIMER", 6000),
        ("PROJECT_FLAGS", 15 | 0x100 | 0x1000 | 0x4000000),   # the Goliath missile's flags (0x100: homing)
        ("PROJECT_DAMAGE", 1200),
        ("PROJECT_MAXDIST", 60.0),
        ("PHYSICS_VELOCITY", (0.0, 12.0, 0.0)),
        ("EFFECT1", "tossTurretProj"),
        ("EFFECT2", "scarabTrail"),
        ("EXPLOSION", "exp_reaverScarab"),
    ]),
    # the Scarab's explosion: the photon cannon shot's (exp_protTurretProj) with the game's own Scarab explosion sound,
    # expPtssScarab (Sounds/expPtssScarab.xsb, cue createnoblock = protoss.xwb wave 0xAA, 2.2 s; no template used it)
    ("exp_reaverScarab", [
        ("basedon", "exp_protTurretProj"),
        ("foley", "expPtssScarab"),
    ]),
]


def value(t, v):
    if t == 0:
        b = v.encode('latin1')
        assert len(b) < 32, v
        return b.ljust(32, b'\0')
    if t == 2: return struct.pack('<f', v).ljust(32, b'\0')
    if t == 3: return struct.pack('<i', v).ljust(32, b'\0')
    if t == 4: return struct.pack('<3f', *v).ljust(32, b'\0')
    raise ValueError(t)


def build():
    n = len(TEMPLATES)
    head = 12 + 36 * n
    recs, index, off = b'', b'', head
    for name, mods in TEMPLATES:
        r = struct.pack('<I', len(mods))
        for m, v in mods:
            mid, t = MODS[m]
            r += struct.pack('<I', mid) + value(t, v)
        r += struct.pack('<I', 1) + bytes(1)   # editor data: empty
        index += name.encode('latin1').ljust(32, b'\0') + struct.pack('<I', off)
        recs += r
        off += len(r)
    return b'NOT\x10' + struct.pack('<II', 1, n) + index + recs


if __name__ == '__main__':
    d = build()
    open(OUT, 'wb').write(d)
    print('wrote %s (%d templates, %d bytes)' % (os.path.normpath(OUT), len(TEMPLATES), len(d)))
