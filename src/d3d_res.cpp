// Xbox D3D resources: textures/surfaces/buffers keep their Xbox memory layout in
// game-visible memory; host (D3D9) textures are created/uploaded lazily.
#include "xd3d.h"
#include "d3d_int.h"
#include <unordered_map>
#include <vector>
#include <malloc.h>
extern UINT g_frame;
UINT XChainBytes(DWORD fmt, UINT w, UINT h, UINT d, UINT levels);

// ---------------------------------------------------------------------------
// Format information
struct FmtInfo { int bpp; bool swz; bool dxt; bool depth; D3DFORMAT host; int conv; };
enum { CV_NONE, CV_AL8, CV_P8, CV_YUY2, CV_UYVY, CV_R6G5B5, CV_ABGR, CV_BGRA, CV_RGBA, CV_R8B8, CV_G8B8,
       CV_DEPTH };
static FmtInfo FI(DWORD f) {
    switch (f) {
    case XF_L8: return {8, true, false, false, D3DFMT_L8, CV_NONE};
    case XF_AL8: return {8, true, false, false, D3DFMT_A8L8, CV_AL8};
    case XF_A1R5G5B5: return {16, true, false, false, D3DFMT_A1R5G5B5, CV_NONE};
    case XF_X1R5G5B5: return {16, true, false, false, D3DFMT_X1R5G5B5, CV_NONE};
    case XF_A4R4G4B4: return {16, true, false, false, D3DFMT_A4R4G4B4, CV_NONE};
    case XF_R5G6B5: return {16, true, false, false, D3DFMT_R5G6B5, CV_NONE};
    case XF_A8R8G8B8: return {32, true, false, false, D3DFMT_A8R8G8B8, CV_NONE};
    case XF_X8R8G8B8: return {32, true, false, false, D3DFMT_X8R8G8B8, CV_NONE};
    case XF_P8: return {8, true, false, false, D3DFMT_A8R8G8B8, CV_P8};
    case XF_DXT1: return {4, false, true, false, D3DFMT_DXT1, CV_NONE};
    case XF_DXT3: return {8, false, true, false, D3DFMT_DXT3, CV_NONE};
    case XF_DXT5: return {8, false, true, false, D3DFMT_DXT5, CV_NONE};
    case XF_LIN_A1R5G5B5: return {16, false, false, false, D3DFMT_A1R5G5B5, CV_NONE};
    case XF_LIN_R5G6B5: return {16, false, false, false, D3DFMT_R5G6B5, CV_NONE};
    case XF_LIN_A8R8G8B8: return {32, false, false, false, D3DFMT_A8R8G8B8, CV_NONE};
    case XF_LIN_L8: return {8, false, false, false, D3DFMT_L8, CV_NONE};
    case XF_LIN_R8B8: return {16, false, false, false, D3DFMT_A8R8G8B8, CV_R8B8};
    case XF_LIN_G8B8: return {16, false, false, false, D3DFMT_V8U8, CV_NONE};
    case XF_A8: return {8, true, false, false, D3DFMT_A8, CV_NONE};
    case XF_A8L8: return {16, true, false, false, D3DFMT_A8L8, CV_NONE};
    case XF_LIN_AL8: return {8, false, false, false, D3DFMT_A8L8, CV_AL8};
    case XF_LIN_X1R5G5B5: return {16, false, false, false, D3DFMT_X1R5G5B5, CV_NONE};
    case XF_LIN_A4R4G4B4: return {16, false, false, false, D3DFMT_A4R4G4B4, CV_NONE};
    case XF_LIN_X8R8G8B8: return {32, false, false, false, D3DFMT_X8R8G8B8, CV_NONE};
    case XF_LIN_A8: return {8, false, false, false, D3DFMT_A8, CV_NONE};
    case XF_LIN_A8L8: return {16, false, false, false, D3DFMT_A8L8, CV_NONE};
    case XF_YUY2: return {16, false, false, false, D3DFMT_X8R8G8B8, CV_YUY2};
    case XF_UYVY: return {16, false, false, false, D3DFMT_X8R8G8B8, CV_UYVY};
    case XF_L6V5U5: return {16, true, false, false, D3DFMT_L6V5U5, CV_NONE};
    case XF_V8U8: return {16, true, false, false, D3DFMT_V8U8, CV_NONE};
    case XF_R8B8: return {16, true, false, false, D3DFMT_A8R8G8B8, CV_R8B8};
    case XF_D24S8: case XF_F24S8: return {32, true, false, true, D3DFMT_D24S8, CV_DEPTH};
    case XF_D16: case XF_F16: return {16, true, false, true, D3DFMT_D16, CV_DEPTH};
    case XF_LIN_D24S8: case XF_LIN_F24S8: return {32, false, false, true, D3DFMT_D24S8, CV_DEPTH};
    case XF_LIN_D16: case XF_LIN_F16: return {16, false, false, true, D3DFMT_D16, CV_DEPTH};
    case XF_L16: return {16, true, false, false, D3DFMT_L16, CV_NONE};
    case XF_V16U16: return {32, true, false, false, D3DFMT_V16U16, CV_NONE};
    case XF_LIN_L16: return {16, false, false, false, D3DFMT_L16, CV_NONE};
    case XF_LIN_V16U16: return {32, false, false, false, D3DFMT_V16U16, CV_NONE};
    case XF_LIN_L6V5U5: return {16, false, false, false, D3DFMT_L6V5U5, CV_NONE};
    case XF_R6G5B5: return {16, true, false, false, D3DFMT_X8R8G8B8, CV_R6G5B5};
    case XF_LIN_R6G5B5: return {16, false, false, false, D3DFMT_X8R8G8B8, CV_R6G5B5};
    case XF_A8B8G8R8: return {32, true, false, false, D3DFMT_A8R8G8B8, CV_ABGR};
    case XF_B8G8R8A8: return {32, true, false, false, D3DFMT_A8R8G8B8, CV_BGRA};
    case XF_R8G8B8A8: return {32, true, false, false, D3DFMT_A8R8G8B8, CV_RGBA};
    case XF_LIN_A8B8G8R8: return {32, false, false, false, D3DFMT_A8R8G8B8, CV_ABGR};
    case XF_LIN_B8G8R8A8: return {32, false, false, false, D3DFMT_A8R8G8B8, CV_BGRA};
    case XF_LIN_R8G8B8A8: return {32, false, false, false, D3DFMT_A8R8G8B8, CV_RGBA};
    }
    static int warned[256];
    if (!warned[f & 255]++) Log("d3d: unknown Xbox format %02x", f);
    return {32, false, false, false, D3DFMT_A8R8G8B8, CV_NONE};
}
bool XFmtIsSwizzled(DWORD f) { return FI(f).swz; }
bool XFmtIsCompressed(DWORD f) { return FI(f).dxt; }
bool XFmtIsDepth(DWORD f) { return FI(f).depth; }
int XFmtBpp(DWORD f) { return FI(f).bpp; }
D3DFORMAT XFmtHost(DWORD f) { return FI(f).host; }
bool XFmtIsLinear(DWORD f) { auto i = FI(f); return !i.swz && !i.dxt; }

void XSurfaceDims(const XPixelContainer *p, UINT *w, UINT *h, UINT *d, UINT *pitch, UINT *levels) {
    DWORD fmt = XFormat(p);
    UINT W, H, D = 1, P, L;
    if (p->Size) {
        W = (p->Size & XSIZE_WIDTH_MASK) + 1;
        H = ((p->Size & XSIZE_HEIGHT_MASK) >> XSIZE_HEIGHT_SHIFT) + 1;
        P = (((p->Size & XSIZE_PITCH_MASK) >> XSIZE_PITCH_SHIFT) + 1) * 64;
        L = 1;
    } else {
        W = 1u << ((p->Format >> XFMT_USIZE_SHIFT) & 0xF);
        H = 1u << ((p->Format >> XFMT_VSIZE_SHIFT) & 0xF);
        D = 1u << ((p->Format >> XFMT_PSIZE_SHIFT) & 0xF);
        L = (p->Format & XFMT_MIPMAP_MASK) >> XFMT_MIPMAP_SHIFT;
        if (!L) L = 1;
        if (XFmtIsCompressed(fmt)) P = (W / 4 ? W / 4 : 1) * (fmt == XF_DXT1 ? 8 : 16);
        else P = W * XFmtBpp(fmt) / 8;
    }
    if (w) *w = W;
    if (h) *h = H;
    if (d) *d = D;
    if (pitch) *pitch = P;
    if (levels) *levels = L;
}

// byte size of one mip level (one face)
static UINT LevelBytes(DWORD fmt, UINT w, UINT h, UINT d) {
    if (XFmtIsCompressed(fmt)) {
        UINT bw = (w + 3) / 4, bh = (h + 3) / 4;
        return bw * bh * (fmt == XF_DXT1 ? 8 : 16) * d;
    }
    return w * h * d * XFmtBpp(fmt) / 8;
}
// Xbox mip chains: each level at least 1 block / pixel; cube faces are 128-byte aligned
static UINT MipMin(DWORD fmt) { return XFmtIsCompressed(fmt) ? 4 : 1; }
UINT XLevelOffset(DWORD fmt, UINT w, UINT h, UINT d, UINT level) {
    UINT off = 0;
    for (UINT i = 0; i < level; i++) {
        off += LevelBytes(fmt, std::max(w, MipMin(fmt)), std::max(h, MipMin(fmt)), d);
        w = std::max(w >> 1, 1u); h = std::max(h >> 1, 1u); d = std::max(d >> 1, 1u);
    }
    return off;
}
UINT XChainBytes(DWORD fmt, UINT w, UINT h, UINT d, UINT levels) { return XLevelOffset(fmt, w, h, d, levels); }
static UINT FaceBytes(DWORD fmt, UINT w, UINT h, UINT levels) {
    return (XChainBytes(fmt, w, h, 1, levels) + 127) & ~127u;
}

// ---------------------------------------------------------------------------
// Swizzle (Morton order, x bit first)
static void SwizzleMasks(UINT w, UINT h, UINT d, uint32_t &mx, uint32_t &my, uint32_t &mz) {
    mx = my = mz = 0;
    uint32_t bit = 1;
    for (UINT i = 1; i < w || i < h || i < d; i <<= 1) {
        if (i < w) { mx |= bit; bit <<= 1; }
        if (i < h) { my |= bit; bit <<= 1; }
        if (i < d) { mz |= bit; bit <<= 1; }
    }
}
static inline uint32_t Deposit(uint32_t v, uint32_t mask) {
    uint32_t r = 0;
    for (uint32_t b = 1; mask; b <<= 1) {
        uint32_t low = mask & (0 - mask);
        if (v & b) r |= low;
        mask &= mask - 1;
    }
    return r;
}
// src swizzled -> dst linear (row pitch w*bpp)
static void Unswizzle(const uint8_t *src, uint8_t *dst, UINT w, UINT h, UINT d, UINT bpp) {
    uint32_t mx, my, mz;
    SwizzleMasks(w, h, d, mx, my, mz);
    std::vector<uint32_t> xo(w), yo(h);
    for (UINT x = 0; x < w; x++) xo[x] = Deposit(x, mx);
    for (UINT y = 0; y < h; y++) yo[y] = Deposit(y, my);
    for (UINT z = 0; z < d; z++) {
        uint32_t zo = Deposit(z, mz);
        for (UINT y = 0; y < h; y++) {
            uint8_t *row = dst + ((size_t)z * h + y) * w * bpp;
            uint32_t base = yo[y] | zo;
            switch (bpp) {
            case 1: for (UINT x = 0; x < w; x++) row[x] = src[base | xo[x]]; break;
            case 2: for (UINT x = 0; x < w; x++) ((uint16_t *)row)[x] = ((const uint16_t *)src)[base | xo[x]]; break;
            case 4: for (UINT x = 0; x < w; x++) ((uint32_t *)row)[x] = ((const uint32_t *)src)[base | xo[x]]; break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Host texture cache
std::unordered_map<const void *, HostRes *> g_hostRes;

HostRes *HostOf(const void *r) {
    auto it = g_hostRes.find(r);
    return it == g_hostRes.end() ? nullptr : it->second;
}
HostRes *HostGet(const void *r) {
    HostRes *&h = g_hostRes[r];
    if (!h) h = new HostRes();
    return h;
}
void HostFree(const void *r) {
    auto it = g_hostRes.find(r);
    if (it == g_hostRes.end()) return;
    if (it->second->tex) it->second->tex->Release();
    if (it->second->surf) it->second->surf->Release();
    delete it->second;
    g_hostRes.erase(it);
}

static uint32_t *g_palette;   // current P8 palette (256 ARGB) set by SetPalette (rare)

static void ConvertRow(int conv, const uint8_t *s, uint8_t *d, UINT w) {
    switch (conv) {
    case CV_AL8: for (UINT x = 0; x < w; x++) { d[x * 2] = s[x]; d[x * 2 + 1] = s[x]; } break;
    case CV_P8:
        for (UINT x = 0; x < w; x++) ((uint32_t *)d)[x] = g_palette ? g_palette[s[x]] : 0xff000000u | s[x] * 0x010101u;
        break;
    case CV_R6G5B5:
        for (UINT x = 0; x < w; x++) {
            uint16_t v = ((const uint16_t *)s)[x];
            uint32_t r = (v >> 10) & 63, g = (v >> 5) & 31, b = v & 31;
            ((uint32_t *)d)[x] = 0xff000000u | ((r * 255 / 63) << 16) | ((g * 255 / 31) << 8) | (b * 255 / 31);
        }
        break;
    case CV_ABGR:
        for (UINT x = 0; x < w; x++) {
            uint32_t v = ((const uint32_t *)s)[x];
            ((uint32_t *)d)[x] = (v & 0xff00ff00u) | ((v >> 16) & 0xff) | ((v & 0xff) << 16);
        }
        break;
    case CV_BGRA:
        for (UINT x = 0; x < w; x++) { uint32_t v = ((const uint32_t *)s)[x]; ((uint32_t *)d)[x] = _byteswap_ulong(v); }
        break;
    case CV_RGBA:
        for (UINT x = 0; x < w; x++) { uint32_t v = ((const uint32_t *)s)[x]; ((uint32_t *)d)[x] = (v >> 8) | (v << 24); }
        break;
    case CV_R8B8:
        for (UINT x = 0; x < w; x++) {
            uint16_t v = ((const uint16_t *)s)[x];
            ((uint32_t *)d)[x] = 0xff000000u | ((v >> 8) << 16) | (v & 0xff);
        }
        break;
    case CV_YUY2: case CV_UYVY:
        for (UINT x = 0; x + 1 < w; x += 2) {
            const uint8_t *p = s + x * 2;
            int y0, u, y1, v;
            if (conv == CV_YUY2) { y0 = p[0]; u = p[1]; y1 = p[2]; v = p[3]; }
            else { u = p[0]; y0 = p[1]; v = p[2]; y1 = p[3]; }
            for (int k = 0; k < 2; k++) {
                int c = (k ? y1 : y0) - 16, dd = u - 128, e = v - 128;
                int r = (298 * c + 409 * e + 128) >> 8, g = (298 * c - 100 * dd - 208 * e + 128) >> 8, b = (298 * c + 516 * dd + 128) >> 8;
                r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
                ((uint32_t *)d)[x + k] = 0xff000000u | (r << 16) | (g << 8) | b;
            }
        }
        break;
    }
}
static int HostBpp(D3DFORMAT f) {
    switch (f) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_V16U16: case D3DFMT_Q8W8V8U8: return 4;
    case D3DFMT_L8: case D3DFMT_A8: return 1;
    default: return 2;
    }
}

// Upload one 2D level (or volume) from Xbox memory into a locked host rect
static void UploadLevel(DWORD fmt, const uint8_t *src, UINT w, UINT h, UINT d, UINT xpitch, uint8_t *dst, UINT dpitch,
                        UINT dslice) {
    FmtInfo fi = FI(fmt);
    if (fi.dxt) {
        UINT rows = (h + 3) / 4, rb = ((w + 3) / 4) * (fmt == XF_DXT1 ? 8 : 16);
        for (UINT z = 0; z < d; z++)
            for (UINT y = 0; y < rows; y++) memcpy(dst + z * dslice + y * dpitch, src + (z * rows + y) * rb, rb);
        return;
    }
    UINT sb = fi.bpp / 8, rowBytes = w * sb;
    std::vector<uint8_t> lin;
    const uint8_t *base = src;
    UINT spitch = xpitch ? xpitch : rowBytes;
    if (fi.swz && (w > 1 || h > 1 || d > 1)) {
        lin.resize((size_t)rowBytes * h * d);
        Unswizzle(src, lin.data(), w, h, d, sb);
        base = lin.data();
        spitch = rowBytes;
    }
    UINT hb = HostBpp(fi.host);
    for (UINT z = 0; z < d; z++)
        for (UINT y = 0; y < h; y++) {
            const uint8_t *s = base + ((size_t)z * h + y) * spitch;
            uint8_t *o = dst + z * dslice + y * dpitch;
            if (fi.conv == CV_NONE || fi.conv == CV_DEPTH) memcpy(o, s, std::min(rowBytes, w * hb));
            else ConvertRow(fi.conv, s, o, w);
        }
}

// Create/refresh the host texture for an Xbox texture or surface (pixel container)
IDirect3DBaseTexture9 *HostTexture(XPixelContainer *p) {
    if (!p || !g_dev) return nullptr;
    HostRes *h = HostGet(p);
    if (h->isBackbuffer && h->surf) {
        // the Xbox samples its framebuffer directly (e.g. the thermal sight view binds the back buffer as a
        // texture): copy the D3D9 back buffer into a texture of the same size each time it is bound
        D3DSURFACE_DESC bd;
        h->surf->GetDesc(&bd);
        if (!h->tex && FAILED(g_dev->CreateTexture(bd.Width, bd.Height, 1, D3DUSAGE_RENDERTARGET, bd.Format, D3DPOOL_DEFAULT,
                                                   (IDirect3DTexture9 **)&h->tex, nullptr))) {
            h->tex = nullptr;
            return nullptr;
        }
        IDirect3DSurface9 *dst = nullptr;
        if (SUCCEEDED(((IDirect3DTexture9 *)h->tex)->GetSurfaceLevel(0, &dst))) {
            g_dev->StretchRect(h->surf, nullptr, dst, nullptr, D3DTEXF_NONE);
            dst->Release();
        }
        return h->tex;
    }
    if (h->isRT && h->tex) return h->tex;
    if (h->isSurfaceView && h->parent) return HostTexture(h->parent);
    UINT w, hh, d, pitch, levels;
    XSurfaceDims(p, &w, &hh, &d, &pitch, &levels);
    DWORD fmt = XFormat(p);
    bool cube = (p->Format & XFMT_CUBEMAP) != 0;
    bool vol = d > 1;
    if (h->tex && (h->fmtKey != p->Format || h->sizeKey != p->Size || h->dataKey != p->Data)) {
        h->tex->Release(); h->tex = nullptr;
    }
    if (h->tex && !h->dirty && h->watch && h->hashFrame != g_frame) {
        // textures the game keeps writing through an old lock pointer: re-upload when the memory changed
        h->hashFrame = g_frame;
        UINT n = h->bytes ? h->bytes : (p->Size ? pitch * hh : XChainBytes(fmt, w, hh, d, levels));
        uint64_t x = 1469598103934665603ull;
        __try {
            const uint64_t *q = (const uint64_t *)p->Data;
            for (UINT i = 0; i < n / 8; i++) x = (x ^ q[i]) * 1099511628211ull;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (x != h->hash) { h->hash = x; h->dirty = true; }
    }
    if (h->tex && !h->dirty) return h->tex;
    FmtInfo fi = FI(fmt);
    if (fi.depth) return nullptr;
    HRESULT hr;
    if (!h->tex) {
        if (cube) hr = g_dev->CreateCubeTexture(w, levels, 0, fi.host, D3DPOOL_MANAGED, (IDirect3DCubeTexture9 **)&h->tex, nullptr);
        else if (vol) hr = g_dev->CreateVolumeTexture(w, hh, d, levels, 0, fi.host, D3DPOOL_MANAGED, (IDirect3DVolumeTexture9 **)&h->tex, nullptr);
        else hr = g_dev->CreateTexture(w, hh, levels, 0, fi.host, D3DPOOL_MANAGED, (IDirect3DTexture9 **)&h->tex, nullptr);
        if (FAILED(hr)) { Log("d3d: CreateTexture %ux%ux%u fmt %02x failed %08x", w, hh, d, fmt, hr); return nullptr; }
        h->fmtKey = p->Format; h->sizeKey = p->Size; h->dataKey = p->Data;
        h->w = w; h->h = hh; h->d = d; h->linear = !fi.swz && !fi.dxt;
    }
    const uint8_t *src = (const uint8_t *)p->Data;
    if (!src) { h->dirty = false; return h->tex; }
    __try {
        if (cube) {
            UINT fb = FaceBytes(fmt, w, w, levels);
            for (UINT f = 0; f < 6; f++) {
                UINT lw = w;
                for (UINT l = 0; l < levels; l++) {
                    D3DLOCKED_RECT lr;
                    if (SUCCEEDED(((IDirect3DCubeTexture9 *)h->tex)->LockRect((D3DCUBEMAP_FACES)f, l, &lr, nullptr, 0))) {
                        UploadLevel(fmt, src + f * fb + XLevelOffset(fmt, w, w, 1, l), lw, lw, 1, 0, (uint8_t *)lr.pBits, lr.Pitch, 0);
                        ((IDirect3DCubeTexture9 *)h->tex)->UnlockRect((D3DCUBEMAP_FACES)f, l);
                    }
                    lw = std::max(lw >> 1, 1u);
                }
            }
        } else if (vol) {
            UINT lw = w, lh = hh, ld = d;
            for (UINT l = 0; l < levels; l++) {
                D3DLOCKED_BOX lb;
                if (SUCCEEDED(((IDirect3DVolumeTexture9 *)h->tex)->LockBox(l, &lb, nullptr, 0))) {
                    UploadLevel(fmt, src + XLevelOffset(fmt, w, hh, d, l), lw, lh, ld, 0, (uint8_t *)lb.pBits, lb.RowPitch, lb.SlicePitch);
                    ((IDirect3DVolumeTexture9 *)h->tex)->UnlockBox(l);
                }
                lw = std::max(lw >> 1, 1u); lh = std::max(lh >> 1, 1u); ld = std::max(ld >> 1, 1u);
            }
        } else {
            UINT lw = w, lh = hh;
            for (UINT l = 0; l < levels; l++) {
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(((IDirect3DTexture9 *)h->tex)->LockRect(l, &lr, nullptr, 0))) {
                    UploadLevel(fmt, src + XLevelOffset(fmt, w, hh, 1, l), lw, lh, 1, p->Size ? pitch : 0, (uint8_t *)lr.pBits, lr.Pitch, 0);
                    ((IDirect3DTexture9 *)h->tex)->UnlockRect(l);
                }
                lw = std::max(lw >> 1, 1u); lh = std::max(lh >> 1, 1u);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("d3d: fault uploading texture %p data %08x fmt %02x %ux%u", p, p->Data, fmt, w, hh);
    }
    h->dirty = false;
    return h->tex;
}

// ---------------------------------------------------------------------------
// API: resource creation
static void InitPixelContainer(XPixelContainer *p, DWORD type, UINT w, UINT h, UINT d, UINT levels, DWORD fmt, bool cube) {
    p->Common = 1 | type | XCOMMON_D3DCREATED;
    bool lin = !FI(fmt).swz && !FI(fmt).dxt;
    if (lin) {
        UINT pitch = (w * FI(fmt).bpp / 8 + 63) & ~63u;
        p->Format = (fmt << XFMT_FORMAT_SHIFT) | (2 << XFMT_DIMENSION_SHIFT) | (1 << XFMT_MIPMAP_SHIFT) | 1;
        p->Size = (w - 1) | ((h - 1) << XSIZE_HEIGHT_SHIFT) | ((pitch / 64 - 1) << XSIZE_PITCH_SHIFT);
    } else {
        auto lg = [](UINT v) { UINT r = 0; while ((1u << r) < v) r++; return r; };
        p->Format = (fmt << XFMT_FORMAT_SHIFT) | ((d > 1 ? 3 : 2) << XFMT_DIMENSION_SHIFT) | (levels << XFMT_MIPMAP_SHIFT) |
                    (lg(w) << XFMT_USIZE_SHIFT) | (lg(h) << XFMT_VSIZE_SHIFT) | (lg(d) << XFMT_PSIZE_SHIFT) | 1 |
                    (cube ? XFMT_CUBEMAP : 0);
        p->Size = 0;
    }
}
static UINT FullLevels(UINT w, UINT h, UINT d) {
    UINT l = 1;
    while (w > 1 || h > 1 || d > 1) { w >>= 1; h >>= 1; d >>= 1; l++; }
    return l;
}

XSTD(XPixelContainer *, D3DDevice_CreateTexture2, 28, (UINT w, UINT h, UINT d, UINT levels, DWORD usage, DWORD fmt, DWORD rtype)) {
    bool cube = rtype == XRT_CUBETEXTURE, vol = rtype == XRT_VOLUMETEXTURE;
    if (!vol) d = 1;
    if (cube) h = w;
    FmtInfo fi = FI(fmt);
    bool lin = !fi.swz && !fi.dxt;
    if (lin) levels = 1;
    else if (levels == 0) levels = FullLevels(w, h, d);
    auto p = (XPixelContainer *)calloc(1, sizeof(XSurface));
    InitPixelContainer(p, XCOMMON_TYPE_TEXTURE, w, h, d, levels, fmt, cube);
    UINT pitch; XSurfaceDims(p, nullptr, nullptr, nullptr, &pitch, nullptr);
    UINT bytes = lin ? pitch * h : cube ? FaceBytes(fmt, w, h, levels) * 6 : XChainBytes(fmt, w, h, d, levels);
    p->Data = (DWORD)_aligned_malloc(bytes + 64, 128);
    memset((void *)p->Data, 0, bytes);
    HostRes *hr = HostGet(p);
    hr->dirty = true;
    hr->bytes = bytes;
    if (usage & D3DUSAGE_RENDERTARGET) MakeRenderTarget(p, hr);
    if (fi.depth || (usage & D3DUSAGE_DEPTHSTENCIL)) MakeDepthTarget(p, hr);
    LOGV("CreateTexture2 %ux%ux%u lv%u usage %x fmt %02x type %u -> %p", w, h, d, levels, usage, fmt, rtype, p);
    return p;
}

XSTD(XResource *, D3DDevice_CreateVertexBuffer2, 4, (UINT len)) {
    auto r = (XResource *)calloc(1, sizeof(XResource) + 8);
    r->Common = 1 | XCOMMON_TYPE_VERTEXBUFFER | XCOMMON_D3DCREATED;
    r->Data = (DWORD)_aligned_malloc(len + 16, 32);
    memset((void *)r->Data, 0, len);
    HostGet(r)->bytes = len;
    return r;
}
XSTD(XResource *, D3DDevice_CreateIndexBuffer2, 4, (UINT len)) {
    auto r = (XResource *)calloc(1, sizeof(XResource) + 8);
    r->Common = 1 | XCOMMON_TYPE_INDEXBUFFER | XCOMMON_D3DCREATED;
    r->Data = (DWORD)_aligned_malloc(len + 16, 32);
    memset((void *)r->Data, 0, len);
    HostGet(r)->bytes = len;
    return r;
}
XSTD(void *, D3D_AllocContiguousMemory, 8, (SIZE_T n, DWORD align)) {
    return _aligned_malloc(n ? n : 1, align < 16 ? 16 : align);
}
XSTD(void, D3D_FreeContiguousMemory, 4, (void *p)) { if (p) _aligned_free(p); }

// Register: Data holds an offset relative to the allocation base
XSTD(void, D3DResource_Register, 8, (XResource *r, void *base)) {
    r->Data += (DWORD)base;
    HostRes *h = HostGet(r);
    h->dirty = true;
    if (h->tex) { h->tex->Release(); h->tex = nullptr; }
}

XSTD(ULONG, D3DResource_AddRef, 4, (XResource *r)) {
    DWORD c = (r->Common & XCOMMON_REFCOUNT_MASK) + 1;
    r->Common = (r->Common & ~XCOMMON_REFCOUNT_MASK) | (c & XCOMMON_REFCOUNT_MASK);
    return c;
}
XSTD(ULONG, D3DResource_Release, 4, (XResource *r)) {
    DWORD c = r->Common & XCOMMON_REFCOUNT_MASK;
    if (c) c--;
    r->Common = (r->Common & ~XCOMMON_REFCOUNT_MASK) | c;
    if (c == 0) {
        bool created = (r->Common & XCOMMON_D3DCREATED) != 0;
        HostRes *h = HostOf(r);
        bool ownsData = h && !h->isSurfaceView && h->bytes && created;
        UnbindResource(r);
        if (ownsData && r->Data) _aligned_free((void *)r->Data);
        HostFree(r);
        if (created) free(r);
    }
    return c;
}
XSTD(DWORD, D3DResource_GetType, 4, (XResource *r)) {
    switch (XType(r)) {
    case XCOMMON_TYPE_VERTEXBUFFER: return XRT_VERTEXBUFFER;
    case XCOMMON_TYPE_INDEXBUFFER: return XRT_INDEXBUFFER;
    case XCOMMON_TYPE_PUSHBUFFER: return XRT_PUSHBUFFER;
    case XCOMMON_TYPE_PALETTE: return XRT_PALETTE;
    case XCOMMON_TYPE_SURFACE: return XRT_SURFACE;
    case XCOMMON_TYPE_FIXUP: return XRT_FIXUP;
    case XCOMMON_TYPE_TEXTURE: {
        auto p = (XPixelContainer *)r;
        if (p->Format & XFMT_CUBEMAP) return XRT_CUBETEXTURE;
        if (((p->Format & XFMT_DIMENSION_MASK) >> XFMT_DIMENSION_SHIFT) == 3) return XRT_VOLUMETEXTURE;
        return XRT_TEXTURE;
    }
    }
    return XRT_TEXTURE;
}

XSTD(BYTE *, D3DVertexBuffer_Lock2, 8, (XResource *vb, DWORD flags)) {
    return (BYTE *)vb->Data;
}

XSTD(DWORD, D3DBaseTexture_GetLevelCount, 4, (XPixelContainer *t)) {
    UINT l; XSurfaceDims(t, nullptr, nullptr, nullptr, nullptr, &l); return l;
}

static void FillDesc(XPixelContainer *p, UINT level, D3DSURFACE_DESC *out, bool tex) {
    // Xbox D3DSURFACE_DESC: Format, Type, Usage, Size, MultiSampleType, Width, Height
    UINT w, h, d, pitch, l;
    XSurfaceDims(p, &w, &h, &d, &pitch, &l);
    w = std::max(w >> level, 1u); h = std::max(h >> level, 1u);
    DWORD *o = (DWORD *)out;
    DWORD fmt = XFormat(p);
    o[0] = fmt;
    o[1] = tex ? XRT_SURFACE : XRT_SURFACE;
    HostRes *hr = HostOf(p);
    o[2] = hr && hr->isRT ? D3DUSAGE_RENDERTARGET : hr && hr->isDepth ? D3DUSAGE_DEPTHSTENCIL : 0;
    o[3] = p->Size ? pitch * h : LevelBytes(fmt, w, h, 1);
    o[4] = 0;
    o[5] = w;
    o[6] = h;
}
XSTD(HRESULT, D3DTexture_GetLevelDesc, 12, (XPixelContainer *t, UINT level, D3DSURFACE_DESC *d)) { FillDesc(t, level, d, true); return 0; }
XSTD(HRESULT, D3DSurface_GetDesc, 8, (XSurface *s, D3DSURFACE_DESC *d)) { FillDesc(s, 0, d, false); return 0; }
XSTD(HRESULT, D3DVolumeTexture_GetLevelDesc, 12, (XPixelContainer *t, UINT level, DWORD *o)) {
    // D3DVOLUME_DESC: Format, Type, Usage, Size, Width, Height, Depth
    UINT w, h, d, pitch, l;
    XSurfaceDims(t, &w, &h, &d, &pitch, &l);
    w = std::max(w >> level, 1u); h = std::max(h >> level, 1u); d = std::max(d >> level, 1u);
    o[0] = XFormat(t); o[1] = XRT_VOLUME; o[2] = 0; o[3] = LevelBytes(XFormat(t), w, h, d); o[4] = w; o[5] = h; o[6] = d;
    return 0;
}

// Surfaces that view one level/face of a texture
XSurface *MakeSurfaceView(XPixelContainer *t, UINT face, UINT level) {
    HostRes *th = HostGet(t);
    for (auto &v : th->views) if (v.face == face && v.level == level) { D3DResource_AddRefInternal(v.surf); return v.surf; }
    UINT w, h, d, pitch, l;
    XSurfaceDims(t, &w, &h, &d, &pitch, &l);
    DWORD fmt = XFormat(t);
    auto s = (XSurface *)calloc(1, sizeof(XSurface) + 16);
    UINT lw = std::max(w >> level, 1u), lh = std::max(h >> level, 1u);
    s->Common = 1 | XCOMMON_TYPE_SURFACE;   // not D3DCREATED: lifetime follows the parent
    if (t->Size) { s->Format = t->Format; s->Size = t->Size; }
    else {
        auto lg = [](UINT v) { UINT r = 0; while ((1u << r) < v) r++; return r; };
        s->Format = (t->Format & ~(XFMT_MIPMAP_MASK | (0xFu << XFMT_USIZE_SHIFT) | (0xFu << XFMT_VSIZE_SHIFT) | (0xFu << XFMT_PSIZE_SHIFT) | XFMT_CUBEMAP)) |
                    (1 << XFMT_MIPMAP_SHIFT) | (lg(lw) << XFMT_USIZE_SHIFT) | (lg(lh) << XFMT_VSIZE_SHIFT);
        s->Size = 0;
    }
    UINT off = (t->Format & XFMT_CUBEMAP) ? face * FaceBytes(fmt, w, w, l) : 0;
    off += t->Size ? 0 : XLevelOffset(fmt, w, h, 1, level);
    s->Data = t->Data + off;
    s->Parent = t;
    HostRes *sh = HostGet(s);
    sh->isSurfaceView = true;
    sh->parent = t;
    sh->face = face;
    sh->level = level;
    th->views.push_back({face, level, s});
    D3DResource_AddRefInternal(s);
    return s;
}
XSTD(XSurface *, D3DTexture_GetSurfaceLevel2, 8, (XPixelContainer *t, UINT level)) { return MakeSurfaceView(t, 0, level); }
XSTD(XSurface *, D3DCubeTexture_GetCubeMapSurface2, 12, (XPixelContainer *t, DWORD face, UINT level)) { return MakeSurfaceView(t, face, level); }

static void LockCommon(XPixelContainer *p, UINT level, D3DLOCKED_RECT *lr, const RECT *rc) {
    UINT w, h, d, pitch, l;
    XSurfaceDims(p, &w, &h, &d, &pitch, &l);
    DWORD fmt = XFormat(p);
    HostRes *hr = HostOf(p);
    XPixelContainer *owner = (hr && hr->isSurfaceView) ? hr->parent : p;
    HostRes *oh = HostGet(owner);
    if (oh->isRT || oh->isDepth) ReadbackTarget(owner);
    oh->dirty = true;
    oh->watch = true;
    UINT lw = std::max(w >> level, 1u);
    UINT off = p->Size ? 0 : XLevelOffset(fmt, w, h, 1, level);
    UINT lp = p->Size ? pitch : XFmtIsCompressed(fmt) ? ((lw + 3) / 4) * (fmt == XF_DXT1 ? 8 : 16) : lw * XFmtBpp(fmt) / 8;
    uint8_t *bits = (uint8_t *)p->Data + off;
    if (rc && p->Size) bits += rc->top * lp + rc->left * XFmtBpp(fmt) / 8;
    lr->Pitch = lp;
    lr->pBits = bits;
}
XSTD(HRESULT, D3DTexture_LockRect, 20, (XPixelContainer *t, UINT level, D3DLOCKED_RECT *lr, const RECT *rc, DWORD fl)) {
    LockCommon(t, level, lr, rc); return 0;
}
XSTD(HRESULT, D3DSurface_LockRect, 16, (XSurface *s, D3DLOCKED_RECT *lr, const RECT *rc, DWORD fl)) {
    LockCommon(s, 0, lr, rc); return 0;
}
XSTD(HRESULT, D3DVolumeTexture_LockBox, 20, (XPixelContainer *t, UINT level, D3DLOCKED_BOX *lb, const D3DBOX *box, DWORD fl)) {
    UINT w, h, d, pitch, l;
    XSurfaceDims(t, &w, &h, &d, &pitch, &l);
    DWORD fmt = XFormat(t);
    HostGet(t)->dirty = true;
    UINT lw = std::max(w >> level, 1u), lh = std::max(h >> level, 1u);
    lb->RowPitch = lw * XFmtBpp(fmt) / 8;
    lb->SlicePitch = lb->RowPitch * lh;
    lb->pBits = (uint8_t *)t->Data + XLevelOffset(fmt, w, h, d, level);
    return 0;
}

void D3DResource_AddRefInternal(XResource *r) { xb_D3DResource_AddRef(r); }

// called when a resource dies: drop any device bindings that reference it
void MarkTextureDirty(const void *data, size_t n) {
    for (auto &kv : g_hostRes) {
        auto p = (const XPixelContainer *)kv.first;
        if (kv.second->bytes && p->Data >= (DWORD)data && p->Data < (DWORD)data + n) kv.second->dirty = true;
    }
}
