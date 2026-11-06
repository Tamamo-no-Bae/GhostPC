# make_shield_models.py -- write GhostPC's Protoss shield shells (game/3D/Models/<model>_shield.nod).
#
# A shielded unit's visible shield is a second model attached to its own: cEffectPsiShield::Init loads
# "<its model name>_shield" (renderModel_LoadInstance) and attaches it to the unit's model instance
# (cModelInst::SetAttachedModel); TurnOn shows it. The build has one for each shielded unit that shipped (Zealot,
# dragoon, hightemplar, observer, photoncannon, purifier, pylon): the unit's mesh on the same skeleton, one material
# "psiShield", a little outside its body (dragoon_shield: about 0.1-0.4 m). The Reaver had none, so its shield never
# showed. This builds one from the unit's own mesh: same skeleton, skinning and UVs, every material psiShield, every
# vertex pushed out along its normal.
#
# NOD (see nod_uvmap.py): header bytes 4..7 = shader / bone / vertex-set / mesh-group counts, +0x24 vertex sets as
# (type, count); 32-byte shader names from 92, 64-byte bones after them, then the vertex sets (type 0: 32 bytes,
# type 1: 36, type 2: 48; position, normal, UV, then skinning).
import os
import struct

H = os.path.dirname(os.path.abspath(__file__))
MODELS = os.path.join(H, '..', 'game', '3D', 'Models')
STRIDE = {0: 32, 1: 36, 2: 48}

SHELLS = [("Reaver.nod", "reaver_shield.nod", 1.0),   # (source, output, push in model units; 0.5 was a little tight, user)
          # Nova with the Psi Blade (src/psiblade.cpp: her model becomes nova_DT43, and cProtossShield loads
          # "<model>_shield"); human-sized, in metres
          # (0.09: the Zealot's shell sits a median 9 cm off its body, the Purifier's 12; user: reference them)
          ("nova_DT43.nod", "nova_DT43_shield.nod", 0.09),
          # Tanagazj (user: an 80-point shield; psiblade.cpp creates it), human-sized like the Zealot
          ("Tanagazj.nod", "Tanagazj_shield.nod", 0.09)]


def make(src, dst, push):
    d = bytearray(open(os.path.join(MODELS, src), 'rb').read())
    sc, bc, vsc = d[4], d[5], d[6]
    for i in range(sc):
        d[92 + 32 * i:92 + 32 * i + 32] = b'psiShield'.ljust(32, b'\0')
    o = 92 + 32 * sc + 64 * bc
    n = 0
    for k in range(vsc):
        t, c = struct.unpack_from('<II', d, 0x24 + 8 * k)
        s = STRIDE[t]
        for i in range(c):
            v = o + s * i
            p = struct.unpack_from('<3f', d, v)
            nx, ny, nz = struct.unpack_from('<3f', d, v + 12)
            struct.pack_into('<3f', d, v, p[0] + nx * push, p[1] + ny * push, p[2] + nz * push)
            n += 1
        o += s * c
    out = os.path.join(MODELS, dst)
    open(out, 'wb').write(bytes(d))
    print('wrote %s (%d vertices pushed %.2f, %d materials -> psiShield)' % (os.path.normpath(out), n, push, sc))


if __name__ == '__main__':
    for src, dst, push in SHELLS:
        make(src, dst, push)
