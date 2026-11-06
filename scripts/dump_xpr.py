# dump_xpr.py <file.xpr> <out dir> -- writes every texture of an Xbox packed resource file (XPR1, e.g.
# game\Materials\global.xpr: the HUD's icons and borders) as <name>.png, plus index.html (all of them, named, on a grey
# checkerboard so alpha shows).
#
# XPR1: "XPR1", u32 total size, u32 header size (data starts there), u32 count, count x (u32 name offset,
# u32 resource offset) (offsets from 12); a resource is an Xbox D3D pixel container: u32 common, u32 data (offset from
# the header size), u32 lock, u32 format, u32 size. format: bits 8-15 the texture format (src/xd3d.h XF_*), bits 20-23
# / 24-27 log2 width / height (swizzled and compressed); size (linear formats): width - 1 in bits 0-11, height - 1 in
# 12-23. Swizzled texels are in Morton order (x bit first, as src/d3d_res.cpp Unswizzle).
import io
import os
import struct
import sys

from PIL import Image

BPP = {0x00: 1, 0x02: 2, 0x03: 2, 0x04: 2, 0x05: 2, 0x06: 4, 0x07: 4, 0x0B: 1, 0x19: 1, 0x1A: 2, 0x01: 1,
       0x10: 2, 0x11: 2, 0x12: 4, 0x13: 1, 0x1C: 2, 0x1D: 2, 0x1E: 4, 0x1F: 1, 0x20: 2}
SWIZZLED = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0B, 0x19, 0x1A}
DXT = {0x0C: b"DXT1", 0x0E: b"DXT3", 0x0F: b"DXT5"}


def unswizzle(src, w, h, bpp):
    out = bytearray(w * h * bpp)
    mx = my = 0
    bit, x, y = 1, 1, 1
    while x < w or y < h:   # Morton masks: x and y bits interleaved, x first, until one runs out
        if x < w:
            mx |= bit
            bit <<= 1
            x <<= 1
        if y < h:
            my |= bit
            bit <<= 1
            y <<= 1
    def spread(v, mask):
        r, b = 0, 1
        while mask:
            low = mask & -mask
            if v & b:
                r |= low
            mask &= mask - 1
            b <<= 1
        return r
    xs = [spread(i, mx) for i in range(w)]
    for j in range(h):
        oy = spread(j, my)
        for i in range(w):
            s = (xs[i] | oy) * bpp
            d = (j * w + i) * bpp
            out[d:d + bpp] = src[s:s + bpp]
    return bytes(out)


def to_image(fmt, w, h, data):
    if fmt in DXT:
        size = max(1, w // 4) * max(1, h // 4) * (8 if fmt == 0x0C else 16)
        head = struct.pack("<4sIIIIIII44sII4sIIIII", b"DDS ", 124, 0x1007, h, w, size, 0, 0, b"\0" * 44, 32, 4, DXT[fmt],
                           0, 0, 0, 0, 0)
        head += struct.pack("<IIIII", 0x1000, 0, 0, 0, 0)
        return Image.open(io.BytesIO(head + data[:size])).convert("RGBA")
    bpp = BPP[fmt]
    raw = data[:w * h * bpp]
    if fmt in SWIZZLED:
        raw = unswizzle(raw, w, h, bpp)
    if bpp == 4:
        return Image.frombytes("RGBA", (w, h), raw, "raw", "BGRA")
    if fmt in (0x02, 0x10):
        return Image.frombytes("RGBA", (w, h), raw, "raw", "BGRA;15")
    if fmt in (0x03, 0x1C):
        return Image.frombytes("RGB", (w, h), raw, "raw", "BGR;15")
    if fmt in (0x05, 0x11):
        return Image.frombytes("RGB", (w, h), raw, "raw", "BGR;16")
    if fmt in (0x04, 0x1D):
        px = struct.unpack("<%dH" % (w * h), raw)
        return Image.frombytes("RGBA", (w, h), bytes(c for p in px for c in (((p >> 8) & 15) * 17, ((p >> 4) & 15) * 17,
                                                                            (p & 15) * 17, (p >> 12) * 17)))
    if fmt in (0x00, 0x13):
        return Image.frombytes("L", (w, h), raw)
    if fmt in (0x19, 0x1F, 0x01):
        return Image.merge("RGBA", [Image.new("L", (w, h), 255)] * 3 + [Image.frombytes("L", (w, h), raw)])
    if fmt in (0x1A, 0x20):
        im = Image.frombytes("LA", (w, h), raw, "raw", "AL" if False else "LA")
        return im.convert("RGBA")
    raise ValueError("format %02x" % fmt)


def main():
    path, out = sys.argv[1], sys.argv[2]
    d = open(path, "rb").read()
    magic, total, hsize, count = struct.unpack_from("<4sIII", d, 0)
    assert magic == b"XPR1", magic
    os.makedirs(out, exist_ok=True)
    rows, skipped = [], []
    for i in range(count):
        noff, roff = struct.unpack_from("<II", d, 16 + 8 * i)
        name = d[12 + noff:12 + noff + 64].split(b"\0")[0].decode("latin1")
        common, data, lock, fmtword, size = struct.unpack_from("<IIIII", d, 12 + roff)
        fmt = (fmtword >> 8) & 0xFF
        if size:
            w, h = (size & 0xFFF) + 1, ((size >> 12) & 0xFFF) + 1
        else:
            w, h = 1 << ((fmtword >> 20) & 15), 1 << ((fmtword >> 24) & 15)
        try:
            im = to_image(fmt, w, h, d[hsize + data:])
        except Exception as e:   # (not a texture, or a format this does not decode)
            skipped.append("%s (format %02x: %s)" % (name, fmt, e))
            continue
        im.save(os.path.join(out, name + ".png"))
        rows.append((name, w, h, fmt))
    with open(os.path.join(out, "index.html"), "w") as f:
        f.write("<html><body style='background:#222;color:#ddd;font:13px sans-serif'><h3>%s: %d textures</h3>\n" % (
            os.path.basename(path), len(rows)))
        for name, w, h, fmt in sorted(rows, key=lambda r: r[0].lower()):
            f.write("<div style='display:inline-block;margin:6px;text-align:center;vertical-align:top'>"
                    "<div style='background:repeating-conic-gradient(#555 0 25%%,#777 0 50%%) 0 0/16px 16px;padding:4px'>"
                    "<img src='%s.png' style='max-width:192px;max-height:192px;image-rendering:pixelated'></div>"
                    "%s<br><small>%dx%d fmt %02x</small></div>\n" % (name, name, w, h, fmt))
        f.write("</body></html>\n")
    print("wrote %d textures to %s (index.html)%s" % (len(rows), out, "; skipped: " + ", ".join(skipped) if skipped else ""))


if __name__ == "__main__":
    main()
