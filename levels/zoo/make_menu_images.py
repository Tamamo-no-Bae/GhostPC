"""Builds the Enemy Zoo mission-select images (game/UI/Materials/911a_MenuImage.dds 256x128 and 911a_MenuImageL.dds
1024x512) from an in-game screenshot, in the same format as the originals: DXT1, no mip levels, with the 128-byte
header copied from 131a_MenuImage(L).dds.

Usage: python make_menu_images.py <screenshot> [x0 y0 x1 y1]   (crop box in screenshot pixels, 2:1)
"""
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.environ.get("GHOST_GAME_DIR") or next(
    (g for g in (os.path.join(HERE, "..", "..", "game"), os.path.join(HERE, "..")) if os.path.exists(os.path.join(g, "UI"))),
    os.path.join(HERE, ".."))
UI = os.path.join(GAME, "UI", "Materials")
NAME = "911a_MenuImage"


def rgb565(c):
    c = np.clip(np.rint(c), 0, 255).astype(np.int32)
    return int(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3))


def unpack565(v):
    return np.array([((v >> 11) & 31) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31])


def encode_dxt1(img):
    """4-colour DXT1: endpoints along the block's principal axis, each pixel mapped to the nearest palette entry"""
    a = np.asarray(img.convert("RGB"), dtype=np.float64)
    h, w, _ = a.shape
    out = bytearray()
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            px = a[by:by + 4, bx:bx + 4].reshape(-1, 3)
            mean = px.mean(axis=0)
            centred = px - mean
            axis = np.linalg.svd(centred, full_matrices=False)[2][0] if np.any(centred) else np.array([1.0, 0, 0])
            t = centred @ axis
            c0, c1 = rgb565(mean + axis * t.max()), rgb565(mean + axis * t.min())
            if c0 < c1:
                c0, c1 = c1, c0
            if c0 == c1:                       # flat block: 4-colour mode needs c0 > c1
                if c0 > 0:
                    c1 = c0 - 1
                else:
                    c0 = 1
            p0, p1 = unpack565(c0), unpack565(c1)
            pal = np.array([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3])
            idx = np.argmin(((px[:, None, :] - pal[None, :, :]) ** 2).sum(axis=2), axis=1)
            bits = 0
            for i, k in enumerate(idx):
                bits |= int(k) << (2 * i)
            out += c0.to_bytes(2, "little") + c1.to_bytes(2, "little") + bits.to_bytes(4, "little")
    return bytes(out)


def main():
    src = Image.open(sys.argv[1]).convert("RGB")
    box = tuple(int(v) for v in sys.argv[2:6]) if len(sys.argv) >= 6 else (420, 250, 1660, 870)
    crop = src.crop(box)
    for suffix, size, template in (("", (256, 128), "131a_MenuImage.dds"), ("L", (1024, 512), "131a_MenuImageL.dds")):
        header = open(os.path.join(UI, template), "rb").read(128)
        data = encode_dxt1(crop.resize(size, Image.LANCZOS))
        path = os.path.join(UI, NAME + suffix + ".dds")
        open(path, "wb").write(header + data)
        print("wrote", os.path.abspath(path), len(header) + len(data), "bytes")


if __name__ == "__main__":
    main()
