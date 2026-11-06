"""Installs the compiled vehicle test map (levels/vehicles/out) into game/Levels with the Zoo's post-compile patches
(levels/zoo/install_zoo.py): Havok world bounds with 100 m of margin, the .nsd trailer's environment fields from an
original level. The sky model and its unmasked flag come from the map (MAPtoNSD).
Also copies the GhostPC level script (the start) and the effect list.
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "zoo"))
sys.path.insert(0, os.path.join(HERE, ".."))   # the patch's mapsrc: install_zoo.py next to the vehicles folder
import install_zoo as z  # noqa: E402  (the patch functions; importing does not run its main)

NAME = "9_2_1a_Vehicle_Test"
OUT = os.path.join(HERE, "out")


def main():
    files = {ext: os.path.join(OUT, NAME + ext) for ext in (".nil", ".nsd", ".npd")}
    missing = [f for f in files.values() if not os.path.exists(f)]
    if missing:
        raise SystemExit("missing compiler output (run compile_vehicles.bat first): " + ", ".join(missing))
    nil, old_b, new_b = z.patch_bounds(open(files[".nil"], "rb").read(), 100.0)
    print("physics bounds %s -> %s" % ([round(v, 1) for v in old_b], [round(v, 1) for v in new_b]))
    nsd = z.patch_nsd_environment(open(files[".nsd"], "rb").read())
    open(os.path.join(z.LEVELS, NAME + ".nil"), "wb").write(nil)
    open(os.path.join(z.LEVELS, NAME + ".nsd"), "wb").write(nsd)
    shutil.copyfile(files[".npd"], os.path.join(z.LEVELS, NAME + ".npd"))
    shutil.copyfile(os.path.join(HERE, "out_levelscript.gpc"), os.path.join(z.LEVELS, NAME + ".gpc"))
    shutil.copyfile(os.path.join(HERE, "out_effects.nlfx"), os.path.join(z.LEVELS, NAME + ".nlfx"))
    print("installed %s.nil/.nsd/.npd/.gpc/.nlfx into %s" % (NAME, os.path.abspath(z.LEVELS)))


if __name__ == "__main__":
    main()
