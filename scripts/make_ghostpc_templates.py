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
        ("ACTOR_SHIELD", 120),      # (user's balance; Ghost's damage scale differs from SC1)
        ("EFFECT4", "ProtBuildDam"),   # damaged: the Protoss blue burn (psiburn, as the pylon / Scout), not the tank's fire
        # AI can target it (user: enemies ignored Nova in the Reaver): the player vehicles' ACTOR_FLAGS 1536 (0x600)
        # include 0x400, which cAIInstance::IsValidTarget refuses on a type-7 thing; 512 keeps the 0x200
        ("ACTOR_FLAGS", 512),
    ]),
    # The Dragoon (an existing unit, user: stats from it; the Goliath's walker control as its base: GhostPC gives
    # dragoon_vehicle_player cGoliathControl, vehicles.cpp swaps its guns for the Dragoon's own shot, proj_dragoon)
    ("dragoon_vehicle_player", [
        ("basedon", "Goliath_player"),
        ("DESCRIPTIONID", "DRAGOON"),
        ("model", "dragoon.nod"),
        ("motionset", "Dragoon.nms"),
        ("foley", "ptssDragoon"),
        ("radius", 1.6),
        ("height", 2.6),
        ("EFFECT1", "none"),       # (the Goliath's spotlight and gun effects)
        ("EFFECT2", "none"),
        ("ACTOR_HEALTH", 240),     # (user's balance; the unit has 100 / 80)
        ("ACTOR_MAXHEALTH", 240),
        ("ACTOR_SHIELD", 120),
        ("ARMOR_NORMAL", 0.4),
        ("ARMOR_FALL", 0.6),
        ("ARMOR_PSI", 0.1),
        ("ARMOR_FIRE", 0.6),
        ("ARMOR_ELECTRIC", 0.6),
        ("ARMOR_ACID", 0.8),
        ("PHYSICS_MASS", 500.0),
        ("EFFECT4", "ProtBuildDam"),   # damaged: the Protoss blue burn (psiburn), not the Goliath's fire
        ("ACTOR_FLAGS", 512),          # AI can target it (the Reaver's note)
    ]),
    # the Zoo's prop Reaver at the vehicle Reaver's size (user; a map placement cannot override MODELSCALE: MAPtoNSD
    # rejects nod_prop_143)
    ("reaver_prop_small", [
        ("basedon", "reaver_prop"),
        ("MODELSCALE", 0.75),
        ("radius", 2.85),
        ("height", 3.75),
    ]),
    # the hellfire grenade pickup: cPowerupCanister::GetItemType gives item 3 (hellfire) to a thing named
    # "powHellfire", but Global.not's pickup is powCanisterHellfire (it gave nothing): one with the name it checks
    ("powHellfire", [
        ("basedon", "powCanisterHellfire"),
    ]),
    # the flamethrower itself (user): its ammo pickup under its own name; vehicles.cpp FlameAmmoActivate makes the
    # weapon selectable only from a pickup with this name (powFThrower stays flame ammo only)
    ("powFlamethrower", [
        ("basedon", "powFThrower"),
        # the rifle on the ground (user: as the spider mine pickup shows a spider mine), make_flame_pickup.py
        ("model", "PU_FlameRifle.nod"),
        # lying still (user: it spun, PU_generic.nms turns pickups): a one-frame stand, make_flame_pickup.py
        ("motionset", "PU_still.nms"),
        ("DESCRIPTIONID", "POWFLAMETHROWER"),   # "Flamethrower" (add_pickup_strings.py)
    ]),
    # the Psi Blade (user): Tanagazj drops it when he dies (src/psiblade.cpp); picking it up turns Nova into NovaDT with
    # the blade and its animations. The flame ammo pickup's script (cPowerupFlameAmmo, whose Activate vehicles.cpp
    # detours: this name gives the blade instead of ammo) with a Protoss power crystal (about 1.1 m) as its model
    ("powPsiBlade", [
        ("basedon", "powFThrower"),
        ("model", "PowerCrystal_01.nod"),
        ("MODELSCALE", 0.6),
        ("DESCRIPTIONID", "POWPSIBLADE"),   # "Psi Blade" (add_pickup_strings.py)
    ]),
    ("proj_reaverScarab", [
        ("basedon", "proj_scarab"),
        ("TIMER", 6000),
        ("PROJECT_FLAGS", 15 | 0x100 | 0x1000 | 0x4000000),   # the Goliath missile's flags (0x100: homing)
        ("PROJECT_DAMAGE", 1200),
        ("PROJECT_MAXDIST", 60.0),
        ("PHYSICS_VELOCITY", (0.0, 12.0, 0.0)),
        ("EFFECT1", "reaverScarabGlow"),   # tossTurretProj's glow, larger (Misc/ghostpc.nfx)
        ("EFFECT2", "scarabTrail"),
        ("EXPLOSION", "exp_reaverScarab"),
    ]),
    # the Scarab's explosion: the photon cannon shot's (exp_protTurretProj) with the game's own Scarab explosion sound,
    # expPtssScarab (Sounds/expPtssScarab.xsb, cue createnoblock = protoss.xwb wave 0xAA, 2.2 s; no template used it)
    ("exp_reaverScarab", [
        ("basedon", "exp_protTurretProj"),
        ("foley", "expPtssScarab"),
    ]),
    # The Vulture's grenade impact (user: StarCraft's tvuhit00-02): proj_vultureGun / proj_vultureGunAI name EXPLOSION
    # vultureShotExp / vultureShotExpAI, which are not templates but game.nfx effect controllers (vulShotExpDam, the
    # splash damage, and vultureShotExp0, the fireball): with no template of that name the game made the effect, which
    # has no sound. These templates take the name (a template is looked up first) and carry the same effect as EFFECT1,
    # so the fireball and damage stay; their creation plays "create", Sounds\customultureShotExp(AI)\create~n.wav
    # (customsound.cpp, by template name). (Bare explosions without the effect lost both, user.)
    ("vultureShotExp", [
        ("basedon", "_explosion"),
        ("EFFECT1", "vultureShotExp"),
    ]),
    ("vultureShotExpAI", [
        ("basedon", "_explosion"),
        ("EFFECT1", "vultureShotExpAI"),
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
