r"""Generates the Quake III MAP for the Psi Blade arena (level 8_3_1a_Blade_Arena), a competitive map for two players
with the Psi Blade only, from the user's layout drawing. Protoss ruins (mossy stone), open to the sky.

Layout (game units = metres, X east, Y north, Z up; north is up), mirror-symmetric about both axes through the
arena's centre C:

                   .------------.
                .'      H       '.
    +---------+'                  '+---------+
    | P       |        .--.        |       P |
    |  spawn       P  (    )  P        spawn |
    | P       |        '--'        |       P |
    +---------+.                  .+---------+
                '.      H       .'
                   '------------'

- a round arena (radius R_ARENA) around a round pillar (R_PILLAR), walled by a ring of broken stone;
- a spawn area east and west, open into the arena (2 x SPAWN_HY wide);
- psi (P) in each spawn's outer corners and either side of the pillar, health (H) north and south.

The centre is at C = (CX, 0), not the world origin: the game creates both players at the origin, which would be inside
the pillar; with CX = 30 the origin is in the west spawn area (the level script then moves the players to their
starts). Both players start with the Psi Blade and no guns (level script psiblade_start / noweapon).

Run:  python make_bladearena_map.py   ->  writes bladearena.map, out_levelscript.gpc and out_effects.nlfx next to this
script and bladearena_overview.png. Then compile_bladearena.bat (the user) and install_bladearena.py.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "bladearena.map")
NAME = "8_3_1a_Blade_Arena"
GAME = os.environ.get("GHOST_GAME_DIR") or next(
    (g for g in (os.path.join(HERE, "..", "..", "game"), os.path.join(HERE, ".."), os.path.join(HERE, "..", ".."))
     if os.path.exists(os.path.join(g, "Misc"))),
    os.path.join(HERE, ".."))
os.environ.setdefault("GHOST_GAME_DIR", GAME)   # for notdump
sys.path.insert(0, os.path.join(HERE, "..", "..", "scripts"))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, HERE)
import notdump  # noqa: E402  (Global.not reader)

T = 1.0                       # wall thickness
OFFSET = (0.0, 0.0, -0.3)     # floors 0.3 m below the origin, as on the other GhostPC levels
CX = 30.0                     # the arena's centre (CX, 0): the origin falls in the west spawn area
R_ARENA = 20.0                # the arena's inner radius (the drawing: 40 m across)
R_PILLAR = 5.0                # the central pillar's radius
SPAWN_HY = 6.5                # half width of the spawn areas (their opening into the arena)
SPAWN_X = 34.0                # the spawn areas' end walls, from the centre (interior)
H_WALL, H_PILLAR = 6.0, 8.0   # wall and pillar heights
SEGS = 48                     # segments per full circle (walls and floor)
SKY_MODEL = "sky_Robota_02.nod"

# Protoss ruins: copies of 3_1_1a_Siege_Pylon's ruins textures as plain map textures (Materials\Stone\GPC_Ruins_*,
# scripts/make_ruins_textures.py): MAPtoNIL refuses the AR_Ruins_* materials themselves ("editor material has no native
# bank definition")
MAT = {"floor": "GPC_Ruins_Squares", "floor_spawn": "GPC_Ruins_Paving", "wall": "GPC_Ruins_Walls",
       "wall_spawn": "GPC_Ruins_Brick", "pillar": "GPC_Ruins_Pillar"}
TILE = {"floor": 4.0, "floor_spawn": 4.0, "wall": 6.0, "wall_spawn": 6.0, "pillar": 4.0}   # metres per repeat


def dds_width(name):
    """texture width from its DDS header (MaterialDirs.txt, and 3D\\Materials where these ruins textures are)"""
    dirs = open(os.path.join(GAME, "MaterialDirs.txt"), encoding="latin1").read().split() + ["3D\\Materials"]
    for d in dirs:
        f = os.path.join(GAME, *d.split(chr(92)), name + ".dds")
        if os.path.exists(f):
            with open(f, "rb") as h:
                hdr = h.read(20)
            return int.from_bytes(hdr[16:20], "little")
    raise SystemExit("texture not found: " + name)


TEX_SCALE = {m: TILE[k] / dds_width(m) for k, m in MAT.items()}

# ---------------------------------------------------------------------------------------------------------------
# Geometry: brushes are convex prisms, (polygon [(x, y) counter-clockwise], z0, z1, material)
brushes = []
GRID = 64


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
    x0, x1 = sorted((x0, x1))
    y0, y1 = sorted((y0, y1))
    prism([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], z0, z1, mat)


def arc(r, a0, a1, n):
    """n + 1 points on the circle of radius r around the centre, angles a0 .. a1 (degrees)"""
    return [(CX + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)), r * math.sin(math.radians(a0 + (a1 - a0) * i / n)))
            for i in range(n + 1)]


def arc_segments(a0, a1):
    """segments for an arc of a1 - a0 degrees at about SEGS per full circle"""
    return max(1, round((a1 - a0) / 360.0 * SEGS))


# Built as the vehicle track is: floor pieces that meet edge to edge, and nothing standing on a floor brush (walls and
# pillar go down to the floor's underside, z -1, beside the floor pieces). MAPtoNIL refused the earlier versions: one
# floor disc under everything ("CSG vertices merge at binary32 precision", walls a hair past its chords; then, with the
# walls on it, "CSG polygon cannot be triangulated": its top face minus the wall ring and the pillar, a ring with a hole).
#
# One list of angles serves the pillar, the floor and the wall ring, so their corners are the same points: the north
# and south arcs (where the ring wall stands) and the east and west openings (where the spawn areas join), split at
# +-aw / 180 +- aw, the angles at which the ring's inner circle reaches y = +-SPAWN_HY.
aw = math.degrees(math.asin(SPAWN_HY / R_ARENA))
n_arc, n_open = arc_segments(aw, 180 - aw), arc_segments(-aw, aw)
ANGLES = ([-aw + 2 * aw * i / n_open for i in range(n_open)] + [aw + (180 - 2 * aw) * i / n_arc for i in range(n_arc)] +
          [180 - aw + 2 * aw * i / n_open for i in range(n_open)] + [180 + aw + (180 - 2 * aw) * i / n_arc for i in range(n_arc)])
ANGLES.append(ANGLES[0] + 360)


def pt(r, a):
    return (CX + r * math.cos(math.radians(a)), r * math.sin(math.radians(a)))


XI = math.sqrt(R_ARENA ** 2 - SPAWN_HY ** 2)                 # the ring's inner corner at the openings (x from CX)
YO = SPAWN_HY + T
XO = math.sqrt((R_ARENA + T) ** 2 - YO ** 2)                 # its outer corner, on the side walls' outer face
ao = math.degrees(math.asin(YO / (R_ARENA + T)))
# the pillar, from the floor's underside up
prism([pt(R_PILLAR, a) for a in ANGLES[:-1]], -1.0, H_PILLAR, MAT["pillar"])
# the floor between the pillar and the ring: one piece per angle step
for a0, a1 in zip(ANGLES, ANGLES[1:]):
    prism([pt(R_PILLAR, a0), pt(R_ARENA, a0), pt(R_ARENA, a1), pt(R_PILLAR, a1)], -1.0, 0.0, MAT["floor"])
# the wall ring on the north and south arcs (the end segments finish on the side walls' faces: inner corner at
# (XI, SPAWN_HY), outer at (XO, SPAWN_HY + T))
for k0 in (n_open, 2 * n_open + n_arc):
    arc_a = ANGLES[k0:k0 + n_arc + 1]
    outer = [pt(R_ARENA + T, a) for a in arc_a]
    north = arc_a[0] < 180
    outer[0] = pt(R_ARENA + T, ao if north else 180 + ao)
    outer[-1] = pt(R_ARENA + T, 180 - ao if north else 360 - ao)
    for i in range(n_arc):
        prism([pt(R_ARENA, arc_a[i]), outer[i], outer[i + 1], pt(R_ARENA, arc_a[i + 1])], -1.0, H_WALL, MAT["wall"])
# the openings: the strip of floor between the ring's circle and the spawn floor (an arc and its chord at x = +-XI),
# then the spawn areas: floor to the end wall, side walls from the ring's ends (inner edge from XI, outer from XO)
for k0, s in ((0, 1), (n_open + n_arc, -1)):
    x = lambda d: CX + s * d
    prism([pt(R_ARENA, a) for a in ANGLES[k0:k0 + n_open + 1]], -1.0, 0.0, MAT["floor"])
    box(x(XI), -SPAWN_HY, -1.0, x(SPAWN_X), SPAWN_HY, 0.0, MAT["floor_spawn"])
    box(x(SPAWN_X), -SPAWN_HY, -1.0, x(SPAWN_X + T), SPAWN_HY, H_WALL, MAT["wall_spawn"])            # end wall
    for sy in (1, -1):
        prism([(x(XI), sy * SPAWN_HY), (x(SPAWN_X + T), sy * SPAWN_HY), (x(SPAWN_X + T), sy * YO), (x(XO), sy * YO)],
              -1.0, H_WALL, MAT["wall_spawn"])   # side wall
assert abs(pt(R_ARENA, aw)[0] - (CX + XI)) < 1e-9 and XI < XO, "the ring and the spawn areas do not meet"


def brush_text(b):
    pts, z0, z1, m = b
    f = lambda v: ("%.6f" % v).rstrip("0").rstrip(".")
    tex = " nod/%s 0 0 0 %.6g %.6g 0 0 0" % (m, TEX_SCALE[m], TEX_SCALE[m])
    p = lambda x, y, z: "( %s %s %s )" % (f(x + OFFSET[0]), f(y + OFFSET[1]), f(z + OFFSET[2]))
    a, b2, c = pts[0], pts[1], pts[2]
    lines = [p(*a, z0) + " " + p(*b2, z0) + " " + p(*c, z0) + tex,     # bottom (normal -z)
             p(*a, z1) + " " + p(*c, z1) + " " + p(*b2, z1) + tex]     # top (+z)
    for i in range(len(pts)):
        u, v = pts[i], pts[(i + 1) % len(pts)]
        lines.append(p(*u, z0) + " " + p(*u, z1) + " " + p(*v, z1) + tex)
    return "{\n" + "\n".join(lines) + "\n}\n"


def check_brushes():
    """convex and counter-clockwise (the 1/64 snap allows a hair of slack on the long arcs)"""
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


def tmpl(name, origin, yaw=0):
    """native template placement (MAPtoNSD: case-sensitive Global.not names; ghostpc.not in every archive list)"""
    if not any(nm == name for nm, _ in notdump.idx):
        raise SystemExit("template name case: " + name)
    PLACED.append((name, tuple(origin), yaw))
    ent("nod_template_instance", origin, nod_authoring="map-template-v1", nod_template=name,
        nod_template_archive="global.not", nod_template_archives="ghostpc.not", nod_template_type=template_type(name),
        angles="0 %g 0" % yaw)


def four(name, dx, dy):
    """a placement at (CX +- dx, +-dy): all its mirror images"""
    for x in {dx, -dx}:
        for y in {dy, -dy}:
            tmpl(name, (CX + x, y, 0.0))


# player starts: player 1 in the east spawn area, player 2 in the west, facing the arena (level-script yaw: 0 = +Y,
# 90 = -X)
START = 29.0
tmpl("ghost", (CX + START, 0.0, 0.1), 270)   # the editor's player placement (MAPtoNSD writes no player start)
# psi in the spawn areas' outer corners and either side of the pillar; health north and south
four("powEnergy", SPAWN_X - 1.2, SPAWN_HY - 1.2)
four("powEnergy", R_PILLAR + 3.0, 0.0)
four("powHealth", 0.0, R_ARENA - 3.5)

# competitive respawns (level script "respawn"): three in each spawn area
RESPAWNS = [(START, 0.0), (START - 3.0, 3.5), (START - 3.0, -3.5)]

# no static lights: the sun lights it (compile_bladearena.bat); waypoints: a ring around the pillar, linked both ways
mid = (R_ARENA + R_PILLAR) / 2
wp = [(x, y, 0.5) for x, y in arc(mid, 0, 360, 12)[:-1]]
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

SPACING = 128.0   # as the other open-sky levels (MAPtoNIL 0.6.8's lighting subdivision; see make_vehicle_map.py)


def symmetry_check():
    """every brush and pickup has its mirror images across x = CX and y = 0"""
    key = lambda pts, z0, z1, m: (frozenset((round(x, 2), round(y, 2)) for x, y in pts), z0, z1, m)
    have = {key(*b) for b in brushes}
    for pts, z0, z1, m in brushes:
        for mirror in (lambda x, y: (2 * CX - x, y), lambda x, y: (x, -y)):
            assert key([mirror(x, y) for x, y in pts], z0, z1, m) in have, "brush without a mirror: %r %s" % (pts, m)
    spots = {(n, round(o[0], 3), round(o[1], 3)) for n, o, _ in PLACED if n != "ghost"}
    for n, x, y in spots:
        assert (n, round(2 * CX - x, 3), y) in spots and (n, x, round(-y, 3)) in spots, "pickup without a mirror: %s %g %g" % (n, x, y)


symmetry_check()

with open(OUT, "w", newline="\n") as f:
    f.write("// Psi Blade arena (%s) - generated by make_bladearena_map.py, do not edit by hand\n" % NAME)
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

# GhostPC level script (src/levelscript.cpp): starts, respawns, the Psi Blade for both and no guns
with open(os.path.join(HERE, "out_levelscript.gpc"), "w", newline="\r\n") as f:
    f.write("# GhostPC level script for %s (generated by make_bladearena_map.py)\n" % NAME)
    f.write("player_start %g %g %g %g\n" % (CX + START + OFFSET[0], OFFSET[1], OFFSET[2], 90))
    f.write("player2_start %g %g %g %g\n" % (CX - START + OFFSET[0], OFFSET[1], OFFSET[2], 270))
    for dx, y in RESPAWNS:
        for s, yaw in ((1, 90), (-1, 270)):
            f.write("respawn %g %g %g %g\n" % (CX + s * dx + OFFSET[0], y + OFFSET[1], OFFSET[2], yaw))
    f.write("psiblade_start\n")
    for w in ("pistol", "gauss", "flamethrower"):
        f.write("noweapon %s\n" % w)
    # (user) no equipment either: every inventory item to 0 (grenades, mines, Firefly, Mantis)
    for item in ("canister", "smokegrenade", "stickygrenade", "hellfiregrenade", "spidermine", "firefly", "mantis"):
        f.write("inventory %s 0\n" % item)

# effect list: an original list's, the pickups', and the Psi Blade's (blade, plunge impact; only names in game.nfx and
# ghostpc.nfx)
nfx = (open(os.path.join(GAME, "Misc", "game.nfx"), "rb").read() + open(os.path.join(GAME, "Misc", "ghostpc.nfx"), "rb").read()).lower()
effects = [l.strip() for l in open(os.path.join(GAME, "Levels", "2_2_2a_Threat_BaseUpper.nlfx"), encoding="latin1") if l.strip()]
effects += ["psiblade", "psiblade2", "psibladeNova", "psibladeNovaGlow", "psibladeNovaBeam", "superdiveshock",
            "superdiveshock0", "superdiveshock1", "psibladeShock", "psibladeImpact", "bladeDive", "diveshock",
            "diveshock0", "diveshock1", "bladeshock", "dragoonProjExp", "dragoonProjExp0"]
for n in sorted({p[0] for p in PLACED}):
    seen, t = set(), n
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

# overview from above (needs Pillow)
try:
    from PIL import Image, ImageDraw
    S, X0, X1, Y1 = 10, CX - SPAWN_X - 4, CX + SPAWN_X + 4, R_ARENA + 4
    img = Image.new("RGB", (int((X1 - X0) * S), int(2 * Y1 * S)), (255, 255, 255))
    d = ImageDraw.Draw(img)
    colour = {MAT["floor"]: (128, 128, 128), MAT["floor_spawn"]: (150, 140, 110), MAT["wall"]: (0, 0, 0),
              MAT["wall_spawn"]: (0, 0, 0), MAT["pillar"]: (30, 30, 30)}
    for pts, z0, z1, m in sorted(brushes, key=lambda b: b[2]):
        d.polygon([((x - X0) * S, (Y1 - y) * S) for x, y in pts], fill=colour[m])
    dot = {"powEnergy": (60, 80, 230), "powHealth": (230, 30, 30), "ghost": (255, 220, 0)}
    for n, o, _ in PLACED:
        d.ellipse([(o[0] - 0.6 - X0) * S, (Y1 - o[1] - 0.6) * S, (o[0] + 0.6 - X0) * S, (Y1 - o[1] + 0.6) * S], fill=dot[n])
    d.ellipse([(-0.4 - X0) * S, (Y1 - 0.4) * S, (0.4 - X0) * S, (Y1 + 0.4) * S], fill=(255, 255, 255))   # the origin
    img.save(os.path.join(HERE, "bladearena_overview.png"))
except ImportError:
    pass

print("brushes %d, entities %d (placements %d, waypoints %d, links %d)" % (len(brushes), len(entities), len(PLACED), len(wp), len(links)))
print("symmetric about x = %g and y = 0; wrote %s" % (CX, OUT))
