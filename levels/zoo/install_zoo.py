"""Installs the compiled Enemy Zoo level (levels/zoo/out) into game/Levels and applies the post-compile patches that
OpenNOD 0.6.7 cannot express:

  * .nil: the ceiling was compiled with a placeholder texture (make_zoo_map.SKY_PLACEHOLDER, because the compiler rejects
    archived materials); its 32-byte shader name becomes the global sky material, so the game does not draw it and the
    sky model shows through. Its collision is untouched, so flyers stay in their rooms.
  * .nil: the Havok world bounds get 100 m of margin (see patch_bounds).
  * .nsd: music and environment fields copied from an original trailer (see patch_nsd_environment).
  * .nsd: track frames for placements that need them (the sniper boss's perches, see patch_nsd_frames).
  * .nsd: the 192-byte trailer ends with two empty 32-byte names (sky model, sky animation); existing levels put the
    sky model's name in both (e.g. MS_Sky_Refinery).
"""
import os
import re
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
NAME = "9_1_1a_Enemy_Zoo"
SKY_MODEL = "sky_Robota_02"   # clear blue skybox; all its materials are in the global 3dmaterials archive
OUT = os.path.join(HERE, "out")
# the game folder: GHOST_GAME_DIR, else ..\..\game (GhostPC workspace), else the parent folder (mapsrc in the
# prototype's folder, as the patch installs it)
GAME = os.environ.get("GHOST_GAME_DIR") or next(
    (g for g in (os.path.join(HERE, "..", "..", "game"), os.path.join(HERE, "..")) if os.path.exists(os.path.join(g, "Misc"))),
    os.path.join(HERE, ".."))
LEVELS = os.path.join(GAME, "Levels")


def field(name):
    b = name.encode("ascii")
    assert 0 < len(b) < 32
    return b + b"\0" * (32 - len(b))


def patch_nil(data, old, new):
    """rename one entry of the shader name table (32-byte NUL-padded names)"""
    pat = field(old)
    at = data.find(pat)
    if at < 0 or data.find(pat, at + 1) >= 0:
        raise SystemExit("shader name %s not found exactly once in the .nil" % old)
    return data[:at] + field(new) + data[at + 32:]


def physics_bounds_offset(d):
    """offset of the 6 physics-bound floats after the sectors (NIL v35 no-terrain layout, nodsdk-docs formats/nil.md)"""
    n = struct.unpack_from("<I", d, 92)[0]
    o = 96 + n * 32
    o += 2 + struct.unpack_from("<H", d, o)[0] * 32                     # env maps
    sectors = struct.unpack_from("<I", d, o)[0]
    o += 4
    for _ in range(sectors):
        o += 16                                                          # flags, center
        o += 1 + d[o] * 16                                               # boundary planes
        pools = d[o]
        o += 1
        for _ in range(pools):
            tbn, nv = d[o], struct.unpack_from("<I", d, o + 1)[0] & 0xFFFF
            o += 5 + nv * 36 * (2 if tbn == 1 else 1)
        o += 2 + struct.unpack_from("<H", d, o)[0] * 47                 # surface groups
        o += 4 + struct.unpack_from("<I", d, o)[0] * 2                  # indices
        o += 1 + d[o] * 68                                               # things
        o += 2 + struct.unpack_from("<H", d, o)[0] * 72                 # static lights
        o += 2 + struct.unpack_from("<H", d, o)[0] * 32                 # env refs
        o += 6                                                           # tail
    if struct.unpack_from("<H", d, o)[0] != 0:
        raise SystemExit(".nil has terrain; layout not handled")
    return o + 4                                                         # skip terrainCount, physicsSetup


def patch_bounds(data, margin):
    """Widen the Havok world bounds. MAPtoNIL fits them 1 m around the geometry; the player's rigid body is created at
    the origin (a corner of this level) before it is moved to the spawn, which raised tPlayer::OutsideHavokWorldEvent
    and killed Nova on frame 3. Original levels keep 70-200 m of margin."""
    o = physics_bounds_offset(data)
    b = list(struct.unpack_from("<6f", data, o))
    if not (b[0] < b[3] and b[1] < b[4] and b[2] < b[5]):
        raise SystemExit("unexpected physics bounds %r" % b)
    new = [b[0] - margin, b[1] - margin, b[2] - margin, b[3] + margin, b[4] + margin, b[5] + margin]
    return data[:o] + struct.pack("<6f", *new) + data[o + 24:], b, new


ENV_SOURCE = "1_2_1b_Miners_Bunker.nsd"   # its trailer has no sound records (counts 0), like MAPtoNSD's output


def patch_nsd_environment(data):
    """MAPtoNSD writes the trailer's music names and environment fields as zeros; with them Nova's position became NaN
    on the first frame (tPlayer::OutsideHavokWorldEvent killed her even 35 m above the level). Copy bytes 16..127 of
    an original trailer (music track, ambient loop, flags, distances), keeping our zero record counts (bytes 0..15)."""
    src = open(os.path.join(LEVELS, ENV_SOURCE), "rb").read()[-192:]
    if struct.unpack_from("<4I", src, 0) != (0, 0, 0, 0) or struct.unpack_from("<4I", data[-192:], 0) != (0, 0, 0, 0):
        raise SystemExit("trailer record counts are not zero; refusing to mix trailers")
    t = len(data) - 192
    return data[:t + 16] + src[16:128] + data[t + 128:]


def patch_nsd(data, sky):
    tail = data[-64:]
    if tail.strip(b"\0") and tail != field(sky) * 2:
        print("note: .nsd sky fields were not empty: %r" % tail.rstrip(b"\0"))
    return data[:-64] + field(sky) + field(sky)


def patch_nsd_frames(data, frames):
    """Give placements track frames (MAPtoNSD writes none). NSD v11 (maptonsd.cpp): 72-byte header, uint32 0, archive
    count + 32-byte names, placement count; per placement a modifier count, that many (uint32 id + 32-byte value)
    modifiers (the first is the template name) and a 20-byte tail that starts with the frame count, followed by
    count x 28-byte frames (rotation PYR, position, uint32 0; tThing::TrackToFrame). frames: {template: [(x, y, z)]}."""
    o = 72 + 4
    archives = struct.unpack_from("<I", data, o)[0]
    o += 4 + 32 * archives
    count = struct.unpack_from("<I", data, o)[0]
    o += 4
    out, done = bytearray(data[:o]), set()
    for _ in range(count):
        mods = struct.unpack_from("<I", data, o)[0]
        end = o + 4 + 36 * mods
        name = data[o + 8:o + 40].split(b"\0")[0].decode("latin1")
        if struct.unpack_from("<I", data, end)[0] != 0:
            raise SystemExit("placement %s already has frames" % name)
        out += data[o:end]
        if name in frames:
            out += struct.pack("<I", len(frames[name]))
            for x, y, z in frames[name]:
                out += struct.pack("<6fI", 0, 0, 0, x, y, z, 0)
            out += data[end + 4:end + 20]
            done.add(name)
        else:
            out += data[end:end + 20]
        o = end + 20
    missing = set(frames) - done
    if missing:
        raise SystemExit("no placement for track frames: " + ", ".join(sorted(missing)))
    return bytes(out + data[o:])


def read_frames():
    frames = {}
    for line in open(os.path.join(HERE, "out_frames.txt")):
        if line.strip() and not line.startswith("#"):
            name, x, y, z = line.split()
            frames.setdefault(name, []).append((float(x), float(y), float(z)))
    return frames


def main():
    # the placeholder/sky names as defined in the generator (read, not imported: importing would rewrite zoo.map)
    src = open(os.path.join(HERE, "make_zoo_map.py"), encoding="utf-8").read()
    m = re.search(r'^SKY_PLACEHOLDER, SKY_MATERIAL = "(\w+)", "(\w+)"', src, re.M)
    if not m:
        raise SystemExit("SKY_PLACEHOLDER/SKY_MATERIAL not found in make_zoo_map.py")
    placeholder, sky_material = m.groups()

    files = {ext: os.path.join(OUT, NAME + ext) for ext in (".nil", ".nsd", ".npd")}
    missing = [f for f in files.values() if not os.path.exists(f)]
    if missing:
        raise SystemExit("missing compiler output: " + ", ".join(missing))

    nil = patch_nil(open(files[".nil"], "rb").read(), placeholder, sky_material)
    nil, old_b, new_b = patch_bounds(nil, 100.0)
    print("physics bounds %s -> %s" % ([round(v, 1) for v in old_b], [round(v, 1) for v in new_b]))
    nsd = patch_nsd(patch_nsd_environment(open(files[".nsd"], "rb").read()), SKY_MODEL)
    frames = read_frames()
    nsd = patch_nsd_frames(nsd, frames)
    print("track frames: " + ", ".join("%s %d" % (k, len(v)) for k, v in frames.items()))
    open(os.path.join(LEVELS, NAME + ".nil"), "wb").write(nil)
    open(os.path.join(LEVELS, NAME + ".nsd"), "wb").write(nsd)
    shutil.copyfile(files[".npd"], os.path.join(LEVELS, NAME + ".npd"))
    # GhostPC level script (Tanagazj trigger), read by src/levelscript.cpp
    shutil.copyfile(os.path.join(HERE, "out_levelscript.gpc"), os.path.join(LEVELS, NAME + ".gpc"))
    # effect list (which effects of Misc\game.nfx this level loads), written by make_zoo_map.py
    shutil.copyfile(os.path.join(HERE, "out_effects.nlfx"), os.path.join(LEVELS, NAME + ".nlfx"))
    # the cut Goliath boss's motion set (Goliath_222c's plus placeholders), named by a map-only override
    shutil.copyfile(os.path.join(HERE, "zoo_goliath_222c.nms"), os.path.join(GAME, "3D", "Motions", "zoo_goliath_222c.nms"))
    # the Mutalisk's flight path (fly_path in the level script), written by make_zoo_map.py
    shutil.copyfile(os.path.join(HERE, "out_mutalisk_path.nad"), os.path.join(GAME, "3D", "Anims", "zoo_mutaliskFlyAround.nad"))
    print("installed %s.nil/.nsd/.npd/.gpc/.nlfx into %s (ceiling %s -> %s, sky model %s)" % (
        NAME, os.path.abspath(LEVELS), placeholder, sky_material, SKY_MODEL))


if __name__ == "__main__":
    main()
