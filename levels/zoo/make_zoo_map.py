"""Generates the Quake III MAP for the "Enemy Zoo Test map" (level 9_1_1a_Enemy_Zoo).

Layout (game units = metres, X east, Y north, Z up; north is up in the sketch):

    +-----------+
    |  Vulcan   |----+ L hallway
    |  room     |--+ |
    +-----------+--| |----+---+------------------+-----------+
    |                      | c |                  |           |
    |   Terran area        | o |   Zerg area      | Ultralisk |
    |   (bunker, turret)   | r |   (infested CC) door room    |
    |                    door r door              |           |
    +----------------------+ r +------------------+-----------+
    | hallway (Nova-sized, roofed)                            |
    +door-+----------------door---------------------------------+
    |spawn|                                                     |
    +-----+          Protoss area (Gateway, Nexus)            door Tanagazj
    |                                                           |  room
    +-----------------------------------------------------------+

Big rooms are open to the sky (sky ceiling at 45 m: the Nexus is 28.6 m tall and flyers cruise around 20-25 m) and
sized for their largest units and buildings. Corridors, hallway, doors and the spawn room are Nova-sized. Every big
room has its own waypoint grid with no links through doors, so units (including the human-sized Vulcan and Tanagazj)
stay in their rooms; the L-shaped hallway also keeps Vulcan's missiles out of the Terran area.

Run:  python make_zoo_map.py   ->  writes zoo.map next to this script and prints a summary.
"""
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "zoo.map")
# the game folder: GHOST_GAME_DIR, else ..\..\game (GhostPC workspace), else the parent folder (mapsrc in the
# prototype's folder, as the patch installs it)
GAME = os.environ.get("GHOST_GAME_DIR") or next(
    (g for g in (os.path.join(HERE, "..", "..", "game"), os.path.join(HERE, "..")) if os.path.exists(os.path.join(g, "Misc"))),
    os.path.join(HERE, ".."))
os.environ.setdefault("GHOST_GAME_DIR", GAME)   # for notdump
sys.path.insert(0, os.path.join(HERE, "..", "..", "scripts"))
sys.path.insert(0, HERE)   # mapsrc keeps notdump.py next to this script
import notdump  # noqa: E402  (Global.not reader)

SPAWN = (0, 102, 10, 110)
T = 1.0             # wall thickness
H = 45.0            # big-room wall height (open sky above)
CEIL = 5.0          # corridor / hallway / spawn room ceiling height
DOOR_W, DOOR_H = 1.5, 2.5   # Nova fits (1.8 m tall); small flyers (Observer 1.6 m, Scourge 2.2 m) do not

# interior extents (x0, y0, x1, y1)
PROTOSS = (0, 0, 200, 110)
TANAGAZJ = (201, 88, 225, 110)     # off the Protoss east wall, facing the Dark Templar row
TCORR_A = (201, 80, 213, 84)        # Tanagazj's L hallway: east from the Protoss room ...
TCORR_B = (209, 80, 213, 87)        # ... then north into his room (inside his trigger: he can't be shot from outside)
HALL = (0, 111, 90, 115)            # ends just past the Terran-Zerg corridor (CORR)
TERRAN = (0, 116, 80, 196)
CORR = (81, 111, 85, 196)
ZERG = (86, 116, 166, 256)          # extends north past the Ultralisk room
LURKER = (167, 197, 200, 256)       # Lurker boss room, north of the Ultralisk room, door in the Zerg east wall
# Ultralisk arena (the user's concept): a central room reached from the Zerg room by a long Nova-sized corridor, and on
# its north and south sides a Nova-sized loop corridor around a solid block, open to the room at both ends
UCORR = (167, 154, 206, 158)        # from the Zerg room's east door (y 156) east to the central room
ULTRA = (206, 140, 266, 200)        # central room
U_LOOPS = {"north": ((210, 201, 214, 229), (210, 225, 262, 229), (258, 201, 262, 229), (214, 201, 258, 225)),
           "south": ((210, 111, 214, 139), (210, 111, 262, 115), (258, 111, 262, 139), (214, 115, 258, 139))}
# (left leg, cross leg, right leg, block) per side; the legs open into the central room at x 212 and 260
VULCAN = (0, 197, 40, 267)
VCORR_A = (68, 197, 72, 210)        # L hallway: north from the Terran room ...
VCORR_B = (41, 206, 72, 210)        # ... then west to the Vulcan room
HAULER = (41, 211, 80, 245)         # Hauler boss room, door in the L hallway's north wall
# pillars to hide behind from Vulcan's and the Hauler's missiles (4 x 4 m, 12 m tall)
PILLARS = [(12, 222), (28, 222), (12, 247), (28, 247),     # Vulcan: he stands between them
           (52, 221), (68, 221), (52, 235), (68, 235)]     # Hauler: around him, the first row 10 m from his door
GOLIATH = (41, 246, 80, 267)        # Goliath boss (Goliath_232) arena, door in the sniper hallway's west wall
GOLIATH2 = (86, 257, 125, 278)
SQUAD = (0, 188, 12, 196)           # the marine squad's side room, inside the Terran room's north-west corner      # cut Goliath boss (Goliath_222c on cAIBossGoliath), door in the hallway's east wall
SCORR_A = (73, 201, 81, 205)        # sniper hallway: east from the Vulcan/Hauler L hallway ...
SCORR_B = (81, 201, 85, 267)        # ... then north between the Hauler and Zerg rooms to the arena's door
SLINK = (81, 197, 85, 200)          # its south end opens into the Terran-Zerg corridor too
# sniper boss arena, north of the Vulcan and Hauler rooms: his perches line its north side and he faces south over the
# courtyard, whose middle is the world origin; his sight (and line-of-sight ray) reaches 40 m
# (cAIInstance::GetCurrentSightDist, +0x330 set by cAIBossSniper::Clear), the perches are 21-29 m up
SNIPER = (0, 268, 85, 320)
SNIPER_ORIGIN = (42.5, 293.0)       # the arena point that is the world origin (see the sniper section)
BOUNDS = (-T, -T, 264, 321)

# The sniper boss's animations move him in world coordinates (tObject::MoveRootWorld puts his actor at the animation
# root's position as is), so his arena must sit at the world origin: the level is shifted to put SNIPER_ORIGIN there,
# with the floors 0.3 m below it. The game creates the player at the world origin (open floor in the arena's back
# strip) and the level script moves her into the spawn room ("player_start", src/levelscript.cpp). Spawning inside
# collision made the physics produce NaN on the first frame, so the origin must stay clear.
OFFSET = (-SNIPER_ORIGIN[0], -SNIPER_ORIGIN[1], -0.3)

# Surfaces name DDS textures directly: MAPtoNIL 0.6.7 rejects archived (.nsa) materials as "scripted sector shaders"
# and only accepts texture basenames that are not also material names in the shaderlist archives. The ceiling is
# compiled with SKY_PLACEHOLDER and renamed to the global sky material afterwards (install_zoo.py). The engine draws
# "sky" surfaces unlit with their own texture, so the ceiling becomes the top face of the clear Robota skybox
# (global 3dmaterials archive), stretched once across the level instead of tiled.
SKY_PLACEHOLDER, SKY_MATERIAL = "MS_Crater", "sky_Robota_02_Up"
SKY_SPAN = 240.0  # metres covered by one repeat of the sky texture
MAT = {
    "floor_protoss": "AR_FloorSwirly_blu_01", "wall_protoss": "AR_Plated_blu_gld_01",
    "floor_terran": "MS_Asphalt", "wall_terran": "AR_PillarPanel_brn_01",
    "floor_zerg": "GE_fleshA_gry", "wall_zerg": "GE_ribbedA_grn",
    "floor_corr": "AR_Bridge_Floor", "wall_corr": "AR_Edge_Plain",
    "sky": SKY_PLACEHOLDER,
}
TILE = 4.0   # metres per texture repeat


def dds_width(name):
    """texture width from its DDS header, searched like the game does (MaterialDirs.txt)"""
    for d in open(os.path.join(GAME, "MaterialDirs.txt"), encoding="latin1").read().split():
        f = os.path.join(GAME, *d.split(chr(92)), name + ".dds")
        if os.path.exists(f):
            with open(f, "rb") as h:
                hdr = h.read(20)
            return int.from_bytes(hdr[16:20], "little")
    raise SystemExit("texture not found: " + name)


TEX_SCALE = {m: TILE / dds_width(m) for m in MAT.values()}
TEX_SCALE[SKY_PLACEHOLDER] = SKY_SPAN / dds_width(SKY_PLACEHOLDER)   # same width as the sky texture (512)

# ---------------------------------------------------------------------------------------------------------------
# Geometry
brushes = []    # (x0, y0, z0, x1, y1, z1, material, texture rotation in degrees, (shift u, shift v) in texels)


def box(x0, y0, z0, x1, y1, z1, mat, rot=0, shift=(0, 0)):
    if x1 - x0 > 1e-6 and y1 - y0 > 1e-6 and z1 - z0 > 1e-6:
        brushes.append((x0, y0, z0, x1, y1, z1, mat, rot, shift))


WALL_LOG, PLACED = [], []   # for the overview (zoo_overview.py): every wall with its doors, every template placed


def wall(x0, y0, x1, y1, mat, height=H, doors=()):
    """Wall box (thin along one axis) from the floor up to height, with Nova-sized door gaps at the given centres
    along its long axis (a lintel closes each gap above DOOR_H)."""
    along_x = (x1 - x0) >= (y1 - y0)
    WALL_LOG.append((x0, y0, x1, y1, along_x, tuple(doors), height))
    lo, hi = (x0, x1) if along_x else (y0, y1)
    cur = lo
    for c in sorted(doors):
        a, b = c - DOOR_W / 2, c + DOOR_W / 2
        assert lo < a and b < hi, "door outside its wall"
        for s0, s1, z0 in ((cur, a, 0.0), (a, b, DOOR_H)):
            if along_x:
                box(s0, y0, z0, s1, y1, height, mat)
            else:
                box(x0, s0, z0, x1, s1, height, mat)
        cur = b
    if along_x:
        box(cur, y0, 0, hi, y1, height, mat)
    else:
        box(x0, cur, 0, x1, hi, height, mat)


def floor(r, mat, grow=T, rot=0):
    box(r[0] - grow, r[1] - grow, -1, r[2] + grow, r[3] + grow, 0, mat, rot)


def corridor_floor(r, mat, along_x, start=None, end=None):
    """Floor of a 4 m corridor with AR_Bridge_Floor's walkway strip (it runs along the texture's u axis, centred in v,
    one strip per TILE) running along the corridor and its tile edges on the corridor walls. Quake 3 floor projection:
    u = x, v = -y (texels = metres / TEX_SCALE) at rotation 0; at 90 degrees u = y, v = x. start/end: where the slab
    begins/ends along the corridor when another floor owns the junction (overlapping coplanar floors with different
    texture axes flicker)."""
    x0, y0, x1, y1 = r[0] - T, r[1] - T, r[2] + T, r[3] + T
    if along_x:
        x0, x1 = (x0 if start is None else start), (x1 if end is None else end)
    else:
        y0, y1 = (y0 if start is None else start), (y1 if end is None else end)
    per_m = 1.0 / TEX_SCALE[mat]
    width = dds_width(mat)
    if along_x:   # rotation 0: tile edge (v = 0 mod width) at the south wall y = r[1]
        rot, sv = 0, ((r[1] + OFFSET[1]) * per_m) % width
    else:         # rotation 90: tile edge at the west wall x = r[0]
        rot, sv = 90, (-(r[0] + OFFSET[0]) * per_m) % width
    box(x0, y0, -1, x1, y1, 0, mat, rot, (0, round(sv, 3)))


def roof(r):
    box(r[0], r[1], CEIL, r[2], r[3], CEIL + 1, MAT["wall_corr"])


DOOR_X = {"spawn": 5.0, "protoss": (CORR[0] + CORR[2]) / 2, "vulcan_corr": (VCORR_A[0] + VCORR_A[2]) / 2,
          "hauler": 55.0}
DOOR_Y = {"terran": 176.0, "zerg": 176.0, "ultra": 156.0, "tanagazj": (TCORR_A[1] + TCORR_A[3]) / 2,
          "vulcan": (VCORR_B[1] + VCORR_B[3]) / 2, "sniper_corr": (SCORR_A[1] + SCORR_A[3]) / 2, "lurker": 230.0,
          "goliath": (GOLIATH[1] + GOLIATH[3]) / 2, "goliath2": 262.0}
DOOR_X["tanagazj"] = (TCORR_B[0] + TCORR_B[2]) / 2
DOOR_X["sniper"] = (SCORR_B[0] + SCORR_B[2]) / 2

# floors (one slab per area so each gets its own material; they reach under the surrounding walls)
floor(PROTOSS, MAT["floor_protoss"])
floor(TANAGAZJ, MAT["floor_protoss"])
corridor_floor(TCORR_A, MAT["floor_corr"], along_x=True)            # owns the corner
corridor_floor(TCORR_B, MAT["floor_corr"], along_x=False, start=TCORR_A[3] + T)
corridor_floor(HALL, MAT["floor_corr"], along_x=True)              # owns the junction with CORR
corridor_floor(CORR, MAT["floor_corr"], along_x=False, start=HALL[3] + T)
floor(TERRAN, MAT["floor_terran"])
floor(VULCAN, MAT["floor_terran"])
corridor_floor(VCORR_A, MAT["floor_corr"], along_x=False, end=VCORR_B[1] - T)
corridor_floor(VCORR_B, MAT["floor_corr"], along_x=True)            # owns the corner with VCORR_A
floor(HAULER, MAT["floor_terran"])
floor(GOLIATH, MAT["floor_terran"])
floor(GOLIATH2, MAT["floor_terran"])
corridor_floor(UCORR, MAT["floor_corr"], along_x=True)
for leg_l, cross, leg_r, _ in U_LOOPS.values():
    corridor_floor(cross, MAT["floor_corr"], along_x=True)                          # owns both corners
    for leg in (leg_l, leg_r):
        north = leg[1] > ULTRA[1]
        corridor_floor(leg, MAT["floor_corr"], along_x=False,
                       start=None if north else cross[3] + T, end=cross[1] - T if north else None)
floor(ZERG, MAT["floor_zerg"])
floor(ULTRA, MAT["floor_zerg"])
floor(LURKER, MAT["floor_zerg"])
corridor_floor(SCORR_A, MAT["floor_corr"], along_x=True, end=SCORR_B[0] - T)
corridor_floor(SCORR_B, MAT["floor_corr"], along_x=False)          # owns the corner
corridor_floor(SLINK, MAT["floor_corr"], along_x=False, start=SLINK[1], end=SLINK[3])
floor(SNIPER, MAT["floor_terran"])

P, W, Z, C = MAT["wall_protoss"], MAT["wall_terran"], MAT["wall_zerg"], MAT["wall_corr"]
# outer walls
wall(-T, -T, 201, 0, P)                                                   # Protoss south
wall(-T, -T, 0, SNIPER[3] + T, P)                                        # west, whole level
wall(200, -T, 201, 197, P, doors=(DOOR_Y["tanagazj"], DOOR_Y["ultra"]))  # Protoss / hallway / old Ultralisk east
wall(200, TANAGAZJ[1] - T, 226, TANAGAZJ[1], P, doors=(DOOR_X["tanagazj"],))   # Tanagazj room south
wall(225, TANAGAZJ[1] - T, 226, 111, P)
# Tanagazj's L hallway
wall(200, TCORR_A[1] - T, TCORR_B[2] + T, TCORR_A[1], C, height=CEIL + 1)              # A south
wall(200, TCORR_A[3], TCORR_B[0], TCORR_A[3] + T, C, height=CEIL + 1)                  # A north
wall(TCORR_B[0] - T, TCORR_A[3], TCORR_B[0], TANAGAZJ[1] - T, C, height=CEIL + 1)      # B west
wall(TCORR_B[2], TCORR_A[1] - T, TCORR_B[2] + T, TANAGAZJ[1] - T, C, height=CEIL + 1)  # B east
# Protoss north wall = hallway south wall: doors into the spawn room and the Protoss area
wall(-T, 110, 226, 111, P, doors=(DOOR_X["spawn"], DOOR_X["protoss"]))   # also the Tanagazj room's north wall
wall(HALL[2], HALL[1] - T, HALL[2] + T, HALL[3] + T, C)                   # hallway east end
wall(10, 101, 11, 110, C, height=CEIL + 1)                                # spawn room (inside the Protoss area)
wall(-T, 101, 11, 102, C, height=CEIL + 1)
# hallway north wall (Terran, Zerg, Ultralisk south walls), leaving the corridor mouth open
wall(-T, 115, 81, 116, W)
wall(85, 115, 201, 116, Z)
# corridor between Terran and Zerg, with doors into both
wall(80, 116, 81, 197, W, doors=(DOOR_Y["terran"],))
wall(85, 116, 86, 197, Z, doors=(DOOR_Y["zerg"],))
wall(80, 196, 86, 197, C, doors=(DOOR_X["sniper"],))                    # door to the sniper hallway
wall(80, SLINK[1], 81, SLINK[3], C, height=CEIL + 1)
# Zerg / Ultralisk / Lurker
wall(85, 196, 86, ZERG[3] + T, Z)                                          # Zerg west (north part), sniper hallway east
wall(166, 116, 167, ZERG[3] + T, Z, doors=(DOOR_Y["ultra"], DOOR_Y["lurker"]))
wall(166, 196, 201, 197, Z)                                                # Ultralisk north = Lurker south
wall(200, 196, 201, LURKER[3] + T, Z)                                      # Lurker east
wall(85, ZERG[3], 201, ZERG[3] + T, Z)                                     # Zerg + Lurker north
wall(85, ZERG[3], 86, SNIPER[1], C, height=CEIL + 1, doors=(DOOR_Y["goliath2"],))   # sniper hallway east, north part
# the marine squad's room (open top, 4 m walls; north and west are the Terran room's walls)
wall(SQUAD[0] - T, SQUAD[1] - 0.5, SQUAD[2] + 0.5, SQUAD[1], W, height=4, doors=(4.0,))   # south, door at x 4
wall(SQUAD[2], SQUAD[1] - 0.5, SQUAD[2] + 0.5, SQUAD[3], W, height=4)                    # east
# second Goliath arena: south = the Zerg room's north wall, west = the hallway / sniper arena east walls
wall(85, GOLIATH2[3], GOLIATH2[2] + T, GOLIATH2[3] + T, W)                 # north
wall(GOLIATH2[2], ZERG[3], GOLIATH2[2] + T, GOLIATH2[3] + T, W)            # east
# Ultralisk arena: corridor from the Zerg room, central room, a loop corridor around a block on each side
wall(166, UCORR[1] - T, ULTRA[0], UCORR[1], C, height=CEIL + 1)
wall(166, UCORR[3], ULTRA[0], UCORR[3] + T, C, height=CEIL + 1)
loop_doors = ((U_LOOPS["north"][0][0] + U_LOOPS["north"][0][2]) / 2, (U_LOOPS["north"][2][0] + U_LOOPS["north"][2][2]) / 2)
wall(ULTRA[0] - T, ULTRA[1] - T, ULTRA[0], ULTRA[3] + T, Z, doors=(DOOR_Y["ultra"],))
wall(ULTRA[2], ULTRA[1] - T, ULTRA[2] + T, ULTRA[3] + T, Z)
wall(ULTRA[0] - T, ULTRA[1] - T, ULTRA[2] + T, ULTRA[1], Z, doors=loop_doors)
wall(ULTRA[0] - T, ULTRA[3], ULTRA[2] + T, ULTRA[3] + T, Z, doors=loop_doors)
for side, (leg_l, cross, leg_r, block) in U_LOOPS.items():
    y0, y1 = min(leg_l[1], cross[1]) - T, max(leg_l[3], cross[3]) + T
    wall(leg_l[0] - T, y0, leg_l[0], y1, C, height=CEIL + 1)                      # outer side walls
    wall(leg_r[2], y0, leg_r[2] + T, y1, C, height=CEIL + 1)
    if side == "north":
        wall(leg_l[0] - T, cross[3], leg_r[2] + T, cross[3] + T, C, height=CEIL + 1)   # outer cross wall
    else:
        wall(leg_l[0] - T, cross[1] - T, leg_r[2] + T, cross[1], C, height=CEIL + 1)
    box(block[0], block[1], 0, block[2], block[3], CEIL + 1, C)                   # the block the loop runs around
# Terran north wall with the door to the L hallway
wall(-T, 196, 81, 197, W, doors=(DOOR_X["vulcan_corr"],))
# Vulcan room and L hallway
wall(-T, SNIPER[1] - T, 86, SNIPER[1], W, doors=(DOOR_X["sniper"],))      # Vulcan + Hauler north = sniper arena south
wall(80, SCORR_A[3], 81, SNIPER[1], W, doors=(DOOR_Y["goliath"],))         # Hauler + Goliath east = sniper hallway west
wall(40, 196, 41, SNIPER[1], W, doors=(DOOR_Y["vulcan"],))
wall(40, HAULER[3], 81, HAULER[3] + T, W)                                  # Hauler room north
for x, y in PILLARS:
    box(x - 2, y - 2, 0, x + 2, y + 2, 12, W)
# cover from the Goliath's guns: low pillars between its door and the boss
GOLIATH_COVER = [(70, 251), (70, 262), (57, 250.5), (57, 262.5),
                 (91, 262), (91, 273)]   # second arena: two pillars by the door, the rest kept clear for the boss's runs
for x, y in GOLIATH_COVER:
    box(x - 1.25, y - 1.25, 0, x + 1.25, y + 1.25, 5, W)

wall(67, 196, 68, 206, C, height=CEIL + 1)
wall(72, 196, 73, 211, C, height=CEIL + 1, doors=(DOOR_Y["sniper_corr"],))
wall(41, 205, 68, 206, C, height=CEIL + 1)
wall(41, 210, 81, 211, W, doors=(DOOR_X["hauler"],))                     # L hallway north = Hauler room south
# sniper hallway (from the L hallway east, then north to the arena; the turn keeps his shots out) and arena
wall(SCORR_A[0], SCORR_A[1] - T, SCORR_B[2] + T, SCORR_A[1], C, height=CEIL + 1,
     doors=(DOOR_X["sniper"],))                                                          # A south / B south end
wall(SCORR_A[0], SCORR_A[3], SCORR_B[0], SCORR_A[3] + T, C, height=CEIL + 1)           # A north
wall(SNIPER[2], SNIPER[1], SNIPER[2] + T, SNIPER[3] + T, W)                              # arena east
wall(SNIPER[0] - T, SNIPER[3], SNIPER[2] + T, SNIPER[3] + T, W)                          # arena north
# The sniper boss (cAIBossSniper) is cut content from a stripped level: he never walks, he plays baked animations whose
# root motion carries him between three perches given in world coordinates (sniper.nms, root bone = feet): MT (middle,
# SniperStandMT) and the two side towers RT/LT, which the MTtoRT/LT and back animations jump between (special1-8;
# action1-9 stand and snipe on each). He faces south (-Y) over the courtyard: his snipes step to his right (-X on MT)
# and forward (-Y) to lean out, and the side towers lean in towards the middle. The platforms' tops are where his
# feet land.
PERCHES = [(0.40, 21.06, 21.07),     # spot 0: MT
           (-21.71, 22.96, 27.47),   # spot 1: RT
           (22.56, 22.83, 27.70)]    # spot 2: LT


def perch_map(p):
    """perch (world) -> map position, with z above the map floor"""
    return (p[0] - OFFSET[0], p[1] - OFFSET[1], p[2] - OFFSET[2])


def nad_root_keys(name):
    """root bone (bone 0) position keys of a .nad (v4 as found in this build: 16-byte header; per track uint32 key
    count, bone, type (1 = position), then 52-byte keys: frame, value, b, c, d)"""
    with open(os.path.join(GAME, "3D", "Anims", name + ".nad"), "rb") as h:
        d = h.read()
    o = 16
    for _ in range(struct.unpack_from("<I", d, 4)[0]):
        nk, bone, typ = struct.unpack_from("<3I", d, o)
        o += 12
        if (bone, typ) == (0, 1):
            return [struct.unpack_from("<3f", d, o + 52 * k + 4) for k in range(nk)]
        o += 52 * nk
    return []


# Each perch is a catwalk: small pads along the paths of his stand and snipe animations (the snipes step 2-5 m to the
# side and forward to lean out, then back), on a support column under the stand spot. No solid platform: his line of
# sight (HasLOSFromSpot, from the stand spot's eye ~1.6 m up, steeply down into the courtyard) hit any floor reaching
# more than ~2 m in front of him, and a 4 m pillar top was too small to snipe from (he stepped off and fell).
PAD, PAD_STEP, PAD_THICK = 1.0, 1.0, 0.4    # pad half-size, path sampling and grid (m), catwalk thickness
for p, spot in zip(PERCHES, ("MT", "RT", "LT")):
    x, y, z = perch_map(p)
    box(x - PAD, y - PAD, 0, x + PAD, y + PAD, z, W)                     # support column, top = his feet
    pads = set()
    for a in ("SniperSnipe%sonR", "SniperSnipe%sonL"):
        keys = nad_root_keys(a % spot)
        for k0, k1 in zip(keys, keys[1:]):
            n = max(1, int(math.dist(k0[:2], k1[:2]) / PAD_STEP))
            for i in range(n + 1):
                px = k0[0] + (k1[0] - k0[0]) * i / n
                py = k0[1] + (k1[1] - k0[1]) * i / n
                pads.add((round(px / PAD_STEP) * PAD_STEP, round(py / PAD_STEP) * PAD_STEP))
    # the union of the pads on a PAD_STEP grid, covered greedily by maximal rectangles: a few boxes per catwalk instead
    # of one overlapping box per pad (each box costs lighting triangles; MAPtoNIL allows 4096)
    k = round(PAD / PAD_STEP)
    cells = {(round(px / PAD_STEP) + di, round(py / PAD_STEP) + dj) for px, py in pads
             for di in range(-k, k) for dj in range(-k, k)}
    while cells:
        i0, j0 = min(cells, key=lambda c: (c[1], c[0]))
        i1 = i0
        while (i1 + 1, j0) in cells:
            i1 += 1
        j1 = j0
        while all((i, j1 + 1) in cells for i in range(i0, i1 + 1)):
            j1 += 1
        for i in range(i0, i1 + 1):
            for j in range(j0, j1 + 1):
                cells.discard((i, j))
        ax, ay, _ = perch_map((i0 * PAD_STEP, j0 * PAD_STEP, 0))
        bx, by, _ = perch_map(((i1 + 1) * PAD_STEP, (j1 + 1) * PAD_STEP, 0))
        box(min(ax, bx), min(ay, by), z - PAD_THICK, max(ax, bx), max(ay, by), z, W)
SNIPER_AT = perch_map(PERCHES[0])[:2]                  # placed standing on MT

# roofs over the Nova-sized spaces
roof(SPAWN)
roof(HALL)
roof((CORR[0], HALL[3], CORR[2], CORR[3]))
roof(VCORR_A)
roof(VCORR_B)
roof(SCORR_A)
roof(SCORR_B)
roof(TCORR_A)
roof(TCORR_B)
roof(SLINK)
roof(UCORR)
for leg_l, cross, leg_r, _ in U_LOOPS.values():
    for r in (leg_l, cross, leg_r):
        roof(r)
# sky ceiling over everything: it also keeps flyers in. Baked without shadows so the sun reaches the rooms.
box(BOUNDS[0], BOUNDS[1], H, BOUNDS[2], BOUNDS[3], H + 1, MAT["sky"])


def brush_text(b):
    x0, y0, z0, x1, y1, z1, m, rot, (su, sv) = b
    x0, x1 = x0 + OFFSET[0], x1 + OFFSET[0]
    y0, y1 = y0 + OFFSET[1], y1 + OFFSET[1]
    z0, z1 = z0 + OFFSET[2], z1 + OFFSET[2]
    f = lambda v: ("%g" % v)
    tex = " nod/%s %g %g %g %.6g %.6g 0 0 0" % (m, su, sv, rot, TEX_SCALE[m], TEX_SCALE[m])
    p = lambda x, y, z: "( %s %s %s )" % (f(x), f(y), f(z))
    lines = [
        p(x0, y0, z0) + " " + p(x1, y0, z0) + " " + p(x1, y1, z0) + tex,   # bottom
        p(x0, y0, z1) + " " + p(x0, y1, z1) + " " + p(x1, y1, z1) + tex,   # top
        p(x0, y0, z0) + " " + p(x0, y1, z0) + " " + p(x0, y1, z1) + tex,   # west
        p(x1, y0, z0) + " " + p(x1, y0, z1) + " " + p(x1, y1, z1) + tex,   # east
        p(x0, y0, z0) + " " + p(x0, y0, z1) + " " + p(x1, y0, z1) + tex,   # south
        p(x0, y1, z0) + " " + p(x1, y1, z0) + " " + p(x1, y1, z1) + tex,   # north
    ]
    return "{\n" + "\n".join(lines) + "\n}\n"


# ---------------------------------------------------------------------------------------------------------------
# Entities
entities = []


def ent(classname, origin, **kv):
    e = {"classname": classname, "origin": "%g %g %g" % tuple(origin[k] + OFFSET[k] for k in range(3))}
    e.update({k: str(v) for k, v in kv.items()})
    entities.append(e)


def template_type(name):
    """native type = template modifier 20 ("type"), inherited through basedon (2 for ghost and props, 0 for actors)"""
    seen, n = set(), name
    while n and n.lower() not in seen:
        seen.add(n.lower())
        off = notdump.find(n)
        if off is None:
            raise SystemExit("unknown template: " + name)
        props = dict(notdump.props(off))
        if "type" in props:
            return int(props["type"])
        n = props.get("basedon")
    return 0


def tmpl(name, origin, yaw, **overrides):
    """Native template placement, written the way nodsdk-radiant does (nodtemplates.h): MAPtoNSD needs the template
    binding (Global.not, no extra archives) and its native type."""
    # MAPtoNSD matches template names case-sensitively ("Unresolved template or stale archive provenance")
    if not any(nm == name for nm, _ in notdump.idx):
        raise SystemExit("template name case: %s (Global.not has %s)" % (
            name, [nm for nm, _ in notdump.idx if nm.lower() == name.lower()]))
    PLACED.append((name, tuple(origin), yaw))
    ent("nod_template_instance", origin, nod_authoring="map-template-v1", nod_template=name,
        nod_template_archive="global.not", nod_template_archives="", nod_template_type=template_type(name),
        angles="0 %g 0" % yaw, **overrides)


# player spawn: in the spawn room, facing north towards the hallway door
tmpl("ghost", ((SPAWN[0] + SPAWN[2]) / 2, (SPAWN[1] + SPAWN[3]) / 2, 0.1), 90)

# lights: (x, y, z, r, g, b, intensity, radius)
LIGHTS = [(5, 106, 4, 1, 1, 1, 6, 8)]                                     # spawn room
corridor_light = (1, 0.95, 0.85, 6, 10)
LIGHTS += [(x, 113, 4) + corridor_light for x in (15, 50, 83)]
LIGHTS += [(83, y, 4) + corridor_light for y in (128, 148, 168, 188)]
LIGHTS += [(70, 200, 4) + corridor_light, (70, 208, 4) + corridor_light, (52, 208, 4) + corridor_light]
LIGHTS += [(x, y, 35, 0.75, 0.85, 1.0, 45, 70) for x in (35, 100, 165) for y in (20, 55, 90)]           # Protoss
LIGHTS += [(213, 99, 15, 0.75, 0.85, 1.0, 20, 25)]                                                      # Tanagazj
LIGHTS += [(205, 82, 4) + corridor_light, (211, 85, 4) + corridor_light]
LIGHTS += [(x, y, 15, 1.0, 0.85, 0.6, 25, 30) for x in (50, 70) for y in (218, 238)]                    # Hauler
LIGHTS += [(x, y, 20, 1.0, 0.85, 0.6, 35, 45) for x in (20, 60) for y in (136, 176)]                    # Terran
LIGHTS += [(20, y, 15, 1.0, 0.85, 0.6, 30, 35) for y in (212, 247)]                                     # Vulcan
LIGHTS += [(x, y, 20, 0.9, 0.55, 0.8, 35, 45) for x in (106, 146) for y in (136, 176, 216, 246)]        # Zerg
LIGHTS += [(183, y, 15, 0.9, 0.55, 0.8, 25, 30) for y in (215, 240)]                                    # Lurker
LIGHTS += [(x, y, 20, 0.9, 0.55, 0.8, 35, 45) for x in (221, 251) for y in (155, 185)]                  # Ultralisk
LIGHTS += [(186, 156, 4) + corridor_light, (236, 227, 4) + corridor_light, (236, 113, 4) + corridor_light]
LIGHTS += [(77, 203, 4) + corridor_light] + [(83, y, 4) + corridor_light for y in (210, 223, 235, 252)]
LIGHTS += [(60, 256.5, 16, 1.0, 0.85, 0.6, 32, 40), (105, 267.5, 16, 1.0, 0.85, 0.6, 32, 40)]          # Goliaths
LIGHTS += [(x, y, 32, 1.0, 0.85, 0.6, 40, 55) for x in (22, 62) for y in (280, 305)]                    # sniper
# under each catwalk: its underside faces down, away from the sun, and baked black
LIGHTS += [(x, y, z - 3, 1.0, 0.85, 0.6, 15, 12) for x, y, z in (perch_map(p_) for p_ in PERCHES)]
assert len(LIGHTS) <= 64, len(LIGHTS)
for x, y, z, r, g, b, i, rad in LIGHTS:
    ent("nod_static_light", (x, y, z), _color="%g %g %g" % (r, g, b), light=i, radius=rad)

def face(x, y, tx, ty):
    """yaw (degrees) that turns a unit at (x, y) towards (tx, ty)"""
    return round(math.degrees(math.atan2(-(tx - x), ty - y))) % 360


NEXUS_AT = (155, 52)

# Buildings: (template, x, y[, yaw 0 or 180]). Their footprint is the model's bounding box around (x, y), mirrored
# for yaw 180.
BUILDINGS = [
    ("gateway_building", 40, 30), ("pylon", 72, 16),                       # Protoss: Gateway next to the Pylon,
    ("shieldbattery_building", 88, 16),                                     # shield battery on its other side
    ("nexus_building", 155, 52),
    ("reaver_prop", 100, 42),         # props_player_only: only Nova's shots cycle its animation
    ("bunker", 14, 128), ("MissileTurret", 66, 128),                        # Terran
    # Zerg: the Sunken Colony and the Nydus Canal in the south part, the Command Center and the Spore Colony in the
    # north part (each turned to face into the room). The Sunken's tongue reaches 60 m with no line-of-sight test
    # (at x 153 y 222 it struck Nova in the Lurker room): the level script's sunken_los adds one.
    ("sunkenColony", 152, 134, 180), ("nydusCanal", 116, 138, 180),
    ("infestedCommandCenter", 104, 239, 180), ("sporeColony", 114, 206, 180),
]
# The Mutalisk (cAIZergMutalisk) only flies and attacks along a flight path its mission script starts
# (cAIBaseFlyer::StartPathMove: a .nad track played with tRenderKeyMixer::PlayMovement, positions in world coordinates
# turned by the actor's facing, which is why it is placed at yaw 0). The only paths are 1_2_1's; its FlyAround loop
# (76 x 203 m at 4-43 m, start = end) is scaled into the Zerg room: 0.59 across (uniform, so the banking still fits the
# turns), heights 30-42 m above the floor: over the Command Center (26 m, the tallest building here) with room for
# the Mutalisk itself, under the 45 m walls. The level script's fly_path
# starts it (make_mutalisk_path writes out_mutalisk_path.nad, install_zoo.py copies it to 3D\Anims).
MUTA_PATH = "zoo_mutaliskFlyAround"
MUTA_SRC = "1_2_1_mutaliskFlyAround"
MUTA_SCALE, MUTA_ZSCALE, MUTA_Z0 = 0.59, 0.3, 30.0
MUTA_SRC_CENTER, MUTA_SRC_ZMIN = (-73.65, -81.3), 3.8   # middle of the source loop's x/y range, its lowest point


def muta_xform(p):
    """source path point -> map coordinates (x, y, height above the floor)"""
    cx, cy = (ZERG[0] + ZERG[2]) / 2, (ZERG[1] + ZERG[3]) / 2
    return (cx + (p[0] - MUTA_SRC_CENTER[0]) * MUTA_SCALE, cy + (p[1] - MUTA_SRC_CENTER[1]) * MUTA_SCALE,
            MUTA_Z0 + (p[2] - MUTA_SRC_ZMIN) * MUTA_ZSCALE)


def make_mutalisk_path():
    """out_mutalisk_path.nad: the source's root tracks (bone 0 rotation and position; tRenderKeyMixer::PlayMovement
    refuses a movement track with more than 2 tracks, and the source also animates all 45 bones) with the position keys
    moved into the zoo. Keys are 13 floats: frame, value xyz, then three tangent vectors, scaled like the positions
    (non-finite ones, on the last key, stay as they are)."""
    with open(os.path.join(GAME, "3D", "Anims", MUTA_SRC + ".nad"), "rb") as h:
        d = bytearray(h.read())
    o, pts = 16, []
    for _ in range(struct.unpack_from("<I", d, 4)[0]):
        k, bone, typ = struct.unpack_from("<3I", d, o)
        o += 12
        if bone == 0 and typ == 1:
            for i in range(k):
                ko = o + i * 52
                v = list(struct.unpack_from("<13f", d, ko))
                m = muta_xform(v[1:4])
                v[1:4] = [m[0] + OFFSET[0], m[1] + OFFSET[1], m[2] + OFFSET[2]]   # world coordinates
                for t in (4, 7, 10):
                    for j, sc in enumerate((MUTA_SCALE, MUTA_SCALE, MUTA_ZSCALE)):
                        if math.isfinite(v[t + j]):
                            v[t + j] *= sc
                struct.pack_into("<13f", d, ko, *v)
                pts.append(m)
        o += 52 * k
        if bone == 0 and typ == 1:
            break
    assert struct.unpack_from("<3I", d, 16)[1:] == (0, 0), "root rotation is not the first track"
    struct.pack_into("<I", d, 4, 2)   # track count: the two root tracks (rotation, position) come first
    with open(os.path.join(HERE, "out_mutalisk_path.nad"), "wb") as h:
        h.write(d[:o])
    xs, ys, zs = [q[0] for q in pts], [q[1] for q in pts], [q[2] for q in pts]
    assert ZERG[0] + 6 < min(xs) and max(xs) < ZERG[2] - 6 and ZERG[1] + 6 < min(ys) and max(ys) < ZERG[3] - 6, \
        "Mutalisk path leaves the Zerg room: x %.0f..%.0f y %.0f..%.0f" % (min(xs), max(xs), min(ys), max(ys))
    print("Mutalisk path: x %.0f..%.0f y %.0f..%.0f height %.0f..%.0f (map)" % (min(xs), max(xs), min(ys), max(ys),
                                                                            min(zs), max(zs)))
    return pts


MUTA_PTS = make_mutalisk_path()

# supplies (rifle and sniper ammunition, grenades) inside each boss room: a row starting point and the row's direction
# (along the door's wall, 2 m inside)
SUPPLY_AT = [(211, 90, (1, 0)),                       # Tanagazj: his room's south door
             (169, DOOR_Y["lurker"], (0, 1)),         # Lurker: west door
             (208, DOOR_Y["ultra"], (0, 1)),          # Ultralisk: the central room's west door
             (38, DOOR_Y["vulcan"] + 4, (0, 1)),      # Vulcan: east door (north of it)
             (DOOR_X["hauler"], 213, (1, 0)),         # Hauler: south door
             (78, DOOR_Y["goliath"], (0, 1)),         # Goliath: east door
             (88, DOOR_Y["goliath2"] + 3, (0, 1)),    # cut Goliath: west door (north of it)
             (78, 270, (1, 0))]                       # sniper: by the hallway door, under the east perch's side
# Units: (template, x, y, yaw). Allegiance needs no setup: cAIInstance::IsFriend treats every non-player actor as a
# friend unless the template carries special allegiance flags (none of these do).
UNITS = [
    # Protoss
    ("photonCannon", 72, 30, 90), ("crystalGuard", 102, 8, 90),
    ("scout", 90, 75, 90), ("shuttle", 60, 85, 90), ("observer", 45, 70, 90),
    ("dragoon", 30, 62, face(30, 62, *NEXUS_AT)), ("zealot", 20, 75, face(20, 75, *NEXUS_AT)), ("highTemplar", 105, 62, 90), ("probe", 85, 50, 90),
    ("purifier", 70, 55, 90),
    ("AR_psiDampener", 120, 110.0, 180),   # touching the hallway wall (y 110; model depth +Y from its back plate at the
                                           # origin), facing the room
    ("cloakGenerator", 11.2, 106, 270),   # wall-mounted on the spawn room's outer (east) wall, facing the room
    # the Dark Templars (friendly, no combat AI) stand together facing the Nexus (yaw: 0 north, 90 west, 180 south,
    # 270 east); darkTemplar itself has no model in this build
    ("Zangakar", 150, 100, 180), ("Solara", 155, 100, 180), ("auraKahn", 160, 100, 180), ("Khandalor", 165, 100, 180),
    ("darkArchon_cut", 172, 100, face(172, 100, *NEXUS_AT)),   # end of the row, 2.4 m clear of Khandalor
    ("Tanagazj", 215, 99, face(215, 99, DOOR_X["tanagazj"], TANAGAZJ[1])),   # boss: woken by the .gpc trigger;
                                                                                  # faces his room's door
    # Terran (Artemis, Vulcan and the sniper boss Aries are the Spectres; the sniper has his own arena)
    ("Goliath", 25, 150, 0), ("miniScienceVessel", 74, 145, 0),
    ("TerranVulture", 40, 140, 0), ("firebat", 15, 165, 0),
    ("SCV_psi", 60, 172, 0),
    ("psiDampener", 0.0, 160, 270),        # touching the west wall (x 0; model depth +Y from its back), facing the room
    ("Marine", 35, 132, face(35, 132, 80, DOOR_Y["terran"])), ("scientist", 50, 132, face(50, 132, 80, DOOR_Y["terran"])),
    ("scvDriver", 54, 134, face(54, 134, 50, 132)),                         # SCV pilot, talking to the scientist
    ("mechanic", 52, 136.5, face(52, 136.5, 51, 133)),                      # with them
    ("Trooper", 30, 160, face(30, 160, 80, DOOR_Y["terran"])), ("siegeTank", 50, 165, 90),                # facing away from the door
    ("hickson_cut", 34, 188, face(34, 188, 80, DOOR_Y["terran"])),
    ("dropship_cut", 60, 150, 0),                                          # landed: 16 x 25 m on the floor (x 52-68, y 133-158)
    ("wraith", 20, 140, 90),                                               # air unit, raised (RAISE)
    # the ghost stands at the far end next to Artemis (by the hallway wall he heard Nova and came out); both face the
    # Terran room's door, as does Nova's cutscene Vulture
    ("ghost_male", 22, 181, face(22, 181, 80, DOOR_Y["terran"])),
    ("novaVulture_cut", 45, 186, face(45, 186, 80, DOOR_Y["terran"])),
    ("hauler_cut", 62, 186, face(62, 186, 80, DOOR_Y["terran"])),         # cutscene Hauler (no AI)
    ("spiderMine", 3, 119, 45),                                            # south-west corner, far from both doors
    # Artemis: her model is an older 32-bone skeleton; Artemis (BOSSARTEMIS, no such AI class) and artemis_prop (plays
    # the 43-bone nova.nms) both render as stretched shards, and so does Artemis_cut with its own artemis.nms (see
    # OVERRIDES, which keeps her in the bind pose).
    ("Artemis_cut", 10, 185, face(10, 185, 80, DOOR_Y["terran"])),
    # the named squad, in their side room (SQUAD) facing its door
    ("Marine10Den", 2, 194.5, 180), ("Marine2Joe", 5, 194.5, 180), ("Marine3Jethro", 8, 194.5, 180),
    ("Marine3Sacker", 11, 194.5, 180), ("Marine4Chuggins", 2, 191, 180), ("Marine5Lister", 5.5, 191, 180),
    ("Marine6Dai", 8.5, 191, 180), ("MarineSidekick", 11, 191, 180),
    # elites and captains in the main room
    ("Marine1_elite", 30, 172, face(30, 172, 80, DOOR_Y["terran"])),
    ("Marine2_elite", 34, 172, face(34, 172, 80, DOOR_Y["terran"])),
    ("Marine_captain_rebel", 38, 173, face(38, 173, 80, DOOR_Y["terran"])),
    ("Marine_captain_elite", 42, 173, face(42, 173, 80, DOOR_Y["terran"])),

    ("Vulcan", 20, 234.5, face(20, 234.5, 40, DOOR_Y["vulcan"])),   # boss, own room behind the L hallway: centre of his four pillars, facing his door
    ("Hauler_Boss", 60, 228, 180),
    ("Goliath_232", 52, DOOR_Y["goliath"], 270),                           # boss: own arena, facing its door (east)
    ("Goliath_222c", 114, 267.5, 90),                                      # cut boss AI (OVERRIDES): facing its door (west)
    ("sniper_boss", SNIPER_AT[0], SNIPER_AT[1], 0),    # facing his door (south)                                            # boss (fires missiles): own room
    # Zerg
    ("mutalisk", MUTA_PTS[0][0], MUTA_PTS[0][1], 0),   # at its flight path's start (MUTA_PATH), yaw 0 ("scourge", 142, 157, 180), ("infestedTerran", 150, 175, 180),
    ("ultralisk_cut", 112, 186, 180),                                       # cutscene Ultralisk (no AI), 16 x 8 m
    ("zergEgg", 132, 180, 180), ("larva", 126, 167, 150),
    ("zergling", 135, 170, 180), ("infestedSCVdriver", 158, 160, 180),
    ("zergPustule", 140, 185, 0), ("overlord", 140, 190, 180),
    ("hydralisk", 124, 236, face(124, 236, 126, 186)),                    # by the Command Center, facing the room
    ("sporeColony_mini", 150, 240, 180),                                   # clear of the Lurker door
    ("lurker_boss", 183, 226, face(183, 226, 166, DOOR_Y["lurker"])),      # boss: own room, woken by its trigger
    ("ultralisk", 236, 175, face(236, 175, ULTRA[0], DOOR_Y["ultra"])),    # boss: central room of his arena
    # supplies just inside each boss room's door (see SUPPLY_AT), with a health and a psi energy pickup at the ends
] + [(t, x + dx, y + dy, 0) for x, y, (ux, uy) in SUPPLY_AT
     for t, k in (("powEnergy", -3), ("powGauss_2x", -2), ("powGauss_2x", -1), ("powSnipe_2x", 0), ("powGrenade", 1),
                  ("powGrenade", 2), ("powHealth", 3))
     for dx, dy in ((k * ux * 1.5, k * uy * 1.5),)] + [
    # spawn room: health, ammunition and psi energy along its south wall (the door is in the north wall at x 5)
    ("powHealth", 3, 103.2, 0), ("powGauss_2x", 4, 103.2, 0), ("powSnipe_2x", 5, 103.2, 0), ("powEnergy", 6, 103.2, 0),
    # masked Nova: the cutscene template never animates (bind pose); an animated Nova prop with the masked model as a
    # placement override did not render at all, so the T-pose version stays
    ("rhynadon_prop", 2, 106, 0), ("Nova_mask_cut", 8.5, 104, 90),
    ("beacon", 6.5, 108.5, 0),                                              # psi energy recharger pad
]
# models that extend below their origin (bounding-box minimum z) or should hover start higher, plus a margin
RAISE = {"mutalisk": 0.0,   # set to its flight path's start height below
         "miniScienceVessel": 3.0, "shuttle": 4.5, "observer": 0.8,
         # the psi dampeners hang on walls (they stay where placed): bottoms about 1.5 m / 0.6 m up
         "psiDampener": 1.5 + 0.78 + 0.05, "AR_psiDampener": 0.6 + 1.36,
         "scout": 14.0,   # the door (2.5 m) hides the hallway from 14 m up at its 35 m distance
         "highTemplar": 0.5,
         "cloakGenerator": 2.4,   # its centre (model z -1.24..1.24) 2.5 m up the wall
         # air units: their bottom (model min z: -0.06, -3.9, -8.0) about 10 m up
         "wraith": 14.0, "overlord": 18.0, "hickson_cut": -0.1,
         # its stand pose (props_start) holds the root bone 1.77 m below the model's rest pose, the body above it
         "rhynadon_prop": -1.85,
         "Hauler_Boss": -0.3,
         # cutscene templates (no AI) have no gravity either: placed at floor level instead of 0.1 m above it
         "Nova_mask_cut": -0.1, "Artemis_cut": -0.1, "novaVulture_cut": -0.1, "hauler_cut": -0.1, "dropship_cut": -0.1,
         "sniper_boss": PERCHES[0][2] - OFFSET[2] - 0.1}   # on the MT pillar (world z = MT's feet)
RAISE["mutalisk"] = MUTA_PTS[0][2] - 0.1
# per-placement template property overrides (this map only): {template: {"nod_prop_<modifier id>": value}}
# Artemis: artemis.nms plays marine and Nova animations, made for other skeletons, on her old 32-bone model (the
# stretched shards). dropship_cut is the masked Nova's motion set (also a 32-bone model); its only animation is a
# scripted one that never plays, so the model stays in its intact bind pose. Modifier 13 = motionset.
# Goliath_232 carries its mission's script (cM232_GoliathMain: plays the boss intro 232a_intro.ncs at the level
# start, whose cast is missing here, crashing as it ends; NextMission when he dies). Modifier 15 = script; a name with
# no script class makes script_New return none.
# Goliath_222c runs the cut Goliath boss AI, BOSSGOLIATH (cAIBossGoliath, registered by GhostPC). Modifier 43 = ACTOR_AI.
OVERRIDES = {"Artemis_cut": {"nod_prop_13": "dropship_cut"}, "Goliath_232": {"nod_prop_15": "none"},
             # ... and the zoo's motion set: goliath_222c.nms plus placeholders for the modes it lacks (zoo_goliath_222c.nms,
             # copied to 3D\Motions by install_zoo.py)
             "Goliath_222c": {"nod_prop_43": "BOSSGOLIATH", "nod_prop_13": "zoo_goliath_222c.nms"}}


def model_bounds(name):
    """model bounding box (min, max) of a template, from its .nod header"""
    seen, n, model = set(), name, None
    while n and n.lower() not in seen and model is None:
        seen.add(n.lower())
        props = dict(notdump.props(notdump.find(n)))
        model = props.get("model")
        n = props.get("basedon")
    models = os.path.join(GAME, "3D", "Models")
    files = {f.lower(): f for f in os.listdir(models)}
    with open(os.path.join(models, files[model.lower()]), "rb") as h:
        hdr = h.read(0x5c)
    return struct.unpack_from("<3f", hdr, 12), struct.unpack_from("<3f", hdr, 24)


footprints = []   # (x0, y0, x1, y1) of buildings
for b in BUILDINGS:
    t, x, y, yaw = b if len(b) == 4 else b + (0,)
    mn, mx = model_bounds(t)
    if yaw == 180:
        mn, mx = (-mx[0], -mx[1], mn[2]), (-mn[0], -mn[1], mx[2])
    footprints.append((x + mn[0], y + mn[1], x + mx[0], y + mx[1], t))
    tmpl(t, (x, y, 0.1 + RAISE.get(t, 0.0)), yaw)

footprints += [(x - 2, y - 2, x + 2, y + 2, "pillar") for x, y in PILLARS]
ROOMS = {"Protoss": PROTOSS, "Terran": TERRAN, "Zerg": ZERG, "Tanagazj": TANAGAZJ, "Ultralisk": ULTRA,
         "Vulcan": VULCAN, "Hauler": HAULER, "Goliath": GOLIATH, "Goliath2": GOLIATH2, "spawn": SPAWN, "sniper": SNIPER, "Lurker": LURKER}


def inside(r, x0, y0, x1, y1, margin=0.0):
    return r[0] + margin <= x0 and x1 <= r[2] - margin and r[1] + margin <= y0 and y1 <= r[3] - margin


# checks: buildings fit their room with 2 m to spare, and no unit stands on or within 3 m of a building
for x0, y0, x1, y1, t in footprints:
    assert any(inside(r, x0, y0, x1, y1, 2.0) for r in ROOMS.values()), "%s does not fit its room" % t
WALL_MOUNTED = {"AR_psiDampener", "psiDampener"}   # sit against a wall, within the 1 m margin
for t, x, y, yaw in UNITS:
    margin = 0.0 if t in WALL_MOUNTED else 1.0
    assert any(inside(r, x, y, x, y, margin) for r in ROOMS.values()), "%s is outside every room" % t
    for x0, y0, x1, y1, b in footprints:
        assert not (x0 - 3 < x < x1 + 3 and y0 - 3 < y < y1 + 3), "%s overlaps %s" % (t, b)
    tmpl(t, (x, y, 0.1 + RAISE.get(t, 0.0)), yaw, **OVERRIDES.get(t, {}))

# waypoints: a grid per big room, 4-connected both ways, kept 6 m from walls and 3 m from buildings
wp_id = [1]
wp_pos = {}
links = []


def blocked(x, y):
    in_spawn = x < SPAWN[2] + 4 and y > SPAWN[1] - 4
    return in_spawn or any(x0 - 3 < x < x1 + 3 and y0 - 3 < y < y1 + 3 for x0, y0, x1, y1, _ in footprints)


def grid(room, step=8.0, margin=6.0):
    x0, y0, x1, y1 = room
    nx = int((x1 - x0 - 2 * margin) // step) + 1
    ny = int((y1 - y0 - 2 * margin) // step) + 1
    ids = {}
    for i in range(nx):
        for j in range(ny):
            x, y = x0 + margin + i * step, y0 + margin + j * step
            if blocked(x, y):
                continue
            ids[(i, j)] = wp_id[0]
            ent("nod_npd_waypoint", (x, y, 0.5), nod_authoring="npd-world-v1", nod_space="world",
                nod_waypoint_id=wp_id[0], nod_flags=0)
            wp_pos[wp_id[0]] = (x, y, 0.5)
            wp_id[0] += 1
    for (i, j), a in ids.items():
        for di, dj in ((1, 0), (0, 1)):
            b = ids.get((i + di, j + dj))
            if b:
                links.append((a, b))
                links.append((b, a))


for r in (PROTOSS, TERRAN, ZERG, ULTRA, VULCAN, TANAGAZJ, HAULER, LURKER):   # ULTRA = the central room
    grid(r, step=8.0 if r[2] - r[0] > 30 else 6.0)
for a, b in links:
    m = [(wp_pos[a][k] + wp_pos[b][k]) / 2 for k in range(3)]
    entities.append({"classname": "nod_npd_link", "nod_authoring": "npd-world-v1", "nod_space": "world",
                     "origin": "%g %g %g" % tuple(m[k] + OFFSET[k] for k in range(3)), "nod_from": str(a),
                     "nod_to": str(b), "nod_link_flags": "0"})


def tri_estimate(spacing):
    """Upper-bound estimate: every brush face subdivided into spacing-sized quads (2 triangles each)."""
    tris = 0
    for x0, y0, z0, x1, y1, z1, m, *_ in brushes:
        dims = (x1 - x0, y1 - y0, z1 - z0)
        for a, b in ((0, 1), (0, 1), (0, 2), (0, 2), (1, 2), (1, 2)):
            tris += 2 * max(1, math.ceil(dims[a] / spacing)) * max(1, math.ceil(dims[b] / spacing))
    return tris


# MAPtoNIL rejected an estimate of ~3100 (its subdivision is finer than this count), so aim well below the cap
TRI_TARGET = 1800
SPACING = 4.0
while tri_estimate(SPACING) > TRI_TARGET and SPACING < 128:
    SPACING += 2.0

with open(OUT, "w", newline="\n") as f:
    f.write("// Enemy Zoo Test map (9_1_1a_Enemy_Zoo) - generated by make_zoo_map.py, do not edit by hand\n")
    f.write('{\n"classname" "worldspawn"\n"nod_light_spacing" "%g"\n' % SPACING)
    for b in brushes:
        f.write(brush_text(b))
    f.write("}\n")
    for e in entities:
        f.write("{\n")
        for k, v in e.items():
            f.write('"%s" "%s"\n' % (k, v))
        f.write("}\n")

# GhostPC level script (src/levelscript.cpp): wake Tanagazj when Nova enters his room. ScriptNotify 101/102/103 =
# phase 1/2/3; only phase 3 draws his psi blade (cAIBossTanagazj::BladeOn runs only while the phase field is 3; phases
# 1-2 fight bare-handed, phase 2 adds cloaking and the arena platforms), so he starts in phase 3.
with open(os.path.join(HERE, "out_levelscript.gpc"), "w", newline="\r\n") as f:
    lo = [TANAGAZJ[0] + OFFSET[0], TCORR_A[1] + OFFSET[1], -5.0]          # room and its L hallway
    hi = [TANAGAZJ[2] + OFFSET[0], TANAGAZJ[3] + OFFSET[1], 10.0]
    f.write("# GhostPC level script for 9_1_1a_Enemy_Zoo (generated by make_zoo_map.py)\n")
    f.write("# notify x0 y0 z0 x1 y1 z1 <ai class> <message>: sent once when the player enters the box or an actor of\n")
    f.write("# that class is damaged (shot from outside the box)\n")
    f.write("notify %g %g %g %g %g %g cAIBossTanagazj 103\n" % tuple(lo + hi))
    # the sniper boss: ScriptNotify 0 (mode 0) targets the player and shows the boss bar
    lo = [SNIPER[0] + OFFSET[0], SNIPER[1] + OFFSET[1], -5.0]
    hi = [SNIPER[2] + OFFSET[0], SNIPER[3] + OFFSET[1], 50.0]
    f.write("notify %g %g %g %g %g %g cAIBossSniper 0\n" % tuple(lo + hi))
    # the Lurker boss: any ScriptNotify but 10 targets the player
    lo = [LURKER[0] + OFFSET[0], LURKER[1] + OFFSET[1], -5.0]
    hi = [LURKER[2] + OFFSET[0], LURKER[3] + OFFSET[1], 20.0]
    f.write("notify %g %g %g %g %g %g cAIBossLurker 0\n" % tuple(lo + hi))
    # the cut Goliath boss: any ScriptNotify targets the player and shows the boss bar (50 sends it idle)
    lo = [GOLIATH2[0] + OFFSET[0], GOLIATH2[1] + OFFSET[1], -5.0]
    hi = [GOLIATH2[2] + OFFSET[0], GOLIATH2[3] + OFFSET[1], 20.0]
    f.write("notify %g %g %g %g %g %g cAIBossGoliath 0\n" % tuple(lo + hi))
    # the Goliath boss (Goliath_232: TERRANGOLIATHBOSS, cAITerranGoliathBoss) needs no trigger: it engages on its own;
    # its ScriptNotify only takes the 232 mission's column messages (999, 1000, 1010)
    # the player is created at the world origin (the sniper arena); start her in the spawn room, facing north
    f.write("player_start %g %g %g 0\n" % ((SPAWN[0] + SPAWN[2]) / 2 + OFFSET[0], (SPAWN[1] + SPAWN[3]) / 2 + OFFSET[1],
                                           OFFSET[2]))
    # the animation-cycling props (rhynadon_prop, reaver_prop) open with their "stand" pose instead of "run"
    f.write("props_start stand\n")
    f.write("props_player_only\n")   # other units' fire does not cycle them (the Reaver near the Scout)
    # the Ultralisk boss pins itself and only its (missing) mission script released it: its charge needs it unpinned
    f.write("unpin cAIBossUltralisk\n")
    f.write("sunken_los\n")   # Sunken Colonies strike only what they can see (not through walls into the hallways)
    # the Mutalisk flies (and attacks from) its path; the rate is key frames per second (30: a 15 s lap)
    f.write("fly_path cAIZergMutalisk %s 30\n" % MUTA_PATH)

# Track frames for install_zoo.py (MAPtoNSD writes none): the sniper's spots are frames 1-3 of his placement's track
# (cAIBossSniper::HasLOSFromSpot reads frame[spot + 1]'s position, tThing +0x134); frame 0 is his placement (MT).
# World positions of his feet.
frames = [PERCHES[0]] + PERCHES
with open(os.path.join(HERE, "out_frames.txt"), "w") as f:
    f.write("# template x y z (one frame per line), written by make_zoo_map.py for install_zoo.py\n")
    for fr in frames:
        f.write("sniper_boss %.3f %.3f %.3f\n" % fr)
    # the cut Goliath boss (cAIBaseBossGoliath::FightToFrame walks to frame N's position, tThing +0x134; frames 1-3 and
    # 5-7 are its rocket frames, cAIBossGoliath::IsRocketFrame): frame 0 = its placement, 1-9 spread over its arena
    # clear of the cover pillars (GhostPC's driver sends it from frame to frame, the work of its mission script)
    for x, y in [(114, 267.5), (119, 261), (119, 274), (113, 267.5), (104, 267.5), (102.5, 259.5), (102.5, 275.5),
                 (115, 260), (115, 275), (121, 267.5)]:
        f.write("Goliath_222c %.3f %.3f %.3f\n" % (x + OFFSET[0], y + OFFSET[1], OFFSET[2]))

# Level effect list (Levels\<level>.nlfx): the game only loads the effects a level lists, e.g. Tanagazj's "psiblade"
# blade (cAIBossTanagazj::BladeOn) was invisible without one. Union of the original lists, the blade effects and every
# EFFECTn of the placed templates, keeping only names defined in Misc\game.nfx.
def template_effects(name):
    seen, n, out = set(), name, []
    while n and n.lower() not in seen:
        seen.add(n.lower())
        props = notdump.props(notdump.find(n))
        out += [v for k, v in props if k.upper().startswith("EFFECT") and v]
        n = dict(props).get("basedon")
    return out


nfx = open(os.path.join(GAME, "Misc", "game.nfx"), "rb").read().lower()
effects = []
for lst in ("1_2_1a_Miners_Bunker.nlfx", "2_2_2a_Threat_BaseUpper.nlfx"):
    effects += [l.strip() for l in open(os.path.join(GAME, "Levels", lst), encoding="latin1") if l.strip()]
effects += ["psiblade", "psiblade2"]
for t in [b[0] for b in BUILDINGS] + [u[0] for u in UNITS]:
    effects += template_effects(t)
unique = []
for e in effects:
    if e.lower() not in [u.lower() for u in unique] and e.lower().encode() in nfx:
        unique.append(e)
with open(os.path.join(HERE, "out_effects.nlfx"), "w", newline="\r\n") as f:
    f.write("\n".join(unique) + "\n")
print("effect list: %d effects (psiblade %s)" % (len(unique), "psiblade" in unique))

print("brushes %d, entities %d (lights %d, buildings %d, units %d, waypoints %d, links %d)" % (
    len(brushes), len(entities), len(LIGHTS), len(BUILDINGS), len(UNITS), wp_id[0] - 1, len(links)))
print("light spacing %g -> triangle estimate %d (upper bound, limit 4096)" % (SPACING, tri_estimate(SPACING)))
print("wrote", OUT)

# the overview: ZOO_OVERVIEW.md (rooms, doors, units, triggers in world coordinates) and zoo_overview.svg
import zoo_overview  # noqa: E402
zoo_overview.write(globals())
