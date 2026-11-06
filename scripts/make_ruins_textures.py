# make_ruins_textures.py -- the Protoss ruins textures as map textures for GhostPC levels (the Psi Blade arena,
# levels/bladearena): copies of 3D\Materials\AR_Ruins_* / AR_RuinBridge_* as Materials\Stone\GPC_Ruins_*.dds, in game\
# and assets\.
# Why copies under new names: MAPtoNIL takes a map surface either as a plain texture in one of the Materials folders
# (what the other GhostPC maps use: AR_RockWall, MS_Asphalt...) or as an archived material with a native bank
# definition. The ruins are editor materials of 3dMaterials.nsa with their textures in 3D\Materials, which it refuses
# ("editor material has no native bank definition", for AR_Ruins_Pillar and AR_Ruins_BrokenWall alike); under their
# own names it would find those materials first. At run time a surface named after a texture with no material
# definition loads the texture (as AR_RockWall does). Safe to run again.
import os
import shutil

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
GAME = os.environ.get("GHOST_GAME_DIR", os.path.join(ROOT, "game"))
COPIES = {"AR_Ruins_Squares": "GPC_Ruins_Squares", "AR_RuinBridge_Top": "GPC_Ruins_Paving",
          "AR_Ruins_Walls": "GPC_Ruins_Walls", "AR_Ruins_Brick": "GPC_Ruins_Brick",
          "AR_Ruins_BrokenWall": "GPC_Ruins_BrokenWall", "AR_Ruins_Pillar": "GPC_Ruins_Pillar"}

for src, dst in COPIES.items():
    for base in (GAME, os.path.join(ROOT, "assets")):
        out = os.path.join(base, "Materials", "Stone")
        os.makedirs(out, exist_ok=True)
        shutil.copyfile(os.path.join(GAME, "3D", "Materials", src + ".dds"), os.path.join(out, dst + ".dds"))
    print("Materials\\Stone\\%s.dds <- 3D\\Materials\\%s.dds" % (dst, src))
