# make_release.py [suffix] -- refresh release/GhostPC_Patch from the current build and the installed Enemy Zoo, keep
# the previous zip as GhostPC_Patch_<its date><suffix>.zip, and write release/GhostPC_Patch.zip.
# The patch's game-derived files (Strings, Misc, Sounds, UI images) are not in the repository: they stay as they are
# in the local release/GhostPC_Patch folder. Update GhostPC_README.txt's changelog before running.
import datetime
import os
import shutil
import sys
import zipfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
REL = os.path.join(ROOT, "release")
PATCH = os.path.join(REL, "GhostPC_Patch")
GAME = os.environ.get("GHOST_GAME_DIR", os.path.join(ROOT, "game"))
ZOO = os.path.join(ROOT, "levels", "zoo")
LEVEL = "9_1_1a_Enemy_Zoo"
MAPSRC = ["make_zoo_map.py", "zoo_overview.py", "install_zoo.py", "make_menu_images.py", "zoo.map", "out_effects.nlfx",
          "out_frames.txt", "out_levelscript.gpc", "out_mutalisk_path.nad", "zoo_goliath_222c.nms", "ZOO_OVERVIEW.md",
          "zoo_overview.png", "zoo_overview.svg"]


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)


def main():
    suffix = sys.argv[1] if len(sys.argv) > 1 else ""
    copy(os.path.join(ROOT, "build", "GhostPC.exe"), os.path.join(PATCH, "GhostPC.exe"))
    for ext in ("nil", "nsd", "npd", "gpc", "nlfx"):   # the installed level (install_zoo.py), not levels/zoo/out
        copy(os.path.join(GAME, "Levels", "%s.%s" % (LEVEL, ext)), os.path.join(PATCH, "Levels", "%s.%s" % (LEVEL, ext)))
    copy(os.path.join(GAME, "3D", "Anims", "zoo_mutaliskFlyAround.nad"),
         os.path.join(PATCH, "3D", "Anims", "zoo_mutaliskFlyAround.nad"))
    copy(os.path.join(ZOO, "zoo_goliath_222c.nms"), os.path.join(PATCH, "3D", "Motions", "zoo_goliath_222c.nms"))
    for f in MAPSRC:
        copy(os.path.join(ZOO, f), os.path.join(PATCH, "mapsrc", f))
    copy(os.path.join(ROOT, "scripts", "notdump.py"), os.path.join(PATCH, "mapsrc", "notdump.py"))
    copy(os.path.join(ROOT, "scripts", "modifiers.txt"), os.path.join(PATCH, "mapsrc", "modifiers.txt"))

    out = os.path.join(REL, "GhostPC_Patch.zip")
    if os.path.exists(out):   # keep the previous release under its own date
        day = datetime.date.fromtimestamp(os.path.getmtime(out)).isoformat()
        keep = os.path.join(REL, "GhostPC_Patch_%s%s.zip" % (day, suffix))
        n = 0
        while os.path.exists(keep):
            n += 1
            keep = os.path.join(REL, "GhostPC_Patch_%s%s_%d.zip" % (day, suffix, n))
        os.replace(out, keep)
        print("previous release kept as", os.path.basename(keep))
    files = sorted(os.path.relpath(os.path.join(d, f), PATCH).replace(os.sep, "/")
                   for d, _, fs in os.walk(PATCH) for f in fs)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for f in files:
            z.write(os.path.join(PATCH, f), f)
    print("wrote %s: %d files, %d KB" % (out, len(files), os.path.getsize(out) // 1024))


if __name__ == "__main__":
    main()
