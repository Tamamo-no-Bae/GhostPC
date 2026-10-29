"""Installs the compiled Protoss arena (levels/arena/out) into game/Levels with the Zoo's post-compile patches
(levels/zoo/install_zoo.py): Havok world bounds with 100 m of margin, the .nsd trailer's environment fields from an
original level and its sky names. The arena has a ceiling everywhere, so there is no sky placeholder to rename.
Also copies the GhostPC level script (spawns, respawn points) and the effect list.
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "zoo"))
sys.path.insert(0, os.path.join(HERE, ".."))   # the patch's mapsrc: install_zoo.py next to the arena folder
import install_zoo as z  # noqa: E402  (the patch functions; importing does not run its main)

NAME = "8_1_1a_Protoss_Arena"
OUT = os.path.join(HERE, "out")


def main():
    files = {ext: os.path.join(OUT, NAME + ext) for ext in (".nil", ".nsd", ".npd")}
    missing = [f for f in files.values() if not os.path.exists(f)]
    if missing:
        raise SystemExit("missing compiler output (run compile_arena.bat first): " + ", ".join(missing))
    nil, old_b, new_b = z.patch_bounds(open(files[".nil"], "rb").read(), 100.0)
    print("physics bounds %s -> %s" % ([round(v, 1) for v in old_b], [round(v, 1) for v in new_b]))
    nsd = z.patch_nsd(z.patch_nsd_environment(open(files[".nsd"], "rb").read()), z.SKY_MODEL)
    open(os.path.join(z.LEVELS, NAME + ".nil"), "wb").write(nil)
    open(os.path.join(z.LEVELS, NAME + ".nsd"), "wb").write(nsd)
    shutil.copyfile(files[".npd"], os.path.join(z.LEVELS, NAME + ".npd"))
    shutil.copyfile(os.path.join(HERE, "out_levelscript.gpc"), os.path.join(z.LEVELS, NAME + ".gpc"))
    shutil.copyfile(os.path.join(HERE, "out_effects.nlfx"), os.path.join(z.LEVELS, NAME + ".nlfx"))
    print("installed %s.nil/.nsd/.npd/.gpc/.nlfx into %s" % (NAME, os.path.abspath(z.LEVELS)))


if __name__ == "__main__":
    main()
