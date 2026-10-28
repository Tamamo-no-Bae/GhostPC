// GhostPC internal D3D plumbing shared by d3d_*.cpp
#pragma once
#include "xd3d.h"
#include <vector>
#include <algorithm>

struct SurfView { UINT face, level; XSurface *surf; };

struct HostRes {
    IDirect3DBaseTexture9 *tex = nullptr;   // texture (2D/cube/volume)
    IDirect3DSurface9 *surf = nullptr;      // render target / depth surface or backbuffer
    bool dirty = true;
    bool isRT = false, isDepth = false, isSurfaceView = false, isBackbuffer = false, linear = false;
    XPixelContainer *parent = nullptr;      // surface views: owning texture
    UINT face = 0, level = 0;
    DWORD fmtKey = 0, sizeKey = 0, dataKey = 0;
    UINT w = 0, h = 0, d = 1;
    UINT bytes = 0;                         // >0: memory allocated by us
    bool watch = false;                     // locked at some point: the CPU may keep writing (e.g. Bink)
    uint64_t hash = 0;
    UINT hashFrame = ~0u;
    std::vector<SurfView> views;
};

HostRes *HostOf(const void *r);
HostRes *HostGet(const void *r);
void HostFree(const void *r);
IDirect3DBaseTexture9 *HostTexture(XPixelContainer *p);
IDirect3DSurface9 *HostSurface(XPixelContainer *p);       // RT/DS binding
void MakeRenderTarget(XPixelContainer *p, HostRes *h);
void MakeDepthTarget(XPixelContainer *p, HostRes *h);
void ReadbackTarget(XPixelContainer *p);
void UnbindResource(XResource *r);
void D3DResource_AddRefInternal(XResource *r);
D3DFORMAT XFmtHost(DWORD f);
bool XFmtIsLinear(DWORD f);
UINT XLevelOffset(DWORD fmt, UINT w, UINT h, UINT d, UINT level);
XSurface *MakeSurfaceView(XPixelContainer *t, UINT face, UINT level);
extern "C" ULONG __stdcall xb_D3DResource_AddRef(XResource *r);
extern "C" ULONG __stdcall xb_D3DResource_Release(XResource *r);

extern int g_scale;                 // internal resolution multiplier
extern DWORD *g_rs;                 // D3D__RenderState inside Ghost.exe
extern DWORD *g_ts;                 // D3D__TextureState [4][32]

// ---------------------------------------------------------------------------
// Shaders (d3d_shader.cpp)
struct VSDeclElem { int stream; int reg; DWORD type; int offset; };
struct XVertexShader {
    std::vector<VSDeclElem> elems;
    int streamStride[16] = {};      // stride implied by the declaration (if no SetStreamSource stride)
    std::vector<DWORD> ucode;       // NV2A microcode (header removed), 4 dwords per instruction
    std::vector<std::pair<int, std::vector<float>>> constmem;   // CONSTMEM tokens
    bool hasFunction = false;
    DWORD fvf = 0;                  // when created from an FVF
    // host objects
    IDirect3DVertexShader9 *vs = nullptr;
    IDirect3DVertexDeclaration9 *decl = nullptr;     // programmable (all float4 TEXCOORDn)
    IDirect3DVertexDeclaration9 *ffdecl = nullptr;   // fixed function (semantic usages)
    bool failed = false;
    bool screenSpace = false;       // writes oPos directly (no r12 trailer)
    UINT hostStride = 0;            // converted vertex size
    int regOffset[16];              // offset of each input register in the converted vertex (-1 none)
    bool ffColorAsDword[16] = {};
};
// same layout as the Xbox D3DPixelShader object: the definition is reached through +8
struct XPixelShaderObj {
    DWORD common, reserved;
    XPixelShaderDef *pDef;
    XPixelShaderDef def;
};

XVertexShader *VSFromHandle(DWORD h);
bool VSPrepare(XVertexShader *s);       // translate + create host objects
IDirect3DPixelShader9 *PSGet(const XPixelShaderDef *def, const DWORD *texTypes, bool *usesFog);
IDirect3DPixelShader9 *PSGetFF(const DWORD *ts, const DWORD *texTypes, bool projDefault, bool ffVertex, DWORD specularEnable);
// Fixed-function vertex processing emulated by a generated vertex shader (FVF draws), so that positions are
// computed the same way as by the game's own vertex shaders. Constants: c0-c3 position rows, c4-c7 world-view rows
// (transposed), c8-c23 texture matrices (transposed, 4 per stage), c24 fog (mode, start, end, density), c25 fog vector,
// lighting (diffuse only): c26 global ambient, c27-c29 material diffuse/ambient/emissive, then 6 per light from c30:
// view-space position, view-space direction, diffuse, ambient, (range, att0, att1, att2), (cos theta/2, cos phi/2, falloff).
struct FFVSKey {
    DWORD fvf;
    DWORD tci[4], ttf[4];   // per stage: texcoord index | texgen << 16, texture transform flags
    BYTE lightType[8];      // D3DLIGHTTYPE of enabled lights, 0 = off (all 0 with lighting disabled)
    BYTE lighting, colorVertex, srcDiffuse, srcAmbient, srcEmissive;   // material sources: 0 material, 1 diffuse, 2 specular
    bool normalize, rangeFog;
    bool operator<(const FFVSKey &o) const { return memcmp(this, &o, sizeof *this) < 0; }
};
IDirect3DVertexShader9 *FFVSGet(const FFVSKey &k);
IDirect3DVertexDeclaration9 *FFVSDecl(DWORD fvf);
void ConvertVertices(XVertexShader *s, bool ff, int first, int count, uint8_t *dst);

// stream state (d3d_core.cpp)
struct StreamState { XResource *vb; UINT stride; };
extern StreamState g_streams[16];
extern float g_vsConst[192 * 4];
extern int g_baseVertex;
