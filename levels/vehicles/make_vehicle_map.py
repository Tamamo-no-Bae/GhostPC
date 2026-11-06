r"""Generates the Quake III MAP for the vehicle test map (level 9_2_1a_Vehicle_Test).

Layout (game units = metres, X east, Y north, Z up; north is up), mirror-symmetric about y = 0, the field and track
also about the track's centre line x = TRACK_CX:

    +---------------------------------------+
    |  grass                                |
    |        .--------------------.         |
    |       /   pavement track     \        |
    |      |   .--------------.     |       |
    |      |  |  grass          |   |       |       +----------+
    |      |  |  +----------+   |   |       +-------+          |
    |      |  |  |  solid   |   |   |                 spawn    |
    |      |  |  |  block   |   |   |       +-------+  (0, 0)  |
    |      |  |  +----------+   |   |       | corr. +----------+
    |      |   '--------------'     |       |
    |       \                      /        |
    |        '--------------------'         |
    |                                       |
    +---------------------------------------+

An open field for the big vehicles (Siege Tank: 12 m across, Goliath, Vulture): a stadium-shaped pavement track
(15 m wide) around a solid block on grass, walled in by rock, open to a clear blue sky (the Robota skybox drawn over
the whole viewport, OpenNOD SDK 0.6.8 worldspawn keys). The spawn room (Protoss) is at the world origin, where the
game creates the player; its floor is 0.3 m below the origin (spawning inside collision gave NaN physics), and a
short corridor leads west into the field. The track's curved ends are convex prisms (one per 11.25 degrees); every
floor piece is a separate convex brush, so pavement and grass meet flush.

Run:  python make_vehicle_map.py   ->  writes vehicle_test.map, out_levelscript.gpc and out_effects.nlfx next to this
script and vehicle_overview.png (the layout from above).
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "vehicle_test.map")
NAME = "9_2_1a_Vehicle_Test"
GAME = os.environ.get("GHOST_GAME_DIR") or next(
    (g for g in (os.path.join(HERE, "..", "..", "game"), os.path.join(HERE, ".."), os.path.join(HERE, "..", ".."))
     if os.path.exists(os.path.join(g, "Misc"))),   # GhostPC workspace, or the patch's mapsrc/vehicles in the game folder
    os.path.join(HERE, ".."))
os.environ.setdefault("GHOST_GAME_DIR", GAME)   # for notdump
sys.path.insert(0, os.path.join(HERE, "..", "..", "scripts"))
sys.path.insert(0, os.path.join(HERE, ".."))   # mapsrc/notdump.py in the patch
sys.path.insert(0, HERE)
import notdump  # noqa: E402  (Global.not reader)

T = 1.0                       # wall thickness
OFFSET = (0.0, 0.0, -0.3)     # floors 0.3 m below the origin, as on the Zoo and the arena

# spawn room (Protoss) at the origin, and the corridor west into the field (interior extents x0, y0, x1, y1)
SPAWN = (-5.0, -7.0, 5.0, 7.0)
CORR = (-13.0, -3.0, -6.0, 3.0)         # between the spawn room's west wall and the field's east wall
H_ROOM = 5.0                            # spawn room and corridor ceiling
# the field: grass inside rock walls, open to the sky
FIELD = (-144.0, -80.0, -14.0, 80.0)    # interior; its east wall (x -14 .. -13) has the corridor's opening
H_FIELD = 12.0                          # wall height
TRACK_CX = (FIELD[0] + FIELD[2]) / 2    # the track's centre line (x -79)
R_OUT, R_IN = 41.0, 26.0                # track: outer / inner radius of its curved ends (15 m of pavement)
STRAIGHT = 28.5                         # the curved ends' centres at y = +-STRAIGHT (outer length 139 m)
BLOCK = (TRACK_CX - 20.0, -40.0, TRACK_CX + 20.0, 40.0)   # the solid block in the middle (40 x 80 m)
H_BLOCK = 12.0
ARC = 16                                # segments per curved end

# Sky (MAPtoNSD worldspawn): the clear blue Robota skybox over the whole viewport (see levels/zoo/make_zoo_map.py)
SKY_MODEL = "sky_Robota_02.nod"

MAT = {
    "grass": "MS_Grass_Green_02", "pavement": "MS_Asphalt", "block": "GE_concrete_01", "rock": "AR_RockWall",
    "floor_spawn": "AR_Plated_Tiles_blu", "wall_spawn": "AR_Striped_blu_gld_01", "ceiling": "AR_Pattern_HComb_gld",
    "floor_corr": "AR_Groovy_blu_01", "wall_corr": "AR_Plated_blu_gld_01",
}
TILE = {"grass": 4.0, "pavement": 6.0, "block": 6.0, "rock": 8.0}   # metres per texture repeat (default 4)


def dds_width(name):
    """texture width from its DDS header, searched like the game does (MaterialDirs.txt)"""
    for d in open(os.path.join(GAME, "MaterialDirs.txt"), encoding="latin1").read().split():
        f = os.path.join(GAME, *d.split(chr(92)), name + ".dds")
        if os.path.exists(f):
            with open(f, "rb") as h:
                hdr = h.read(20)
            return int.from_bytes(hdr[16:20], "little")
    raise SystemExit("texture not found: " + name)


TEX_SCALE = {m: TILE.get(k, 4.0) / dds_width(m) for k, m in MAT.items()}

# ---------------------------------------------------------------------------------------------------------------
# Geometry: brushes are convex prisms, (polygon [(x, y) counter-clockwise], z0, z1, material); boxes are 4-gons
brushes = []


GRID = 64   # corners snapped to 1/64 m: exact in binary, so the planes MAPtoNIL derives from them, and its CSG of the
            # faces two brushes share, come out identical on both sides (decimal rounding failed: "CSG vertices merge at
            # binary32 precision" where the track met the grass)


def snap(v):
    return round(v * GRID) / GRID


def prism(pts, z0, z1, mat):
    pts = [(snap(x), snap(y)) for x, y in pts]
    area = sum(pts[i][0] * pts[i - 1][1] - pts[i - 1][0] * pts[i][1] for i in range(len(pts))) / -2
    if area < 0:
        pts.reverse()
    if abs(area) > 1e-6 and z1 - z0 > 1e-6:
        brushes.append((tuple(pts), z0, z1, mat))


def box(x0, y0, z0, x1, y1, z1, mat):
    if x1 - x0 > 1e-6 and y1 - y0 > 1e-6:
        prism([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], z0, z1, mat)


def floor(r, mat, z0=-1.0):
    box(r[0], r[1], z0, r[2], r[3], 0.0, mat)


def wall(x0, y0, x1, y1, mat, height, gap=None):
    """wall box (thin along one axis) from the floor up; gap = (centre, width) along its long axis, full height"""
    along_x = (x1 - x0) >= (y1 - y0)
    lo, hi = (x0, x1) if along_x else (y0, y1)
    pieces = [(lo, hi)] if not gap else [(lo, gap[0] - gap[1] / 2), (gap[0] + gap[1] / 2, hi)]
    for a, b in pieces:
        if along_x:
            box(a, y0, 0.0, b, y1, height, mat)
        else:
            box(x0, a, 0.0, x1, b, height, mat)


G, P = MAT["grass"], MAT["pavement"]
cx, (fx0, fy0, fx1, fy1) = TRACK_CX, FIELD


def ring(r, cy, a0, a1, n):
    """n + 1 points on a circle of radius r around (cx, cy), from angle a0 to a1 (degrees)"""
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)), cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / n)))
            for i in range(n + 1)]


def to_rect(x, y, cy, rect):
    """where the ray from (cx, cy) through (x, y) leaves rect (x0, y0, x1, y1)"""
    dx, dy = x - cx, y - cy
    ts = []
    for k, (d, o) in enumerate(((dx, cx), (dy, cy))):
        if abs(d) > 1e-9:
            for edge in (rect[k], rect[k + 2]):
                t = (edge - o) / d
                if t > 0:
                    ts.append(t)
    t = min(t for t in ts if all(rect[j] - 1e-6 <= (cx, cy)[j] + t * (dx, dy)[j] <= rect[j + 2] + 1e-6 for j in (0, 1)))
    return cx + t * dx, cy + t * dy


# straights: pavement on both sides of the middle, grass inside and outside
for s in (-1, 1):
    a, b = sorted((cx + s * R_IN, cx + s * R_OUT))
    box(a, -STRAIGHT, -1.0, b, STRAIGHT, 0.0, P)                     # pavement
    a, b = sorted((cx + s * R_OUT, fx1 if s > 0 else fx0))
    box(a, -STRAIGHT, -1.0, b, STRAIGHT, 0.0, G)                     # grass between the track and the field wall
box(cx - R_IN, -STRAIGHT, -1.0, cx + R_IN, STRAIGHT, 0.0, G)         # grass in the middle
# curved ends (north: centre y +STRAIGHT, angles 0..180; south mirrored): pavement segments, the grass half-disc
# inside, and outside slices out to the field's walls (a wedge from the centre cut by the field and the outer chord is
# convex)
for s in (1, -1):
    cy = s * STRAIGHT
    a0, a1 = (0.0, 180.0) if s > 0 else (180.0, 360.0)
    outer, inner = ring(R_OUT, cy, a0, a1, ARC), ring(R_IN, cy, a0, a1, ARC)
    for i in range(ARC):
        prism([inner[i], outer[i], outer[i + 1], inner[i + 1]], -1.0, 0.0, P)
    prism(inner, -1.0, 0.0, G)                                          # the grass inside: a half-disc (convex)
    rect = (fx0, cy, fx1, fy1) if s > 0 else (fx0, fy0, fx1, cy)
    corners = [(x, y) for x in (fx0, fx1) for y in ((cy, fy1) if s > 0 else (fy0, cy))]
    for i in range(ARC):
        p, q = outer[i], outer[i + 1]
        pr, qr = to_rect(*p, cy, rect), to_rect(*q, cy, rect)
        angp, angq = (math.atan2(v[1] - cy, v[0] - cx) % (2 * math.pi) for v in (p, q))
        inside = [c for c in corners if c[1] != cy and angp < math.atan2(c[1] - cy, c[0] - cx) % (2 * math.pi) < angq]
        prism([p, pr] + inside + [qr, q], -1.0, 0.0, G)
# the solid block, the field's rock walls (the east one open to the corridor)
box(BLOCK[0], BLOCK[1], 0.0, BLOCK[2], BLOCK[3], H_BLOCK, MAT["block"])
R = MAT["rock"]
wall(fx0 - T, fy1, fx1 + T, fy1 + T, R, H_FIELD)
wall(fx0 - T, fy0 - T, fx1 + T, fy0, R, H_FIELD)
wall(fx0 - T, fy0, fx0, fy1, R, H_FIELD)
wall(fx1, fy0, fx1 + T, fy1, R, H_FIELD, gap=(0.0, CORR[3] - CORR[1]))
box(fx1, CORR[1], H_ROOM, fx1 + T, CORR[3], H_FIELD, R)   # above the opening (the corridor is only H_ROOM high)
# corridor and spawn room: Protoss, roofed
floor((fx1, CORR[1], CORR[2], CORR[3]), MAT["floor_corr"])   # through the field wall's opening (x fx1 .. CORR[0])
box(fx1, CORR[1] - T, H_ROOM, CORR[2], CORR[3] + T, H_ROOM + 1, MAT["ceiling"])
wall(CORR[0], CORR[3], CORR[2], CORR[3] + T, MAT["wall_corr"], H_ROOM + 1)
wall(CORR[0], CORR[1] - T, CORR[2], CORR[1], MAT["wall_corr"], H_ROOM + 1)
sx0, sy0, sx1, sy1 = SPAWN
floor((sx0 - T, sy0 - T, sx1 + T, sy1 + T), MAT["floor_spawn"])
box(sx0 - T, sy0 - T, H_ROOM, sx1 + T, sy1 + T, H_ROOM + 1, MAT["ceiling"])
wall(sx0 - T, sy1, sx1 + T, sy1 + T, MAT["wall_spawn"], H_ROOM + 1)
wall(sx0 - T, sy0 - T, sx1 + T, sy0, MAT["wall_spawn"], H_ROOM + 1)
wall(sx1, sy0, sx1 + T, sy1, MAT["wall_spawn"], H_ROOM + 1)
wall(sx0 - T, sy0, sx0, sy1, MAT["wall_spawn"], H_ROOM + 1, gap=(0.0, CORR[3] - CORR[1]))
assert CORR[0] == fx1 + T and CORR[2] == sx0 - T, "corridor does not join the field and the spawn room"


def brush_text(b):
    pts, z0, z1, m = b
    f = lambda v: ("%.6f" % v).rstrip("0").rstrip(".")   # 1/64 multiples print exactly with 6 decimals
    tex = " nod/%s 0 0 0 %.6g %.6g 0 0 0" % (m, TEX_SCALE[m], TEX_SCALE[m])
    p = lambda x, y, z: "( %s %s %s )" % (f(x + OFFSET[0]), f(y + OFFSET[1]), f(z + OFFSET[2]))
    # plane points ordered as Quake / MAPtoNIL expect: (b - a) x (c - a) points into the brush
    a, b2, c = pts[0], pts[1], pts[2]
    lines = [p(*a, z0) + " " + p(*b2, z0) + " " + p(*c, z0) + tex,     # bottom (normal -z)
             p(*a, z1) + " " + p(*c, z1) + " " + p(*b2, z1) + tex]     # top (+z)
    for i in range(len(pts)):
        u, v = pts[i], pts[(i + 1) % len(pts)]
        lines.append(p(*u, z0) + " " + p(*u, z1) + " " + p(*v, z1) + tex)
    return "{\n" + "\n".join(lines) + "\n}\n"


def check_brushes():
    """convex, counter-clockwise, and each face's plane normal points out of the brush"""
    for pts, z0, z1, m in brushes:
        n = len(pts)
        for i in range(n):
            (x1, y1), (x2, y2), (x3, y3) = pts[i], pts[(i + 1) % n], pts[(i + 2) % n]
            assert (x2 - x1) * (y3 - y2) - (y2 - y1) * (x3 - x2) > -1e-6, "not convex / not CCW: %r" % (pts,)


check_brushes()

# ---------------------------------------------------------------------------------------------------------------
# Entities
entities = []


def ent(classname, origin, **kv):
    e = {"classname": classname, "origin": "%g %g %g" % tuple(origin[k] + OFFSET[k] for k in range(3))}
    e.update({k: str(v) for k, v in kv.items()})
    entities.append(e)


def template_type(name):
    """native type = template modifier 20 ("type"), inherited through basedon"""
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


PLACED = []


# GhostPC's own templates (game/Misc/Templates/ghostpc.not, scripts/make_ghostpc_templates.py) -> their basedon.
# MAPtoNSD wants one archive context for every placement (nod_template_archives; Global is implicit): the scene then
# lists ghostpc.not and the game opens it with the level.
GHOSTPC_TEMPLATES = {"reaver_vehicle_player": "Arclite_player", "dragoon_vehicle_player": "Goliath_player"}


def tmpl(name, origin, yaw=0):
    """native template placement, written the way nodsdk-radiant does (MAPtoNSD: case-sensitive Global.not names)"""
    if name in GHOSTPC_TEMPLATES:
        archive, ttype = "ghostpc.not", template_type(GHOSTPC_TEMPLATES[name])
    elif any(nm == name for nm, _ in notdump.idx):
        archive, ttype = "global.not", template_type(name)
    else:
        raise SystemExit("template name case: " + name)
    PLACED.append((name, tuple(origin), yaw))
    ent("nod_template_instance", origin, nod_authoring="map-template-v1", nod_template=name,
        nod_template_archive=archive, nod_template_archives="ghostpc.not", nod_template_type=ttype,
        angles="0 %g 0" % yaw)


# the player: created at the origin (the spawn room's middle), facing west towards the corridor; yaw as the level
# script takes it (0 = +Y, 90 = -X)
tmpl("ghost", (0.0, 0.0, 0.1), 180)
# a pilotable Vulture (Vulture_player: tThing::PreFinalize gives it cVultureControl) on the track's east straight, in
# front of the corridor, facing north along the track
VULTURE = (cx + (R_OUT + R_IN) / 2, 0.0)
tmpl("Vulture_player", (VULTURE[0], VULTURE[1], 0.1), 0)
# the other player vehicles of the build: the Goliath beside the Vulture, the Siege Tank below (the Gauss and flame turrets go to the Zoo once they work; the Refinery's crane arm is worked remotely, not entered;
# Spider Mine, Firefly and Mantis are inventory items)
# (0.3 m lower than the others: an idle player Goliath is not simulated and stays where it is put, and at 0.1 its feet
# hovered over the track)
tmpl("Goliath_player", (VULTURE[0] + 3.0, -12.0, -0.2), 0)
# the Dragoon (GhostPC's dragoon_vehicle_player: the Goliath's walker control with the Dragoon's model, motions and
# shot; vehicles.cpp) south of the Goliath; on the floor (z -0.3): like an idle player Goliath it is not simulated
# until mounted and stays where it is put (at 0.1 it floated, user)
tmpl("dragoon_vehicle_player", (VULTURE[0] + 3.0, -22.0, -0.3), 0)
# the Siege Tank (Arclite_player: "arclite" in its name gives it cTankControl; 12 m across) on the east straight with
# the others, north of the Vulture
tmpl("Arclite_player", (VULTURE[0], 14.0, 0.1), 0)
# the Reaver (GhostPC's reaver_vehicle_player: the Siege Tank's control with the Reaver's model, a slow crawler firing
# Scarabs; vehicles.cpp): 12 x 17 m at native size, on the open grass east of the track (between the track's outer
# edge, x = TRACK_CX + R_OUT, and the field's east wall), clear of the other vehicles
tmpl("reaver_vehicle_player", ((cx + R_OUT + fx1) / 2, -40.0, 0.1), 0)
# its console (the Reaver is a robot worked remotely, vehicles.cpp): the Protoss floor console AR_hackConsole_floor (a
# Global.not prop no level of the build places), in the spawn room against its east wall, facing the room (user's
# choice); Nova uses it from the west, facing east
tmpl("AR_hackConsole_floor", (sx1 - 0.7, 0.0, 0.0), 90)
# test targets on the west straight, facing the vehicles across the block (awake units: hibernating ones (_hib) cannot
# be hit by projectiles until they wake; these come for Nova once they see her)
WEST = cx - (R_OUT + R_IN) / 2
TARGETS = [("zerglingNORMAL", WEST - 3, -18.0), ("zerglingNORMAL", WEST + 3, -16.0),
           ("zerglingNORMAL", WEST, -12.0), ("hydralisk", WEST - 2, -4.0), ("firebat_elite", WEST + 2, 4.0),
           ("Marine", WEST - 3, 12.0), ("Marine", WEST + 3, 14.0), ("zealot", WEST, 20.0)]
for name, x, y in TARGETS:
    tmpl(name, (x, y, 0.0), 0)
# an enemy Siege Tank (user: something for the Reaver to fight), north of the other targets on the west straight,
# facing them (the AI tank as the Miners' Bunker and the Zoo place it)
TANK = ("siegeTank", WEST, 36.0)
tmpl(TANK[0], (TANK[1], TANK[2], 0.0), 180)
ONE_OFF = {"Vulture_player", "Goliath_player", "Arclite_player", "reaver_vehicle_player", "dragoon_vehicle_player",
           "AR_hackConsole_floor", "siegeTank"} | {t[0] for t in TARGETS}   # not mirrored
# spawn room supplies along its north and south walls (mirrored)
for name, x in (("powHealth", -2.0), ("powGauss_2x", 0.0), ("powEnergy", 2.0)):
    for y in (sy1 - 0.8, sy0 + 0.8):
        tmpl(name, (x, y, 0))

# lights: (x, y, z, r, g, b, intensity, radius); the field is lit by the sun (compile_vehicles.bat)
blue = (0.75, 0.85, 1.0)
LIGHTS = [(0.0, y, 4.0) + blue + (8, 10) for y in (-3.5, 3.5)]               # spawn room
LIGHTS += [((CORR[0] + CORR[2]) / 2, 0.0, 4.0) + blue + (5, 7)]               # corridor
for x, y, z, r, g, b, i, rad in LIGHTS:
    ent("nod_static_light", (x, y, z), _color="%g %g %g" % (r, g, b), light=i, radius=rad)

# waypoints (MAPtoNPD): the track's centre line, linked both ways all round (for vehicle AI later)
mid = (R_OUT + R_IN) / 2
wp = [(cx + s * mid, y, 0.5) for s in (1,) for y in (-STRAIGHT, -STRAIGHT / 3, STRAIGHT / 3)]
wp += [p + (0.5,) for p in ring(mid, STRAIGHT, 0.0, 180.0, 8)]
wp += [(cx - mid, y, 0.5) for y in (STRAIGHT / 3, -STRAIGHT / 3)]
wp += [p + (0.5,) for p in ring(mid, -STRAIGHT, 180.0, 360.0, 8)][:-1]
links = []
for n, (x, y, z) in enumerate(wp, 1):
    ent("nod_npd_waypoint", (x, y, z), nod_authoring="npd-world-v1", nod_space="world", nod_waypoint_id=n, nod_flags=0)
for i in range(len(wp)):
    a, b = i + 1, (i + 1) % len(wp) + 1
    links += [(a, b), (b, a)]
for a, b in links:
    m = [(wp[a - 1][k] + wp[b - 1][k]) / 2 for k in range(3)]
    entities.append({"classname": "nod_npd_link", "nod_authoring": "npd-world-v1", "nod_space": "world",
                     "origin": "%g %g %g" % tuple(m[k] + OFFSET[k] for k in range(3)), "nod_from": str(a),
                     "nod_to": str(b), "nod_link_flags": "0"})


def tri_estimate(spacing):
    """upper-bound estimate: every face subdivided into spacing-sized quads (2 triangles each)"""
    tris = 0
    for pts, z0, z1, m in brushes:
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        w, d, h = max(xs) - min(xs), max(ys) - min(ys), z1 - z0
        cap = 2 * max(1, math.ceil(w / spacing)) * max(1, math.ceil(d / spacing))
        tris += 2 * cap
        for i in range(len(pts)):
            e = math.dist(pts[i], pts[(i + 1) % len(pts)])
            tris += 2 * max(1, math.ceil(e / spacing)) * max(1, math.ceil(h / spacing))
    return tris


TRI_TARGET = 1800   # MAPtoNIL rejected ~3100 on the Zoo; stay well below
# 128 at least: with the lit compile (compile_vehicles.bat) MAPtoNIL 0.6.8's lighting subdivision of the long rock
# walls produced stray triangles (vertices up to 60 m outside the level) at spacings 21..64; 128 is clean, as on the Zoo
SPACING = 128.0
while tri_estimate(SPACING) > TRI_TARGET and SPACING < 128:
    SPACING += 1.0


def symmetry_check():
    """every brush and placement has its mirror image across y = 0; the field's floors and block also across x = TRACK_CX"""
    key = lambda pts, z0, z1, m: (frozenset((round(x, 2), round(y, 2)) for x, y in pts), z0, z1, m)
    have = {key(*b) for b in brushes}
    for pts, z0, z1, m in brushes:
        assert key([(x, -y) for x, y in pts], z0, z1, m) in have, "brush without a y mirror: %r" % (pts,)
        if max(x for x, _ in pts) <= fx1 + 1e-6 and m != R:   # the walls differ: the east one has the corridor
            assert key([(2 * cx - x, y) for x, y in pts], z0, z1, m) in have, "brush without an x mirror: %r" % (pts,)
    spots = {(n, round(o[0], 3), round(o[1], 3)) for n, o, _ in PLACED if n not in ONE_OFF}
    for n, x, y in spots:
        assert (n, x, round(-y, 3)) in spots, "placement without a mirror: %s %g %g" % (n, x, y)


symmetry_check()

with open(OUT, "w", newline="\n") as f:
    f.write("// Vehicle test map (%s) - generated by make_vehicle_map.py, do not edit by hand\n" % NAME)
    f.write('{\n"classname" "worldspawn"\n"nod_light_spacing" "%g"\n' % SPACING)
    f.write('"nod_sky_model" "%s"\n"nod_sky_unmasked" "1"\n' % SKY_MODEL)
    for b in brushes:
        f.write(brush_text(b))
    f.write("}\n")
    for e in entities:
        f.write("{\n")
        for k, v in e.items():
            f.write('"%s" "%s"\n' % (k, v))
        f.write("}\n")

# GhostPC level script (src/levelscript.cpp): the player is created at the origin, already in the spawn room; the
# start only turns her to face the corridor (yaw 90 = -X)
with open(os.path.join(HERE, "out_levelscript.gpc"), "w", newline="\r\n") as f:
    f.write("# GhostPC level script for %s (generated by make_vehicle_map.py)\n" % NAME)
    f.write("player_start %g %g %g %g\n" % (OFFSET[0], OFFSET[1], OFFSET[2], 90))
    f.write("player2_start %g %g %g %g\n" % (OFFSET[0], 2.0 + OFFSET[1], OFFSET[2], 90))

# effect list: the placements' effects plus an original list's (only names in Misc\game.nfx)
nfx = open(os.path.join(GAME, "Misc", "game.nfx"), "rb").read().lower()
effects = []
for lst in ("2_2_2a_Threat_BaseUpper.nlfx", "1_2_1a_Miners_Bunker.nlfx"):   # (the second has the Siege Tank's shells)
    effects += [l.strip() for l in open(os.path.join(GAME, "Levels", lst), encoding="latin1") if l.strip()]
effects += ["tossTurretProj", "reaverScarabGlow", "scarabTrail", "exp_protTurretProj"]   # the Reaver's Scarab (proj_reaverScarab)
effects += ["dragoonProj", "dragoonProjExp", "dragoonChrg"]   # the Dragoon's shot (proj_dragoon, exp_dragoonProj)
effects += ["ProtBuildDam", "psiburn"]   # the Reaver's and Dragoon's damage burn (EFFECT4)
nfx += open(os.path.join(GAME, "Misc", "ghostpc.nfx"), "rb").read().lower()
for n in sorted({p[0] for p in PLACED}):
    seen, t = set(), GHOSTPC_TEMPLATES.get(n, n)
    while t and t.lower() not in seen:
        seen.add(t.lower())
        props = notdump.props(notdump.find(t))
        effects += [v for k, v in props if k.upper().startswith("EFFECT") and v]
        t = dict(props).get("basedon")
unique = []
for e in effects:
    if e.lower() not in [u.lower() for u in unique] and e.lower().encode() in nfx:
        unique.append(e)
with open(os.path.join(HERE, "out_effects.nlfx"), "w", newline="\r\n") as f:
    f.write("\n".join(unique) + "\n")

# overview from above (needs Pillow): grass, pavement, block, walls, Protoss rooms
try:
    from PIL import Image, ImageDraw
    S, X0, Y1 = 4, fx0 - 4, fy1 + 4
    img = Image.new("RGB", (int((SPAWN[2] + 4 - X0) * S), int((Y1 - fy0 + 4) * S)), (255, 255, 255))
    d = ImageDraw.Draw(img)
    colour = {G: (40, 170, 70), P: (128, 128, 128), MAT["block"]: (0, 0, 0), R: (90, 70, 60)}
    for pts, z0, z1, m in sorted(brushes, key=lambda b: b[2]):
        if z1 <= 0 or m in colour or z0 < H_ROOM:
            d.polygon([((x - X0) * S, (Y1 - y) * S) for x, y in pts], fill=colour.get(m, (255, 230, 0)), outline=(0, 0, 0))
    d.ellipse([(-X0 - 1) * S, (Y1 - 1) * S, (-X0 + 1) * S, (Y1 + 1) * S], fill=(255, 0, 0))   # the origin
    img.save(os.path.join(HERE, "vehicle_overview.png"))
except ImportError:
    pass

print("brushes %d, entities %d (lights %d, placements %d, waypoints %d, links %d)" % (
    len(brushes), len(entities), len(LIGHTS), len(PLACED), len(wp), len(links)))
print("light spacing %g -> triangle estimate %d (upper bound, limit 4096)" % (SPACING, tri_estimate(SPACING)))
print("symmetric about y = 0 (field and track also about x = %g); wrote %s" % (cx, OUT))
