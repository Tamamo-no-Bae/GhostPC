// Xbox D3D device emulation on Direct3D 9: device, targets, state, draw calls.
#include "d3d_int.h"
#include "settings.h"
#include <unordered_map>
#include <set>
#include <intrin.h>
#include <string>
#pragma comment(lib, "d3d9.lib")

IDirect3D9 *g_d3d;
IDirect3DDevice9 *g_dev;
extern HWND g_hwnd;
int g_scale = 2;
DWORD *g_rs, *g_ts;
static DWORD *g_dirtyFlags, *g_indexData;

static XSurface *g_backbuffer, *g_depthbuf;
// Multisampling (the game asks for it at CreateDevice): the game's back buffer and depth buffer are multisampled
// surfaces; g_realBB (the swap chain's back buffer) gets the resolved image before Present. Off-screen render targets
// are not multisampled, so they get g_depthPlain when the game pairs them with the main depth buffer.
static D3DMULTISAMPLE_TYPE g_msaa = D3DMULTISAMPLE_NONE;
static IDirect3DSurface9 *g_realBB, *g_depthPlain;
// a non-multisampled copy of `s` that can be read back or locked (s itself when it is not multisampled)
static IDirect3DSurface9 *ResolveForRead(IDirect3DSurface9 *s) {
    static IDirect3DSurface9 *tmp;
    if (!s) return nullptr;
    D3DSURFACE_DESC d; s->GetDesc(&d);
    if (d.MultiSampleType == D3DMULTISAMPLE_NONE) return s;
    D3DSURFACE_DESC td = {};
    if (tmp) tmp->GetDesc(&td);
    if (!tmp || td.Width != d.Width || td.Height != d.Height || td.Format != d.Format) {
        if (tmp) tmp->Release();
        tmp = nullptr;
        if (FAILED(g_dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &tmp, nullptr))) return nullptr;
    }
    g_dev->StretchRect(s, nullptr, tmp, nullptr, D3DTEXF_NONE);
    return tmp;
}
static XPixelContainer *g_curRT, *g_curDS;
static UINT g_rtW = 640, g_rtH = 480;           // unscaled size of the current render target
static D3DVIEWPORT9 g_vp = {0, 0, 640, 480, 0, 1};
static DWORD g_curVS;                           // handle passed to SetVertexShader
static const XPixelShaderDef *g_curPS;          // null = fixed function
static XPixelContainer *g_tex[4];
StreamState g_streams[16];
static XResource *g_ib;
int g_baseVertex;
float g_vsConst[192 * 4];
static D3DMATRIX g_xform[XTS_MAX];
UINT g_frame;
static DWORD g_createInterval;   // FullScreen_PresentationInterval requested at CreateDevice
static bool g_overlayOn;
static XSurface *g_overlaySurf;
static RECT g_overlaySrc, g_overlayDst;
static DWORD g_fence;

// GHOSTPC_TRACE=<frame>: log every draw call of that frame
static bool g_traceFrame;
UINT g_traceRequest;   // >0: trace that many frames from the next one
static UINT g_traceAt = (UINT)-1;
static UINT g_frameDraw;
static UINT g_fsOffscreen, g_fsDepthTex;   // per-frame stats for GHOSTPC_FRAMESTATS
static std::vector<std::string> g_fsSig, g_fsPrevSig;   // per-draw signatures (this / previous frame)
static bool g_framePoints;                              // this frame drew a point list (GHOSTPC_TRACEPOINTS)
static bool g_shadowSaved;                              // GHOSTPC_TRACEJUMP: shadow texture saved this frame
static UINT g_fsVSDraws;                                // draws through a vertex shader program this frame
extern std::string *g_logCapture;
extern DWORD g_logCaptureThread;
static bool g_inPB;   // drawing from RunPushBuffer
static IDirect3DVertexBuffer9 *g_dynVB;
static IDirect3DIndexBuffer9 *g_dynIB;
static UINT g_vbPos, g_ibPos;
static const UINT VB_SIZE = 8 << 20, IB_SIZE = 2 << 20;

// ---------------------------------------------------------------------------
// D3D9 state cache
static DWORD g_rsCache[256], g_rsValid[8];
static void SetRS(D3DRENDERSTATETYPE s, DWORD v) {
    if ((g_rsValid[s >> 5] & (1u << (s & 31))) && g_rsCache[s] == v) return;
    g_rsValid[s >> 5] |= 1u << (s & 31);
    g_rsCache[s] = v;
    g_dev->SetRenderState(s, v);
}
static DWORD g_ssCache[4][14], g_ssValid[4];
static void SetSS(DWORD st, D3DSAMPLERSTATETYPE s, DWORD v) {
    if ((g_ssValid[st] & (1u << s)) && g_ssCache[st][s] == v) return;
    g_ssValid[st] |= 1u << s;
    g_ssCache[st][s] = v;
    g_dev->SetSamplerState(st, s, v);
}
static DWORD g_tssCache[8][33], g_tssValid[8];
static void SetTSS(DWORD st, D3DTEXTURESTAGESTATETYPE s, DWORD v) {
    if ((g_tssValid[st] & (1u << s)) && g_tssCache[st][s] == v) return;
    g_tssValid[st] |= 1u << s;
    g_tssCache[st][s] = v;
    g_dev->SetTextureStageState(st, s, v);
}
static void InvalidateCaches() { memset(g_rsValid, 0, sizeof g_rsValid); memset(g_ssValid, 0, sizeof g_ssValid); memset(g_tssValid, 0, sizeof g_tssValid); }

// ---------------------------------------------------------------------------
// GL-style NV2A enums -> D3D9
static D3DCMPFUNC Cmp(DWORD v) { return (v >= 0x200 && v <= 0x207) ? (D3DCMPFUNC)(v - 0x200 + 1) : D3DCMP_ALWAYS; }
static DWORD Blend(DWORD v) {
    switch (v) {
    case 0: return D3DBLEND_ZERO; case 1: return D3DBLEND_ONE;
    case 0x300: return D3DBLEND_SRCCOLOR; case 0x301: return D3DBLEND_INVSRCCOLOR;
    case 0x302: return D3DBLEND_SRCALPHA; case 0x303: return D3DBLEND_INVSRCALPHA;
    case 0x304: return D3DBLEND_DESTALPHA; case 0x305: return D3DBLEND_INVDESTALPHA;
    case 0x306: return D3DBLEND_DESTCOLOR; case 0x307: return D3DBLEND_INVDESTCOLOR;
    case 0x308: return D3DBLEND_SRCALPHASAT;
    case 0x8001: case 0x8003: return D3DBLEND_BLENDFACTOR;
    case 0x8002: case 0x8004: return D3DBLEND_INVBLENDFACTOR;
    }
    return D3DBLEND_ONE;
}
static DWORD BlendOp(DWORD v) {
    switch (v) {
    case 0x8006: return D3DBLENDOP_ADD; case 0x800A: return D3DBLENDOP_SUBTRACT; case 0x800B: return D3DBLENDOP_REVSUBTRACT;
    case 0x8007: return D3DBLENDOP_MIN; case 0x8008: return D3DBLENDOP_MAX;
    }
    return D3DBLENDOP_ADD;
}
static DWORD StencilOp(DWORD v) {
    switch (v) {
    case 0x1E00: return D3DSTENCILOP_KEEP; case 0: return D3DSTENCILOP_ZERO; case 0x1E01: return D3DSTENCILOP_REPLACE;
    case 0x1E02: return D3DSTENCILOP_INCRSAT; case 0x1E03: return D3DSTENCILOP_DECRSAT; case 0x150A: return D3DSTENCILOP_INVERT;
    case 0x8507: return D3DSTENCILOP_INCR; case 0x8508: return D3DSTENCILOP_DECR;
    }
    return D3DSTENCILOP_KEEP;
}

// Simple render states arrive as NV2A methods; map them to XRS indices (stored in g_rs)
enum { XRS_ZFUNC = 56, XRS_ALPHAFUNC, XRS_ALPHABLENDENABLE, XRS_ALPHATESTENABLE, XRS_ALPHAREF, XRS_SRCBLEND,
       XRS_DESTBLEND, XRS_ZWRITEENABLE, XRS_DITHERENABLE, XRS_SHADEMODE, XRS_COLORWRITEENABLE, XRS_STENCILZFAIL,
       XRS_STENCILPASS, XRS_STENCILFUNC, XRS_STENCILREF, XRS_STENCILMASK, XRS_STENCILWRITEMASK, XRS_BLENDOP,
       XRS_BLENDCOLOR, XRS_SWATHWIDTH, XRS_POLYGONOFFSETZSLOPESCALE, XRS_POLYGONOFFSETZOFFSET,
       XRS_POINTOFFSETENABLE, XRS_WIREFRAMEOFFSETENABLE, XRS_SOLIDOFFSETENABLE, XRS_DEPTHCLIPCONTROL,
       XRS_STIPPLEENABLE };
// our own copy of simple states (by method) so we never depend on the exact simple-index layout
static std::unordered_map<DWORD, DWORD> g_simple;
static DWORD Simple(DWORD method, DWORD def) { auto it = g_simple.find(method); return it == g_simple.end() ? def : it->second; }

// ---------------------------------------------------------------------------
// Window
bool InputWndProc(HWND h, UINT m, WPARAM w, LPARAM l);
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (InputWndProc(h, m, w, l)) return 0;
    switch (m) {
    case WM_CLOSE: Log("window closed"); ExitProcess(0);   // a CDS_FULLSCREEN display mode is restored by Windows at exit
    case WM_ACTIVATE: { extern void DisplayFocusChanged(bool); DisplayFocusChanged(LOWORD(w) != WA_INACTIVE); break; }
    case WM_SETCURSOR: if (LOWORD(l) == HTCLIENT) { SetCursor(nullptr); return TRUE; } break;
    }
    return DefWindowProcA(h, m, w, l);
}
static void PumpMessages() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
}
// Window per the settings: windowed (client = chosen resolution), fullscreen (display mode switched to the
// chosen resolution, borderless popup on top) or borderless (popup covering the desktop).
static bool g_modeChanged;
static DEVMODEA g_fsMode;
static bool ApplyFullscreenMode() {
    g_fsMode = {};
    g_fsMode.dmSize = sizeof g_fsMode;
    g_fsMode.dmPelsWidth = g_settings.width;
    g_fsMode.dmPelsHeight = g_settings.height;
    g_fsMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    g_modeChanged = ChangeDisplaySettingsA(&g_fsMode, CDS_FULLSCREEN) == DISP_CHANGE_SUCCESSFUL;
    if (!g_modeChanged) Log("display: %dx%d is not available, using borderless", g_settings.width, g_settings.height);
    return g_modeChanged;
}
void DisplayFocusChanged(bool active) {
    if (g_settings.mode != DM_FULLSCREEN || !g_hwnd) return;
    if (!active && g_modeChanged) { ChangeDisplaySettingsA(nullptr, 0); g_modeChanged = false; ShowWindow(g_hwnd, SW_MINIMIZE); }
    else if (active && !g_modeChanged) {
        if (ChangeDisplaySettingsA(&g_fsMode, CDS_FULLSCREEN) == DISP_CHANGE_SUCCESSFUL) {
            g_modeChanged = true;
            SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, g_fsMode.dmPelsWidth, g_fsMode.dmPelsHeight, SWP_SHOWWINDOW);
        }
    }
}
static void RestoreDisplay() { if (g_modeChanged) { ChangeDisplaySettingsA(nullptr, 0); g_modeChanged = false; } }
static HWND MakeWindow() {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);   // letterbox bars
    wc.lpszClassName = "GhostPC";
    RegisterClassA(&wc);
    const char *title = "StarCraft: Ghost (GhostPC)";
    int mode = g_settings.mode;
    if (mode == DM_FULLSCREEN && !ApplyFullscreenMode()) mode = DM_BORDERLESS;
    if (mode == DM_WINDOWED) {
        RECT r = {0, 0, g_settings.width, g_settings.height};
        DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        AdjustWindowRect(&r, style, FALSE);
        return CreateWindowA("GhostPC", title, style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    }
    atexit(RestoreDisplay);
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    return CreateWindowExA(mode == DM_FULLSCREEN ? WS_EX_TOPMOST : 0, "GhostPC", title, WS_POPUP | WS_VISIBLE, 0, 0, w, h,
                           nullptr, nullptr, wc.hInstance, nullptr);
}
// the 4:3 picture fitted into the client area
static RECT PresentRect() {
    RECT c;
    GetClientRect(g_hwnd, &c);
    int cw = c.right, ch = c.bottom;
    int w = cw, h = cw * 3 / 4;
    if (h > ch) { h = ch; w = ch * 4 / 3; }
    RECT r = {(cw - w) / 2, (ch - h) / 2, (cw - w) / 2 + w, (ch - h) / 2 + h};
    return r;
}

// ---------------------------------------------------------------------------
// Render targets
void MakeRenderTarget(XPixelContainer *p, HostRes *h) {
    UINT w, hh, d, pitch, l;
    XSurfaceDims(p, &w, &hh, &d, &pitch, &l);
    D3DFORMAT f = XFmtHost(XFormat(p));
    if (f != D3DFMT_A8R8G8B8 && f != D3DFMT_X8R8G8B8 && f != D3DFMT_R5G6B5 && f != D3DFMT_A1R5G5B5) f = D3DFMT_A8R8G8B8;
    h->isRT = true;
    h->w = w; h->h = hh;
    h->linear = XFmtIsLinear(XFormat(p));
    HRESULT hr;
    if (p->Format & XFMT_CUBEMAP)
        hr = g_dev->CreateCubeTexture(w * g_scale, 1, D3DUSAGE_RENDERTARGET, f, D3DPOOL_DEFAULT, (IDirect3DCubeTexture9 **)&h->tex, nullptr);
    else
        hr = g_dev->CreateTexture(w * g_scale, hh * g_scale, 1, D3DUSAGE_RENDERTARGET, f, D3DPOOL_DEFAULT, (IDirect3DTexture9 **)&h->tex, nullptr);
    if (FAILED(hr)) Log("d3d: create RT %ux%u failed %08x", w, hh, hr);
    LOGV("RT %p %ux%u fmt %02x", p, w, hh, XFormat(p));
}
void MakeDepthTarget(XPixelContainer *p, HostRes *h) {
    UINT w, hh, d, pitch, l;
    XSurfaceDims(p, &w, &hh, &d, &pitch, &l);
    h->isDepth = true;
    h->w = w; h->h = hh;
    D3DFORMAT f = XFmtHost(XFormat(p));
    // a depth *texture* allows shadow-map sampling (hardware PCF on all D3D9 vendors)
    HRESULT hr = g_dev->CreateTexture(w * g_scale, hh * g_scale, 1, D3DUSAGE_DEPTHSTENCIL, f, D3DPOOL_DEFAULT, (IDirect3DTexture9 **)&h->tex, nullptr);
    if (FAILED(hr)) {
        h->tex = nullptr;
        hr = g_dev->CreateDepthStencilSurface(w * g_scale, hh * g_scale, f, D3DMULTISAMPLE_NONE, 0, FALSE, &h->surf, nullptr);
        if (FAILED(hr)) Log("d3d: create depth %ux%u failed %08x", w, hh, hr);
    }
}
IDirect3DSurface9 *HostSurface(XPixelContainer *p) {
    if (!p) return nullptr;
    HostRes *h = HostOf(p);
    if (!h) return nullptr;
    if (h->surf) return h->surf;
    if (h->isSurfaceView && h->parent) {
        HostRes *ph = HostOf(h->parent);
        if (!ph || !ph->tex) return nullptr;
        IDirect3DSurface9 *s = nullptr;
        if (h->parent->Format & XFMT_CUBEMAP) ((IDirect3DCubeTexture9 *)ph->tex)->GetCubeMapSurface((D3DCUBEMAP_FACES)h->face, h->level, &s);
        else ((IDirect3DTexture9 *)ph->tex)->GetSurfaceLevel(h->level, &s);
        h->surf = s;
        return s;
    }
    if (h->tex && (h->isRT || h->isDepth)) {
        ((IDirect3DTexture9 *)h->tex)->GetSurfaceLevel(0, &h->surf);
        return h->surf;
    }
    return nullptr;
}
static HostRes *TargetInfo(XPixelContainer *p) {
    HostRes *h = HostOf(p);
    if (h && h->isSurfaceView && h->parent) return HostOf(h->parent);
    return h;
}
void ReadbackTarget(XPixelContainer *p) {
    HostRes *h = HostOf(p);
    if (!h || !h->isRT || !h->tex) return;
    IDirect3DSurface9 *rt = ResolveForRead(HostSurface(p)), *sys = nullptr;
    if (!rt) return;
    D3DSURFACE_DESC d;
    rt->GetDesc(&d);
    if (FAILED(g_dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr))) return;
    if (SUCCEEDED(g_dev->GetRenderTargetData(rt, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            UINT w, hh, dd, pitch, l;
            XSurfaceDims(p, &w, &hh, &dd, &pitch, &l);
            if (h->linear && XFmtBpp(XFormat(p)) == 32)
                for (UINT y = 0; y < hh; y++)
                    for (UINT x = 0; x < w; x++)
                        ((uint32_t *)(p->Data + y * pitch))[x] = ((uint32_t *)((uint8_t *)lr.pBits + y * g_scale * lr.Pitch))[x * g_scale];
            sys->UnlockRect();
        }
    }
    sys->Release();
}

// remove a dying resource from all bindings
void UnbindResource(XResource *r) {
    for (int i = 0; i < 4; i++) if (g_tex[i] == (XPixelContainer *)r) { g_tex[i] = nullptr; g_dev->SetTexture(i, nullptr); }
    for (auto &s : g_streams) if (s.vb == r) s.vb = nullptr;
    if (g_ib == r) g_ib = nullptr;
    HostRes *h = HostOf(r);
    if (h) {
        for (auto &v : h->views) {
            HostRes *vh = HostOf(v.surf);
            if (vh && vh->surf) { vh->surf->Release(); vh->surf = nullptr; }
            HostFree(v.surf);
            free(v.surf);
        }
        h->views.clear();
    }
}


// Draw a host texture into a render target surface with a screen-space quad (StretchRect can't read managed textures)
static void ApplyViewport();
static void BlitTexture(IDirect3DBaseTexture9 *tex, UINT tw, UINT th, const RECT *src, IDirect3DSurface9 *dst, const RECT *dstRc) {
    if (!tex || !dst) return;
    IDirect3DSurface9 *oldRT = nullptr, *oldDS = nullptr;
    g_dev->GetRenderTarget(0, &oldRT);
    g_dev->GetDepthStencilSurface(&oldDS);
    g_dev->SetRenderTarget(0, dst);
    g_dev->SetDepthStencilSurface(nullptr);
    D3DSURFACE_DESC dd; dst->GetDesc(&dd);
    RECT d = dstRc ? *dstRc : RECT{0, 0, (LONG)dd.Width, (LONG)dd.Height};
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (src && src->right > src->left) { u0 = (float)src->left / tw; v0 = (float)src->top / th; u1 = (float)src->right / tw; v1 = (float)src->bottom / th; }
    struct V { float x, y, z, w, u, v; } q[4] = {
        {d.left - 0.5f, d.top - 0.5f, 0, 1, u0, v0}, {d.right - 0.5f, d.top - 0.5f, 0, 1, u1, v0},
        {d.left - 0.5f, d.bottom - 0.5f, 0, 1, u0, v1}, {d.right - 0.5f, d.bottom - 0.5f, 0, 1, u1, v1}};
    D3DVIEWPORT9 vp = {0, 0, dd.Width, dd.Height, 0, 1};
    g_dev->SetViewport(&vp);
    g_dev->SetVertexShader(nullptr);
    g_dev->SetPixelShader(nullptr);
    g_dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    g_dev->SetTexture(0, tex);
    g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g_dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    g_dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, 0);
    g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    g_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    g_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    g_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    g_dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    g_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    g_dev->SetRenderTarget(0, oldRT);
    g_dev->SetDepthStencilSurface(oldDS);
    if (oldRT) oldRT->Release();
    if (oldDS) oldDS->Release();
    InvalidateCaches();
    ApplyViewport();
    for (int i = 0; i < 4; i++) g_dev->SetTexture(i, nullptr);
}

static void BindTargets() {
    IDirect3DSurface9 *rt = HostSurface(g_curRT);
    if (rt) g_dev->SetRenderTarget(0, rt);
    IDirect3DSurface9 *ds = HostSurface(g_curDS);
    if (g_msaa) {
        // depth and colour must agree on multisampling
        bool rtMS = g_curRT == (XPixelContainer *)g_backbuffer;
        if (g_curDS == (XPixelContainer *)g_depthbuf && !rtMS) ds = g_depthPlain;
        else if (rtMS && g_curDS && g_curDS != (XPixelContainer *)g_depthbuf) {
            static int warned;
            if (warned++ < 5) Log("d3d: back buffer paired with depth %p (not multisampled): drawing without depth", g_curDS);
            ds = nullptr;
        }
    }
    g_dev->SetDepthStencilSurface(ds);
    HostRes *h = TargetInfo(g_curRT);
    if (h && h->w) {
        g_rtW = h->w; g_rtH = h->h;
        if (h->isSurfaceView || (g_curRT && HostOf(g_curRT)->isSurfaceView)) {
            HostRes *vh = HostOf(g_curRT);
            if (vh->isSurfaceView) { g_rtW = std::max(h->w >> vh->level, 1u); g_rtH = std::max(h->h >> vh->level, 1u); }
        }
    }
    g_vp = {0, 0, g_rtW, g_rtH, 0, 1};
    ApplyViewport();
}

// ---------------------------------------------------------------------------
// Device creation
static XSurface *NewSurface(DWORD fmt, UINT w, UINT h) {
    auto s = (XSurface *)calloc(1, sizeof(XSurface) + 16);
    UINT pitch = (w * XFmtBpp(fmt) / 8 + 63) & ~63u;
    s->Common = 1 | XCOMMON_TYPE_SURFACE | XCOMMON_D3DCREATED;
    s->Format = (fmt << XFMT_FORMAT_SHIFT) | (2 << XFMT_DIMENSION_SHIFT) | (1 << XFMT_MIPMAP_SHIFT) | 1;
    s->Size = (w - 1) | ((h - 1) << XSIZE_HEIGHT_SHIFT) | ((pitch / 64 - 1) << XSIZE_PITCH_SHIFT);
    s->Data = (DWORD)_aligned_malloc(pitch * h, 128);
    memset((void *)s->Data, 0, pitch * h);
    return s;
}

static void *g_fakeD3D[16], *g_fakeDevice[64];

XSTD(void *, Direct3DCreate8, 4, (UINT)) { return g_fakeD3D; }
XSTD(HRESULT, Direct3D_SetPushBufferSize, 8, (DWORD, DWORD)) { return 0; }

static void DefaultStates();
XSTD(HRESULT, Direct3D_CreateDevice, 24, (UINT ad, DWORD type, HWND hw, DWORD flags, XPresentParams *pp, void **out)) {
    g_rs = (DWORD *)SymAddr("_D3D__RenderState");
    g_ts = (DWORD *)SymAddr("_D3D__TextureState");
    g_dirtyFlags = (DWORD *)SymAddr("_D3D__DirtyFlags");
    g_indexData = (DWORD *)SymAddr("_D3D__IndexData");
    UINT w = pp->BackBufferWidth ? pp->BackBufferWidth : 640, h = pp->BackBufferHeight ? pp->BackBufferHeight : 480;
    Log("d3d: CreateDevice %ux%u fmt %02x depth %d/%02x ms %x flags %x interval %x", w, h, pp->BackBufferFormat,
        pp->EnableAutoDepthStencil, pp->AutoDepthStencilFormat, pp->MultiSampleType, pp->Flags,
        pp->FullScreen_PresentationInterval);
    g_createInterval = pp->FullScreen_PresentationInterval;
    int outH = g_settings.mode == DM_BORDERLESS ? GetSystemMetrics(SM_CYSCREEN) : g_settings.height;
    g_scale = RenderScaleFor(outH);
    if (const char *s = getenv("GHOSTPC_SCALE")) g_scale = std::max(1, atoi(s));
    if (!g_hwnd) g_hwnd = MakeWindow();
    Log("display: window %dx%d mode %d, rendering at %ux%u", g_settings.width, g_settings.height, g_settings.mode, w * g_scale, h * g_scale);
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) Fatal("Direct3DCreate9 failed");
    D3DPRESENT_PARAMETERS p = {};
    p.BackBufferWidth = w * g_scale;
    p.BackBufferHeight = h * g_scale;
    p.BackBufferFormat = D3DFMT_X8R8G8B8;
    p.BackBufferCount = 1;
    p.SwapEffect = D3DSWAPEFFECT_COPY;   // needed for Present with a destination rect (scaling + letterbox)
    p.hDeviceWindow = g_hwnd;
    p.Windowed = TRUE;
    p.EnableAutoDepthStencil = FALSE;
    p.PresentationInterval = g_settings.vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    HRESULT hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED, &p, &g_dev);
    if (FAILED(hr)) Fatal("CreateDevice failed %08x", hr);
    g_dev->CreateVertexBuffer(VB_SIZE, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &g_dynVB, nullptr);
    g_dev->CreateIndexBuffer(IB_SIZE, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX32, D3DPOOL_DEFAULT, &g_dynIB, nullptr);

    // multisampling: 4x where supported (GHOSTPC_MSAA=0/2/4/8 overrides), falling back to 2x / none
    {
        const char *e = getenv("GHOSTPC_MSAA");
        int want = e ? atoi(e) : 4;
        for (int n = want; n >= 2; n /= 2) {
            D3DMULTISAMPLE_TYPE t = (D3DMULTISAMPLE_TYPE)n;
            if (SUCCEEDED(g_d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, TRUE, t, nullptr)) &&
                SUCCEEDED(g_d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_D24S8, TRUE, t, nullptr))) {
                g_msaa = t;
                break;
            }
        }
        Log("d3d: multisampling %dx", g_msaa ? (int)g_msaa : 1);
    }
    g_backbuffer = NewSurface(XF_LIN_X8R8G8B8, w, h);
    HostRes *bh = HostGet(g_backbuffer);
    bh->isBackbuffer = bh->isRT = true;
    bh->w = w; bh->h = h;
    g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &g_realBB);
    if (!g_msaa || FAILED(g_dev->CreateRenderTarget(w * g_scale, h * g_scale, D3DFMT_X8R8G8B8, g_msaa, 0, FALSE, &bh->surf, nullptr))) {
        g_msaa = D3DMULTISAMPLE_NONE;
        bh->surf = g_realBB;
        bh->surf->AddRef();
    }
    g_depthbuf = NewSurface(XF_LIN_D24S8, w, h);
    HostRes *dh = HostGet(g_depthbuf);
    dh->isDepth = true; dh->w = w; dh->h = h;
    g_dev->CreateDepthStencilSurface(w * g_scale, h * g_scale, D3DFMT_D24S8, g_msaa, 0, FALSE, &dh->surf, nullptr);
    if (g_msaa) g_dev->CreateDepthStencilSurface(w * g_scale, h * g_scale, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &g_depthPlain, nullptr);
    if (pp->BufferSurfaces[0] == nullptr && out) {}
    g_curRT = g_backbuffer;
    g_curDS = pp->EnableAutoDepthStencil || true ? g_depthbuf : nullptr;
    BindTargets();
    DefaultStates();
    g_dev->BeginScene();
    if (out) *out = g_fakeDevice;
    return 0;
}
XSTD(ULONG, D3DDevice_AddRef, 0, ()) { return 1; }
XSTD(ULONG, D3DDevice_Release, 0, ()) { return 1; }
XSTD(HRESULT, D3DDevice_GetDirect3D, 4, (void **p)) { *p = g_fakeD3D; return 0; }
XSTD(HRESULT, D3DDevice_GetDisplayMode, 4, (DWORD *m)) {
    m[0] = 640; m[1] = 480; m[2] = 60; m[3] = 0; m[4] = XF_LIN_X8R8G8B8;
    return 0;
}
XSTD(HRESULT, D3DDevice_GetDeviceCaps, 4, (DWORD *c)) {
    // D3DCAPS8 with NV2A-like values
    memset(c, 0, 0xD4);
    auto F = [](float f) { DWORD d; memcpy(&d, &f, 4); return d; };
    c[0x00 / 4] = D3DDEVTYPE_HAL;
    c[0x08 / 4] = 0x00020000;                  // Caps: READ_SCANLINE
    c[0x0C / 4] = 0x00020000;                  // Caps2: FULLSCREENGAMMA
    c[0x14 / 4] = 0x8000000F;                  // presentation intervals
    c[0x1C / 4] = 0x001BBEF0;                  // DevCaps
    c[0x20 / 4] = 0x0000FFF2;                  // PrimitiveMiscCaps
    c[0x24 / 4] = 0x00F7F1B1;                  // RasterCaps
    c[0x28 / 4] = c[0x34 / 4] = 0xFF;          // Z/alpha compare
    c[0x2C / 4] = c[0x30 / 4] = 0x1FFF;        // blend caps
    c[0x38 / 4] = 0x000C4208;                  // ShadeCaps
    c[0x3C / 4] = 0x0001EC45;                  // TextureCaps: persp, pow2, alpha, projected, cube, volume, mip*
    c[0x40 / 4] = c[0x44 / 4] = c[0x48 / 4] = 0x03030700;
    c[0x4C / 4] = c[0x50 / 4] = 0x3F;          // address caps
    c[0x54 / 4] = 0x1F;                        // LineCaps
    c[0x58 / 4] = c[0x5C / 4] = 4096;
    c[0x60 / 4] = 512;
    c[0x64 / 4] = 8192;
    c[0x68 / 4] = 8192;
    c[0x6C / 4] = 8;
    c[0x70 / 4] = F(1e10f);
    c[0x74 / 4] = F(-1e8f); c[0x78 / 4] = F(-1e8f); c[0x7C / 4] = F(1e8f); c[0x80 / 4] = F(1e8f);
    c[0x88 / 4] = 0xFF;                        // StencilCaps
    c[0x8C / 4] = 0x00080004;                  // FVFCaps: 4 texcoord sets
    c[0x90 / 4] = 0x03FFFFFF;                  // TextureOpCaps
    c[0x94 / 4] = 4; c[0x98 / 4] = 4;
    c[0x9C / 4] = 0x3B;                        // VertexProcessingCaps
    c[0xA0 / 4] = 8; c[0xA4 / 4] = 4; c[0xA8 / 4] = 4; c[0xAC / 4] = 0;
    c[0xB0 / 4] = F(64.f);
    c[0xB4 / 4] = 0x000FFFFF; c[0xB8 / 4] = 0x000FFFFF;
    c[0xBC / 4] = 16; c[0xC0 / 4] = 256;
    c[0xC4 / 4] = 0xFFFE0101; c[0xC8 / 4] = 192;
    c[0xCC / 4] = 0xFFFF0101; c[0xD0 / 4] = F(1.0f);
    return 0;
}

static void DefaultStates() {
    for (int s = 0; s < 4; s++) {
        DWORD *t = g_ts + s * 32;
        if (!t[XTSS_ADDRESSU]) { t[XTSS_ADDRESSU] = t[XTSS_ADDRESSV] = t[XTSS_ADDRESSW] = 1; }
        if (!t[XTSS_MAGFILTER]) t[XTSS_MAGFILTER] = t[XTSS_MINFILTER] = 1;
        if (!t[XTSS_COLOROP]) { t[XTSS_COLOROP] = s ? 1 : 4; t[XTSS_ALPHAOP] = s ? 1 : 2; t[XTSS_COLORARG1] = 2; t[XTSS_COLORARG2] = 1; t[XTSS_ALPHAARG1] = 2; t[XTSS_ALPHAARG2] = 1; }
        if (!t[XTSS_TEXCOORDINDEX]) t[XTSS_TEXCOORDINDEX] = s;
    }
    g_rs[XRS_ZENABLE] = 1;
    g_rs[XRS_CULLMODE] = 0x901;
    g_rs[XRS_FRONTFACE] = 0x900;
    g_rs[XRS_FILLMODE] = 0x1B02;
    g_rs[XRS_TEXTUREFACTOR] = 0xffffffff;
    g_rs[XRS_COLORVERTEX] = 1;
    g_rs[XRS_DIFFUSEMATERIALSOURCE] = 1;
    g_rs[XRS_SPECULARMATERIALSOURCE] = 2;
    if (!g_rs[XRS_POINTSIZE]) { float one = 1.0f; memcpy(&g_rs[XRS_POINTSIZE], &one, 4); memcpy(&g_rs[XRS_POINTSIZE_MAX], &one, 4); memcpy(&g_rs[XRS_POINTSCALE_A], &one, 4); }
    g_simple[0x354] = 0x203;    // ZFUNC LEQUAL
    g_simple[0x35C] = 1;        // ZWRITE
    g_simple[0x358] = 0x01010101;
    g_simple[0x37C] = 0x1D01;
    g_simple[0x33C] = 0x207;
    g_simple[0x344] = 1; g_simple[0x348] = 0;
    g_simple[0x350] = 0x8006;
    g_simple[0x364] = 0x207; g_simple[0x36C] = 0xff; g_simple[0x360] = 0xff;
    g_simple[0x370] = g_simple[0x374] = g_simple[0x378] = 0x1E00;
    for (int i = 0; i < XTS_MAX; i++) { memset(&g_xform[i], 0, sizeof(D3DMATRIX)); g_xform[i]._11 = g_xform[i]._22 = g_xform[i]._33 = g_xform[i]._44 = 1; }
}

// ---------------------------------------------------------------------------
// Present
static void DrawOverlay();
static LARGE_INTEGER g_fpsT0;
// The Xbox build presents every second vblank (D3DPRESENT_INTERVAL_TWO = 30 Hz); the movie players advance one
// frame per Swap, so presenting at 60 Hz doubled their speed. Present is vsynced; wait out the rest of N vblanks.
static void PaceFrame() {
    DWORD iv = g_rs ? g_rs[XRS_PRESENTATIONINTERVAL] : 0;
    if (!iv) iv = g_createInterval;
    if (iv & 0x80000000) return;                       // IMMEDIATE
    int n = (iv >= 2 && iv <= 4) ? (int)iv : 1;
    static LARGE_INTEGER f;
    static double next;                                // scheduled time of the next frame (seconds)
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double t = (double)now.QuadPart / f.QuadPart, period = n / 59.94;
    if (n == 1) { next = 0; return; }                  // plain vsync
    if (next == 0 || t - next > 0.1) next = t;         // first frame or we fell far behind: resync
    next += period;
    double rem = next - t;
    if (rem > 0.002) Sleep((DWORD)((rem - 0.0015) * 1000));
    do { QueryPerformanceCounter(&now); } while ((double)now.QuadPart / f.QuadPart < next);
}
// GHOSTPC_SHOT="n1,n2,..." saves those frames to testrameN.bmp (for automated checks)
static void SaveSurface(IDirect3DSurface9 *bb, const char *path);
// debug: average luma (0..255) of a back buffer region given in 640x480 coordinates
static float RegionLuma(const int *r) {
    static IDirect3DSurface9 *sys;
    IDirect3DSurface9 *bb = ResolveForRead(HostOf(g_backbuffer)->surf);
    if (!bb) return -1;
    D3DSURFACE_DESC d; bb->GetDesc(&d);
    if (!sys && FAILED(g_dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr))) return -1;
    if (FAILED(g_dev->GetRenderTargetData(bb, sys))) return -1;
    D3DLOCKED_RECT lr;
    if (FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return -1;
    double sum = 0; int n = 0;
    for (int y = r[1] * g_scale; y < r[3] * g_scale && y < (int)d.Height; y += g_scale)
        for (int x = r[0] * g_scale; x < r[2] * g_scale && x < (int)d.Width; x += g_scale) {
            const uint8_t *p = (const uint8_t *)lr.pBits + y * lr.Pitch + x * 4;
            sum += 0.299 * p[2] + 0.587 * p[1] + 0.114 * p[0]; n++;
        }
    sys->UnlockRect();
    return n ? (float)(sum / n) : -1;
}
static void MaybeShot() {
    static std::vector<UINT> want;
    static bool init;
    if (!init) {
        init = true;
        if (const char *e = getenv("GHOSTPC_SHOT")) for (const char *p = e; *p;) { want.push_back(atoi(p)); while (*p && *p != ',') p++; if (*p) p++; }
    }
    if (std::find(want.begin(), want.end(), g_frame) == want.end()) return;
    char path[MAX_PATH];
    _snprintf(path, MAX_PATH, "%s\\..\\test\\frame%u.bmp", g_gameDir, g_frame);
    SaveSurface(HostOf(g_backbuffer)->surf, path);
}
// writes a 32-bit render target to a .bmp (debug output)
static void SaveSurface(IDirect3DSurface9 *bb, const char *path) {
    IDirect3DSurface9 *sys = nullptr;
    bb = ResolveForRead(bb);
    if (!bb) return;
    D3DSURFACE_DESC d; bb->GetDesc(&d);
    if (FAILED(g_dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr))) return;
    if (SUCCEEDED(g_dev->GetRenderTargetData(bb, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            FILE *f = fopen(path, "wb");
            if (f) {
                BITMAPFILEHEADER fh = {0x4D42, (DWORD)(54 + d.Width * d.Height * 4), 0, 0, 54};
                BITMAPINFOHEADER ih = {40, (LONG)d.Width, -(LONG)d.Height, 1, 32, BI_RGB};
                fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
                for (UINT y = 0; y < d.Height; y++) fwrite((uint8_t *)lr.pBits + y * lr.Pitch, 4, d.Width, f);
                fclose(f);
                Log("saved %s", path);
            }
            sys->UnlockRect();
        }
    }
    sys->Release();
}
void ShellWatch();
XSTD(DWORD, D3DDevice_Swap, 4, (DWORD flags)) {
    ShellWatch();
    // GHOSTPC_TRACE=<frame> or <first>-<last>
    static UINT traceEnd;
    if (g_traceAt == (UINT)-1) {
        const char *e = getenv("GHOSTPC_TRACE");
        g_traceAt = e ? atoi(e) : (UINT)-2;
        const char *dash = e ? strchr(e, '-') : nullptr;
        traceEnd = dash ? atoi(dash + 1) : g_traceAt;
    }
    if (g_traceRequest) {   // another module asked to trace the next frames (e.g. debugdeath.cpp, GHOSTPC_TRACECLOAK)
        g_traceAt = g_frame + 1;
        traceEnd = g_frame + g_traceRequest;
        g_traceRequest = 0;
    }
    g_traceFrame = g_frame + 1 >= g_traceAt && g_frame + 1 <= traceEnd;
    // GHOSTPC_TRACEJUMP: trace every frame into memory; when a frame has 5+ more draws than the previous one, log
    // both (first 3 such frames)
    static bool tracePoints = getenv("GHOSTPC_TRACEJUMP") != nullptr;
    if (tracePoints) {
        static std::string bufs[2];
        static int kept, cur;
        static UINT prevDraws;
        auto dump = [](const std::string &b, const char *what, UINT f) {
            Log("=== %s frame %u ===", what, f);
            for (size_t a = 0, e; a < b.size(); a = e + 1) {
                e = b.find('\n', a);
                if (e == std::string::npos) e = b.size();
                Log("%.*s", (int)(e - a), b.c_str() + a);
            }
        };
        // GHOSTPC_TRACEJUMP=x0,y0,x1,y1 (640x480 coordinates): instead of the draw count, select frames where that
        // screen region gets clearly brighter than in the previous frame
        // or GHOSTPC_TRACEJUMP=d<n>: frames with at least n more draws than the previous one (default 5)
        static int reg[4] = {-1};
        static UINT minJump = 5, prevVS;
        static bool vsMode;
        if (reg[0] == -1) {
            reg[0] = -2;
            const char *e = getenv("GHOSTPC_TRACEJUMP");
            if (e[0] == 'd') minJump = atoi(e + 1);
            else if (e[0] == 'v') { minJump = atoi(e + 1); vsMode = true; }   // v<n>: vertex-shader draws +n
            else if (sscanf(e, "%d,%d,%d,%d", &reg[0], &reg[1], &reg[2], &reg[3]) != 4) reg[0] = -2;
        }
        static float prevLum = -1;
        float lum = -1;
        if (reg[0] >= 0) lum = RegionLuma(reg);
        bool jump = reg[0] >= 0 ? (prevLum >= 0 && lum > prevLum + 4)
                  : vsMode      ? (prevVS && g_fsVSDraws >= prevVS + minJump)
                                : (prevDraws && g_frameDraw >= prevDraws + minJump);
        prevVS = g_fsVSDraws;
        g_fsVSDraws = 0;
        prevLum = lum;
        g_shadowSaved = false;
        if (g_frame > 600) {   // the frame itself, alternating files like the shadow texture
            char path[MAX_PATH];
            _snprintf(path, MAX_PATH, "%s\\..\\test\\jframe%u.bmp", g_gameDir, g_frame & 1);
            SaveSurface(HostOf(g_backbuffer)->surf, path);
        }
        if (g_logCapture) {
            g_logCapture = nullptr;
            if (jump && kept < 3) {
                kept++;
                for (int k = 0; k < 2; k++) {   // keep this frame's and the previous frame's images
                    char a[MAX_PATH], b[MAX_PATH];
                    UINT f = g_frame - 1 + k;
                    _snprintf(a, MAX_PATH, "%s\\..\\test\\jframe%u.bmp", g_gameDir, f & 1);
                    _snprintf(b, MAX_PATH, "%s\\..\\test\\keep_frame%u.bmp", g_gameDir, f);
                    CopyFileA(a, b, FALSE);
                    _snprintf(a, MAX_PATH, "%s\\..\\test\\jshadow%u.bmp", g_gameDir, f & 1);
                    _snprintf(b, MAX_PATH, "%s\\..\\test\\keep_shadow%u.bmp", g_gameDir, f);
                    CopyFileA(a, b, FALSE);
                }
                dump(bufs[cur ^ 1], "before-jump", g_frame - 1);
                dump(bufs[cur], "jump", g_frame);
            }
        }
        prevDraws = g_frameDraw;
        cur ^= 1;
        bufs[cur].clear();
        g_framePoints = false;
        if (kept < 3 && g_frame > 600) { g_logCapture = &bufs[cur]; g_logCaptureThread = GetCurrentThreadId(); g_traceFrame = true; }
    }
    // GHOSTPC_FRAMESTATS: one line per frame (draws, off-screen draws, draws sampling a depth texture)
    static bool frameStats = getenv("GHOSTPC_FRAMESTATS") != nullptr;
    if (frameStats) {
        Log("fs %u draws %u off %u depthtex %u", g_frame, g_frameDraw, g_fsOffscreen, g_fsDepthTex);
        // a frame with a few more draws than the previous one: log the draws the previous frame did not have
        if (!g_fsPrevSig.empty() && g_fsSig.size() > g_fsPrevSig.size() && g_fsSig.size() <= g_fsPrevSig.size() + 16) {
            std::multiset<std::string> prev(g_fsPrevSig.begin(), g_fsPrevSig.end());
            for (size_t i = 0; i < g_fsSig.size(); i++) {
                auto it = prev.find(g_fsSig[i]);
                if (it != prev.end()) prev.erase(it);
                else Log("fs %u new draw %u: %s", g_frame, (UINT)i + 1, g_fsSig[i].c_str());
            }
        }
        g_fsPrevSig.swap(g_fsSig);
        g_fsSig.clear();
    }
    g_fsOffscreen = g_fsDepthTex = 0;
    g_frameDraw = 0;
    if (g_traceFrame) Log("=== trace frame %u ===", g_frame + 1);
    if (g_overlayOn && g_overlaySurf) DrawOverlay();
    MaybeShot();
    g_dev->EndScene();
    if (g_msaa) g_dev->StretchRect(HostOf(g_backbuffer)->surf, nullptr, g_realBB, nullptr, D3DTEXF_NONE);   // resolve
    RECT dst = PresentRect();
    HRESULT hr = g_dev->Present(nullptr, &dst, nullptr, nullptr);
    PaceFrame();
    if (FAILED(hr)) Log("Present failed %08x", hr);
    g_dev->BeginScene();
    PumpMessages();
    g_frame++;
    if ((g_frame % 300) == 0) {
        LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
        if (g_fpsT0.QuadPart) Log("frame %u, %.1f fps", g_frame, 300.0 * f.QuadPart / (t.QuadPart - g_fpsT0.QuadPart));
        g_fpsT0 = t;
    }
    return 0;
}
XSTD(HRESULT, D3DDevice_Clear, 24, (DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)) {
    static UINT logFrame = getenv("GHOSTPC_LOG_HUDFRAME") ? atoi(getenv("GHOSTPC_LOG_HUDFRAME")) : 0;
    if (logFrame && g_frame == logFrame) Log("hudframe: clear flags %lx viewport %lu,%lu", flags, g_vp.X, g_vp.Y);
    DWORD f = 0;
    if (flags & 0xF0) f |= D3DCLEAR_TARGET;
    if ((flags & 1) && g_curDS) f |= D3DCLEAR_ZBUFFER;
    if ((flags & 2) && g_curDS) f |= D3DCLEAR_STENCIL;
    if (!f) return 0;
    std::vector<D3DRECT> r;
    for (DWORD i = 0; i < count && rects; i++)
        r.push_back({rects[i].x1 * g_scale, rects[i].y1 * g_scale, rects[i].x2 * g_scale, rects[i].y2 * g_scale});
    // clear ignores the viewport on Xbox: use the whole target unless rects are given. Except in split screen (two
    // local players: pRightLocalPlayer, the map's gameMotion_apNames+0x72C), where each player's view clears its own
    // half (LocalCameras_UpdateSplitScreenDimensions' canvases) and a full clear wiped the left view.
    static uint32_t names = SymAddr("?gameMotion_apNames@@3PAPADA");
    if (r.empty() && names && *(void **)(names + 0x72c) && g_vp.Width < g_rtW)
        r.push_back({(LONG)(g_vp.X * g_scale), (LONG)(g_vp.Y * g_scale), (LONG)((g_vp.X + g_vp.Width) * g_scale),
                     (LONG)((g_vp.Y + g_vp.Height) * g_scale)});
    D3DVIEWPORT9 full = {0, 0, g_rtW * g_scale, g_rtH * g_scale, 0, 1};
    g_dev->SetViewport(&full);
    g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_rsCache[D3DRS_SCISSORTESTENABLE] = FALSE;
    g_dev->Clear((DWORD)r.size(), r.empty() ? nullptr : r.data(), f, color, z, stencil);
    ApplyViewport();
    return 0;
}

// ---------------------------------------------------------------------------
// Targets
XSTD(XSurface *, D3DDevice_GetBackBuffer2, 4, (INT idx)) { xb_D3DResource_AddRef(g_backbuffer); return g_backbuffer; }
XSTD(XSurface *, D3DDevice_GetRenderTarget2, 0, ()) { if (g_curRT) xb_D3DResource_AddRef(g_curRT); return (XSurface *)g_curRT; }
XSTD(XSurface *, D3DDevice_GetDepthStencilSurface2, 0, ()) { if (g_curDS) xb_D3DResource_AddRef(g_curDS); return (XSurface *)g_curDS; }
XSTD(HRESULT, D3DDevice_SetRenderTarget, 8, (XSurface *rt, XSurface *ds)) {
    if (rt) g_curRT = rt;
    g_curDS = ds;
    BindTargets();
    return 0;
}
XSTD(HRESULT, D3DDevice_SetRenderTargetFast, 12, (XSurface *rt, XSurface *ds, DWORD flags)) {
    return xb_D3DDevice_SetRenderTarget(rt, ds);
}

// ---------------------------------------------------------------------------
// Viewport / scissor / transforms / lights
static float g_vpScale[4], g_vpOffset[4];
static void ApplyScissor();
// Split screen: each player's view (viewport = its canvas: tRenderCamera +0x210 size, +0x214 position, e.g. 0,0 /
// 320,0 320x480) draws its HUD in full-screen coordinates (radar at 40,456, ammo at 492,438; the objective box with a
// viewport of its own at 201,39), which D3D clipped to the player's half: only the parts that fell there showed, laid
// out for the whole screen. With two local players, pre-transformed vertices and viewports other than a player's
// canvas are mapped from the full screen into the current player's canvas (x * canvas width / 640 + canvas x, y
// likewise), as cPlayerInfo::GetScreenPos does for the few HUD parts that call it (shell.cpp makes that one an
// identity). SetHudMapping(false) suspends it (the briefing stays full-screen).
static bool g_hudOff;
static void ApplyViewport();
void SetHudMapping(bool on) { g_hudOff = !on; if (g_dev) ApplyViewport(); }
// full-screen 2D drawing over a split view (competitive.cpp's match texts): the whole 640x480 screen whatever
// viewport the game left set, and no HUD mapping
static bool g_fullScreen2D;
void SetFullScreen2D(bool on) {
    static DWORD scissor;
    g_fullScreen2D = on, g_hudOff = on;
    if (!g_dev) return;
    if (on) g_dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor), g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    else g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, scissor);
    ApplyViewport();
}
static const uint16_t *Canvas(int i) {   // 0/1: left/right local player's canvas (w, h, x, y), 2: the current one's
    static uint32_t names = SymAddr("?gameMotion_apNames@@3PAPADA");
    uint8_t *info = names ? *(uint8_t **)(names + (i == 2 ? 0x720 : 0x728 + 4 * i)) : nullptr;
    uint8_t *sim = info ? *(uint8_t **)(info + 4) : nullptr;
    uint8_t *cam = sim ? *(uint8_t **)(sim + 0x250) : nullptr;
    return cam ? (const uint16_t *)(cam + 0x210) : nullptr;
}
// the current player's canvas when the HUD is mapped (two local players, a 640x480 target, no shell menu or load
// screen up), else null
bool ShellMenuShown();
static const uint16_t *HudCanvas() {
    static uint32_t names = SymAddr("?gameMotion_apNames@@3PAPADA");
    if (g_hudOff || !names || !*(void **)(names + 0x72c) || g_rtW != 640 || g_rtH != 480 || ShellMenuShown()) return nullptr;
    return Canvas(2);
}
static bool IsCanvas(const D3DVIEWPORT9 &v) {
    for (int i = 0; i < 2; i++)
        if (const uint16_t *c = Canvas(i))
            if (v.Width == c[0] && v.Height == c[1] && v.X == c[2] && v.Y == c[3]) return true;
    return false;
}
static void ApplyViewport() {
    D3DVIEWPORT9 v = g_vp;
    if (g_fullScreen2D && g_rtW == 640 && g_rtH == 480) v.X = v.Y = 0, v.Width = 640, v.Height = 480;
    else if (const uint16_t *c = HudCanvas()) {
        if (!IsCanvas(v)) {
            float sx = c[0] / 640.f, sy = c[1] / 480.f;
            v.X = (DWORD)(v.X * sx + c[2] + 0.5f), v.Y = (DWORD)(v.Y * sy + c[3] + 0.5f);
            v.Width = (DWORD)(v.Width * sx + 0.5f), v.Height = (DWORD)(v.Height * sy + 0.5f);
        }
    } else if (ShellMenuShown() && g_rtW == 640 && g_rtH == 480 && IsCanvas(v))
        v.X = v.Y = 0, v.Width = 640, v.Height = 480;   // a shell screen (pause menu, the load screen between
                                                        // missions) over a split view: the whole screen, not a half
    v.X *= g_scale; v.Y *= g_scale; v.Width *= g_scale; v.Height *= g_scale;
    UINT mw = g_rtW * g_scale, mh = g_rtH * g_scale;
    if (v.X >= mw) v.X = 0;
    if (v.Y >= mh) v.Y = 0;
    if (v.X + v.Width > mw) v.Width = mw - v.X;
    if (v.Y + v.Height > mh) v.Height = mh - v.Y;
    g_dev->SetViewport(&v);
    // Xbox screen-space mapping used by screen-space vertex shaders (oPos in pixels)
    g_vpScale[0] = g_vp.Width * 0.5f; g_vpScale[1] = -(float)g_vp.Height * 0.5f; g_vpScale[2] = (g_vp.MaxZ - g_vp.MinZ); g_vpScale[3] = 1;
    g_vpOffset[0] = g_vp.X + g_vp.Width * 0.5f; g_vpOffset[1] = g_vp.Y + g_vp.Height * 0.5f; g_vpOffset[2] = g_vp.MinZ; g_vpOffset[3] = 0;
}
XSTD(HRESULT, D3DDevice_SetViewport, 4, (const D3DVIEWPORT9 *v)) {
    g_vp = *v;
    ApplyViewport();
    ApplyScissor();
    return 0;
}
// Scissor rects are relative to the viewport (split screen: the right-hand player's view at x 320 sets 0,0-320,480),
// so the D3D9 rect moves with the viewport (ApplyViewport re-applies it)
static D3DRECT g_scissor;
static bool g_hasScissor;
static void ApplyScissor() {
    if (!g_hasScissor) return;
    RECT r = {(LONG)((g_scissor.x1 + g_vp.X) * g_scale), (LONG)((g_scissor.y1 + g_vp.Y) * g_scale),
              (LONG)((g_scissor.x2 + g_vp.X) * g_scale), (LONG)((g_scissor.y2 + g_vp.Y) * g_scale)};
    g_dev->SetScissorRect(&r);
}
XSTD(HRESULT, D3DDevice_SetScissors, 12, (DWORD count, BOOL exclusive, const D3DRECT *rects)) {
    if (!count || !rects) { SetRS(D3DRS_SCISSORTESTENABLE, FALSE); return 0; }
    g_scissor = rects[0], g_hasScissor = true;
    ApplyScissor();
    SetRS(D3DRS_SCISSORTESTENABLE, !exclusive);
    return 0;
}
static D3DTRANSFORMSTATETYPE MapTS(DWORD s) {
    if (s == XTS_VIEW) return D3DTS_VIEW;
    if (s == XTS_PROJECTION) return D3DTS_PROJECTION;
    if (s >= XTS_TEXTURE0 && s <= XTS_TEXTURE0 + 3) return (D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s - XTS_TEXTURE0);
    return D3DTS_WORLDMATRIX(s - XTS_WORLD);
}
static bool g_texXformDirty = true;
XSTD(HRESULT, D3DDevice_SetTransform, 8, (DWORD s, const D3DMATRIX *m)) {
    if (s >= XTS_MAX) return 0;
    g_xform[s] = *m;
    if (s >= XTS_TEXTURE0 && s <= XTS_TEXTURE0 + 3) g_texXformDirty = true;
    else g_dev->SetTransform(MapTS(s), m);
    return 0;
}
// fixed-function lights and material, also kept here for the vertex emulation (ApplyFFEmu)
static D3DLIGHT9 g_lights[8];
static bool g_lightOn[8];
static D3DMATERIAL9 g_material;
XSTD(HRESULT, D3DDevice_SetLight, 8, (DWORD i, const D3DLIGHT9 *l)) { if (i < 8 && l) g_lights[i] = *l; return g_dev->SetLight(i, l); }
XSTD(HRESULT, D3DDevice_LightEnable, 8, (DWORD i, BOOL e)) { if (i < 8) g_lightOn[i] = e != 0; return g_dev->LightEnable(i, e); }
XSTD(HRESULT, D3DDevice_SetMaterial, 4, (const D3DMATERIAL9 *m)) { if (m) g_material = *m; return g_dev->SetMaterial(m); }

// ---------------------------------------------------------------------------
// Render / texture state setters (values live in D3D__RenderState; applied at draw time)
#define RSSET(name, idx) XSTD(void, D3DDevice_SetRenderState_##name, 4, (DWORD v)) { g_rs[idx] = v; }
RSSET(PSTextureModes, XRS_PSTEXTUREMODES)
RSSET(VertexBlend, XRS_VERTEXBLEND)
RSSET(FogColor, XRS_FOGCOLOR)
RSSET(FillMode, XRS_FILLMODE)
RSSET(BackFillMode, XRS_BACKFILLMODE)
RSSET(TwoSidedLighting, XRS_TWOSIDEDLIGHTING)
RSSET(NormalizeNormals, XRS_NORMALIZENORMALS)
XSTD(void, D3DDevice_SetRenderState_ZEnable, 4, (DWORD v)) {
    static DWORD seen = 0;   // log each value once (2 = D3DZB_USEW)
    if (v < 32 && !(seen & (1u << v))) { seen |= 1u << v; Log("d3d: ZEnable %u", v); }
    g_rs[XRS_ZENABLE] = v;
}
RSSET(StencilEnable, XRS_STENCILENABLE)
RSSET(StencilFail, XRS_STENCILFAIL)
RSSET(FrontFace, XRS_FRONTFACE)
RSSET(CullMode, XRS_CULLMODE)
RSSET(TextureFactor, XRS_TEXTUREFACTOR)
RSSET(ZBias, XRS_ZBIAS)
RSSET(LogicOp, XRS_LOGICOP)
RSSET(EdgeAntiAlias, XRS_EDGEANTIALIAS)
RSSET(MultiSampleAntiAlias, XRS_MULTISAMPLEANTIALIAS)
RSSET(MultiSampleMask, XRS_MULTISAMPLEMASK)
RSSET(MultiSampleMode, XRS_MULTISAMPLEMODE)
RSSET(MultiSampleRenderTargetMode, XRS_MULTISAMPLERENDERTARGETMODE)
RSSET(ShadowFunc, XRS_SHADOWFUNC)
RSSET(LineWidth, XRS_LINEWIDTH)
RSSET(SampleAlpha, XRS_SAMPLEALPHA)
RSSET(Dxt1NoiseEnable, XRS_DXT1NOISEENABLE)
RSSET(YuvEnable, XRS_YUVENABLE)
RSSET(OcclusionCullEnable, XRS_OCCLUSIONCULLENABLE)
RSSET(StencilCullEnable, XRS_STENCILCULLENABLE)
RSSET(RopZCmpAlwaysRead, XRS_ROPZCMPALWAYSREAD)
RSSET(RopZRead, XRS_ROPZREAD)
RSSET(DoNotCullUncompressed, XRS_DONOTCULLUNCOMPRESSED)

XFAST(void, D3DDevice_SetRenderState_Simple, 8, (DWORD method, DWORD value)) {
    g_simple[method & 0x1FFC] = value;
}
XSTD(void, D3DDevice_SetRenderStateNotInline, 8, (DWORD state, DWORD value)) {
    if (state < XRS_MAX) g_rs[state] = value;
}
XSTD(void, D3DDevice_SetTextureState_TexCoordIndex, 8, (DWORD st, DWORD v)) { g_ts[st * 32 + XTSS_TEXCOORDINDEX] = v; }
XSTD(void, D3DDevice_SetTextureState_BorderColor, 8, (DWORD st, DWORD v)) { g_ts[st * 32 + XTSS_BORDERCOLOR] = v; }
XSTD(void, D3DDevice_SetTextureState_ColorKeyColor, 8, (DWORD st, DWORD v)) { g_ts[st * 32 + XTSS_COLORKEYCOLOR] = v; }
XSTD(void, D3DDevice_SetTextureState_BumpEnv, 12, (DWORD st, DWORD type, DWORD v)) { if (type < 32) g_ts[st * 32 + type] = v; }

XSTD(HRESULT, D3DDevice_SetTexture, 8, (DWORD st, XPixelContainer *t)) {
    if (st >= 4) return 0;
    if (t) xb_D3DResource_AddRef(t);
    if (g_tex[st]) {
        XPixelContainer *old = g_tex[st];
        g_tex[st] = nullptr;
        xb_D3DResource_Release(old);
    }
    g_tex[st] = t;
    return 0;
}
XSTD(HRESULT, D3DDevice_SetStreamSource, 12, (UINT n, XResource *vb, UINT stride)) {
    if (n < 16) { g_streams[n].vb = vb; g_streams[n].stride = stride; }
    return 0;
}
XSTD(HRESULT, D3DDevice_SetIndices, 8, (XResource *ib, UINT base)) {
    g_ib = ib;
    g_baseVertex = base;
    if (g_indexData) *g_indexData = ib ? ib->Data : 0;
    return 0;
}
XSTD(HRESULT, D3DDevice_SetVertexShader, 4, (DWORD h)) { g_curVS = h; return 0; }
// The internal setters take the hardware register (API register + 96, already added by the
// inlined D3DDevice_SetVertexShaderConstant wrapper) and, for NotInline, a count in DWORDs.
static void SetVSConst(int hw, const void *data, int dwords, void *caller = nullptr) {
    if (hw < 0 || hw >= 192) return;
    static bool logC96 = getenv("GHOSTPC_FRAMESTATS") != nullptr;
    static std::set<void *> seen;
    if (logC96 && hw <= 96 && hw + dwords / 4 > 96 && seen.insert(caller).second) {
        char nm[256];
        Log("c96 written by %s (hw %d, %d dwords)", SymName((uint32_t)caller, nm, sizeof nm), hw, dwords);
    }
    dwords = std::min(dwords, (192 - hw) * 4);
    memcpy(&g_vsConst[hw * 4], data, dwords * 4);
}
XFAST(void, D3DDevice_SetVertexShaderConstantNotInline, 12, (INT reg, const void *data, DWORD dwords)) { SetVSConst(reg, data, dwords, _ReturnAddress()); }
XFAST(void, D3DDevice_SetVertexShaderConstantNotInlineFast, 12, (INT reg, const void *data, DWORD dwords)) { SetVSConst(reg, data, dwords, _ReturnAddress()); }
XFAST(void, D3DDevice_SetVertexShaderConstant1, 8, (INT reg, const void *data)) { SetVSConst(reg, data, 4, _ReturnAddress()); }
XFAST(void, D3DDevice_SetVertexShaderConstant4, 8, (INT reg, const void *data)) { SetVSConst(reg, data, 16, _ReturnAddress()); }

static void ColorToF4(DWORD c, float *f) { f[0] = ((c >> 16) & 255) / 255.f; f[1] = ((c >> 8) & 255) / 255.f; f[2] = (c & 255) / 255.f; f[3] = (c >> 24) / 255.f; }
static DWORD F4ToColor(const float *f) {
    auto b = [](float x) { return (DWORD)(std::min(std::max(x, 0.f), 1.f) * 255.f + 0.5f); };
    return (b(f[3]) << 24) | (b(f[0]) << 16) | (b(f[1]) << 8) | b(f[2]);
}
// The PS render states mirror D3DPIXELSHADERDEF; SetPixelShader copies the definition into them
// and the combiner constants are read back from D3D__RenderState at draw time.
static void LoadPSDefIntoRS(const XPixelShaderDef *d) {
    // like the XDK: the first 57 dwords of the definition, plus PSTextureModes at its complex-state slot
    const DWORD *src = (const DWORD *)d;
    for (int i = 0; i < 57; i++) g_rs[i] = src[i];
    g_rs[XRS_PSTEXTUREMODES] = d->PSTextureModes;
}
XSTD(HRESULT, D3DDevice_SetPixelShader, 4, (DWORD h)) {
    g_curPS = h ? ((XPixelShaderObj *)h)->pDef : nullptr;
    if (g_curPS) LoadPSDefIntoRS(g_curPS);
    return 0;
}
XSTD(void, D3DDevice_SetPixelShaderProgram, 4, (const XPixelShaderDef *d)) {
    g_curPS = d;
    if (d) LoadPSDefIntoRS(d);
}
XSTD(HRESULT, D3DDevice_SetPixelShaderConstant, 12, (DWORD reg, const float *data, DWORD count)) {
    if (!g_curPS) return 0;
    for (DWORD k = 0; k < count; k++) {
        DWORD r = reg + k, c = F4ToColor(data + k * 4);
        for (int s = 0; s < 8; s++) {
            if (((g_curPS->PSC0Mapping >> (s * 4)) & 0xF) == r) g_rs[XRS_PSCONSTANT0_0 + s] = c;
            if (((g_curPS->PSC1Mapping >> (s * 4)) & 0xF) == r) g_rs[XRS_PSCONSTANT1_0 + s] = c;
        }
        if ((g_curPS->PSFinalCombinerConstants & 0xF) == r) g_rs[XRS_PSFINALCOMBINERCONSTANT0] = c;
        if (((g_curPS->PSFinalCombinerConstants >> 4) & 0xF) == r) g_rs[XRS_PSFINALCOMBINERCONSTANT1] = c;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Apply Xbox state to the D3D9 device before a draw
static DWORD MapTexOp(DWORD x) {
    static const DWORD t[] = {0, D3DTOP_DISABLE, D3DTOP_SELECTARG1, D3DTOP_SELECTARG2, D3DTOP_MODULATE, D3DTOP_MODULATE2X,
                              D3DTOP_MODULATE4X, D3DTOP_ADD, D3DTOP_ADDSIGNED, D3DTOP_ADDSIGNED2X, D3DTOP_SUBTRACT,
                              D3DTOP_ADDSMOOTH, D3DTOP_BLENDDIFFUSEALPHA, D3DTOP_BLENDCURRENTALPHA, D3DTOP_BLENDTEXTUREALPHA,
                              D3DTOP_BLENDFACTORALPHA, D3DTOP_BLENDTEXTUREALPHAPM, D3DTOP_PREMODULATE,
                              D3DTOP_MODULATEALPHA_ADDCOLOR, D3DTOP_MODULATECOLOR_ADDALPHA, D3DTOP_MODULATEINVALPHA_ADDCOLOR,
                              D3DTOP_MODULATEINVCOLOR_ADDALPHA, D3DTOP_DOTPRODUCT3, D3DTOP_MULTIPLYADD, D3DTOP_LERP,
                              D3DTOP_BUMPENVMAP, D3DTOP_BUMPENVMAPLUMINANCE};
    return x < sizeof t / sizeof t[0] ? t[x] : D3DTOP_MODULATE;
}
static DWORD MapAddr(DWORD a) { return a == 5 ? D3DTADDRESS_CLAMP : (a >= 1 && a <= 4) ? a : D3DTADDRESS_WRAP; }
static DWORD MapFilter(DWORD f) { return f <= 3 ? f : D3DTEXF_LINEAR; }

bool g_texIsLinear[4];
float g_texSize[4][4];          // w, h, 1/w, 1/h of bound textures (unscaled)
DWORD g_texType[4];             // 0 none, 1 2D, 2 cube, 3 volume, 4 depth(shadow)

static void ApplyTextures(bool ffPixel, bool ffVertex) {
    for (int s = 0; s < 4; s++) {
        XPixelContainer *t = g_tex[s];
        IDirect3DBaseTexture9 *ht = nullptr;
        g_texIsLinear[s] = false;
        g_texType[s] = 0;
        if (t) {
            HostRes *h = HostOf(t);
            if (h && h->isDepth && h->tex) { ht = h->tex; g_texType[s] = 4; }
            else ht = HostTexture(t);
            UINT w, hh, d, pitch, l;
            XSurfaceDims(t, &w, &hh, &d, &pitch, &l);
            g_texIsLinear[s] = XFmtIsLinear(XFormat(t)) || (h && h->isRT && h->linear) || (h && h->isDepth && XFmtIsLinear(XFormat(t)));
            g_texSize[s][0] = (float)w; g_texSize[s][1] = (float)hh; g_texSize[s][2] = 1.f / w; g_texSize[s][3] = 1.f / hh;
            if (!g_texType[s]) g_texType[s] = (t->Format & XFMT_CUBEMAP) ? 2 : d > 1 ? 3 : 1;
            if (!ht) g_texType[s] = 0;
        }
        g_dev->SetTexture(s, ht);
        DWORD *x = g_ts + s * 32;
        SetSS(s, D3DSAMP_ADDRESSU, MapAddr(x[XTSS_ADDRESSU]));
        SetSS(s, D3DSAMP_ADDRESSV, MapAddr(x[XTSS_ADDRESSV]));
        SetSS(s, D3DSAMP_ADDRESSW, MapAddr(x[XTSS_ADDRESSW]));
        SetSS(s, D3DSAMP_MAGFILTER, MapFilter(x[XTSS_MAGFILTER]));
        SetSS(s, D3DSAMP_MINFILTER, MapFilter(x[XTSS_MINFILTER]));
        static bool noMips = getenv("GHOSTPC_NOMIPS") != nullptr;
        SetSS(s, D3DSAMP_MIPFILTER, noMips ? D3DTEXF_NONE : MapFilter(x[XTSS_MIPFILTER]));
        static bool mipDefaults = getenv("GHOSTPC_MIPDEFAULT") != nullptr;
        SetSS(s, D3DSAMP_MIPMAPLODBIAS, mipDefaults ? 0 : x[XTSS_MIPMAPLODBIAS]);
        SetSS(s, D3DSAMP_MAXMIPLEVEL, mipDefaults ? 0 : x[XTSS_MAXMIPLEVEL]);
        SetSS(s, D3DSAMP_MAXANISOTROPY, std::max(1u, (unsigned)x[XTSS_MAXANISOTROPY]));
        SetSS(s, D3DSAMP_BORDERCOLOR, x[XTSS_BORDERCOLOR]);
        if (ffPixel) {
            SetTSS(s, D3DTSS_COLOROP, MapTexOp(x[XTSS_COLOROP]));
            SetTSS(s, D3DTSS_COLORARG0, x[XTSS_COLORARG0]);
            SetTSS(s, D3DTSS_COLORARG1, x[XTSS_COLORARG1]);
            SetTSS(s, D3DTSS_COLORARG2, x[XTSS_COLORARG2]);
            SetTSS(s, D3DTSS_ALPHAOP, MapTexOp(x[XTSS_ALPHAOP]));
            SetTSS(s, D3DTSS_ALPHAARG0, x[XTSS_ALPHAARG0]);
            SetTSS(s, D3DTSS_ALPHAARG1, x[XTSS_ALPHAARG1]);
            SetTSS(s, D3DTSS_ALPHAARG2, x[XTSS_ALPHAARG2]);
            SetTSS(s, D3DTSS_RESULTARG, x[XTSS_RESULTARG] ? x[XTSS_RESULTARG] : D3DTA_CURRENT);
            SetTSS(s, D3DTSS_BUMPENVMAT00, x[XTSS_BUMPENVMAT00]);
            SetTSS(s, D3DTSS_BUMPENVMAT01, x[XTSS_BUMPENVMAT01]);
            SetTSS(s, D3DTSS_BUMPENVMAT10, x[XTSS_BUMPENVMAT10]);
            SetTSS(s, D3DTSS_BUMPENVMAT11, x[XTSS_BUMPENVMAT11]);
            SetTSS(s, D3DTSS_BUMPENVLSCALE, x[XTSS_BUMPENVLSCALE]);
            SetTSS(s, D3DTSS_BUMPENVLOFFSET, x[XTSS_BUMPENVLOFFSET]);
        }
        if (ffVertex) {
            SetTSS(s, D3DTSS_TEXCOORDINDEX, x[XTSS_TEXCOORDINDEX]);
            // linear Xbox textures take texel coordinates: fold 1/size into the texture matrix
            DWORD ttf = x[XTSS_TEXTURETRANSFORMFLAGS];
            if (g_texIsLinear[s] && ffPixel) {
                D3DMATRIX m = (ttf & 0xFF) ? g_xform[XTS_TEXTURE0 + s] : D3DMATRIX{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                bool proj = (ttf & D3DTTFF_PROJECTED) != 0;
                for (int r = 0; r < 4; r++) { m.m[r][0] *= g_texSize[s][2]; m.m[r][1] *= g_texSize[s][3]; }
                g_dev->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), &m);
                SetTSS(s, D3DTSS_TEXTURETRANSFORMFLAGS, (ttf & 0xFF) ? ttf : D3DTTFF_COUNT2 | (proj ? D3DTTFF_PROJECTED : 0));
            } else {
                if (ttf & 0xFF) g_dev->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), &g_xform[XTS_TEXTURE0 + s]);
                SetTSS(s, D3DTSS_TEXTURETRANSFORMFLAGS, ttf);
            }
        } else {
            SetTSS(s, D3DTSS_TEXCOORDINDEX, s);
            SetTSS(s, D3DTSS_TEXTURETRANSFORMFLAGS, 0);
        }
    }
    if (ffPixel) { SetTSS(4, D3DTSS_COLOROP, D3DTOP_DISABLE); SetTSS(4, D3DTSS_ALPHAOP, D3DTOP_DISABLE); }
}

static bool g_forceNoLighting;   // GHOSTPC_NOWORLDLIT: lighting off for the redirected draw
static float g_psDepthBias;   // constant depth bias of the current draw (applied by PSDepthWrapper shaders)
static void ApplyRenderStates(bool psFog, bool vsFog) {
    DWORD *r = g_rs;
    SetRS(D3DRS_ZENABLE, r[XRS_ZENABLE] ? (g_curDS ? D3DZB_TRUE : D3DZB_FALSE) : D3DZB_FALSE);
    SetRS(D3DRS_ZFUNC, Cmp(Simple(0x354, 0x203)));
    SetRS(D3DRS_ZWRITEENABLE, Simple(0x35C, 1) != 0);
    SetRS(D3DRS_ALPHATESTENABLE, Simple(0x300, 0) != 0);
    SetRS(D3DRS_ALPHAFUNC, Cmp(Simple(0x33C, 0x207)));
    SetRS(D3DRS_ALPHAREF, Simple(0x340, 0));
    SetRS(D3DRS_ALPHABLENDENABLE, Simple(0x304, 0) != 0);
    DWORD sb = Simple(0x344, 1), db = Simple(0x348, 0);
    SetRS(D3DRS_SRCBLEND, Blend(sb));
    SetRS(D3DRS_DESTBLEND, Blend(db));
    SetRS(D3DRS_BLENDOP, BlendOp(Simple(0x350, 0x8006)));
    DWORD bc = Simple(0x34C, 0);
    if (sb == 0x8003 || sb == 0x8004 || db == 0x8003 || db == 0x8004) bc = (bc & 0xff000000u) | ((bc >> 24) * 0x010101u);
    SetRS(D3DRS_BLENDFACTOR, bc);
    DWORD cm = Simple(0x358, 0x01010101);
    SetRS(D3DRS_COLORWRITEENABLE, ((cm >> 16) & 1 ? D3DCOLORWRITEENABLE_RED : 0) | ((cm >> 8) & 1 ? D3DCOLORWRITEENABLE_GREEN : 0) |
                                      (cm & 1 ? D3DCOLORWRITEENABLE_BLUE : 0) | ((cm >> 24) & 1 ? D3DCOLORWRITEENABLE_ALPHA : 0));
    SetRS(D3DRS_SHADEMODE, Simple(0x37C, 0x1D01) == 0x1D00 ? D3DSHADE_FLAT : D3DSHADE_GOURAUD);
    SetRS(D3DRS_DITHERENABLE, Simple(0x310, 0) != 0);
    bool st = r[XRS_STENCILENABLE] != 0 && g_curDS;
    SetRS(D3DRS_STENCILENABLE, st);
    if (st) {
        SetRS(D3DRS_STENCILFUNC, Cmp(Simple(0x364, 0x207)));
        SetRS(D3DRS_STENCILREF, Simple(0x368, 0));
        SetRS(D3DRS_STENCILMASK, Simple(0x36C, 0xff));
        SetRS(D3DRS_STENCILWRITEMASK, Simple(0x360, 0xff));
        SetRS(D3DRS_STENCILFAIL, StencilOp(r[XRS_STENCILFAIL] ? r[XRS_STENCILFAIL] : 0x1E00));
        SetRS(D3DRS_STENCILZFAIL, StencilOp(Simple(0x374, 0x1E00)));
        SetRS(D3DRS_STENCILPASS, StencilOp(Simple(0x378, 0x1E00)));
    }
    // culling: Xbox CULLMODE names the winding to cull, relative to FRONTFACE
    DWORD cull = r[XRS_CULLMODE];
    DWORD c9 = D3DCULL_NONE;
    if (cull == 0x900) c9 = D3DCULL_CW;
    else if (cull == 0x901) c9 = D3DCULL_CCW;
    if (c9 != D3DCULL_NONE && r[XRS_FRONTFACE] == 0x901) c9 = c9 == D3DCULL_CW ? D3DCULL_CCW : D3DCULL_CW;
    SetRS(D3DRS_CULLMODE, c9);
    DWORD fm = r[XRS_FILLMODE];
    SetRS(D3DRS_FILLMODE, fm == 0x1B00 ? D3DFILL_POINT : fm == 0x1B01 ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
    SetRS(D3DRS_TEXTUREFACTOR, r[XRS_TEXTUREFACTOR]);
    // fog
    bool fog = r[XRS_FOGENABLE] != 0 && psFog;
    SetRS(D3DRS_FOGENABLE, fog);
    if (fog) {
        SetRS(D3DRS_FOGCOLOR, r[XRS_FOGCOLOR]);
        // with a vertex shader the shader outputs the final fog factor (computed with Xbox semantics)
        SetRS(D3DRS_FOGTABLEMODE, vsFog ? D3DFOG_NONE : r[XRS_FOGTABLEMODE]);
        SetRS(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
        SetRS(D3DRS_FOGSTART, r[XRS_FOGSTART]);
        SetRS(D3DRS_FOGEND, r[XRS_FOGEND]);
        SetRS(D3DRS_FOGDENSITY, r[XRS_FOGDENSITY]);
        SetRS(D3DRS_RANGEFOGENABLE, r[XRS_RANGEFOGENABLE]);
    }
    // fixed-function lighting
    SetRS(D3DRS_LIGHTING, r[XRS_LIGHTING] != 0 && !g_forceNoLighting);
    SetRS(D3DRS_SPECULARENABLE, r[XRS_SPECULARENABLE] != 0);
    SetRS(D3DRS_LOCALVIEWER, r[XRS_LOCALVIEWER] != 0);
    SetRS(D3DRS_COLORVERTEX, r[XRS_COLORVERTEX] != 0);
    SetRS(D3DRS_DIFFUSEMATERIALSOURCE, r[XRS_DIFFUSEMATERIALSOURCE]);
    SetRS(D3DRS_SPECULARMATERIALSOURCE, r[XRS_SPECULARMATERIALSOURCE]);
    SetRS(D3DRS_AMBIENTMATERIALSOURCE, r[XRS_AMBIENTMATERIALSOURCE]);
    SetRS(D3DRS_EMISSIVEMATERIALSOURCE, r[XRS_EMISSIVEMATERIALSOURCE]);
    SetRS(D3DRS_AMBIENT, r[XRS_AMBIENT]);
    SetRS(D3DRS_NORMALIZENORMALS, r[XRS_NORMALIZENORMALS] != 0);
    DWORD vb = r[XRS_VERTEXBLEND];
    SetRS(D3DRS_VERTEXBLEND, vb == 1 ? D3DVBF_1WEIGHTS : vb == 3 ? D3DVBF_2WEIGHTS : vb == 5 ? D3DVBF_3WEIGHTS : D3DVBF_DISABLE);
    SetRS(D3DRS_POINTSIZE, r[XRS_POINTSIZE]);
    SetRS(D3DRS_POINTSIZE_MIN, r[XRS_POINTSIZE_MIN]);
    SetRS(D3DRS_POINTSIZE_MAX, r[XRS_POINTSIZE_MAX]);
    SetRS(D3DRS_POINTSPRITEENABLE, r[XRS_POINTSPRITEENABLE]);
    SetRS(D3DRS_POINTSCALEENABLE, r[XRS_POINTSCALEENABLE]);
    SetRS(D3DRS_POINTSCALE_A, r[XRS_POINTSCALE_A]);
    SetRS(D3DRS_POINTSCALE_B, r[XRS_POINTSCALE_B]);
    SetRS(D3DRS_POINTSCALE_C, r[XRS_POINTSCALE_C]);
    for (int i = 0; i < 4; i++) SetRS((D3DRENDERSTATETYPE)(D3DRS_WRAP0 + i), r[XRS_WRAP0 + i]);
    // depth bias: Xbox ZBIAS (0..16) and polygon offset states
    float slope = 0, bias = 0;
    if (Simple(0x338, 0)) { DWORD a = Simple(0x384, 0), b = Simple(0x388, 0); memcpy(&slope, &a, 4); memcpy(&bias, &b, 4); }
    if (r[XRS_ZBIAS]) bias -= (float)r[XRS_ZBIAS];
    // Overlays drawn with fixed-function transform and depth writes off (projected and blob shadows, decals) depth-test
    // against surfaces that may have been drawn through a vertex shader (e.g. the world while a muzzle flash lights
    // it). NV2A computes both transforms identically; D3D9 does not, and the overlay z-fights. Pull such draws slightly
    // towards the camera (16 units of the 24-bit depth range).
    static bool noOverlayBias = getenv("GHOSTPC_NOOVERLAYBIAS") != nullptr;
    if (!noOverlayBias && !Simple(0x35C, 1) && !vsFog) { bias -= 16.f; slope -= 1.f; }
    float dbias = bias * (1.0f / 16777215.0f), sd = slope;
    g_psDepthBias = dbias;
    DWORD dbi, sdi;
    memcpy(&dbi, &dbias, 4); memcpy(&sdi, &sd, 4);
    SetRS(D3DRS_DEPTHBIAS, dbi);
    SetRS(D3DRS_SLOPESCALEDEPTHBIAS, sdi);
}

// debug (GHOSTPC_NOWORLDLIT): is this the game's defaultWorldLit1/2/3 shader (world geometry lit by dynamic
// lights)? Identified by its microcode in VertexShaders\XBox.
static bool IsWorldLitShader(XVertexShader *vs) {
    static std::vector<std::string> files;
    static bool loaded;
    if (!loaded) {
        loaded = true;
        for (int i = 1; i <= 3; i++) {
            char p[MAX_PATH];
            _snprintf(p, MAX_PATH, "%s\\VertexShaders\\XBox\\defaultWorldLit%d.xvu", g_gameDir, i);
            if (FILE *f = fopen(p, "rb")) {
                std::string d; char buf[4096]; size_t n;
                while ((n = fread(buf, 1, sizeof buf, f)) > 0) d.append(buf, n);
                fclose(f);
                files.push_back(d);
            }
        }
    }
    if (vs->ucode.empty()) return false;
    std::string u((const char *)vs->ucode.data(), vs->ucode.size() * 4);
    for (auto &f : files) if (f.find(u) != std::string::npos) return true;
    return false;
}
// ---------------------------------------------------------------------------
// Fixed-function vertex emulation (FFVSGet): unlit FVF draws use a generated vertex shader instead of D3D9's
// fixed-function transform. NV2A runs fixed-function and vertex programs on the same unit with the same arithmetic;
// on D3D9 the two differ in rounding, so world chunks that switch to the game's lighting shaders (e.g. lit by a
// muzzle flash) did not line up with their fixed-function neighbours (z-fighting, sparkling seams).
static bool g_ffEmu;   // the current draw uses the emulation (set by ApplyDraw, used by UploadVertices)
static DWORD g_emuFVF; // FVF equivalent of the current fixed-function draw (FVF, or a declaration-only shader handle)
// FVF describing a declaration-only vertex shader (fixed-function transform), 0 if it has no FVF equivalent. Its
// vertices still go through ConvertVertices with the fixed-function declaration (same semantics).
static DWORD DeclFVF(const XVertexShader *vs) {
    if (!vs || vs->hasFunction || vs->regOffset[0] < 0) return 0;
    DWORD fvf = D3DFVF_XYZ;
    int ntex = 0;
    for (int r = 1; r < 16; r++) {
        if (vs->regOffset[r] < 0) continue;
        if (r == 2) fvf |= D3DFVF_NORMAL;
        else if (r == 3) fvf |= D3DFVF_DIFFUSE;
        else if (r == 4) fvf |= D3DFVF_SPECULAR;
        else if (r >= 9 && r <= 12) {
            if (r - 9 != ntex) return 0;   // texcoords must be consecutive from TEXCOORD0
            DWORD xt = 0;
            for (auto &x : vs->elems) if (x.reg == r) xt = x.type;
            int comps = xt == 0x40 ? 4 : (int)(xt >> 4);         // components (D3DCOLOR 4, else type >> 4)
            static const DWORD code[5] = {0, 3, 0, 1, 2};       // D3DFVF_TEXCOORDSIZE1/2/3/4
            fvf |= code[comps >= 1 && comps <= 4 ? comps : 2] << (16 + ntex * 2);
            ntex++;
        } else return 0;   // blend weights, fog, point size, back colours
    }
    return fvf | (ntex << D3DFVF_TEXCOUNT_SHIFT);
}
// 0 = eligible, else the reason it stays on D3D9's fixed function (logged under GHOSTPC_FRAMESTATS)
static int FFEmuReason() {
    static bool off = getenv("GHOSTPC_NOFFVS") != nullptr;
    if (off || !g_emuFVF) return 1;
    DWORD fvf = g_emuFVF;
    if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ) return 2;   // pre-transformed / blended: D3D9 fixed function
    if (g_rs[XRS_VERTEXBLEND]) return 3;
    if (g_rs[XRS_LIGHTING] && g_rs[XRS_SPECULARENABLE]) return 4;   // only the diffuse lighting is emulated
    static bool noLit = getenv("GHOSTPC_FFVS_NOLIT") != nullptr;      // debug: lit draws stay on D3D9
    if (noLit && g_rs[XRS_LIGHTING]) return 5;
    for (int s = 0; s < 4; s++) {
        DWORD tci = g_ts[s * 32 + XTSS_TEXCOORDINDEX];
        if ((tci >> 16) > 3 || (tci & 0xFFFF) > 7) return 6;
    }
    return 0;
}
static bool FFEmuEligible() {
    int r = FFEmuReason();
    static bool stats = getenv("GHOSTPC_FRAMESTATS") != nullptr;
    if (stats && r > 1) {
        static std::set<std::pair<DWORD, int>> seen;
        if (seen.insert({g_curVS, r}).second && seen.size() < 40) {
            extern UINT g_frame;
            Log("ffvs skip: frame %u fvf %08x reason %d tci %x %x zfunc %u zwrite %u", g_frame, g_curVS, r,
                g_ts[XTSS_TEXCOORDINDEX], g_ts[32 + XTSS_TEXCOORDINDEX], Simple(0x354, 0x203), Simple(0x35C, 1));
        }
    }
    return r == 0;
}
static void MatMul(const D3DMATRIX &a, const D3DMATRIX &b, D3DMATRIX &o) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) o.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c] + a.m[r][3] * b.m[3][c];
}
static void Columns(const D3DMATRIX &m, float *out) {   // 4 rows of the transpose (dp4 with a row vector)
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) out[c * 4 + r] = m.m[r][c];
}
// The game's own world shaders take their position matrix in c[96..99]: the current render camera's view*projection
// (transposed, times a constant factor), copied from tRenderCamera +0x100, with its fog plane at +0x140 (c[103]).
// Read it from the camera itself, not from the registers (which other passes may have overwritten), and when it
// describes the current view*projection use it, so that both kinds of draw compute bit-identical positions. A
// non-identity world matrix is folded in (rows times the world transposed).
static const float *GameCamera() {
    static uint8_t **cur = (uint8_t **)SymAddr("?renderCamera_pCurCamera@@3PAVtRenderCamera@@A");
    return cur && *cur ? (const float *)(*cur + 0x100) : nullptr;
}
static bool GameMatrix(const D3DMATRIX &vp, float *rows, float *fog) {
    const float *g = GameCamera();
    if (!g) return false;
    float t[16], mx = 0;
    Columns(vp, t);
    int bi = 0;
    for (int i = 0; i < 16; i++) if (fabsf(t[i]) > mx) { mx = fabsf(t[i]); bi = i; }
    if (mx == 0 || g[bi] == 0) return false;
    float k = g[bi] / t[bi];
    // GHOSTPC_FFVS_TOL: relative tolerance of the match (debug)
    static float tol = getenv("GHOSTPC_FFVS_TOL") ? (float)atof(getenv("GHOSTPC_FFVS_TOL")) : 1e-3f;
    float dev = 0;
    for (int i = 0; i < 16; i++) dev = std::max(dev, fabsf(g[i] - k * t[i]) / (fabsf(k) * mx));
    static bool stats = getenv("GHOSTPC_FRAMESTATS") != nullptr;
    static int devLogged;
    if (stats && dev > 1e-5f && devLogged < 60) {
        devLogged++;
        extern UINT g_frame;
        Log("ffvs dev: frame %u fvf %08x deviation %.2e (%s)", g_frame, g_curVS, dev, dev > tol ? "own" : "game");
    }
    if (dev > tol) return false;
    const D3DMATRIX &w = g_xform[XTS_WORLD];
    bool ident = true;
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) if (w.m[r][c] != (r == c ? 1.f : 0.f)) ident = false;
    if (ident) { memcpy(rows, g, 64); memcpy(fog, g + 16, 16); return true; }
    for (int i = 0; i < 5; i++) {        // 4 position rows, then the fog plane
        const float *src = i < 4 ? g + i * 4 : g + 16;
        float *dst = i < 4 ? rows + i * 4 : fog;
        for (int k2 = 0; k2 < 4; k2++) dst[k2] = src[0] * w.m[k2][0] + src[1] * w.m[k2][1] + src[2] * w.m[k2][2] + src[3] * w.m[k2][3];
    }
    return true;
}
// W-buffer emulation (PSDepthWrapper): depth = clip w / (far plane * the factor carried by the clip coordinates). The
// game's shaders and the emulation with the game's matrix output w scaled by the camera matrix factor (~27962), D3D9
// matrices unscaled. 0 when not W-buffering.
static float WInv(bool gameScale) {
    if (g_rs[XRS_ZENABLE] != 2) return 0;
    const D3DMATRIX &p = g_xform[XTS_PROJECTION];
    float zf = fabsf(1 - p.m[2][2]) > 1e-7f ? p.m[3][2] / (1 - p.m[2][2]) : 0;
    if (!(zf > 0) || zf > 1e7f) zf = 10000;
    float k = 1;
    if (const float *g = gameScale ? GameCamera() : nullptr) {
        D3DMATRIX vp;
        MatMul(g_xform[XTS_VIEW], g_xform[XTS_PROJECTION], vp);
        int bi = 0;
        for (int i = 1; i < 4; i++) if (fabsf(vp.m[i][3]) > fabsf(vp.m[bi][3])) bi = i;
        if (vp.m[bi][3] != 0 && g[12 + bi] != 0) k = fabsf(g[12 + bi] / vp.m[bi][3]);
    }
    return 1.f / (zf * k);
}
static void ApplyFFEmu() {
    FFVSKey key;
    memset(&key, 0, sizeof key);
    key.fvf = g_emuFVF;
    for (int s = 0; s < 4; s++) {
        key.tci[s] = g_ts[s * 32 + XTSS_TEXCOORDINDEX];
        key.ttf[s] = g_ts[s * 32 + XTSS_TEXTURETRANSFORMFLAGS] & (0xFF | D3DTTFF_PROJECTED);
    }
    key.normalize = g_rs[XRS_NORMALIZENORMALS] != 0;
    key.rangeFog = g_rs[XRS_RANGEFOGENABLE] != 0;
    if (g_rs[XRS_LIGHTING]) {
        key.lighting = 1;
        key.colorVertex = g_rs[XRS_COLORVERTEX] != 0;
        key.srcDiffuse = (BYTE)g_rs[XRS_DIFFUSEMATERIALSOURCE];
        key.srcAmbient = (BYTE)g_rs[XRS_AMBIENTMATERIALSOURCE];
        key.srcEmissive = (BYTE)g_rs[XRS_EMISSIVEMATERIALSOURCE];
        for (int i = 0; i < 8; i++) key.lightType[i] = g_lightOn[i] ? (BYTE)g_lights[i].Type : 0;
    }
    D3DMATRIX wv, vp, wvp;
    MatMul(g_xform[XTS_WORLD], g_xform[XTS_VIEW], wv);
    MatMul(g_xform[XTS_VIEW], g_xform[XTS_PROJECTION], vp);
    MatMul(wv, g_xform[XTS_PROJECTION], wvp);
    static bool ownOnly = getenv("GHOSTPC_FFVS_OWN") != nullptr;   // debug: never use the game's matrix
    float grows[16], gfog[4];
    bool game = !ownOnly && GameMatrix(vp, grows, gfog);
    IDirect3DVertexShader9 *vs = FFVSGet(key);
    if (!vs) { g_ffEmu = false; g_dev->SetVertexShader(nullptr); return; }
    float c[78][4] = {};

    if (key.lighting) {
        auto col = [](const D3DCOLORVALUE &v, float *o) { o[0] = v.r; o[1] = v.g; o[2] = v.b; o[3] = v.a; };
        DWORD a = g_rs[XRS_AMBIENT];
        c[26][0] = ((a >> 16) & 255) / 255.f; c[26][1] = ((a >> 8) & 255) / 255.f; c[26][2] = (a & 255) / 255.f; c[26][3] = (a >> 24) / 255.f;
        col(g_material.Diffuse, c[27]); col(g_material.Ambient, c[28]); col(g_material.Emissive, c[29]);
        const D3DMATRIX &v = g_xform[XTS_VIEW];
        for (int i = 0; i < 8; i++) {
            if (!key.lightType[i]) continue;
            const D3DLIGHT9 &l = g_lights[i];
            float *b = c[30 + i * 6];
            const float p[3] = {l.Position.x, l.Position.y, l.Position.z}, d[3] = {l.Direction.x, l.Direction.y, l.Direction.z};
            for (int j = 0; j < 3; j++) {
                b[j] = p[0] * v.m[0][j] + p[1] * v.m[1][j] + p[2] * v.m[2][j] + v.m[3][j];   // view-space position
                b[4 + j] = d[0] * v.m[0][j] + d[1] * v.m[1][j] + d[2] * v.m[2][j];         // view-space direction
            }
            col(l.Diffuse, b + 8); col(l.Ambient, b + 12);
            b[16] = l.Range; b[17] = l.Attenuation0; b[18] = l.Attenuation1; b[19] = l.Attenuation2;
            b[20] = cosf(l.Theta * 0.5f); b[21] = cosf(l.Phi * 0.5f); b[22] = l.Falloff;
        }
    }
    static UINT nGame, nOwn;
    (game ? nGame : nOwn)++;
    if (((nGame + nOwn) % 20000) == 0) Log("ffvs: %u draws with the game matrix, %u with own", nGame, nOwn);
    if (game) memcpy(c[0], grows, 64);
    else Columns(wvp, c[0]);
    Columns(wv, c[4]);
    for (int s = 0; s < 4; s++) Columns(g_xform[XTS_TEXTURE0 + s], c[8 + s * 4]);
    c[24][0] = (float)g_rs[XRS_FOGTABLEMODE];
    memcpy(&c[24][1], &g_rs[XRS_FOGSTART], 4);
    memcpy(&c[24][2], &g_rs[XRS_FOGEND], 4);
    memcpy(&c[24][3], &g_rs[XRS_FOGDENSITY], 4);
    // fog distance: the game's fog plane (c[103]) along with its matrix, else view-space depth
    if (game) memcpy(c[25], gfog, 16);
    else for (int r = 0; r < 4; r++) c[25][r] = wv.m[r][2];
    g_dev->SetVertexShader(vs);
    g_dev->SetVertexShaderConstantF(0, c[0], key.lighting ? 78 : 26);
    float wv4[4] = {WInv(game), 0, 0, 0};
    g_dev->SetVertexShaderConstantF(78, wv4, 1);
    static int logged;
    if (logged < 8) { logged++; Log("ffvs: fvf %08x emulated (%s matrix)", key.fvf, game ? "game" : "own"); }
    static bool stats = getenv("GHOSTPC_FRAMESTATS") != nullptr;
    static int ownLogged;
    if (stats && !game && ownLogged < 30) {
        ownLogged++;
        extern UINT g_frame;
        Log("ffvs own: frame %u fvf %08x lit %u cam %p", g_frame, key.fvf, key.lighting, GameCamera());
    }
}

static void ApplyDraw(XVertexShader *&vsOut, bool &ffVertex) {
    XVertexShader *vs = (g_curVS & 1) ? VSFromHandle(g_curVS) : nullptr;
    ffVertex = !vs || !vs->hasFunction;
    if (vs && !VSPrepare(vs)) ffVertex = true;
    static bool noWorldLit = getenv("GHOSTPC_NOWORLDLIT") != nullptr;
    g_forceNoLighting = false;
    if (noWorldLit && vs && !ffVertex && IsWorldLitShader(vs)) { ffVertex = true; g_forceNoLighting = true; }
    vsOut = vs;
    g_emuFVF = !vs ? g_curVS : DeclFVF(vs);
    g_ffEmu = ffVertex && FFEmuEligible();
    // debug: GHOSTPC_LOGTEXMAT: the texture-0 matrix and view of the first reflection-texgen draw of each frame
    static bool logTexMat = getenv("GHOSTPC_LOGTEXMAT") != nullptr;
    static UINT texMatFrame = ~0u;
    if (logTexMat && ffVertex && !vs && (g_ts[XTSS_TEXCOORDINDEX] >> 16) == 3 && texMatFrame != g_frame && g_frame > 500 && g_frame < 700) {
        texMatFrame = g_frame;
        const float *t = &g_xform[XTS_TEXTURE0].m[0][0], *v = &g_xform[XTS_VIEW].m[0][0], *w = &g_xform[XTS_WORLD].m[0][0];
        Log("texmat frame %u fvf %08x T0 %.5f %.5f %.5f | %.5f %.5f %.5f | %.5f %.5f %.5f | %.4f %.4f %.4f  V %.5f %.5f %.5f %.3f %.3f %.3f  W %.3f %.3f %.3f",
            g_frame, g_curVS, t[0], t[1], t[2], t[4], t[5], t[6], t[8], t[9], t[10], t[12], t[13], t[14], v[0], v[1], v[2], v[12], v[13], v[14], w[12], w[13], w[14]);
    }
    static bool logLit = getenv("GHOSTPC_FRAMESTATS") != nullptr;
    static int litLogged;
    if (logLit && ffVertex && !vs && g_rs[XRS_LIGHTING] && litLogged < 40) {
        litLogged++;
        const D3DMATERIAL9 &m = g_material;
        Log("litff: fvf %08x ambient %08x colorvertex %u src d/s/a/e %u/%u/%u/%u spec %u localviewer %u normalize %u", g_curVS,
            g_rs[XRS_AMBIENT], g_rs[XRS_COLORVERTEX], g_rs[XRS_DIFFUSEMATERIALSOURCE], g_rs[XRS_SPECULARMATERIALSOURCE],
            g_rs[XRS_AMBIENTMATERIALSOURCE], g_rs[XRS_EMISSIVEMATERIALSOURCE], g_rs[XRS_SPECULARENABLE], g_rs[XRS_LOCALVIEWER],
            g_rs[XRS_NORMALIZENORMALS]);
        Log("   material D %.2f %.2f %.2f %.2f A %.2f %.2f %.2f E %.2f %.2f %.2f S %.2f %.2f %.2f P %.1f", m.Diffuse.r, m.Diffuse.g,
            m.Diffuse.b, m.Diffuse.a, m.Ambient.r, m.Ambient.g, m.Ambient.b, m.Emissive.r, m.Emissive.g, m.Emissive.b, m.Specular.r,
            m.Specular.g, m.Specular.b, m.Power);
        for (int i = 0; i < 8; i++)
            if (g_lightOn[i]) {
                const D3DLIGHT9 &l = g_lights[i];
                Log("   light %d type %u D %.2f %.2f %.2f A %.2f %.2f %.2f pos %.1f %.1f %.1f dir %.2f %.2f %.2f range %.1f att %.3f %.3f %.3f"
                    " theta %.2f phi %.2f falloff %.2f", i, l.Type, l.Diffuse.r, l.Diffuse.g, l.Diffuse.b, l.Ambient.r, l.Ambient.g, l.Ambient.b,
                    l.Position.x, l.Position.y, l.Position.z, l.Direction.x, l.Direction.y, l.Direction.z, l.Range, l.Attenuation0,
                    l.Attenuation1, l.Attenuation2, l.Theta, l.Phi, l.Falloff);
            }
    }
    // every draw uses a generated pixel shader: register combiners, or the texture stages with Xbox semantics
    bool psFog = true;
    ApplyTextures(false, ffVertex && !g_ffEmu);
    IDirect3DPixelShader9 *ps;
    // pixel shaders write the depth for draws through our vertex shaders (W-buffer emulation, PSDepthWrapper)
    extern bool g_psWriteDepth;
    static bool noWBuffer = getenv("GHOSTPC_NOWBUFFER") != nullptr;   // debug: hardware z as before
    g_psWriteDepth = !noWBuffer && (!ffVertex || g_ffEmu);
    float c[20][4] = {};
    if (g_curPS) {
        ps = PSGet(g_curPS, g_texType, &psFog);
        for (int i = 0; i < 8; i++) { ColorToF4(g_rs[XRS_PSCONSTANT0_0 + i], c[i]); ColorToF4(g_rs[XRS_PSCONSTANT1_0 + i], c[8 + i]); }
        ColorToF4(g_rs[XRS_PSFINALCOMBINERCONSTANT0], c[16]);
        ColorToF4(g_rs[XRS_PSFINALCOMBINERCONSTANT1], c[17]);
    } else {
        ps = PSGetFF(g_ts, g_texType, !ffVertex, ffVertex, g_rs[XRS_SPECULARENABLE]);
        ColorToF4(g_rs[XRS_TEXTUREFACTOR], c[16]);
    }
    g_dev->SetPixelShader(ps);
    g_dev->SetPixelShaderConstantF(0, c[0], 18);
    // c18..c21: 1/size for linear textures (texel-coordinate addressing)
    float ts[4][4];
    for (int s = 0; s < 4; s++) {
        ts[s][0] = g_texIsLinear[s] ? g_texSize[s][2] : 1; ts[s][1] = g_texIsLinear[s] ? g_texSize[s][3] : 1;
        ts[s][2] = 1; ts[s][3] = 1;
    }
    g_dev->SetPixelShaderConstantF(18, ts[0], 4);
    float misc[4] = {(float)(g_rs[XRS_FOGCOLOR] >> 16 & 255) / 255.f, (float)(g_rs[XRS_FOGCOLOR] >> 8 & 255) / 255.f,
                     (float)(g_rs[XRS_FOGCOLOR] & 255) / 255.f, 1};
    g_dev->SetPixelShaderConstantF(22, misc, 1);
    float bm[8][4];
    for (int s = 0; s < 4; s++) {
        DWORD *x = g_ts + s * 32;
        memcpy(&bm[s][0], &x[XTSS_BUMPENVMAT00], 4); memcpy(&bm[s][1], &x[XTSS_BUMPENVMAT01], 4);
        memcpy(&bm[s][2], &x[XTSS_BUMPENVMAT10], 4); memcpy(&bm[s][3], &x[XTSS_BUMPENVMAT11], 4);
        memcpy(&bm[4 + s][0], &x[XTSS_BUMPENVLSCALE], 4); memcpy(&bm[4 + s][1], &x[XTSS_BUMPENVLOFFSET], 4);
        bm[4 + s][2] = bm[4 + s][3] = 0;
    }
    g_dev->SetPixelShaderConstantF(24, bm[0], 8);
    ApplyRenderStates(psFog, !ffVertex || g_ffEmu);
    if (g_psWriteDepth) {
        float dm[4] = {g_rs[XRS_ZENABLE] == 2 ? 1.f : 0.f, g_psDepthBias, 0, 0};
        g_dev->SetPixelShaderConstantF(23, dm, 1);
    }
    if (g_ffEmu) ApplyFFEmu();
    else if (!ffVertex) {
        g_dev->SetVertexShader(vs->vs);
        // c[-38]/c[-37] (hw 58/59): viewport scale/offset used by the XDK trailer, in Xbox pixels (D24 depth)
        float *vs = &g_vsConst[58 * 4], *vo = &g_vsConst[59 * 4];
        vs[0] = g_vp.Width * 0.5f; vs[1] = -(float)g_vp.Height * 0.5f; vs[2] = 16777215.f * (g_vp.MaxZ - g_vp.MinZ); vs[3] = 1;
        vo[0] = g_vp.X + g_vp.Width * 0.5f; vo[1] = g_vp.Y + g_vp.Height * 0.5f; vo[2] = 16777215.f * g_vp.MinZ; vo[3] = 0;
        static bool noLight = getenv("GHOSTPC_NOVSLIGHT") != nullptr;   // debug: black light colours (c105/108/118/121)
        if (noLight) {
            float saved[4][4]; const int lc[4] = {105, 108, 118, 121};
            for (int k = 0; k < 4; k++) { memcpy(saved[k], &g_vsConst[lc[k] * 4], 16); memset(&g_vsConst[lc[k] * 4], 0, 12); }
            g_dev->SetVertexShaderConstantF(0, g_vsConst, 192);
            for (int k = 0; k < 4; k++) memcpy(&g_vsConst[lc[k] * 4], saved[k], 16);
        } else
            g_dev->SetVertexShaderConstantF(0, g_vsConst, 192);
        float extra[8][4] = {};
        for (int s = 0; s < 4; s++) {
            // linear textures use texel coordinates; scale once (here only when the pixel stage is fixed-function)
            bool sc = false;   // scaling happens in the pixel shader
            extra[s][0] = sc ? g_texSize[s][2] : 1; extra[s][1] = sc ? g_texSize[s][3] : 1;
            extra[s][2] = extra[s][3] = 1;
        }
        memcpy(extra[4], g_vpScale, 16);
        memcpy(extra[5], g_vpOffset, 16);
        extra[6][0] = (float)g_scale;
        extra[6][1] = WInv(true);
        extra[7][0] = (float)g_rs[XRS_FOGTABLEMODE];
        memcpy(&extra[7][1], &g_rs[XRS_FOGSTART], 4);
        memcpy(&extra[7][2], &g_rs[XRS_FOGEND], 4);
        memcpy(&extra[7][3], &g_rs[XRS_FOGDENSITY], 4);
        g_dev->SetVertexShaderConstantF(192, extra[0], 8);
    } else {
        g_dev->SetVertexShader(nullptr);
    }
}

// ---------------------------------------------------------------------------
// Draw helpers
static UINT PrimCount(DWORD xp, UINT n) {
    switch (xp) {
    case XPT_POINTLIST: return n;
    case XPT_LINELIST: return n / 2;
    case XPT_LINESTRIP: return n > 1 ? n - 1 : 0;
    case XPT_TRIANGLELIST: return n / 3;
    case XPT_TRIANGLESTRIP: case XPT_TRIANGLEFAN: return n > 2 ? n - 2 : 0;
    }
    return 0;
}
static D3DPRIMITIVETYPE PrimType(DWORD xp) {
    switch (xp) {
    case XPT_POINTLIST: return D3DPT_POINTLIST;
    case XPT_LINELIST: return D3DPT_LINELIST;
    case XPT_LINESTRIP: case XPT_LINELOOP: return D3DPT_LINESTRIP;
    case XPT_TRIANGLELIST: case XPT_QUADLIST: case XPT_QUADSTRIP: return D3DPT_TRIANGLELIST;
    case XPT_TRIANGLESTRIP: return D3DPT_TRIANGLESTRIP;
    case XPT_TRIANGLEFAN: case XPT_POLYGON: return D3DPT_TRIANGLEFAN;
    }
    return D3DPT_TRIANGLELIST;
}
// expand quads / loops into an index list (indices relative to vertex 0 of the draw)
static void ExpandIndices(DWORD xp, const uint32_t *in, UINT n, std::vector<uint32_t> &out) {
    out.clear();
    if (xp == XPT_QUADLIST) {
        for (UINT q = 0; q + 3 < n; q += 4) {
            out.insert(out.end(), {in[q], in[q + 1], in[q + 2], in[q], in[q + 2], in[q + 3]});
        }
    } else if (xp == XPT_QUADSTRIP) {
        for (UINT q = 0; q + 3 < n; q += 2) {
            out.insert(out.end(), {in[q], in[q + 1], in[q + 3], in[q], in[q + 3], in[q + 2]});
        }
    } else if (xp == XPT_LINELOOP) {
        out.assign(in, in + n);
        if (n) out.push_back(in[0]);
    } else out.assign(in, in + n);
}
static UINT ExpandedPrims(DWORD xp, UINT n) {
    if (xp == XPT_QUADLIST) return n / 4 * 2;
    if (xp == XPT_QUADSTRIP) return n >= 4 ? (n - 2) / 2 * 2 : 0;
    if (xp == XPT_LINELOOP) return n - 1;
    return PrimCount(xp == XPT_POLYGON ? XPT_TRIANGLEFAN : xp, n);
}

static std::vector<uint8_t> g_vtmp;
static std::vector<uint32_t> g_itmp, g_itmp2;

// Upload converted vertices [first, first+count) of the current streams; returns host stride or 0
static UINT UploadVertices(XVertexShader *vs, bool ffVertex, int first, UINT count, UINT &baseOut,
                           const void *upData = nullptr, UINT upStride = 0) {
    UINT stride;
    if (!vs) {
        // FVF: the Xbox layout matches D3D9
        DWORD fvf = g_curVS;
        stride = upData ? upStride : g_streams[0].stride;
        if (!stride) return 0;
        const uint8_t *src = upData ? (const uint8_t *)upData : g_streams[0].vb ? (const uint8_t *)g_streams[0].vb->Data : nullptr;
        if (!src) return 0;
        g_vtmp.resize((size_t)stride * count);
        __try { memcpy(g_vtmp.data(), src + (size_t)first * stride, (size_t)stride * count); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("d3d: bad vertex read"); return 0; }
        static UINT logFrame = getenv("GHOSTPC_LOG_HUDFRAME") ? atoi(getenv("GHOSTPC_LOG_HUDFRAME")) : 0;
        if (logFrame && g_frame == logFrame && (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW) {   // debug: 2D draws
            const float *p = (const float *)g_vtmp.data();
            Log("hudframe: rhw %u verts at %.0f,%.0f mapped %d viewport %lu,%lu", count, p[0], p[1], HudCanvas() != nullptr, g_vp.X, g_vp.Y);
        }
        const uint16_t *c = (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW ? HudCanvas() : nullptr;
        bool split = c != nullptr;
        float sx = split ? c[0] / 640.f : 1, sy = split ? c[1] / 480.f : 1, ox = split ? c[2] : 0, oy = split ? c[3] : 0;
        if ((fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW && (g_scale != 1 || split))
            for (UINT i = 0; i < count; i++) {
                float *p = (float *)(g_vtmp.data() + i * stride);
                p[0] = (p[0] * sx + ox) * g_scale; p[1] = (p[1] * sy + oy) * g_scale;
            }
        // debug: GHOSTPC_LOGBLEND: vertex alphas of the first BLENDDIFFUSEALPHA (stage 1) draw of each frame
        static bool logBlend = getenv("GHOSTPC_LOGBLEND") != nullptr;
        static UINT blendFrame = ~0u;
        if (logBlend && (fvf & D3DFVF_DIFFUSE) && g_ts[32 + XTSS_COLOROP] == 12 && blendFrame != g_frame && g_frame > 400 && g_frame < 640) {
            blendFrame = g_frame;
            UINT off = 12 + ((fvf & D3DFVF_NORMAL) ? 12 : 0), n0 = 0, n255 = 0, mid = 0;
            uint64_t sum = 0;
            for (UINT i = 0; i < count; i++) {
                DWORD c = *(DWORD *)(g_vtmp.data() + i * stride + off);
                BYTE a = (BYTE)(c >> 24);
                (a == 0 ? n0 : a == 255 ? n255 : mid)++;
                sum += c & 0xFFFFFF;
            }
            DWORD c0 = *(DWORD *)(g_vtmp.data() + off);
            Log("blend frame %u vb %08x first %d n %u: a0 %u a255 %u mid %u  v0 %08x sum %llx lighting %u ffEmu %d", g_frame,
                g_streams[0].vb ? g_streams[0].vb->Data : 0, first, count, n0, n255, mid, c0, sum, g_rs[XRS_LIGHTING], g_ffEmu);
        }
        IDirect3DVertexDeclaration9 *ffd = g_ffEmu ? FFVSDecl(fvf) : nullptr;
        if (ffd) g_dev->SetVertexDeclaration(ffd);
        else g_dev->SetFVF(fvf);
    } else {
        stride = vs->hostStride;
        g_vtmp.resize((size_t)stride * count);
        StreamState saved = g_streams[0];
        if (upData) { static XResource fake; fake.Data = (DWORD)upData; g_streams[0].vb = &fake; g_streams[0].stride = upStride; }
        __try { ConvertVertices(vs, ffVertex, first, count, g_vtmp.data()); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("d3d: bad vertex read (decl)"); if (upData) g_streams[0] = saved; return 0; }
        if (upData) g_streams[0] = saved;
        IDirect3DVertexDeclaration9 *d = ffVertex ? vs->ffdecl : vs->decl;
        if (!d) return 0;
        g_dev->SetVertexDeclaration(d);
    }
    UINT bytes = stride * count;
    if (bytes > VB_SIZE) { Log("d3d: draw too large (%u bytes)", bytes); return 0; }
    UINT pos = (g_vbPos + stride - 1) / stride * stride;
    DWORD lockf = D3DLOCK_NOOVERWRITE;
    if (pos + bytes > VB_SIZE) { pos = 0; lockf = D3DLOCK_DISCARD; }
    void *p;
    if (FAILED(g_dynVB->Lock(pos, bytes, &p, lockf))) return 0;
    memcpy(p, g_vtmp.data(), bytes);
    g_dynVB->Unlock();
    g_vbPos = pos + bytes;
    g_dev->SetStreamSource(0, g_dynVB, 0, stride);
    baseOut = pos / stride;
    return stride;
}
static bool UploadIndices(const std::vector<uint32_t> &idx, UINT &startOut) {
    UINT bytes = (UINT)idx.size() * 4;
    if (bytes > IB_SIZE) return false;
    UINT pos = g_ibPos;
    DWORD lockf = D3DLOCK_NOOVERWRITE;
    if (pos + bytes > IB_SIZE) { pos = 0; lockf = D3DLOCK_DISCARD; }
    void *p;
    if (FAILED(g_dynIB->Lock(pos, bytes, &p, lockf))) return false;
    memcpy(p, idx.data(), bytes);
    g_dynIB->Unlock();
    g_ibPos = pos + bytes;
    g_dev->SetIndices(g_dynIB);
    startOut = pos / 4;
    return true;
}

static UINT g_drawCalls;
// GHOSTPC_TINT=a-b: draw per-frame draws a..b in solid magenta (debugging)
static int g_tintA = -2, g_tintB = -1;
static IDirect3DPixelShader9 *g_tintPS;
static bool TintThis() {
    if (g_tintA == -2) {
        g_tintA = -1;
        if (const char *e = getenv("GHOSTPC_TINT")) sscanf(e, "%d-%d", &g_tintA, &g_tintB);
    }
    return g_tintA >= 0 && (int)g_frameDraw >= g_tintA && (int)g_frameDraw <= g_tintB;
}
extern int g_psDumpId;
static void TraceDraw(DWORD xp, UINT count, bool indexed, XVertexShader *vs, bool ffv) {
    char t[256] = "";
    for (int i = 0; i < 4; i++) if (g_tex[i]) {
        UINT w, h; XSurfaceDims(g_tex[i], &w, &h, nullptr, nullptr, nullptr);
        sprintf(t + strlen(t), " t%d=%02x/%ux%u%s", i, XFormat(g_tex[i]), w, h, HostOf(g_tex[i]) && HostOf(g_tex[i])->isRT ? "RT" : "");
    }
    Log("draw#%u/%u%s prim%u n%u%s vs=%s%p ps=%s(%d) at=%u/%x/%u ab=%u %x>%x zw=%u cull=%x/%x cw=%x%s rt=%p",
        g_drawCalls, g_frameDraw, g_inPB ? " PB" : "", xp, count, indexed ? " idx" : "", vs ? (ffv ? "FF:" : "VS:") : "FVF:", vs ? (void *)vs : (void *)g_curVS,
        g_curPS ? "PS" : "FF", g_curPS ? g_psDumpId : -1, Simple(0x300, 0), Simple(0x33C, 0), Simple(0x340, 0), Simple(0x304, 0),
        Simple(0x344, 1), Simple(0x348, 0), Simple(0x35C, 1), g_rs[XRS_CULLMODE], g_rs[XRS_FRONTFACE], Simple(0x358, 0), t, g_curRT);
    // dump constants + first converted vertex of a 74-instruction shader (EnvmapSkinned3) for offline checks
    static int vsDumps;
    static int capN = getenv("GHOSTPC_CAPVS") ? atoi(getenv("GHOSTPC_CAPVS")) : 74;
    if (vs && !ffv && vs->ucode.size() == (size_t)capN * 4 && vsDumps++ == 0) {
        char path[MAX_PATH];
        _snprintf(path, MAX_PATH, "%s\\..\\test\\vsstate.bin", g_gameDir);
        if (FILE *f = fopen(path, "wb")) {
            fwrite(g_vsConst, 4, 192 * 4, f);
            std::vector<uint8_t> v(vs->hostStride * 4);
            ConvertVertices(vs, false, 0, 4, v.data());
            fwrite(vs->regOffset, 4, 16, f);
            fwrite(&vs->hostStride, 4, 1, f);
            fwrite(v.data(), 1, v.size(), f);
            fclose(f);
            Log("dumped vsstate.bin");
        }
    }
    static int constDumps;
    static bool lightConsts = getenv("GHOSTPC_TRACEJUMP") != nullptr;
    static bool allConsts = getenv("GHOSTPC_TRACECONSTALL") != nullptr;   // constants of every traced shader draw
    if (vs && !ffv && (constDumps++ < 2 || lightConsts || allConsts))
        for (int i = 96; i < (lightConsts ? 122 : 140); i++)
            Log("   c[%d] = %.9g %.9g %.9g %.9g", i, g_vsConst[i * 4], g_vsConst[i * 4 + 1], g_vsConst[i * 4 + 2], g_vsConst[i * 4 + 3]);
    if (vs && !ffv && lightConsts) {
        // the fixed-function View*Projection at this point, transposed like the shader constants
        D3DMATRIX vp;
        const float *V = (const float *)&g_xform[XTS_VIEW], *P = (const float *)&g_xform[XTS_PROJECTION];
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) {
                float s = 0;
                for (int k = 0; k < 4; k++) s += V[r * 4 + k] * P[k * 4 + c];
                vp.m[c][r] = s;
            }
        for (int i = 0; i < 4; i++) Log("   VPt[%d] = %.9g %.9g %.9g %.9g", i, vp.m[i][0], vp.m[i][1], vp.m[i][2], vp.m[i][3]);
    }
    if (g_curPS)
        Log("   psconst C0 %08x %08x %08x %08x C1 %08x %08x %08x %08x map %08x/%08x cc %08x fc %08x %08x (map %03x) tf %08x",
            g_rs[XRS_PSCONSTANT0_0], g_rs[XRS_PSCONSTANT0_0 + 1], g_rs[XRS_PSCONSTANT0_0 + 2], g_rs[XRS_PSCONSTANT0_0 + 3],
            g_rs[XRS_PSCONSTANT1_0], g_rs[XRS_PSCONSTANT1_0 + 1], g_rs[XRS_PSCONSTANT1_0 + 2], g_rs[XRS_PSCONSTANT1_0 + 3],
            g_curPS->PSC0Mapping, g_curPS->PSC1Mapping, g_curPS->PSCombinerCount, g_rs[XRS_PSFINALCOMBINERCONSTANT0],
            g_rs[XRS_PSFINALCOMBINERCONSTANT1], g_curPS->PSFinalCombinerConstants, g_rs[XRS_TEXTUREFACTOR]);
    if (!g_curPS) {
        char b[512] = "";
        for (int i = 0; i < 4; i++) {
            DWORD *x = g_ts + i * 32;
            if (x[XTSS_COLOROP] <= 1 && i) break;
            sprintf(b + strlen(b), " [%d mip%x/%x/%x c%u(%x,%x,%x) a%u(%x,%x,%x) tci%x ttf%x r%x]", i, x[XTSS_MIPFILTER], x[XTSS_MIPMAPLODBIAS], x[XTSS_MAXMIPLEVEL], x[XTSS_COLOROP], x[XTSS_COLORARG0], x[XTSS_COLORARG1],
                    x[XTSS_COLORARG2], x[XTSS_ALPHAOP], x[XTSS_ALPHAARG0], x[XTSS_ALPHAARG1], x[XTSS_ALPHAARG2], x[XTSS_TEXCOORDINDEX],
                    x[XTSS_TEXTURETRANSFORMFLAGS], x[XTSS_RESULTARG]);
        }
        Log("   stages:%s tf=%08x light=%u", b, g_rs[XRS_TEXTUREFACTOR], g_rs[XRS_LIGHTING]);
    }
}
// Core: draw `count` vertices addressed by `idx` (absolute vertex numbers) or sequentially from `first`
static void DoDrawInner(DWORD xp, const uint32_t *idx, UINT count, int first, const void *upData, UINT upStride);
// GHOSTPC_DUMPDRAWS="n1,n2,...": in the GHOSTPC_TRACE frame, save the render target after those draws
// (numbered as in the trace) to test\drawN.bmp
static void DoDraw(DWORD xp, const uint32_t *idx, UINT count, int first, const void *upData = nullptr, UINT upStride = 0) {
    DoDrawInner(xp, idx, count, first, upData, upStride);
    if (xp == 1) g_framePoints = true;
    if (g_curVS & 1) g_fsVSDraws++;
    // GHOSTPC_TRACEJUMP: save the first render-target texture sampled on stage 1 (the projected shadow) each frame
    static bool jumpDbg = getenv("GHOSTPC_TRACEJUMP") != nullptr;
    if (jumpDbg && !g_shadowSaved && g_tex[1] && HostOf(g_tex[1]) && HostOf(g_tex[1])->isRT && g_frame > 600) {
        g_shadowSaved = true;
        char path[MAX_PATH];
        _snprintf(path, MAX_PATH, "%s\\..\\test\\jshadow%u.bmp", g_gameDir, g_frame & 1);
        SaveSurface(HostSurface(g_tex[1]), path);
    }
    static bool frameStats = getenv("GHOSTPC_FRAMESTATS") != nullptr;
    if (frameStats) {
        char sig[160];
        _snprintf(sig, sizeof sig, "prim%u n%u vs%p ps%p t0 %p/%ux%u rt%p ab%u", xp, count, (void *)(uintptr_t)g_curVS, (void *)g_curPS, (void *)g_tex[0],
                  g_texSize[0][0] ? (UINT)g_texSize[0][0] : 0, g_texSize[0][1] ? (UINT)g_texSize[0][1] : 0, (void *)g_curRT, g_rs[XRS_ALPHABLENDENABLE]);
        g_fsSig.push_back(sig);
    }
    if (g_curRT != (XPixelContainer *)g_backbuffer) g_fsOffscreen++;
    for (int i = 0; i < 4; i++) if (g_tex[i] && HostOf(g_tex[i]) && HostOf(g_tex[i])->isDepth) { g_fsDepthTex++; break; }
    if (!g_traceFrame) return;
    static std::vector<UINT> want;
    static bool init;
    if (!init) {
        init = true;
        if (const char *e = getenv("GHOSTPC_DUMPDRAWS")) for (const char *p = e; *p;) { want.push_back(atoi(p)); while (*p && *p != ',') p++; if (*p) p++; }
    }
    // "auto": every draw that renders into an off-screen target or samples a render target
    static bool autoMode = getenv("GHOSTPC_DUMPDRAWS") && !strcmp(getenv("GHOSTPC_DUMPDRAWS"), "auto");
    bool rtIO = g_curRT != (XPixelContainer *)g_backbuffer;
    for (int i = 0; i < 4 && !rtIO; i++) rtIO = g_tex[i] && HostOf(g_tex[i]) && HostOf(g_tex[i])->isRT;
    if (autoMode ? !rtIO : std::find(want.begin(), want.end(), g_frameDraw) == want.end()) return;
    char path[MAX_PATH];
    _snprintf(path, MAX_PATH, "%s\\..\\test\\draw%u.bmp", g_gameDir, g_frameDraw);
    IDirect3DSurface9 *s = HostSurface(g_curRT), *bound = nullptr;
    g_dev->GetRenderTarget(0, &bound);
    D3DSURFACE_DESC sd = {}; if (s) s->GetDesc(&sd);
    HostRes *h = HostOf(g_curRT);
    Log("dump draw %u: curRT %p view %d parent %p surf %p (%ux%u) bound %p bb %p", g_frameDraw, g_curRT,
        h ? h->isSurfaceView : -1, h ? h->parent : nullptr, s, sd.Width, sd.Height, bound, HostOf(g_backbuffer)->surf);
    if (bound) bound->Release();
    SaveSurface(s, path);
}
static void DoDrawInner(DWORD xp, const uint32_t *idx, UINT count, int first, const void *upData, UINT upStride) {
    if (!count || !g_dev) return;
    XVertexShader *vs; bool ffv;
    ApplyDraw(vs, ffv);
    g_drawCalls++;
    g_frameDraw++;
    static const char *tintMode = getenv("GHOSTPC_TINTMODE");
    bool tintCat = false;
    if (tintMode) {
        if (!strcmp(tintMode, "ps")) tintCat = g_curPS != nullptr;
        else if (!strcmp(tintMode, "vsff")) tintCat = vs && !ffv && !g_curPS;
        else if (!strcmp(tintMode, "ff")) tintCat = (!vs || ffv) && !g_curPS;
        else if (!strcmp(tintMode, "pb")) tintCat = g_inPB;
        else if (!strcmp(tintMode, "fvf")) tintCat = !vs && !g_curPS && !g_inPB;
        else if (!strcmp(tintMode, "decl")) tintCat = vs && ffv && !g_curPS && !g_inPB;
    }
    if (TintThis() || tintCat) {
        // solid magenta, no depth test, so the draw's coverage is visible
        g_dev->SetPixelShader(nullptr);
        g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFF00FF);
        g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
        g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
        g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        g_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        InvalidateCaches();
    }
    if (g_traceFrame) TraceDraw(xp, count, idx != nullptr, vs, ffv);
    if (idx) {
        uint32_t mn = 0xffffffff, mx = 0;
        for (UINT i = 0; i < count; i++) { mn = std::min(mn, idx[i]); mx = std::max(mx, idx[i]); }
        UINT base;
        if (!UploadVertices(vs, ffv, (int)mn, mx - mn + 1, base, upData, upStride)) return;
        g_itmp.resize(count);
        for (UINT i = 0; i < count; i++) g_itmp[i] = idx[i] - mn;
        ExpandIndices(xp, g_itmp.data(), count, g_itmp2);
        UINT start;
        if (!UploadIndices(g_itmp2, start)) return;
        UINT prims = ExpandedPrims(xp, count);
        if (prims) g_dev->DrawIndexedPrimitive(PrimType(xp), base, 0, mx - mn + 1, start, prims);
    } else {
        UINT base;
        if (!UploadVertices(vs, ffv, first, count, base, upData, upStride)) return;
        if (xp == XPT_QUADLIST || xp == XPT_QUADSTRIP || xp == XPT_LINELOOP) {
            g_itmp.resize(count);
            for (UINT i = 0; i < count; i++) g_itmp[i] = i;
            ExpandIndices(xp, g_itmp.data(), count, g_itmp2);
            UINT start;
            if (!UploadIndices(g_itmp2, start)) return;
            g_dev->DrawIndexedPrimitive(PrimType(xp), base, 0, count, start, ExpandedPrims(xp, count));
        } else {
            UINT prims = ExpandedPrims(xp, count);
            if (prims) g_dev->DrawPrimitive(PrimType(xp), base, prims);
        }
    }
}

XSTD(void, D3DDevice_DrawVertices, 12, (DWORD prim, UINT start, UINT count)) { DoDraw(prim, nullptr, count, (int)start + g_baseVertex * 0); }
XSTD(void, D3DDevice_DrawIndexedVertices, 12, (DWORD prim, UINT count, const WORD *indices)) {
    g_itmp.resize(count);
    static std::vector<uint32_t> abs;
    abs.resize(count);
    __try { for (UINT i = 0; i < count; i++) abs[i] = indices[i] + g_baseVertex; }
    __except (EXCEPTION_EXECUTE_HANDLER) { Log("d3d: bad index pointer %p", indices); return; }
    DoDraw(prim, abs.data(), count, 0);
}
XSTD(void, D3DDevice_DrawVerticesUP, 16, (DWORD prim, UINT count, const void *data, UINT stride)) {
    DoDraw(prim, nullptr, count, 0, data, stride);
}

// Immediate mode: Begin / SetVertexData* / End
static bool g_inBegin;
static DWORD g_beginPrim;
static float g_imm[16][4];
static std::vector<float> g_immVerts;   // 16 regs x 4 floats per vertex
static void ImmEmit() { g_immVerts.insert(g_immVerts.end(), &g_imm[0][0], &g_imm[0][0] + 64); }
static void ImmSet(int reg, float a, float b, float c, float d) {
    if (reg < 0 || reg >= 16) return;
    g_imm[reg][0] = a; g_imm[reg][1] = b; g_imm[reg][2] = c; g_imm[reg][3] = d;
    if (reg == 0) ImmEmit();
}
XSTD(void, D3DDevice_Begin, 4, (DWORD prim)) {
    g_inBegin = true; g_beginPrim = prim; g_immVerts.clear();
    for (int i = 0; i < 16; i++) { g_imm[i][0] = g_imm[i][1] = g_imm[i][2] = 0; g_imm[i][3] = 1; }
    g_imm[3][0] = g_imm[3][1] = g_imm[3][2] = g_imm[3][3] = 1;   // diffuse defaults to white
}
XSTD(void, D3DDevice_SetVertexData2f, 12, (int reg, float a, float b)) { ImmSet(reg, a, b, 0, 1); }
XSTD(void, D3DDevice_SetVertexData4f, 20, (int reg, float a, float b, float c, float d)) { ImmSet(reg, a, b, c, d); }
XSTD(void, D3DDevice_SetVertexData4ub, 20, (int reg, BYTE a, BYTE b, BYTE c, BYTE d)) { ImmSet(reg, a / 255.f, b / 255.f, c / 255.f, d / 255.f); }
XSTD(void, D3DDevice_SetVertexDataColor, 8, (int reg, D3DCOLOR c)) {
    ImmSet(reg, ((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, (c >> 24) / 255.f);
}
void DrawImmediate(DWORD prim, const float *verts, UINT count);
XSTD(void, D3DDevice_End, 0, ()) {
    g_inBegin = false;
    UINT n = (UINT)(g_immVerts.size() / 64);
    if (n) DrawImmediate(g_beginPrim, g_immVerts.data(), n);
}
// immediate-mode vertices: every register as float4
static UINT FvfTexSize(DWORD fvf, int i) {
    DWORD f = (fvf >> (16 + i * 2)) & 3;
    return f == 0 ? 2 : f == 1 ? 3 : f == 2 ? 4 : 1;
}
static DWORD F4Color(const float *v) {
    auto b = [](float x) { return (DWORD)(std::min(std::max(x, 0.f), 1.f) * 255.f + 0.5f); };
    return (b(v[3]) << 24) | (b(v[0]) << 16) | (b(v[1]) << 8) | b(v[2]);
}
void DrawImmediate(DWORD prim, const float *verts, UINT count) {
    static XResource fake;
    if (!(g_curVS & 1)) {
        // FVF: assemble vertices in the FVF layout from the register values
        DWORD fvf = g_curVS;
        std::vector<uint8_t> buf;
        UINT stride = 0;
        for (UINT i = 0; i < count; i++) {
            const float (*r)[4] = (const float (*)[4])(verts + i * 64);
            size_t start = buf.size();
            auto put = [&](const void *p, size_t n) { buf.insert(buf.end(), (const uint8_t *)p, (const uint8_t *)p + n); };
            DWORD pos = fvf & D3DFVF_POSITION_MASK;
            if (pos == D3DFVF_XYZRHW) put(r[0], 16);
            else if (pos) put(r[0], 12);
            if (pos >= D3DFVF_XYZB1 && pos <= D3DFVF_XYZB5) put(r[1], 4 * ((pos - 4) / 2));
            if (fvf & D3DFVF_NORMAL) put(r[2], 12);
            if (fvf & D3DFVF_PSIZE) put(r[6], 4);
            if (fvf & D3DFVF_DIFFUSE) { DWORD c = F4Color(r[3]); put(&c, 4); }
            if (fvf & D3DFVF_SPECULAR) { DWORD c = F4Color(r[4]); put(&c, 4); }
            UINT nt = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
            for (UINT t = 0; t < nt && t < 4; t++) put(r[9 + t], 4 * FvfTexSize(fvf, t));
            stride = (UINT)(buf.size() - start);
        }
        g_itmp.resize(count);
        for (UINT i = 0; i < count; i++) g_itmp[i] = i;
        DoDraw(prim, nullptr, count, 0, buf.data(), stride);
        return;
    }
    XVertexShader *vs = VSFromHandle(g_curVS);
    VSPrepare(vs);
    // source layout: 16 float4 registers per vertex on stream 0
    XVertexShader tmp = *vs;
    tmp.elems.clear();
    for (auto &e : vs->elems) tmp.elems.push_back({0, e.reg, 0x42, e.reg * 16});
    tmp.vs = nullptr; tmp.decl = tmp.ffdecl = nullptr;   // not owned
    XVertexShader *s; bool ffv;
    ApplyDraw(s, ffv);
    StreamState save = g_streams[0];
    fake.Data = (DWORD)verts;
    g_streams[0].vb = &fake; g_streams[0].stride = 64;
    UINT stride = vs->hostStride;
    g_vtmp.resize(stride * count);
    ConvertVertices(&tmp, ffv, 0, count, g_vtmp.data());
    g_streams[0] = save;
    IDirect3DVertexDeclaration9 *decl = ffv ? vs->ffdecl : vs->decl;
    if (!decl) return;
    void *p;
    UINT bytes = stride * count;
    UINT pos = (g_vbPos + stride - 1) / stride * stride;
    DWORD lf = D3DLOCK_NOOVERWRITE;
    if (pos + bytes > VB_SIZE) { pos = 0; lf = D3DLOCK_DISCARD; }
    if (FAILED(g_dynVB->Lock(pos, bytes, &p, lf))) return;
    memcpy(p, g_vtmp.data(), bytes);
    g_dynVB->Unlock();
    g_vbPos = pos + bytes;
    g_dev->SetStreamSource(0, g_dynVB, 0, stride);
    g_dev->SetVertexDeclaration(decl);
    UINT base = pos / stride;
    g_itmp.resize(count);
    for (UINT i = 0; i < count; i++) g_itmp[i] = i;
    ExpandIndices(prim, g_itmp.data(), count, g_itmp2);
    UINT start;
    if (!UploadIndices(g_itmp2, start)) return;
    UINT prims = ExpandedPrims(prim, count);
    if (prims) g_dev->DrawIndexedPrimitive(PrimType(prim), base, 0, count, start, prims);
}

// ---------------------------------------------------------------------------
// Push buffers: the game records index lists as NV2A BEGIN_END / ARRAY_ELEMENT methods
static void RunPushBufferImpl(XPushBuffer *pb);
XSTD(void, D3DDevice_RunPushBuffer, 8, (XPushBuffer *pb, void *fixup)) {
    g_inPB = true;
    RunPushBufferImpl(pb);
    g_inPB = false;
}
static void RunPushBufferImpl(XPushBuffer *pb) {
    const DWORD *p = (const DWORD *)pb->Data, *end = (const DWORD *)(pb->Data + pb->Size);
    DWORD prim = 0;
    static std::vector<uint32_t> idx;
    idx.clear();
    while (p < end) {
        DWORD h = *p++;
        if (!h) continue;
        DWORD method = h & 0x1FFC, count = (h >> 18) & 0x7FF;
        bool noinc = (h & 0x40000000) != 0;
        if (h & 0x20000000) { Log("pushbuffer: jump %08x unsupported", h); break; }
        for (DWORD i = 0; i < count && p < end; i++, p++) {
            DWORD m = noinc ? method : method + i * 4;
            DWORD v = *p;
            if (m == 0x17FC) {             // SET_BEGIN_END
                if (v == 0) {
                    if (prim && !idx.empty()) {
                        for (auto &x : idx) x += g_baseVertex;
                        DoDraw(prim, idx.data(), (UINT)idx.size(), 0);
                    }
                    prim = 0; idx.clear();
                } else { prim = v; idx.clear(); }
            } else if (m >= 0x1800 && m < 0x1808) {   // ARRAY_ELEMENT16 (two indices)
                idx.push_back(v & 0xFFFF); idx.push_back(v >> 16);
            } else if (m >= 0x1808 && m < 0x1810) {   // ARRAY_ELEMENT32
                idx.push_back(v);
            } else if (m == 0x1810) {                  // DRAW_ARRAYS
                UINT s = v & 0xFFFFFF, c = (v >> 24) + 1;
                for (UINT k = 0; k < c; k++) idx.push_back(s + k);
            } else {
                static int warned;
                if (warned++ < 20) Log("pushbuffer: method %04x = %08x ignored", m, v);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Visibility tests (occlusion queries)
static std::unordered_map<DWORD, IDirect3DQuery9 *> g_queries;
static IDirect3DQuery9 *g_activeQuery;
XSTD(HRESULT, D3DDevice_BeginVisibilityTest, 0, ()) {
    if (FAILED(g_dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &g_activeQuery))) { g_activeQuery = nullptr; return 0; }
    g_activeQuery->Issue(D3DISSUE_BEGIN);
    return 0;
}
XSTD(HRESULT, D3DDevice_EndVisibilityTest, 4, (DWORD index)) {
    if (!g_activeQuery) return 0;
    g_activeQuery->Issue(D3DISSUE_END);
    auto &q = g_queries[index];
    if (q) q->Release();
    q = g_activeQuery;
    g_activeQuery = nullptr;
    return 0;
}
XSTD(HRESULT, D3DDevice_GetVisibilityTestResult, 12, (DWORD index, UINT *result, ULONGLONG *ts)) {
    auto it = g_queries.find(index);
    if (it == g_queries.end() || !it->second) {
        if (getenv("GHOSTPC_FRAMESTATS")) Log("vis frame %u: test %u unknown -> 1000", g_frame, index);
        if (result) *result = 1000;
        return 0;
    }
    DWORD pixels = 0;
    HRESULT hr = it->second->GetData(&pixels, sizeof pixels, D3DGETDATA_FLUSH);
    if (hr == S_FALSE) return 0x8876086Eu;   // D3DERR_TESTINCOMPLETE
    if (result) *result = pixels / (g_scale * g_scale);
    static bool logVis = getenv("GHOSTPC_FRAMESTATS") != nullptr;
    if (logVis) Log("vis frame %u: test %u -> %u pixels (hr %08x)", g_frame, index, pixels, hr);
    if (ts) *ts = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// Misc device functions
XSTD(DWORD, D3DDevice_InsertFence, 0, ()) { return ++g_fence; }
XSTD(void, D3DDevice_BlockOnFence, 4, (DWORD)) {}
XSTD(DWORD, D3DDevice_GetPushDistance, 4, (DWORD)) { return 0; }
XSTD(void, D3DDevice_SetWaitCallback, 4, (void *)) {}
XSTD(void, D3DDevice_SetFlickerFilter, 4, (DWORD)) {}
XSTD(void, D3DDevice_SetSoftDisplayFilter, 4, (BOOL)) {}
XSTD(void, D3DDevice_SetGammaRamp, 8, (DWORD flags, const BYTE *ramp)) {
    D3DGAMMARAMP r;
    for (int i = 0; i < 256; i++) { r.red[i] = ramp[i] * 257; r.green[i] = ramp[256 + i] * 257; r.blue[i] = ramp[512 + i] * 257; }
    g_dev->SetGammaRamp(0, D3DSGR_NO_CALIBRATION, &r);   // ignored in windowed mode
}
static DWORD g_copyState[8], g_copyRop[8];
XSTD(void, D3DDevice_SetCopyRectsState, 8, (const DWORD *s, const DWORD *r)) {
    if (s) memcpy(g_copyState, s, sizeof g_copyState);
    if (r) memcpy(g_copyRop, r, sizeof g_copyRop);
}
XSTD(void, D3DDevice_GetCopyRectsState, 8, (DWORD *s, DWORD *r)) {
    if (s) memcpy(s, g_copyState, sizeof g_copyState);
    if (r) memcpy(r, g_copyRop, sizeof g_copyRop);
}
XSTD(HRESULT, D3DDevice_CopyRects, 20, (XSurface *src, const RECT *rects, UINT n, XSurface *dst, const POINT *pts)) {
    IDirect3DSurface9 *s = HostSurface(src), *d = HostSurface(dst);
    if (s && d) {
        UINT cnt = rects ? n : 1;
        for (UINT i = 0; i < cnt; i++) {
            RECT sr, dr;
            if (rects) sr = rects[i];
            else { UINT w, h; XSurfaceDims(src, &w, &h, nullptr, nullptr, nullptr); sr = {0, 0, (LONG)w, (LONG)h}; }
            POINT p = pts ? pts[i] : POINT{sr.left, sr.top};
            dr = {p.x, p.y, p.x + sr.right - sr.left, p.y + sr.bottom - sr.top};
            RECT ss = {sr.left * g_scale, sr.top * g_scale, sr.right * g_scale, sr.bottom * g_scale};
            RECT dd = {dr.left * g_scale, dr.top * g_scale, dr.right * g_scale, dr.bottom * g_scale};
            HRESULT hr = g_dev->StretchRect(s, &ss, d, &dd, D3DTEXF_LINEAR);
            static int logged;
            if (FAILED(hr) && logged++ < 5) {
                D3DSURFACE_DESC sd, ddsc;
                s->GetDesc(&sd);
                d->GetDesc(&ddsc);
                Log("CopyRects: StretchRect failed %08x (src fmt %d usage %x pool %d, dst fmt %d usage %x pool %d %ux%u)",
                    (unsigned)hr, sd.Format, sd.Usage, sd.Pool, ddsc.Format, ddsc.Usage, ddsc.Pool, ddsc.Width, ddsc.Height);
            }
        }
        return 0;
    }
    if (d && !s) {
        // CPU surface -> GPU target: upload through a texture
        HostRes *sh = HostOf(src);
        XPixelContainer *owner = (sh && sh->isSurfaceView) ? sh->parent : src;
        IDirect3DBaseTexture9 *t = HostTexture(owner);
        UINT w, h; XSurfaceDims(src, &w, &h, nullptr, nullptr, nullptr);
        UINT cnt = rects ? n : 1;
        for (UINT i = 0; i < cnt; i++) {
            RECT sr = rects ? rects[i] : RECT{0, 0, (LONG)w, (LONG)h};
            POINT p = pts ? pts[i] : POINT{sr.left, sr.top};
            RECT dr = {p.x * g_scale, p.y * g_scale, (p.x + sr.right - sr.left) * g_scale, (p.y + sr.bottom - sr.top) * g_scale};
            BlitTexture(t, w, h, &sr, d, &dr);
        }
        return 0;
    }
    static int warned;
    if (warned++ < 10) Log("CopyRects: unsupported combination src %p (%d) dst %p (%d)", src, s != nullptr, dst, d != nullptr);
    return 0;
}

// Overlay (used by the video players): draw the YUV surface over the frame at Swap
XSTD(void, D3DDevice_EnableOverlay, 4, (BOOL e)) { g_overlayOn = e != 0; }
XSTD(void, D3DDevice_UpdateOverlay, 20, (XSurface *s, const RECT *src, const RECT *dst, BOOL ck, D3DCOLOR key)) {
    g_overlaySurf = s;
    if (s) {
        HostRes *h = HostOf(s);
        XPixelContainer *owner = (h && h->isSurfaceView) ? h->parent : s;
        HostGet(owner)->dirty = true;
    }
    if (src) g_overlaySrc = *src; else g_overlaySrc = {0, 0, 0, 0};
    if (dst) g_overlayDst = *dst; else g_overlayDst = {0, 0, 640, 480};
}
XSTD(BOOL, D3DDevice_GetOverlayUpdateStatus, 0, ()) { return TRUE; }
static void DrawOverlay() {
    HostRes *h = HostOf(g_overlaySurf);
    XPixelContainer *owner = (h && h->isSurfaceView) ? h->parent : g_overlaySurf;
    IDirect3DBaseTexture9 *t = HostTexture(owner);
    if (!t) return;
    UINT w, hh; XSurfaceDims(g_overlaySurf, &w, &hh, nullptr, nullptr, nullptr);
    RECT d = {g_overlayDst.left * g_scale, g_overlayDst.top * g_scale, g_overlayDst.right * g_scale, g_overlayDst.bottom * g_scale};
    BlitTexture(t, w, hh, g_overlaySrc.right ? &g_overlaySrc : nullptr, HostOf(g_backbuffer)->surf, &d);
}

