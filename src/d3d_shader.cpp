// Xbox shaders on D3D9:
//  - vertex declarations + vertex format conversion
//  - NV2A vertex program microcode -> HLSL (vs_2_a)
//  - NV2A register combiner pixel shaders (D3DPIXELSHADERDEF) -> HLSL (ps_2_b)
#include "d3d_int.h"
#include <d3dcompiler.h>
#include <string>
#include <unordered_map>
#include <map>

// ---------------------------------------------------------------------------
// HLSL compiler
typedef HRESULT(WINAPI *D3DCompile_t)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob **, ID3DBlob **);
static D3DCompile_t g_compile;
static ID3DBlob *Compile(const std::string &src, const char *profile, const char *what) {
    if (!g_compile) {
        HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
        if (m) g_compile = (D3DCompile_t)GetProcAddress(m, "D3DCompile");
        if (!g_compile) Fatal("d3dcompiler_47.dll not available");
    }
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = g_compile(src.data(), src.size(), what, nullptr, nullptr, "main", profile,
                           D3DCOMPILE_OPTIMIZATION_LEVEL1 | D3DCOMPILE_AVOID_FLOW_CONTROL, 0, &code, &err);
    if (FAILED(hr)) {
        Log("shader compile failed (%s, %s): %s", what, profile, err ? (const char *)err->GetBufferPointer() : "?");
        static int dumps;
        if (dumps++ < 10) Log("--- source ---\n%s", src.c_str());
        if (err) err->Release();
        return nullptr;
    }
    if (err) err->Release();
    return code;
}

// ---------------------------------------------------------------------------
// Vertex declarations
static int TypeSize(DWORD t) {
    switch (t & 0xF) {
    case 2: return (t >> 4) == 7 ? 12 : (t >> 4) * 4;   // FLOATn / FLOAT2H / NONE(0x02 -> 0)
    case 0: return 4;                                   // D3DCOLOR
    case 5: case 1: return (t >> 4) * 2;                // SHORTn / NORMSHORTn
    case 6: return 4;                                   // NORMPACKED3
    case 4: return t >> 4;                              // PBYTEn
    }
    return 0;
}
static int TypeComps(DWORD t) {
    if (t == 0x40) return 4;
    if (t == 0x16) return 3;
    if (t == 0x72) return 4;
    return t >> 4;
}

XSTD(HRESULT, D3DDevice_CreateVertexShader, 16, (const DWORD *decl, const DWORD *func, DWORD *handle, DWORD usage)) {
    auto s = new XVertexShader();
    int stream = 0, off[16] = {};
    for (int i = 0; i < 16; i++) s->regOffset[i] = -1;
    if (decl) {
        for (const DWORD *t = decl; *t != 0xFFFFFFFF; t++) {
            DWORD tok = *t, type = tok >> 29;
            if (type == 1) stream = tok & 0xF;
            else if (type == 2) {
                if (tok & 0x10000000) {
                    DWORD n = (tok >> 16) & 0xFF;
                    off[stream] += (tok & 0x08000000) ? n : n * 4;
                } else {
                    VSDeclElem e = {stream, (int)(tok & 0x1F), (tok >> 16) & 0xFF, off[stream]};
                    s->elems.push_back(e);
                    off[stream] += TypeSize(e.type);
                }
            } else if (type == 4) {
                int count = (tok >> 25) & 0xF;
                int addr = (int)(signed char)(tok & 0xFF) + 96;
                std::vector<float> v(count * 4);
                memcpy(v.data(), t + 1, count * 16);
                s->constmem.push_back({addr, v});
                t += count * 4;
            } else if (type == 5) {
                t += (tok >> 24) & 0x1F;
            } else if (type == 0 || type == 3) {
            } else break;
        }
    }
    for (int i = 0; i < 16; i++) s->streamStride[i] = off[i];
    if (func) {
        DWORD hdr = func[0];
        int n = hdr >> 16;
        s->ucode.assign(func + 1, func + 1 + n * 4);
        s->hasFunction = true;
        if ((hdr & 0xFFFF) != 0x2078) Log("vs: shader type %04x (%d instructions)", hdr & 0xFFFF, n);
    }
    // host layout: one 16-byte slot per used input register
    UINT o = 0;
    for (auto &e : s->elems) if (s->regOffset[e.reg] < 0) { s->regOffset[e.reg] = o; o += 16; }
    s->hostStride = o ? o : 16;
    *handle = (DWORD)s | 1;
    return 0;
}
XVertexShader *VSFromHandle(DWORD h) { return (XVertexShader *)(h & ~1u); }

// Convert vertices of the current streams into the host layout
void ConvertVertices(XVertexShader *s, bool ff, int first, int count, uint8_t *dst) {
    memset(dst, 0, (size_t)s->hostStride * count);
    for (auto &e : s->elems) {
        int ro = s->regOffset[e.reg];
        if (ro < 0) continue;
        StreamState &st = g_streams[e.stream];
        if (!st.vb || !st.vb->Data) continue;
        UINT stride = st.stride ? st.stride : s->streamStride[e.stream];
        const uint8_t *src = (const uint8_t *)st.vb->Data + (size_t)first * stride + e.offset;
        bool asColor = ff && s->ffColorAsDword[e.reg];
        for (int i = 0; i < count; i++, src += stride) {
            float *f = (float *)(dst + (size_t)i * s->hostStride + ro);
            if (asColor) {
                if (e.type == 0x40) *(DWORD *)f = *(const DWORD *)src;
                else {
                    float v[4] = {0, 0, 0, 1};
                    if ((e.type & 0xF) == 2) for (int k = 0; k < (int)(e.type >> 4) && k < 4; k++) v[k] = ((const float *)src)[k];
                    auto b = [](float x) { return (DWORD)(std::min(std::max(x, 0.f), 1.f) * 255.f + 0.5f); };
                    *(DWORD *)f = (b(v[3]) << 24) | (b(v[0]) << 16) | (b(v[1]) << 8) | b(v[2]);
                }
                continue;
            }
            f[0] = f[1] = f[2] = 0; f[3] = 1;
            switch (e.type) {
            case 0x12: case 0x22: case 0x32: case 0x42:
                for (int k = 0; k < (int)(e.type >> 4); k++) f[k] = ((const float *)src)[k];
                break;
            case 0x72: f[0] = ((const float *)src)[0]; f[1] = ((const float *)src)[1]; f[3] = ((const float *)src)[2]; break;
            case 0x40: f[0] = src[2] / 255.f; f[1] = src[1] / 255.f; f[2] = src[0] / 255.f; f[3] = src[3] / 255.f; break;
            case 0x15: case 0x25: case 0x35: case 0x45:
                for (int k = 0; k < (int)(e.type >> 4); k++) f[k] = ((const short *)src)[k];
                break;
            case 0x11: case 0x21: case 0x31: case 0x41:
                for (int k = 0; k < (int)(e.type >> 4); k++) f[k] = std::max(-1.f, ((const short *)src)[k] / 32767.f);
                break;
            case 0x14: case 0x24: case 0x34: case 0x44:
                for (int k = 0; k < (int)(e.type >> 4); k++) f[k] = src[k] / 255.f;
                break;
            case 0x16: {
                DWORD v = *(const DWORD *)src;
                int x = (int)(v << 21) >> 21, y = (int)(v << 10) >> 21, z = (int)v >> 22;
                f[0] = std::max(-1.f, x / 1023.f); f[1] = std::max(-1.f, y / 1023.f); f[2] = std::max(-1.f, z / 511.f);
                break;
            }
            case 0x02: break;
            default: {
                static int warned;
                if (warned++ < 10) Log("vs: unsupported vertex type %02x", e.type);
            }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// NV2A vertex microcode -> HLSL
static DWORD Field(const DWORD *w, int word, int lo, int bits) { return (w[word] >> lo) & ((1u << bits) - 1); }
static const char *SWZ = "xyzw";

static std::string VSrc(const DWORD *w, int which, bool a0x) {
    // which: 0=A 1=B 2=C
    DWORD mux, neg, sx, sy, sz, sw, r;
    if (which == 0) {
        neg = Field(w, 1, 8, 1); sx = Field(w, 1, 6, 2); sy = Field(w, 1, 4, 2); sz = Field(w, 1, 2, 2); sw = Field(w, 1, 0, 2);
        r = Field(w, 2, 28, 4); mux = Field(w, 2, 26, 2);
    } else if (which == 1) {
        neg = Field(w, 2, 25, 1); sx = Field(w, 2, 23, 2); sy = Field(w, 2, 21, 2); sz = Field(w, 2, 19, 2); sw = Field(w, 2, 17, 2);
        r = Field(w, 2, 13, 4); mux = Field(w, 2, 11, 2);
    } else {
        neg = Field(w, 2, 10, 1); sx = Field(w, 2, 8, 2); sy = Field(w, 2, 6, 2); sz = Field(w, 2, 4, 2); sw = Field(w, 2, 2, 2);
        r = (Field(w, 2, 0, 2) << 2) | Field(w, 3, 30, 2); mux = Field(w, 3, 28, 2);
    }
    char base[64];
    DWORD v = Field(w, 1, 9, 4), c = Field(w, 1, 13, 8);
    if (mux == 1) sprintf(base, "r%u", r);
    else if (mux == 2) sprintf(base, "i.v%u", v);
    else if (mux == 3) { if (a0x) sprintf(base, "c[a0 + %u]", c); else sprintf(base, "c[%u]", c); }
    else sprintf(base, "float4(0,0,0,0)");
    std::string s = std::string(neg ? "(-" : "(") + base + "." + SWZ[sx] + SWZ[sy] + SWZ[sz] + SWZ[sw] + ")";
    return s;
}
static std::string Mask(DWORD m) {
    std::string s;
    if (m & 8) s += 'x';
    if (m & 4) s += 'y';
    if (m & 2) s += 'z';
    if (m & 1) s += 'w';
    return s;
}

static bool TranslateVS(XVertexShader *sh, std::string &out) {
    const DWORD *uc = sh->ucode.data();
    int n = (int)sh->ucode.size() / 4;
    std::string body;
    bool hasTrailer = false, usedV[16] = {}, usesA0 = false;
    std::vector<bool> trailer(n, false);
    bool savedClip = false;
    // pre-scan
    for (int k = 0; k < n; k++) {
        const DWORD *w = uc + k * 4;
        DWORD mac = Field(w, 1, 21, 4), ilu = Field(w, 1, 25, 3);
        DWORD macMask = Field(w, 3, 24, 4), outR = Field(w, 3, 20, 4), iluMask = Field(w, 3, 16, 4);
        (void)macMask; (void)outR; (void)iluMask;
        bool readsVp = false;
        for (int s = 0; s < 3; s++) {
            DWORD mux = s == 0 ? Field(w, 2, 26, 2) : s == 1 ? Field(w, 2, 11, 2) : Field(w, 3, 28, 2);
            if (mux == 2) usedV[Field(w, 1, 9, 4)] = true;
            DWORD cst = Field(w, 1, 13, 8);
            if (mux == 3 && (cst == 58 || cst == 59)) readsVp = true;
        }
        // XDK viewport trailer: oPos = r12 * c[-38] (+ rcc(r12.w) * ... + c[-37])
        if (readsVp && Field(w, 3, 12, 4) && Field(w, 3, 11, 1) && Field(w, 3, 3, 8) == 0) { trailer[k] = true; hasTrailer = true; }
        if (Field(w, 3, 1, 1) || mac == 13) usesA0 = true;
    }
    sh->screenSpace = !hasTrailer;
    char line[512];
    for (int k = 0; k < n; k++) {
        const DWORD *w = uc + k * 4;
        DWORD ilu = Field(w, 1, 25, 3), mac = Field(w, 1, 21, 4);
        DWORD macMask = Field(w, 3, 24, 4), outR = Field(w, 3, 20, 4), iluMask = Field(w, 3, 16, 4);
        DWORD oMask = Field(w, 3, 12, 4), orb = Field(w, 3, 11, 1), oAddr = Field(w, 3, 3, 8), oMux = Field(w, 3, 2, 1);
        bool a0x = Field(w, 3, 1, 1) != 0;
        std::string A = VSrc(w, 0, a0x), B = VSrc(w, 1, a0x), C = VSrc(w, 2, a0x);
        std::string macE, iluE;
        switch (mac) {
        case 0: break;
        case 1: macE = A; break;
        case 2: macE = A + "*" + B; break;
        case 3: macE = A + "+" + C; break;
        case 4: macE = A + "*" + B + "+" + C; break;
        case 5: macE = "dot(" + A + ".xyz," + B + ".xyz).xxxx"; break;
        case 6: macE = "(dot(" + A + ".xyz," + B + ".xyz)+" + B + ".w).xxxx"; break;
        case 7: macE = "dot(" + A + "," + B + ").xxxx"; break;
        case 8: macE = "float4(1," + A + ".y*" + B + ".y," + A + ".z," + B + ".w)"; break;
        case 9: macE = "min(" + A + "," + B + ")"; break;
        case 10: macE = "max(" + A + "," + B + ")"; break;
        case 11: macE = "float4(" + A + "<" + B + ")"; break;
        case 12: macE = "float4(" + A + ">=" + B + ")"; break;
        case 13: macE = A; break;   // ARL
        default: Log("vs: unknown MAC op %u", mac);
        }
        switch (ilu) {
        case 0: break;
        case 1: iluE = C; break;
        case 2: iluE = "rcp_(" + C + ".x).xxxx"; break;
        case 3: iluE = "rcc_(" + C + ".x).xxxx"; break;
        case 4: iluE = "rsq_(" + C + ".x).xxxx"; break;
        case 5: iluE = "expp_(" + C + ".x)"; break;
        case 6: iluE = "logp_(" + C + ".x)"; break;
        case 7: iluE = "lit_(" + C + ")"; break;
        }
        // the clip-space position is r12 just before the XDK viewport trailer runs (the trailer itself is
        // executed: shaders may read r12 afterwards to get screen coordinates, e.g. the cloak's refraction)
        if (trailer[k] && !savedClip) { body += "  clipPos = r12;\n"; savedClip = true; }
        // results are computed first, then written (inputs read the old values)
        if (!macE.empty()) { sprintf(line, "  m = %s;\n", macE.c_str()); body += line; }
        if (!iluE.empty()) { sprintf(line, "  l = %s;\n", iluE.c_str()); body += line; }
        if (mac == 13) body += "  a0 = (int)floor(m.x + 0.0001);\n";
        else if (mac && macMask) {
            std::string m = Mask(macMask);
            sprintf(line, "  r%u.%s = m.%s;\n", outR, m.c_str(), m.c_str());
            body += line;
        }
        if (ilu && iluMask) {
            std::string m = Mask(iluMask);
            DWORD dst = mac ? 1 : outR;   // paired with a MAC op, the ILU result always goes to R1
            sprintf(line, "  r%u.%s = l.%s;\n", dst, m.c_str(), m.c_str());
            body += line;
        }
        if (oMask) {
            std::string m = Mask(oMask);
            const char *srcv = oMux ? "l" : "m";
            if (orb) {
                const char *reg = nullptr;
                switch (oAddr) {
                case 0: reg = "oPos"; break;
                case 3: reg = "oD0"; break;
                case 4: reg = "oD1"; break;
                case 5: reg = "oFog"; break;
                case 6: reg = "oPts"; break;
                case 7: reg = "oB0"; break;
                case 8: reg = "oB1"; break;
                case 9: reg = "oT0"; break;
                case 10: reg = "oT1"; break;
                case 11: reg = "oT2"; break;
                case 12: reg = "oT3"; break;
                }
                if (oAddr == 0) {
                    // oPos aliases r12; the trailer's screen-space writes are dropped (we keep clip space)
                    sprintf(line, "  r12.%s = oPos.%s = %s.%s;\n", m.c_str(), m.c_str(), srcv, m.c_str());
                    body += line;
                } else if (reg) { sprintf(line, "  %s.%s = %s.%s;\n", reg, m.c_str(), srcv, m.c_str()); body += line; }
            } else {
                // write to constant memory: not representable in HLSL uniforms; keep a local shadow
                static int warned;
                if (warned++ < 5) Log("vs: constant write c[%u] ignored", oAddr);
            }
        }
        if (Field(w, 3, 0, 1)) break;   // FINAL
    }
    std::string s;
    s += "float4 c[192] : register(c0);\nfloat4 ex[8] : register(c192);\n";
    s += "float4 rcp_(float x){ return 1.0/x; }\n";
    s += "float4 rcc_(float x){ float r = 1.0/x; float a = abs(r); a = clamp(a, 5.42101e-20, 1.84467e19); return r < 0 ? -a : a; }\n";
    s += "float4 rsq_(float x){ return rsqrt(abs(x)); }\n";
    s += "float4 expp_(float x){ float f = floor(x); return float4(exp2(f), x - f, exp2(x), 1); }\n";
    s += "float4 logp_(float x){ float a = abs(x); if (a == 0) return float4(-1e30, 1, -1e30, 1); float e = floor(log2(a)); return float4(e, a / exp2(e), log2(a), 1); }\n";
    s += "float4 lit_(float4 v){ float4 r = float4(1,0,0,1); float p = clamp(v.w, -127.9961, 127.9961); if (v.x > 0) { r.y = v.x; if (v.y > 0) r.z = pow(v.y, p); } return r; }\n";
    s += "struct VIN {\n";
    for (int i = 0; i < 16; i++) {
        if (sh->regOffset[i] >= 0) { sprintf(line, "  float4 v%d : TEXCOORD%d;\n", i, i); s += line; }
    }
    s += "};\n";
    // inputs referenced but not declared read as (0,0,0,1)
    s += "struct VOUT { float4 pos : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;"
         " float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; float4 tf : TEXCOORD4; float4 wd : TEXCOORD5; float fog : FOG; float pts : PSIZE; };\n";
    s += "VOUT main(VIN i) {\n  VOUT o;\n";
    s += "  float4 m = 0, l = 0, clipPos = 0;\n  int a0 = 0;\n";
    for (int r = 0; r < 16; r++) { sprintf(line, "  float4 r%d = 0;\n", r); s += line; }
    s += "  float4 oPos = 0, oD0 = float4(0,0,0,1), oD1 = float4(0,0,0,0), oFog = 0, oPts = 0, oB0 = 0, oB1 = 0, oT0 = float4(0,0,0,1), oT1 = float4(0,0,0,1), oT2 = float4(0,0,0,1), oT3 = float4(0,0,0,1);\n";
    // any v register used but not declared: define a local
    std::string undeclared;
    for (int v = 0; v < 16; v++)
        if (usedV[v] && sh->regOffset[v] < 0) { sprintf(line, "#define i_v%d float4(0,0,0,1)\n", v); undeclared += line; }
    if (!undeclared.empty()) {
        // rewrite i.vN -> i_vN for undeclared inputs
        for (int v = 0; v < 16; v++) if (usedV[v] && sh->regOffset[v] < 0) {
            char from[16], to[16];
            sprintf(from, "i.v%d.", v); sprintf(to, "i_v%d.", v);
            size_t p;
            while ((p = body.find(from)) != std::string::npos) body.replace(p, strlen(from), to);
        }
        s = undeclared + s;
    }
    s += body;
    if (sh->screenSpace) {
        // oPos is in Xbox screen space (pixels, z in 0..2^24-1): undo the viewport mapping
        s += "  float4 p = oPos;\n";
        s += "  o.pos = float4((p.x - ex[5].x) / ex[4].x, (p.y - ex[5].y) / ex[4].y, p.z / 16777215.0, 1);\n";
    } else {
        s += "  o.pos = clipPos;\n";
    }
    // depth for the pixel shader (W-buffer emulation, see PSDepthWrapper): clip z and w, and w * ex[6].y
    s += sh->screenSpace ? "  o.wd = float4(o.pos.z, 1, o.pos.z, 0);\n" : "  o.wd = float4(o.pos.z, o.pos.w, o.pos.w * ex[6].y, 0);\n";
    const char *wh = getenv("GHOSTPC_WHITE");   // debug: force white diffuse (all shaders, or those with N instructions)
    bool white = wh && (atoi(wh) <= 1 || atoi(wh) == n);
    s += white ? "  o.d0 = 1; o.d1 = saturate(oD1);\n" : "  o.d0 = saturate(oD0); o.d1 = saturate(oD1);\n";
    s += "  o.t0 = oT0; o.t1 = oT1; o.t2 = oT2; o.t3 = oT3;\n";
    s += "  o.t0.xy *= ex[0].xy; o.t1.xy *= ex[1].xy; o.t2.xy *= ex[2].xy; o.t3.xy *= ex[3].xy;\n";
    // Xbox applies the fog table to the shader's fog coordinate (D3D9 table fog would use depth instead):
    // ex[7] = (mode, start, end, density); mode 0 = coordinate is the factor, 1 = exp, 2 = exp2, 3 = linear
    s += "  float fc = oFog.x, ff = fc;\n";
    s += "  if (ex[7].x == 3) ff = (ex[7].z - fc) / (ex[7].z - ex[7].y);\n";
    s += "  else if (ex[7].x == 1) ff = exp(-ex[7].w * fc);\n";
    s += "  else if (ex[7].x == 2) ff = exp(-(ex[7].w * fc) * (ex[7].w * fc));\n";
    s += "  o.fog = saturate(ff); o.tf = oFog.xxxx; o.pts = oPts.x;\n";
    s += "  return o;\n}\n";
    out = s;
    return true;
}

static IDirect3DVertexDeclaration9 *MakeDecl(XVertexShader *s, bool ff) {
    std::vector<D3DVERTEXELEMENT9> el;
    for (int r = 0; r < 16; r++) {
        if (s->regOffset[r] < 0) continue;
        D3DVERTEXELEMENT9 e = {0, (WORD)s->regOffset[r], D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, (BYTE)r};
        if (ff) {
            DWORD xt = 0;
            for (auto &x : s->elems) if (x.reg == r) xt = x.type;
            int comps = std::max(1, std::min(4, TypeComps(xt)));
            static const BYTE ft[] = {D3DDECLTYPE_FLOAT1, D3DDECLTYPE_FLOAT1, D3DDECLTYPE_FLOAT2, D3DDECLTYPE_FLOAT3, D3DDECLTYPE_FLOAT4};
            switch (r) {
            case 0: e.Usage = D3DDECLUSAGE_POSITION; e.UsageIndex = 0; e.Type = D3DDECLTYPE_FLOAT3; break;
            case 1: e.Usage = D3DDECLUSAGE_BLENDWEIGHT; e.UsageIndex = 0; e.Type = ft[comps]; break;
            case 2: e.Usage = D3DDECLUSAGE_NORMAL; e.UsageIndex = 0; e.Type = D3DDECLTYPE_FLOAT3; break;
            case 3: e.Usage = D3DDECLUSAGE_COLOR; e.UsageIndex = 0; e.Type = D3DDECLTYPE_D3DCOLOR; s->ffColorAsDword[r] = true; break;
            case 4: e.Usage = D3DDECLUSAGE_COLOR; e.UsageIndex = 1; e.Type = D3DDECLTYPE_D3DCOLOR; s->ffColorAsDword[r] = true; break;
            case 5: e.Usage = D3DDECLUSAGE_FOG; e.UsageIndex = 0; e.Type = D3DDECLTYPE_FLOAT1; break;
            case 6: e.Usage = D3DDECLUSAGE_PSIZE; e.UsageIndex = 0; e.Type = D3DDECLTYPE_FLOAT1; break;
            case 9: case 10: case 11: case 12:
                e.Usage = D3DDECLUSAGE_TEXCOORD; e.UsageIndex = (BYTE)(r - 9); e.Type = ft[comps]; break;
            default: continue;   // back colors etc. have no FF equivalent
            }
        }
        el.push_back(e);
    }
    el.push_back(D3DDECL_END());
    IDirect3DVertexDeclaration9 *d = nullptr;
    if (FAILED(g_dev->CreateVertexDeclaration(el.data(), &d))) Log("vs: CreateVertexDeclaration failed (ff=%d)", ff);
    return d;
}

bool VSPrepare(XVertexShader *s) {
    if (!s->ffdecl) {
        s->ffdecl = MakeDecl(s, true);
        s->decl = MakeDecl(s, false);
        for (auto &c : s->constmem)
            for (size_t k = 0; k < c.second.size() / 4; k++)
                if (c.first + k < 192) memcpy(&g_vsConst[(c.first + k) * 4], &c.second[k * 4], 16);
    }
    if (!s->hasFunction) return false;
    if (s->vs) return true;
    if (s->failed) return false;
    std::string src;
    TranslateVS(s, src);
    static int dumpN;
    if (getenv("GHOSTPC_DUMPVS")) {
        char path[MAX_PATH];
        _snprintf(path, MAX_PATH, "%s\\..\\test\\vs%03d.hlsl", g_gameDir, dumpN++);
        if (FILE *f = fopen(path, "w")) {
            fprintf(f, "// handle %p, %u instructions, decl:", (void *)s, (unsigned)s->ucode.size() / 4);
            for (auto &e : s->elems) fprintf(f, " s%d v%d t%02x @%d", e.stream, e.reg, e.type, e.offset);
            fprintf(f, "\n// ucode:");
            for (size_t i = 0; i < s->ucode.size(); i++) fprintf(f, "%s%08x", i % 4 ? " " : "\n// ", s->ucode[i]);
            fprintf(f, "\n%s", src.c_str());
            fclose(f);
        }
    }
    ID3DBlob *b = Compile(src, "vs_2_a", "xbox_vs");
    if (!b) { s->failed = true; return false; }
    if (FAILED(g_dev->CreateVertexShader((const DWORD *)b->GetBufferPointer(), &s->vs))) { s->failed = true; b->Release(); return false; }
    b->Release();
    static int count;
    LOGV("vs: compiled shader #%d (%u instructions, %s)", ++count, (unsigned)s->ucode.size() / 4, s->screenSpace ? "screen space" : "clip space");
    return true;
}

// ---------------------------------------------------------------------------
// Pixel shaders (register combiners)
XSTD(HRESULT, D3DDevice_CreatePixelShader, 8, (const XPixelShaderDef *def, DWORD *handle)) {
    auto p = new XPixelShaderObj();
    p->def = *def;
    p->pDef = &p->def;
    *handle = (DWORD)p;
    return 0;
}

static std::string Reg(DWORD r) {
    static const char *n[] = {"zero", "C0", "C1", "fogr", "v0", "v1", "zero", "zero", "t0", "t1", "t2", "t3", "r0", "r1", "v1r0", "efp"};
    return n[r & 0xF];
}
// one combiner input -> HLSL expression (float3 for rgb, float for alpha)
static std::string Input(DWORD b, bool alphaPortion, bool finalComb = false) {
    DWORD reg = b & 0xF;
    bool alphaChan = (b & 0x10) != 0;
    DWORD map = b & 0xE0;
    std::string v = Reg(reg);
    std::string x;
    if (alphaPortion) x = v + (alphaChan ? ".a" : ".b");
    else x = alphaChan ? v + ".aaa" : v + ".rgb";
    const char *t = alphaPortion ? "float" : "float3";
    switch (map) {
    case 0x00: return "saturate(" + x + ")";
    case 0x20: return "(1 - saturate(" + x + "))";
    case 0x40: return "(2 * saturate(" + x + ") - 1)";
    case 0x60: return "(1 - 2 * saturate(" + x + "))";
    case 0x80: return "(saturate(" + x + ") - 0.5)";
    case 0xA0: return "(0.5 - saturate(" + x + "))";
    case 0xC0: return x;
    case 0xE0: return "(-" + x + ")";
    }
    (void)t;
    return x;
}
static std::string OutMap(DWORD flags, const std::string &e) {
    switch (flags & 0x38) {
    case 0x08: return "(" + e + " - 0.5)";
    case 0x10: return "(" + e + " * 2)";
    case 0x18: return "((" + e + " - 0.5) * 2)";
    case 0x20: return "(" + e + " * 4)";
    case 0x30: return "(" + e + " * 0.5)";
    }
    return e;
}

struct PSKey {
    XPixelShaderDef def;
    DWORD texTypes;
    DWORD alphaKill;
    DWORD depth;            // writes depth (PSDepthWrapper)
    bool operator==(const PSKey &o) const { return !memcmp(this, &o, sizeof *this); }
};
struct PSKeyHash {
    size_t operator()(const PSKey &k) const {
        const uint32_t *p = (const uint32_t *)&k;
        size_t h = 2166136261u;
        for (size_t i = 0; i < sizeof k / 4; i++) h = (h ^ p[i]) * 16777619u;
        return h;
    }
};
struct PSEntry { IDirect3DPixelShader9 *ps; bool fog; int id; };
static std::unordered_map<PSKey, PSEntry, PSKeyHash> g_psCache;

static std::string GenPS(const XPixelShaderDef *d, DWORD texTypes, DWORD alphaKill, bool &usesFog) {
    char ln[1024];
    std::string s;
    s += "float4 C0s[8] : register(c0);\nfloat4 C1s[8] : register(c8);\nfloat4 FC[2] : register(c16);\n";
    s += "float4 texScale[4] : register(c18);\nfloat4 fogColor : register(c22);\nfloat4 bumpMat[4] : register(c24);\nfloat4 bumpLum[4] : register(c28);\n";
    DWORD modes = d->PSTextureModes;
    for (int i = 0; i < 4; i++) {
        DWORD tt = (texTypes >> (i * 8)) & 0xFF;
        DWORD m = (modes >> (i * 5)) & 0x1F;
        const char *st = tt == 2 ? "samplerCUBE" : tt == 3 ? "sampler3D" : "sampler2D";
        if (m == 3 || m == 0xE || m == 0xC || m == 0xB || m == 0x12) st = "samplerCUBE";
        if (m == 0xD) st = "sampler3D";
        sprintf(ln, "%s s%d : register(s%d);\n", st, i, i);
        s += ln;
    }
    s += "struct PIN { float4 d0 : COLOR0; float4 d1 : COLOR1; float4 tc0 : TEXCOORD0; float4 tc1 : TEXCOORD1; float4 tc2 : TEXCOORD2; float4 tc3 : TEXCOORD3; float4 tf : TEXCOORD4; };\n";
    s += "float3 dotmap(float3 v, int m) { return m == 0 ? v : (2 * v - 1); }\n";
    s += "float4 main(PIN i) : COLOR0 {\n";
    s += "  float4 zero = 0, v0 = saturate(i.d0), v1 = saturate(i.d1), r0 = 0, r1 = 0, C0 = 0, C1 = 0;\n";
    s += "  float4 fogr = float4(fogColor.rgb, 1);\n";
    s += "  float4 t0 = 0, t1 = 0, t2 = 0, t3 = 0;\n";
    s += "  float4 tc[4] = { i.tc0, i.tc1, i.tc2, i.tc3 };\n";
    s += "  float dp1 = 0, dp2 = 0;\n";
    // texture address stage
    for (int st = 0; st < 4; st++) {
        DWORD m = (modes >> (st * 5)) & 0x1F;
        DWORD tt = (texTypes >> (st * 8)) & 0xFF;
        int in = st == 1 ? 0 : st == 2 ? ((d->PSInputTexture >> 16) & 1) : st == 3 ? ((d->PSInputTexture >> 20) & 3) : 0;
        int dm = st >= 1 ? ((d->PSDotMapping >> ((st - 1) * 4)) & 7) : 0;
        char T[8], TC[16], S[8], INP[8];
        sprintf(T, "t%d", st); sprintf(TC, "tc[%d]", st); sprintf(S, "s%d", st); sprintf(INP, "t%d", in);
        std::string tcs = std::string("float4(") + TC + ".xy * texScale[" + std::to_string(st) + "].xy, " + TC + ".zw)";
        switch (m) {
        case 0: break;
        case 1:   // PROJECT2D
            if (tt == 2) sprintf(ln, "  %s = texCUBE(%s, %s.xyz);\n", T, S, TC);
            else if (tt == 3) sprintf(ln, "  %s = tex3D(%s, %s.xyz);\n", T, S, TC);
            else sprintf(ln, "  %s = tex2Dproj(%s, %s);\n", T, S, tcs.c_str());
            s += ln; break;
        case 2:   // PROJECT3D (also shadow-buffer lookups on depth textures)
            if (tt == 4 || tt == 1) sprintf(ln, "  %s = tex2Dproj(%s, %s);\n", T, S, tcs.c_str());
            else if (tt == 2) sprintf(ln, "  %s = texCUBE(%s, %s.xyz);\n", T, S, TC);
            else sprintf(ln, "  %s = tex3D(%s, %s.xyz / %s.w);\n", T, S, TC, TC);
            s += ln; break;
        case 3: sprintf(ln, "  %s = texCUBE(%s, %s.xyz);\n", T, S, TC); s += ln; break;
        case 4: sprintf(ln, "  %s = saturate(%s);\n", T, TC); s += ln; break;
        case 5: {   // CLIPPLANE
            DWORD cm = (d->PSCompareMode >> (st * 4)) & 0xF;
            for (int c = 0; c < 4; c++) {
                sprintf(ln, "  clip(%s%s.%c);\n", (cm >> c) & 1 ? "" : "-", TC, "xyzw"[c]);
                if ((cm >> c) & 1) sprintf(ln, "  clip(%s.%c);\n", TC, "xyzw"[c]);
                else sprintf(ln, "  clip(-%s.%c - 1e-7);\n", TC, "xyzw"[c]);
                s += ln;
            }
            break;
        }
        case 6: case 7: {   // BUMPENVMAP(_LUM)
            sprintf(ln, "  { float2 du = %s.rg; float2 uv = %s.xy * texScale[%d].xy + float2(dot(du, bumpMat[%d].xz), dot(du, bumpMat[%d].yw));\n"
                        "    %s = tex2D(%s, uv);\n", INP, TC, st, st, st, T, S);
            s += ln;
            if (m == 7) { sprintf(ln, "    %s.rgb *= saturate(%s.b * bumpLum[%d].x + bumpLum[%d].y);\n", T, INP, st, st); s += ln; }
            s += "  }\n";
            break;
        }
        case 0x11:   // DOTPRODUCT
            sprintf(ln, "  dp%d = dot(%s.xyz, dotmap(%s.rgb, %d));\n", st == 1 ? 1 : 2, TC, INP, dm);
            s += ln; break;
        case 0x9:    // DOT_ST
            sprintf(ln, "  %s = tex2D(%s, float2(dp1, dot(%s.xyz, dotmap(%s.rgb, %d))));\n", T, S, TC, INP, dm); s += ln; break;
        case 0xA:    // DOT_ZW
            sprintf(ln, "  %s = 0;\n", T); s += ln; break;
        case 0xB:    // DOT_RFLCT_DIFF (stage 2): normal lookup
            sprintf(ln, "  dp2 = dot(%s.xyz, dotmap(%s.rgb, %d));\n", TC, INP, dm); s += ln;
            break;
        case 0xC: case 0x12: {   // DOT_RFLCT_SPEC(_CONST) (stage 3)
            sprintf(ln, "  { float3 N = float3(dp1, dp2, dot(%s.xyz, dotmap(%s.rgb, %d)));\n", TC, INP, dm); s += ln;
            if (m == 0xC) s += "    float3 E = float3(i.tc1.w, i.tc2.w, i.tc3.w);\n";
            else s += "    float3 E = C0s[0].xyz;\n";
            sprintf(ln, "    float3 R = 2 * N * dot(N, E) / max(dot(N, N), 1e-8) - E;\n    %s = texCUBE(%s, R); }\n", T, S);
            s += ln; break;
        }
        case 0xD: case 0xE:      // DOT_STR_3D / DOT_STR_CUBE
            sprintf(ln, "  %s = %s(%s, float3(dp1, dp2, dot(%s.xyz, dotmap(%s.rgb, %d))));\n", T, m == 0xD ? "tex3D" : "texCUBE", S, TC, INP, dm);
            s += ln; break;
        case 0xF:    // DPNDNT_AR
            sprintf(ln, "  %s = tex2D(%s, %s.ar);\n", T, S, INP); s += ln; break;
        case 0x10:   // DPNDNT_GB
            sprintf(ln, "  %s = tex2D(%s, %s.gb);\n", T, S, INP); s += ln; break;
        default: {
            static int warned;
            if (warned++ < 10) Log("ps: texture mode %x on stage %d not supported", m, st);
        }
        }
        if ((alphaKill >> st) & 1) { sprintf(ln, "  clip(%s.a - 0.5/255);\n", T); s += ln; }
    }
    s += "  r0.a = t0.a;\n";
    // combiners
    DWORD cc = d->PSCombinerCount;
    int count = std::max(1, std::min(8, (int)(cc & 0xFF)));
    bool muxMsb = (cc & 0x100) != 0;
    // NV097_SET_COMBINER_CONTROL: bit 12 / bit 16 = C0 / C1 per stage (else stage 0's for all)
    bool uniqueC0 = (cc & 0x1000) != 0, uniqueC1 = (cc & 0x10000) != 0;
    for (int i = 0; i < count; i++) {
        sprintf(ln, "  C0 = C0s[%d]; C1 = C1s[%d];\n", uniqueC0 ? i : 0, uniqueC1 ? i : 0);
        s += ln;
        DWORD ri = d->PSRGBInputs[i], ai = d->PSAlphaInputs[i], ro = d->PSRGBOutputs[i], ao = d->PSAlphaOutputs[i];
        s += "  {\n";
        // RGB portion
        {
            std::string A = Input(ri >> 24, false), B = Input(ri >> 16, false), C = Input(ri >> 8, false), D = Input(ri, false);
            DWORD fl = ro >> 12;
            std::string ab = (fl & 2) ? "dot(" + A + "," + B + ").xxx" : "(" + A + "*" + B + ")";
            std::string cd = (fl & 1) ? "dot(" + C + "," + D + ").xxx" : "(" + C + "*" + D + ")";
            sprintf(ln, "    float3 rab = %s, rcd = %s;\n", ab.c_str(), cd.c_str()); s += ln;
            if (fl & 4) sprintf(ln, "    float3 rsum = (r0.a %s) ? rcd : rab;\n", muxMsb ? ">= 0.5" : "> 0.5 / 255");
            else sprintf(ln, "    float3 rsum = rab + rcd;\n");
            s += ln;
            sprintf(ln, "    rab = clamp(%s, -1, 1); rcd = clamp(%s, -1, 1); rsum = clamp(%s, -1, 1);\n",
                    OutMap(fl, "rab").c_str(), OutMap(fl, "rcd").c_str(), OutMap(fl, "rsum").c_str());
            s += ln;
        }
        // Alpha portion
        {
            std::string A = Input(ai >> 24, true), B = Input(ai >> 16, true), C = Input(ai >> 8, true), D = Input(ai, true);
            DWORD fl = ao >> 12;
            sprintf(ln, "    float aab = %s*%s, acd = %s*%s;\n", A.c_str(), B.c_str(), C.c_str(), D.c_str()); s += ln;
            if (fl & 4) sprintf(ln, "    float asum = (r0.a %s) ? acd : aab;\n", muxMsb ? ">= 0.5" : "> 0.5 / 255");
            else sprintf(ln, "    float asum = aab + acd;\n");
            s += ln;
            sprintf(ln, "    aab = clamp(%s, -1, 1); acd = clamp(%s, -1, 1); asum = clamp(%s, -1, 1);\n",
                    OutMap(fl, "aab").c_str(), OutMap(fl, "acd").c_str(), OutMap(fl, "asum").c_str());
            s += ln;
        }
        // writes (after both portions are computed)
        auto wr = [&](DWORD reg, const char *chan, const char *val) {
            if ((reg & 0xF) == 0 || (reg & 0xF) == 1 || (reg & 0xF) == 2 || (reg & 0xF) == 3) return;
            sprintf(ln, "    %s.%s = %s;\n", Reg(reg).c_str(), chan, val);
            s += ln;
        };
        DWORD rfl = ro >> 12;
        wr(ro & 0xF, "rgb", "rcd");
        wr((ro >> 4) & 0xF, "rgb", "rab");
        wr((ro >> 8) & 0xF, "rgb", "rsum");
        wr(ao & 0xF, "a", "acd");
        wr((ao >> 4) & 0xF, "a", "aab");
        wr((ao >> 8) & 0xF, "a", "asum");
        if (rfl & 0x80) wr((ro >> 4) & 0xF, "a", "rab.b");   // AB_BLUE_TO_ALPHA
        if (rfl & 0x40) wr(ro & 0xF, "a", "rcd.b");          // CD_BLUE_TO_ALPHA
        s += "  }\n";
    }
    // final combiner
    DWORD fa = d->PSFinalCombinerInputsABCD, fe = d->PSFinalCombinerInputsEFG;
    usesFog = false;
    if (fa || fe) {
        DWORD fl = fe & 0xFF;
        s += "  C0 = FC[0]; C1 = FC[1];\n";
        sprintf(ln, "  float4 v1r0 = float4(%s + %s, 0);\n", (fl & 0x40) ? "(1 - v1.rgb)" : "v1.rgb", (fl & 0x20) ? "(1 - r0.rgb)" : "r0.rgb");
        s += ln;
        if (fl & 0x80) s += "  v1r0 = saturate(v1r0);\n";
        std::string E = Input(fe >> 24, false), F = Input(fe >> 16, false);
        sprintf(ln, "  float4 efp = float4(%s * %s, 0);\n", E.c_str(), F.c_str()); s += ln;
        DWORD a = (fa >> 24) & 0xFF, c = (fa >> 8) & 0xFF;
        std::string A = Input(fa >> 24, false), B = Input(fa >> 16, false), C = Input(fa >> 8, false), D = Input(fa, false);
        std::string G = Input(fe >> 8, true);
        if ((a & 0xF) == 3 && (a & 0x10) && (c & 0xF) == 3) {
            // standard fog blend: lerp(fog.rgb, B, fog.a) + D -> let the D3D9 fog unit do it
            usesFog = true;
            sprintf(ln, "  float4 o = float4(%s + %s, %s);\n", B.c_str(), D.c_str(), G.c_str());
        } else {
            sprintf(ln, "  float4 o = float4(%s * %s + (1 - %s) * %s + %s, %s);\n", A.c_str(), B.c_str(), A.c_str(), C.c_str(), D.c_str(), G.c_str());
        }
        s += ln;
        s += "  return o;\n}\n";
    } else {
        s += "  return r0;\n}\n";
    }
    return s;
}

// W-buffer emulation. The Xbox renders 3D with D3DZB_USEW: depth is the linear clip w, with even precision at all
// distances, and level geometry relies on it (e.g. flesh layers lying just above rock walls). D3D9 hardware has no
// W-buffer, so the vertex shaders pass (clip z, clip w, w / far) in TEXCOORD5 and the pixel shader writes the depth:
// c23 = (1: W-buffer / 0: z/w, depth bias, 0, 0). Only for draws through our vertex shaders (g_psWriteDepth).
bool g_psWriteDepth;
static void PSDepthWrapper(std::string &s) {
    const char *head = "float4 main(PIN i) : COLOR0 {";
    size_t p = s.find(head);
    if (p == std::string::npos) return;
    s.replace(p, strlen(head), "float4 main_c(PIN i) {");
    s = "float4 depthMode : register(c23);\n" + s;
    s += "struct POUT { float4 c : COLOR0; float d : DEPTH; };\n"
         "POUT main(PIN i, float4 wd : TEXCOORD5) {\n  POUT o;\n  o.c = main_c(i);\n"
         "  o.d = saturate((depthMode.x > 0 ? wd.z : wd.x / wd.y) + depthMode.y);\n  return o;\n}\n";
}

int g_psDumpId;
IDirect3DPixelShader9 *PSGet(const XPixelShaderDef *def, const DWORD *texTypes, bool *usesFog) {
    PSKey k;
    memset(&k, 0, sizeof k);
    k.def = *def;
    k.def.PSConstant0[0] = 0;   // constants don't affect the code
    memset(k.def.PSConstant0, 0, sizeof k.def.PSConstant0);
    memset(k.def.PSConstant1, 0, sizeof k.def.PSConstant1);
    k.def.PSFinalCombinerConstant0 = k.def.PSFinalCombinerConstant1 = 0;
    for (int i = 0; i < 4; i++) k.texTypes |= (texTypes[i] & 0xFF) << (i * 8);
    for (int i = 0; i < 4; i++) if (g_ts[i * 32 + XTSS_ALPHAKILL]) k.alphaKill |= 1 << i;
    k.depth = g_psWriteDepth;
    auto it = g_psCache.find(k);
    if (it != g_psCache.end()) { *usesFog = it->second.fog; g_psDumpId = it->second.id; return it->second.ps; }
    bool fog = false;
    std::string src = GenPS(def, k.texTypes, k.alphaKill, fog);
    if (k.depth) PSDepthWrapper(src);
    PSEntry e = {nullptr, fog, (int)g_psCache.size()};
    g_psDumpId = e.id;
    if (getenv("GHOSTPC_DUMPVS")) {
        char path[MAX_PATH];
        _snprintf(path, MAX_PATH, "%s\\..\\test\\ps%03d.hlsl", g_gameDir, e.id);
        if (FILE *f = fopen(path, "w")) {
            const DWORD *d = (const DWORD *)def;
            fprintf(f, "// def:");
            for (int i = 0; i < (int)(sizeof(XPixelShaderDef) / 4); i++) fprintf(f, " %08x", d[i]);
            fprintf(f, "\n%s", src.c_str());
            fclose(f);
        }
    }
    if (ID3DBlob *b = Compile(src, "ps_2_b", "xbox_ps")) {
        g_dev->CreatePixelShader((const DWORD *)b->GetBufferPointer(), &e.ps);
        b->Release();
    } else if (ID3DBlob *b3 = Compile(src, "ps_3_0", "xbox_ps3")) {
        g_dev->CreatePixelShader((const DWORD *)b3->GetBufferPointer(), &e.ps);
        b3->Release();
    }
    g_psCache[k] = e;
    LOGV("ps: compiled combiner shader #%u (%u stages)", (unsigned)g_psCache.size(), def->PSCombinerCount & 0xFF);
    *usesFog = fog;
    return e.ps;
}

// ---------------------------------------------------------------------------
// Fixed-function texture stages -> HLSL with Xbox semantics (CURRENT starts as diffuse,
// linear textures take texel coordinates, TEMP register, Xbox op numbering).
struct FFKey {
    DWORD st[4][10];   // colorop, carg0..2, alphaop, aarg0..2, resultarg, flags(texType | projected<<8 | alphakill<<9)
    DWORD specular;
    DWORD depth;
    bool operator==(const FFKey &o) const { return !memcmp(this, &o, sizeof *this); }
};
struct FFKeyHash {
    size_t operator()(const FFKey &k) const {
        const uint32_t *p = (const uint32_t *)&k;
        size_t h = 2166136261u;
        for (size_t i = 0; i < sizeof k / 4; i++) h = (h ^ p[i]) * 16777619u;
        return h;
    }
};
static std::unordered_map<FFKey, IDirect3DPixelShader9 *, FFKeyHash> g_ffCache;

static std::string FFArg(DWORD a, bool alpha, int stage) {
    std::string r;
    switch (a & 0xF) {
    case 0: r = "dif"; break;
    case 1: r = "cur"; break;
    case 2: r = "t" + std::to_string(stage); break;
    case 3: r = "tf"; break;
    case 4: r = "spec"; break;
    case 5: r = "tmp"; break;
    default: r = "cur";
    }
    if (a & 0x20) r = r + ".aaaa";          // ALPHAREPLICATE
    if (a & 0x10) r = "(1 - " + r + ")";    // COMPLEMENT
    return "(" + r + (alpha ? ").a" : ").rgb");
}
static std::string FFOp(DWORD op, const std::string &a0, const std::string &a1, const std::string &a2, bool alpha,
                        int stage) {
    std::string T = "t" + std::to_string(stage);
    const char *ch = alpha ? ".a" : ".rgb";
    switch (op) {
    case 2: return a1;
    case 3: return a2;
    case 4: return a1 + "*" + a2;
    case 5: return "2*" + a1 + "*" + a2;
    case 6: return "4*" + a1 + "*" + a2;
    case 7: return a1 + "+" + a2;
    case 8: return a1 + "+" + a2 + "-0.5";
    case 9: return "2*(" + a1 + "+" + a2 + "-0.5)";
    case 10: return a1 + "-" + a2;
    case 11: return a1 + "+" + a2 + "-" + a1 + "*" + a2;
    case 12: return "lerp(" + a2 + "," + a1 + ",dif.a)";
    case 13: return "lerp(" + a2 + "," + a1 + ",cur.a)";
    case 14: return "lerp(" + a2 + "," + a1 + "," + T + ".a)";
    case 15: return "lerp(" + a2 + "," + a1 + ",tf.a)";
    case 16: return a1 + "+" + a2 + "*(1-" + T + ".a)";
    case 17: return a1;
    case 18: return alpha ? a1 : a1 + "+(" + a1.substr(0, a1.size() - 4) + ").a*" + a2;
    case 19: return alpha ? a1 : a1 + "*" + a2;
    case 20: return alpha ? a1 : a1 + "+(1-(" + a1.substr(0, a1.size() - 4) + ").a)*" + a2;
    case 21: return alpha ? a1 : "(1-" + a1 + ")*" + a2;
    case 22: return std::string("(4*dot(") + (alpha ? "float3(0,0,0)" : a1 + "-0.5") + "," + (alpha ? "float3(0,0,0)" : a2 + "-0.5") + "))" + (alpha ? "" : ".xxx");
    case 23: return a0 + "+" + a1 + "*" + a2;
    case 24: return "lerp(" + a2 + "," + a1 + "," + a0 + ")";
    case 25: case 26: return alpha ? a1 : "cur.rgb";   // bump: handled in the texture lookup of the next stage
    }
    (void)ch;
    return a1;
}

IDirect3DPixelShader9 *PSGetFF(const DWORD *ts, const DWORD *texTypes, bool projDefault, bool ffVertex, DWORD specularEnable) {
    FFKey k;
    memset(&k, 0, sizeof k);
    int nst = 0;
    for (int s = 0; s < 4; s++) {
        const DWORD *x = ts + s * 32;
        if (x[XTSS_COLOROP] <= 1) break;
        k.st[s][0] = x[XTSS_COLOROP]; k.st[s][1] = x[XTSS_COLORARG0]; k.st[s][2] = x[XTSS_COLORARG1]; k.st[s][3] = x[XTSS_COLORARG2];
        k.st[s][4] = x[XTSS_ALPHAOP]; k.st[s][5] = x[XTSS_ALPHAARG0]; k.st[s][6] = x[XTSS_ALPHAARG1]; k.st[s][7] = x[XTSS_ALPHAARG2];
        k.st[s][8] = x[XTSS_RESULTARG] == 5 ? 5 : 1;
        bool proj = ffVertex ? (x[XTSS_TEXTURETRANSFORMFLAGS] & D3DTTFF_PROJECTED) != 0 : projDefault;
        k.st[s][9] = (texTypes[s] & 0xFF) | (proj ? 0x100 : 0) | (x[XTSS_ALPHAKILL] ? 0x200 : 0);
        nst = s + 1;
    }
    k.specular = specularEnable;
    k.depth = g_psWriteDepth;
    auto it = g_ffCache.find(k);
    if (it != g_ffCache.end()) return it->second;
    char ln[512];
    std::string s;
    s += "float4 tfC : register(c16);\nfloat4 texScale[4] : register(c18);\nfloat4 bumpMat[4] : register(c24);\nfloat4 bumpLum[4] : register(c28);\n";
    for (int i = 0; i < 4; i++) {
        DWORD tt = k.st[i][9] & 0xFF;
        sprintf(ln, "%s s%d : register(s%d);\n", tt == 2 ? "samplerCUBE" : tt == 3 ? "sampler3D" : "sampler2D", i, i);
        s += ln;
    }
    s += "struct PIN { float4 d0 : COLOR0; float4 d1 : COLOR1; float4 tc0 : TEXCOORD0; float4 tc1 : TEXCOORD1; float4 tc2 : TEXCOORD2; float4 tc3 : TEXCOORD3; };\n";
    s += "float4 main(PIN i) : COLOR0 {\n";
    s += "  float4 dif = saturate(i.d0), spec = saturate(i.d1), cur = dif, tmp = 0, tf = tfC, zero_ = 0;\n";
    s += "  float4 tc[4] = { i.tc0, i.tc1, i.tc2, i.tc3 };\n  float4 t0 = 0, t1 = 0, t2 = 0, t3 = 0;\n";
    for (int st = 0; st < nst; st++) {
        DWORD fl = k.st[st][9], tt = fl & 0xFF;
        bool proj = (fl & 0x100) != 0;
        bool prevBump = st > 0 && (k.st[st - 1][0] == 25 || k.st[st - 1][0] == 26);
        if (tt) {
            std::string tcs = "tc[" + std::to_string(st) + "]";
            std::string uv = "(" + tcs + ".xy * texScale[" + std::to_string(st) + "].xy)";
            if (prevBump) {
                int p = st - 1;
                sprintf(ln, "  { float2 du = t%d.rg; float2 uv = %s / %s + float2(dot(du, bumpMat[%d].xz), dot(du, bumpMat[%d].yw));\n    t%d = tex2D(s%d, uv);\n",
                        p, uv.c_str(), proj ? (tcs + ".w").c_str() : "1", p, p, st, st);
                s += ln;
                if (k.st[p][0] == 26) { sprintf(ln, "    t%d.rgb *= saturate(t%d.b * bumpLum[%d].x + bumpLum[%d].y);\n", st, p, p, p); s += ln; }
                s += "  }\n";
            } else if (tt == 2) sprintf(ln, "  t%d = texCUBE(s%d, %s.xyz);\n", st, st, tcs.c_str()), s += ln;
            else if (tt == 3) sprintf(ln, "  t%d = tex3D(s%d, %s.xyz);\n", st, st, tcs.c_str()), s += ln;
            else if (proj || tt == 4) sprintf(ln, "  t%d = tex2Dproj(s%d, float4(%s, %s.zw));\n", st, st, uv.c_str(), tcs.c_str()), s += ln;
            else sprintf(ln, "  t%d = tex2D(s%d, %s);\n", st, st, uv.c_str()), s += ln;
            if (fl & 0x200) { sprintf(ln, "  clip(t%d.a - 0.5/255);\n", st); s += ln; }
        }
        DWORD cop = k.st[st][0], aop = k.st[st][4];
        std::string c = FFOp(cop, FFArg(k.st[st][1], false, st), FFArg(k.st[st][2], false, st), FFArg(k.st[st][3], false, st), false, st);
        std::string a = aop <= 1 ? "cur.a" : FFOp(aop, FFArg(k.st[st][5], true, st), FFArg(k.st[st][6], true, st), FFArg(k.st[st][7], true, st), true, st);
        const char *dst = k.st[st][8] == 5 ? (getenv("GHOSTPC_NOENV") ? "zero_" : "tmp") : "cur";
        sprintf(ln, "  { float3 rc = saturate(%s); float ra = saturate(%s); %s = float4(rc, ra); }\n", c.c_str(), a.c_str(), dst);
        s += ln;
    }
    if (k.specular) s += "  cur.rgb = saturate(cur.rgb + spec.rgb);\n";
    s += "  return cur;\n}\n";
    if (k.depth) PSDepthWrapper(s);
    IDirect3DPixelShader9 *ps = nullptr;
    if (ID3DBlob *b = Compile(s, "ps_2_b", "xbox_ffps")) {
        g_dev->CreatePixelShader((const DWORD *)b->GetBufferPointer(), &ps);
        b->Release();
    } else if (ID3DBlob *b3 = Compile(s, "ps_3_0", "xbox_ffps3")) {
        g_dev->CreatePixelShader((const DWORD *)b3->GetBufferPointer(), &ps);
        b3->Release();
    }
    g_ffCache[k] = ps;
    if (getenv("GHOSTPC_DUMPVS")) {
        char path[MAX_PATH];
        _snprintf(path, MAX_PATH, "%s\\..\\test\\ff%03d.hlsl", g_gameDir, (int)g_ffCache.size());
        if (FILE *f = fopen(path, "w")) { fputs(s.c_str(), f); fclose(f); }
    }
    return ps;
}

// ---------------------------------------------------------------------------
// Fixed-function vertex processing as a vertex shader (see FFVSKey in d3d_int.h)

// size in floats of FVF texture coordinate set i (D3DFVF_TEXCOORDSIZEn: 0 = 2, 1 = 3, 2 = 4, 3 = 1)
static int FvfTexSize(DWORD fvf, int i) {
    static const int sz[4] = {2, 3, 4, 1};
    return sz[(fvf >> (16 + i * 2)) & 3];
}
IDirect3DVertexDeclaration9 *FFVSDecl(DWORD fvf) {
    static std::unordered_map<DWORD, IDirect3DVertexDeclaration9 *> cache;
    auto it = cache.find(fvf);
    if (it != cache.end()) return it->second;
    std::vector<D3DVERTEXELEMENT9> e;
    WORD off = 0;
    auto add = [&](BYTE type, BYTE usage, BYTE index, WORD size) {
        e.push_back({0, off, type, D3DDECLMETHOD_DEFAULT, usage, index});
        off += size;
    };
    add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 12);   // callers only use XYZ positions
    if (fvf & D3DFVF_NORMAL) add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0, 12);
    if (fvf & D3DFVF_PSIZE) off += 4;
    if (fvf & D3DFVF_DIFFUSE) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0, 4);
    if (fvf & D3DFVF_SPECULAR) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1, 4);
    int ntex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    static const BYTE types[5] = {0, D3DDECLTYPE_FLOAT1, D3DDECLTYPE_FLOAT2, D3DDECLTYPE_FLOAT3, D3DDECLTYPE_FLOAT4};
    for (int i = 0; i < ntex; i++) {
        int n = FvfTexSize(fvf, i);
        add(types[n], D3DDECLUSAGE_TEXCOORD, (BYTE)i, (WORD)(n * 4));
    }
    e.push_back(D3DDECL_END());
    IDirect3DVertexDeclaration9 *d = nullptr;
    if (FAILED(g_dev->CreateVertexDeclaration(e.data(), &d))) d = nullptr;
    cache[fvf] = d;
    return d;
}

IDirect3DVertexShader9 *FFVSGet(const FFVSKey &k) {
    static std::map<FFVSKey, IDirect3DVertexShader9 *> cache;
    auto it = cache.find(k);
    if (it != cache.end()) return it->second;
    bool hasN = (k.fvf & D3DFVF_NORMAL) != 0, hasD = (k.fvf & D3DFVF_DIFFUSE) != 0, hasS = (k.fvf & D3DFVF_SPECULAR) != 0;
    int ntex = (k.fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    char ln[512];
    std::string s = "float4 c[79] : register(c0);\nstruct VIN {\n  float4 pos : POSITION;\n";
    if (hasN) s += "  float3 nrm : NORMAL;\n";
    if (hasD) s += "  float4 d0 : COLOR0;\n";
    if (hasS) s += "  float4 d1 : COLOR1;\n";
    for (int i = 0; i < ntex; i++) { sprintf(ln, "  float4 t%d : TEXCOORD%d;\n", i, i); s += ln; }
    s += "};\n"
         "struct VOUT { float4 pos : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;"
         " float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; float4 wd : TEXCOORD5; float fog : FOG; };\n"
         "VOUT main(VIN i) {\n  VOUT o;\n";
    // the same expression as the translated game shaders (dot(v0, c[96..99])), so both give identical positions
    s += "  float4 p = i.pos;\n";
    s += "  o.pos = float4(dot(p, c[0]), dot(p, c[1]), dot(p, c[2]), dot(p, c[3]));\n";
    s += "  float3 vp = float3(dot(p, c[4]), dot(p, c[5]), dot(p, c[6]));\n";
    if (hasN) {
        s += "  float3 vn = float3(dot(i.nrm, c[4].xyz), dot(i.nrm, c[5].xyz), dot(i.nrm, c[6].xyz));\n";
        if (k.normalize) s += "  vn = normalize(vn);\n";
    } else s += "  float3 vn = 0;\n";
    if (!k.lighting) {
        s += hasD ? "  o.d0 = i.d0;\n" : "  o.d0 = 1;\n";
        s += hasS ? "  o.d1 = i.d1;\n" : "  o.d1 = 0;\n";
    } else {
        // D3D9 fixed-function lighting, diffuse part (specular-enabled draws are not emulated), in camera space
        auto src = [&](BYTE sel, int matReg) -> std::string {
            if (k.colorVertex && sel == 1 && hasD) return "i.d0";
            if (k.colorVertex && sel == 2 && hasS) return "i.d1";
            return "c[" + std::to_string(matReg) + "]";
        };
        std::string md = src(k.srcDiffuse, 27), ma = src(k.srcAmbient, 28), me = src(k.srcEmissive, 29);
        s += "  float4 md = " + md + ", ma = " + ma + ", me = " + me + ";\n";
        s += "  float3 lit = me.rgb + c[26].rgb * ma.rgb;\n";
        for (int li = 0; li < 8; li++) {
            int t = k.lightType[li];
            if (!t) continue;
            int b = 30 + li * 6;
            s += "  {\n";
            if (t == D3DLIGHT_DIRECTIONAL) {
                sprintf(ln, "    float3 L = -normalize(c[%d].xyz); float att = 1;\n", b + 1); s += ln;
            } else {
                sprintf(ln, "    float3 d = c[%d].xyz - vp; float dist = length(d); float3 L = d / max(dist, 1e-20);\n"
                            "    float att = dist > c[%d].x ? 0 : 1 / max(c[%d].y + c[%d].z * dist + c[%d].w * dist * dist, 1e-20);\n",
                        b, b + 4, b + 4, b + 4, b + 4);
                s += ln;
                if (t == D3DLIGHT_SPOT) {
                    sprintf(ln, "    float rho = dot(-L, normalize(c[%d].xyz));\n"
                                "    float spot = rho > c[%d].x ? 1 : rho <= c[%d].y ? 0 : pow(saturate((rho - c[%d].y) / (c[%d].x - c[%d].y)), c[%d].z);\n"
                                "    att *= spot;\n",
                            b + 1, b + 5, b + 5, b + 5, b + 5, b + 5, b + 5);
                    s += ln;
                }
            }
            sprintf(ln, "    lit += att * (c[%d].rgb * ma.rgb + c[%d].rgb * md.rgb * max(dot(vn, L), 0));\n  }\n", b + 3, b + 2);
            s += ln;
        }
        s += "  o.d0 = float4(saturate(lit), md.a);\n  o.d1 = 0;\n";
    }
    for (int st = 0; st < 4; st++) {
        DWORD idx = k.tci[st] & 0xFFFF, gen = k.tci[st] >> 16, ttf = k.ttf[st];
        int n = ttf & 0xFF;
        std::string pad, raw;   // input padded for a texture matrix / passed through untransformed
        if (gen == 1) { pad = raw = "float4(vn, 1)"; }
        else if (gen == 2) { pad = raw = "float4(vp, 1)"; }
        else if (gen == 3) {
            static bool noReflect = getenv("GHOSTPC_NOREFLECT") != nullptr;   // debug: a fixed direction instead
            // without vertex normals vn stays 0 (normalizing it would constant-fold to NaN and fail to compile)
            pad = raw = noReflect ? "float4(0, 0, 1, 1)" : hasN ? "float4(reflect(normalize(vp), normalize(vn)), 1)"
                                                                : "float4(normalize(vp), 1)";
        } else if ((int)idx < ntex) {
            int sz = FvfTexSize(k.fvf, idx);
            sprintf(ln, "i.t%u", idx);
            std::string t = ln;
            if (sz == 1) { pad = "float4(" + t + ".x, 1, 0, 0)"; raw = "float4(" + t + ".x, 0, 0, 1)"; }
            else if (sz == 2) { pad = "float4(" + t + ".xy, 1, 0)"; raw = "float4(" + t + ".xy, 0, 1)"; }
            else if (sz == 3) { pad = raw = "float4(" + t + ".xyz, 1)"; }
            else pad = raw = t;
        } else pad = raw = "float4(0, 0, 0, 1)";
        if (n) {
            int b = 8 + st * 4;
            sprintf(ln, "  { float4 a = %s; float4 r = float4(dot(a, c[%d]), dot(a, c[%d]), dot(a, c[%d]), dot(a, c[%d]));\n",
                    pad.c_str(), b, b + 1, b + 2, b + 3);
            s += ln;
            if (ttf & D3DTTFF_PROJECTED) {
                static const char comp[4] = {'x', 'y', 'z', 'w'};
                sprintf(ln, "    r.w = r.%c;\n", comp[n - 1]); s += ln;
            } else if (n < 4) {
                static const char *keep[4] = {"", "r.x, 0, 0, 1", "r.xy, 0, 1", "r.xyz, 1"};
                sprintf(ln, "    r = float4(%s);\n", keep[n]); s += ln;
            }
            sprintf(ln, "    o.t%d = r; }\n", st); s += ln;
        } else {
            sprintf(ln, "  o.t%d = %s;\n", st, raw.c_str()); s += ln;
        }
    }
    s += k.rangeFog ? "  float fc = length(vp);\n" : "  float fc = dot(p, c[25]);\n";
    s += "  float ff = 1;\n"
         "  if (c[24].x == 3) ff = (c[24].z - fc) / (c[24].z - c[24].y);\n"
         "  else if (c[24].x == 1) ff = exp(-c[24].w * fc);\n"
         "  else if (c[24].x == 2) ff = exp(-(c[24].w * fc) * (c[24].w * fc));\n"
         "  o.fog = saturate(ff);\n"
         "  o.wd = float4(o.pos.z, o.pos.w, o.pos.w * c[78].x, 0);\n"   // depth for the pixel shader (PSDepthWrapper)
         "  return o;\n}\n";
    IDirect3DVertexShader9 *vs = nullptr;
    if (ID3DBlob *b = Compile(s, "vs_2_a", "xbox_ffvs")) {
        g_dev->CreateVertexShader((const DWORD *)b->GetBufferPointer(), &vs);
        b->Release();
    }
    cache[k] = vs;
    if (getenv("GHOSTPC_DUMPVS")) {
        char path[MAX_PATH];
        _snprintf(path, MAX_PATH, "%s\\..\\test\\ffvs%03d.hlsl", g_gameDir, (int)cache.size());
        if (FILE *f = fopen(path, "w")) { fprintf(f, "// fvf %08x\n", k.fvf); fputs(s.c_str(), f); fclose(f); }
    }
    return vs;
}
