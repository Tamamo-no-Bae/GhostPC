# make_release.py [suffix] -- refresh release/GhostPC_Patch from the current build, the installed Enemy Zoo and
# Protoss arena (with their menu images and sources) and GhostPC's own data files (assets), keep
# the previous zip as GhostPC_Patch_<its date><suffix>.zip, and write release/GhostPC_Patch.zip.
# The patch's game-derived files (Strings, Misc, Sounds, UI images) are not in the repository: they stay as they are
# in the local release/GhostPC_Patch folder. Update GhostPC_README.txt's changelog before running.
import datetime
import glob
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
ARENA = os.path.join(ROOT, "levels", "arena")
ARENA_LEVEL = "8_1_1a_Protoss_Arena"
ARENA_SRC = ["make_arena_map.py", "install_arena.py", "arena.map", "out_levelscript.gpc", "out_effects.nlfx",
             "arena_overview.png", "menu_shot.png", "README.txt"]
MENU_IMAGES = ["911a_MenuImage", "911a_MenuImageL", "811a_MenuImage", "811a_MenuImageL", "821a_MenuImage", "821a_MenuImageL",
               "831a_MenuImage", "831a_MenuImageL", "921a_MenuImage", "921a_MenuImageL"]
# the other GhostPC levels (installed by their install_*.py) and their sources: (level, folder, files, compile script)
MORE_LEVELS = [
    ("9_2_1a_Vehicle_Test", "vehicles", ["make_vehicle_map.py", "install_vehicles.py", "vehicle_test.map", "out_levelscript.gpc",
                                         "out_effects.nlfx", "vehicle_overview.png", "menu_shot.png"], "compile_vehicles.bat"),
    ("8_2_1a_Vehicle_Duel", "duel", ["make_duel_map.py", "install_duel.py", "duel.map", "out_levelscript.gpc", "out_effects.nlfx",
                                     "duel_overview.png", "menu_shot.png"], "compile_duel.bat"),
    ("8_3_1a_Blade_Arena", "bladearena", ["make_bladearena_map.py", "install_bladearena.py", "bladearena.map", "out_levelscript.gpc",
                                          "out_effects.nlfx", "bladearena_overview.png", "menu_shot.png"], "compile_bladearena.bat"),
]
# GhostPC's other files in the game folder (made by its scripts; game-relative globs). Not here: Misc\game.nls (the
# original string table, which the mission scripts also update: the build reads Strings\game.nls) and any change to an
# original level (the Sabotage Protoss test stays local).
GAME_FILES = [
    "3D/Anims/nova43_K_*.nad", "3D/Anims/szlachta_bindpose.nad",
    "3D/Materials/EchoDarkTemplar.dds", "3D/Materials/Echo_DT_New.dds", "3D/Materials/SzlachtaCombo.dds",
    "3D/Models/nova_DT43*.nod", "3D/Models/novaVulture_echo.nod", "3D/Models/reaver_shield.nod", "3D/Models/Tanagazj_shield.nod", "3D/Models/szlachta.nod",
    "3D/Motions/nova_psiblade.nms", "3D/Motions/Szlachta.nms",
    "Materials/Stone/GPC_Ruins_*.dds",
    "Misc/ghostpc.nfx", "Misc/ghostpc.nlx", "Misc/ghostpc_*.nlt", "Misc/Templates/ghostpc.not",
    "Misc/ghostsp.nsc", "Misc/ghostmpcoop.nsc", "Misc/ghostmpcomp.nsc", "Strings/game.nls",
    "Sounds/custom/**/*.wav",
]


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
    for ext in ("nil", "nsd", "npd", "gpc", "nlfx"):   # the installed arena (install_arena.py)
        copy(os.path.join(GAME, "Levels", "%s.%s" % (ARENA_LEVEL, ext)),
             os.path.join(PATCH, "Levels", "%s.%s" % (ARENA_LEVEL, ext)))
    for n in MENU_IMAGES:   # mission-select pictures (make_menu_images.py)
        copy(os.path.join(GAME, "UI", "Materials", n + ".dds"), os.path.join(PATCH, "UI", "Materials", n + ".dds"))
    for level, folder, srcs, bat_name in MORE_LEVELS:
        for ext in ("nil", "nsd", "npd", "gpc", "nlfx"):
            copy(os.path.join(GAME, "Levels", "%s.%s" % (level, ext)), os.path.join(PATCH, "Levels", "%s.%s" % (level, ext)))
        src_dir = os.path.join(ROOT, "levels", folder)
        for f in srcs:
            copy(os.path.join(src_dir, f), os.path.join(PATCH, "mapsrc", folder, f))
        lines = open(os.path.join(src_dir, bat_name), encoding="latin-1").read().splitlines()
        lines = ['set "SDK=%~dp0..\\sdk"' if l.startswith('set "SDK=') else l for l in lines]
        open(os.path.join(PATCH, "mapsrc", folder, bat_name), "w", encoding="latin-1", newline="\r\n").write(
            "\n".join(lines) + "\n")
    for pat in GAME_FILES:
        found = glob.glob(os.path.join(GAME, pat), recursive=True)
        if not found:
            raise SystemExit("make_release: nothing matches " + pat)
        for src in found:
            copy(src, os.path.join(PATCH, os.path.relpath(src, GAME)))
    for f in ARENA_SRC:
        copy(os.path.join(ARENA, f), os.path.join(PATCH, "mapsrc", "arena", f))
    # the compile script with the SDK looked for in mapsrc/sdk (shared with compile_zoo.bat) or NOD_SDK_DIR
    bat = open(os.path.join(ARENA, "compile_arena.bat"), encoding="latin-1").read().splitlines()
    bat = ['set "SDK=%~dp0..\\sdk"' if l.startswith('set "SDK=') else l for l in bat]
    os.makedirs(os.path.join(PATCH, "mapsrc", "arena"), exist_ok=True)
    open(os.path.join(PATCH, "mapsrc", "arena", "compile_arena.bat"), "w", encoding="latin-1", newline="\r\n").write(
        "\n".join(bat) + "\n")
    for d, ds, fs in os.walk(os.path.join(ROOT, "assets")):   # GhostPC's own data files (e.g. Materials\ghostpc.nsa)
        ds[:] = [x for x in ds if x != "source"]   # (assets\source: the source images, not game files)
        for f in fs:
            src = os.path.join(d, f)
            copy(src, os.path.join(PATCH, os.path.relpath(src, os.path.join(ROOT, "assets"))))
    for f in MAPSRC:
        copy(os.path.join(ZOO, f), os.path.join(PATCH, "mapsrc", f))
    copy(os.path.join(ROOT, "scripts", "notdump.py"), os.path.join(PATCH, "mapsrc", "notdump.py"))
    copy(os.path.join(ROOT, "re", "modifiers.txt"), os.path.join(PATCH, "mapsrc", "modifiers.txt"))

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
