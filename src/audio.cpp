// Audio: the Xbox DirectSound API on XAudio2. The XACT engine (xacteng) from Ghost.exe runs unmodified on top
// of this layer; Bink and the DivX player use it directly. Voices are 2D (mix bins) or 3D (voice data from the
// Xbox 3D calculator, which also runs unmodified: distance/cone attenuation, azimuth -> stereo pan, doppler).
#include "common.h"
#include "audio.h"
#include <vector>
#include <mutex>
#include <map>
#include <memory>

IXAudio2 *g_xa;
IXAudio2MasteringVoice *g_master;
static std::recursive_mutex g_dsMu;   // DirectSound calls come from game threads and XACT's DPC
#define DSLOCK std::lock_guard<std::recursive_mutex> dslock_(g_dsMu)

bool AudioInit() {
    static bool tried;
    if (tried) return g_xa != nullptr;
    tried = true;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE m = LoadLibraryA("xaudio2_9.dll");
    if (!m) m = LoadLibraryA("xaudio2_8.dll");
    typedef HRESULT(WINAPI * XA2Create)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR);
    auto create = m ? (XA2Create)GetProcAddress(m, "XAudio2Create") : nullptr;
    if (!create || FAILED(create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) { Log("audio: XAudio2 unavailable, running silent"); g_xa = nullptr; return false; }
    if (FAILED(g_xa->CreateMasteringVoice(&g_master, 2, 48000))) { Log("audio: no output device, running silent"); g_xa->Release(); g_xa = nullptr; return false; }
    Log("audio: XAudio2 ready");
    return true;
}

// ---------------------------------------------------------------------------
// Xbox ADPCM (IMA variant: 36-byte blocks per channel, channels interleaved per 4 bytes)
static const int kStep[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
                              88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
                              598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749,
                              3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
                              12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int kIdx[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
static inline short ImaNibble(int &pred, int &idx, int n) {
    int step = kStep[idx];
    int diff = step >> 3;
    if (n & 1) diff += step >> 2;
    if (n & 2) diff += step >> 1;
    if (n & 4) diff += step;
    if (n & 8) diff = -diff;
    pred = std::max(-32768, std::min(32767, pred + diff));
    idx = std::max(0, std::min(88, idx + kIdx[n]));
    return (short)pred;
}
// 65 samples per block (header sample + 64 nibbles); returns frames written
size_t XboxAdpcmDecode(const uint8_t *src, size_t bytes, int ch, short *out) {
    size_t block = 36 * ch, frames = 0;
    for (size_t b = 0; b + block <= bytes; b += block) {
        const uint8_t *p = src + b;
        int pred[8], idx[8];
        for (int c = 0; c < ch; c++) {
            pred[c] = (short)(p[c * 4] | (p[c * 4 + 1] << 8));
            idx[c] = std::min(88, (int)p[c * 4 + 2]);
            out[frames * ch + c] = (short)pred[c];
        }
        const uint8_t *d = p + 4 * ch;
        for (int g = 0; g < 8; g++)
            for (int c = 0; c < ch; c++) {
                const uint8_t *q = d + (g * ch + c) * 4;
                for (int k = 0; k < 8; k++) {
                    int n = (q[k >> 1] >> ((k & 1) * 4)) & 0xF;
                    out[(frames + 1 + g * 8 + k) * ch + c] = ImaNibble(pred[c], idx[c], n);
                }
            }
        frames += 65;
    }
    return frames;
}
size_t XboxAdpcmFrames(size_t bytes, int ch) { return bytes / (36 * ch) * 65; }

static float MbToGain(long mb) { return mb <= -9600 ? 0.f : powf(10.f, mb / 2000.f); }

XSTD(HRESULT, XAudioCreatePcmFormat, 16, (WORD ch, DWORD sps, WORD bits, WAVEFORMATEX *w)) {
    w->wFormatTag = WAVE_FORMAT_PCM; w->nChannels = ch; w->nSamplesPerSec = sps; w->wBitsPerSample = bits;
    w->nBlockAlign = ch * bits / 8; w->nAvgBytesPerSec = w->nBlockAlign * sps; w->cbSize = 0;
    return 0;
}
XSTD(HRESULT, XAudioCreateAdpcmFormat, 12, (WORD ch, DWORD sps, WAVEFORMATEX *w)) {
    w->wFormatTag = 0x0069; w->nChannels = ch; w->nSamplesPerSec = sps; w->wBitsPerSample = 4;
    w->nBlockAlign = 36 * ch; w->nAvgBytesPerSec = sps / 64 * w->nBlockAlign; w->cbSize = 2;
    ((WORD *)(w + 1))[0] = 64;
    return 0;
}
XSTD(HRESULT, DirectSoundDoWork, 0, ()) { return 0; }
// fills CHRTFSource::m_vtable with the full-HRTF math used by the 3D calculator (pure code, runs as is)
XSTD(void, DirectSoundUseFullHRTF, 0, ()) { ((void(__stdcall *)())SymAddr("?SetFullHRTF5Channel@CHRTFSource@DirectSound@@SGXXZ"))(); }

// ---------------------------------------------------------------------------
// Voice parameters shared by buffers and streams
struct MixBinPair { DWORD bin; LONG vol; };
struct DSMixBins { DWORD count; const MixBinPair *pairs; };
struct Voice3D {   // DS3DCALCVOICEDATA (as filled by the Xbox 3D calculator)
    DWORD flags;
    LONG distVol, coneVol, frontVol, rearVol, centerA, centerB, unk1c, unk20, doppler;
    float azimuth, elevation;
};
struct VoiceParams {
    WAVEFORMATEX fmt = {};
    bool hasFmt = false, adpcm = false;
    LONG pitch = 0, volume = 0, headroom = 0;
    bool pitchSet = false;   // until SetPitch/SetFrequency, a voice plays at its format's own rate
    float binL = 1, binR = 1;   // 2D panning from mix bins (mono sources)
    bool use3D = false;
    LONG distVol = 0, coneVol = 0, doppler = 0;
    float azimuth = 0;
};
static void ParseMixBins(VoiceParams &v, const DSMixBins *mb) {
    if (!mb || !mb->pairs || !mb->count) return;
    // Xbox mix bins: 0 FL, 1 FR, 2 C, 3 LFE, 4 RL, 5 RR; others are effect sends
    float l = 0, r = 0;
    for (DWORD i = 0; i < mb->count && i < 32; i++) {
        float g = MbToGain(mb->pairs[i].vol);
        switch (mb->pairs[i].bin) {
        case 0: case 4: l += g; break;
        case 1: case 5: r += g; break;
        case 2: l += g * 0.707f; r += g * 0.707f; break;
        }
    }
    v.binL = std::min(l, 1.f);
    v.binR = std::min(r, 1.f);
}
static void ApplyParams(IXAudio2SourceVoice *voice, const VoiceParams &v) {
    if (!voice || !v.hasFmt) return;
    // pitch: 4096 units per octave relative to 48 kHz
    float ratio = v.pitchSet ? powf(2.f, (v.pitch + (v.use3D ? v.doppler : 0)) / 4096.f) * 48000.f / std::max<DWORD>(v.fmt.nSamplesPerSec, 1)
                             : powf(2.f, (v.use3D ? v.doppler : 0) / 4096.f);
    voice->SetFrequencyRatio(std::min(std::max(ratio, 1.f / 1024), 8.f));
    LONG mb = v.volume + (v.use3D ? v.distVol + v.coneVol : 0);
    voice->SetVolume(MbToGain(mb));
    float l, r;
    if (v.use3D) {
        // azimuth (degrees, 0 = ahead, positive = right, 180 = behind): constant-power pan
        float s = sinf(v.azimuth * 0.0174532925f);
        float a = (s + 1.f) * 0.25f * 3.14159265f;
        l = cosf(a) * 1.41421356f; r = sinf(a) * 1.41421356f;
        l = std::min(l, 1.f); r = std::min(r, 1.f);
    } else { l = v.binL; r = v.binR; }
    if (v.fmt.nChannels == 1) {
        float m[2] = {l, r};
        voice->SetOutputMatrix(g_master, 1, 2, m);
    } else if (v.fmt.nChannels == 2) {
        float m[4] = {l, 0, 0, r};
        voice->SetOutputMatrix(g_master, 2, 2, m);
    }
}
static void SetFormatInto(VoiceParams &v, const WAVEFORMATEX *f) {
    if (!f) return;
    v.fmt = *f;
    v.fmt.cbSize = 0;
    v.adpcm = f->wFormatTag == 0x0069;
    v.hasFmt = true;
}
static WAVEFORMATEX PcmOf(const VoiceParams &v) {
    WAVEFORMATEX o = {};
    o.wFormatTag = WAVE_FORMAT_PCM;
    o.nChannels = v.fmt.nChannels;
    o.nSamplesPerSec = v.fmt.nSamplesPerSec;
    o.wBitsPerSample = v.adpcm ? 16 : v.fmt.wBitsPerSample;
    o.nBlockAlign = o.nChannels * o.wBitsPerSample / 8;
    o.nAvgBytesPerSec = o.nBlockAlign * o.nSamplesPerSec;
    return o;
}
static void Apply3D(VoiceParams &v, const Voice3D *d) {
    if (!d) return;
    // dwChangeMask only flags values GetVoiceData changed in XACT's per-source struct; XACT reuses pooled voices,
    // so a newly assigned buffer may get a struct whose (unchanged) values it has never seen. The struct always
    // holds the current values, so take them all.
    v.distVol = d->distVol;
    v.coneVol = d->coneVol;
    v.doppler = d->doppler;
    v.azimuth = d->azimuth;
}
static void FillVoiceProps(const VoiceParams &v, DWORD *p) {
    // DSVOICEPROPS: dwMixBinCount, MixBinVolumePairs[8], lPitch, lI3DL2DirectVolume, lI3DL2RoomVolume
    memset(p, 0, 80);
    p[0] = 2;
    p[1] = 0; p[2] = v.volume; p[3] = 1; p[4] = v.volume;
    p[17] = v.pitch;
}

// ---------------------------------------------------------------------------
// DirectSound streams (XMediaObject): packets are submitted to an XAudio2 voice
struct XMediaPacket {
    void *pvBuffer;
    DWORD dwMaxSize;
    DWORD *pdwCompletedSize;
    DWORD *pdwStatus;
    union { HANDLE hCompletionEvent; void *pContext; };
    LONGLONG *prtTimestamp;
};
#define XMP_STATUS_SUCCESS 0
#define XMP_STATUS_PENDING 0x8000000A
#define XMP_STATUS_FLUSHED 0x8000000B
typedef void(CALLBACK *XMOCallback)(void *streamCtx, void *packetCtx, DWORD status);
struct DSStreamDesc { DWORD dwFlags; DWORD dwMaxAttachedPackets; const WAVEFORMATEX *lpwfxFormat; XMOCallback cb; void *ctx; const DSMixBins *mixbins; };

struct PacketRec { XMediaPacket pkt; std::vector<short> pcm; std::vector<uint8_t> raw; };
struct DSStream : IXAudio2VoiceCallback {
    LONG refs = 1;
    IXAudio2SourceVoice *voice = nullptr;
    VoiceParams vp;
    XMOCallback cb = nullptr;
    void *ctx = nullptr;
    bool paused = false;
    std::vector<uint8_t> carry;   // bytes of an incomplete frame/ADPCM block left over from the previous packet
    unsigned packets = 0, starved = 0;
    double secsSubmitted = 0;
    DWORD created = GetTickCount();
    void Complete(PacketRec *r, DWORD status) {
        if (r->pkt.pdwCompletedSize) *r->pkt.pdwCompletedSize = status == XMP_STATUS_SUCCESS ? r->pkt.dwMaxSize : 0;
        if (r->pkt.pdwStatus) *r->pkt.pdwStatus = status;
        if (cb) cb(ctx, r->pkt.pContext, status);
        else if (r->pkt.hCompletionEvent) SetEvent(r->pkt.hCompletionEvent);
        delete r;
    }
    void STDMETHODCALLTYPE OnBufferEnd(void *c) override { Complete((PacketRec *)c, XMP_STATUS_SUCCESS); }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void *) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void *) override {}
    void STDMETHODCALLTYPE OnVoiceError(void *, HRESULT) override {}
    void EnsureVoice() {
        if (voice || !vp.hasFmt || !AudioInit()) return;
        WAVEFORMATEX f = PcmOf(vp);
        if (FAILED(g_xa->CreateSourceVoice(&voice, &f, 0, 8.0f, this, nullptr, nullptr))) { voice = nullptr; return; }
        ApplyParams(voice, vp);
        if (!paused) voice->Start();
    }
};
// The game and XACT call XMediaObject methods through this vtable
struct DSStreamObj { void **vtbl; DSStream *s; };

static ULONG __stdcall SO_AddRef(DSStreamObj *o) { return InterlockedIncrement(&o->s->refs); }
static ULONG __stdcall SO_Release(DSStreamObj *o) {
    LONG r = InterlockedDecrement(&o->s->refs);
    if (r == 0) {
        Log("stream %p released after %.2f s, %.2f s of audio submitted (%uHz)", o, (GetTickCount() - o->s->created) / 1000.0,
            o->s->secsSubmitted, o->s->vp.fmt.nSamplesPerSec);
        if (o->s->voice) o->s->voice->DestroyVoice();
        delete o->s;
        delete o;
    }
    return r;
}
static HRESULT __stdcall SO_GetInfo(DSStreamObj *o, DWORD *info) {
    // XMEDIAINFO: dwFlags, dwInputSize, dwOutputSize, dwMaxLookahead
    info[0] = 0; info[1] = o->s->vp.fmt.nBlockAlign; info[2] = 0; info[3] = 0;
    return 0;
}
static HRESULT __stdcall SO_GetStatus(DSStreamObj *o, DWORD *st) {
    // DSSTREAMSTATUS_READY 1, PAUSED 2, STARVED 4, PLAYING 0x10000
    XAUDIO2_VOICE_STATE vs = {};
    if (o->s->voice) o->s->voice->GetState(&vs);
    *st = 1 | (vs.BuffersQueued ? 0x10000 : 4) | (o->s->paused ? 2 : 0);
    return 0;
}
static void SafeCopy(void *d, const void *s, size_t n) {
    __try { memcpy(d, s, n); } __except (EXCEPTION_EXECUTE_HANDLER) { memset(d, 0, n); }
}
// Packets may end in the middle of a sample frame or ADPCM block: whole units are submitted and the rest is
// carried into the next packet (dropping it shifted all later audio and crackled).
static HRESULT __stdcall SO_Process(DSStreamObj *o, XMediaPacket *in, XMediaPacket *out) {
    DSStream *s = o->s;
    auto r = new PacketRec{*in};
    if (in->pdwStatus) *in->pdwStatus = XMP_STATUS_PENDING;
    {
        DSLOCK;
        s->EnsureVoice();
    }
    if (!s->voice) { s->Complete(r, XMP_STATUS_SUCCESS); return 0; }
    int ch = std::max<int>(s->vp.fmt.nChannels, 1);
    DWORD unit = s->vp.adpcm ? 36 * ch : std::max<WORD>(s->vp.fmt.nBlockAlign, 1);
    r->raw.reserve(s->carry.size() + in->dwMaxSize);
    r->raw.assign(s->carry.begin(), s->carry.end());
    size_t had = r->raw.size();
    r->raw.resize(had + in->dwMaxSize);
    SafeCopy(r->raw.data() + had, in->pvBuffer, in->dwMaxSize);
    size_t whole = r->raw.size() - r->raw.size() % unit;
    s->carry.assign(r->raw.begin() + whole, r->raw.end());
    r->raw.resize(whole);
    XAUDIO2_BUFFER b = {};
    if (s->vp.adpcm) {
        r->pcm.resize(XboxAdpcmFrames(whole, ch) * ch);
        size_t fr = XboxAdpcmDecode(r->raw.data(), whole, ch, r->pcm.data());
        b.pAudioData = (const BYTE *)r->pcm.data();
        b.AudioBytes = (UINT32)(fr * ch * 2);
    } else {
        b.pAudioData = r->raw.data();
        b.AudioBytes = (UINT32)whole;
    }
    {
        XAUDIO2_VOICE_STATE vs; s->voice->GetState(&vs, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        s->packets++;
        if (vs.BuffersQueued == 0 && s->packets > 4) s->starved++;
        if ((s->packets % 1000) == 0)
            Log("stream %p: %u packets, %u times the queue had run dry (%uHz)", o, s->packets, s->starved, s->vp.fmt.nSamplesPerSec);
    }
    s->secsSubmitted += (double)b.AudioBytes / (2.0 * ch) / std::max<DWORD>(s->vp.fmt.nSamplesPerSec, 1) *
                        (s->vp.adpcm ? 1.0 : 16.0 / std::max<WORD>(s->vp.fmt.wBitsPerSample, 8));
    b.pContext = r;
    static bool dump = getenv("GHOSTPC_DUMPSTREAM") != nullptr;
    if (dump && b.AudioBytes) {
        // debug: append the PCM each stream submits to test/stream_<ptr>.raw (16-bit for ADPCM sources)
        char path[MAX_PATH];
        _snprintf(path, MAX_PATH, "%s/../test/stream_%p_%uch_%uHz_%ub.raw", g_gameDir, o, ch, s->vp.fmt.nSamplesPerSec,
                  s->vp.adpcm ? 16 : s->vp.fmt.wBitsPerSample);
        if (FILE *f = fopen(path, "ab")) { fwrite(b.pAudioData, 1, b.AudioBytes, f); fclose(f); }
    }
    if (!b.AudioBytes || FAILED(s->voice->SubmitSourceBuffer(&b))) s->Complete(r, XMP_STATUS_SUCCESS);
    return 0;
}
static HRESULT __stdcall SO_Discontinuity(DSStreamObj *o) { return 0; }
static HRESULT __stdcall SO_Flush(DSStreamObj *o) { o->s->carry.clear(); if (o->s->voice) o->s->voice->FlushSourceBuffers(); return 0; }
static void *g_streamVtbl[] = {(void *)SO_AddRef, (void *)SO_Release, (void *)SO_GetInfo, (void *)SO_GetStatus,
                               (void *)SO_Process, (void *)SO_Discontinuity, (void *)SO_Flush};

static DSStreamObj *NewStream(const DSStreamDesc *d) {
    auto s = new DSStream();
    auto o = new DSStreamObj{g_streamVtbl, s};
    if (d->lpwfxFormat) SetFormatInto(s->vp, d->lpwfxFormat);
    ParseMixBins(s->vp, d->mixbins);
    s->cb = d->cb;
    s->ctx = d->ctx;
    s->EnsureVoice();
    Log("stream %p: fmt %x %uch %uHz %ubit align %u flags %x packets %u cb %p", o, s->vp.fmt.wFormatTag, s->vp.fmt.nChannels,
        s->vp.fmt.nSamplesPerSec, s->vp.fmt.wBitsPerSample, s->vp.fmt.nBlockAlign, d->dwFlags, d->dwMaxAttachedPackets, d->cb);
    return o;
}
XSTD(HRESULT, DirectSoundCreateStream, 8, (const DSStreamDesc *d, DSStreamObj **out)) { DSLOCK; *out = NewStream(d); return 0; }
XSTD(HRESULT, IDirectSound_CreateSoundStream, 16, (void *ds, const DSStreamDesc *d, DSStreamObj **out, void *unk)) { DSLOCK; *out = NewStream(d); return 0; }
XSTD(HRESULT, IDirectSoundStream_FlushEx, 16, (DSStreamObj *o, LONGLONG ts, DWORD flags)) { return SO_Flush(o); }
XSTD(HRESULT, IDirectSoundStream_Pause, 8, (DSStreamObj *o, DWORD p)) {
    DSLOCK;
    o->s->paused = p == 1;   // DSSTREAMPAUSE_RESUME 0, PAUSE 1
    if (o->s->voice) { if (p == 1) o->s->voice->Stop(); else o->s->voice->Start(); }
    return 0;
}
XSTD(HRESULT, IDirectSoundStream_SetFrequency, 8, (DSStreamObj *o, DWORD f)) {
    DSLOCK;
    if (f) { o->s->vp.pitch = (LONG)(4096.0 * log2(f / 48000.0)); o->s->vp.pitchSet = true; }
    ApplyParams(o->s->voice, o->s->vp);
    return 0;
}
#define STREAM_SETTER(name, nb, decl, body) \
    XSTD(HRESULT, IDirectSoundStream_##name, nb, decl) { DSLOCK; DSStream *s = o->s; body; ApplyParams(s->voice, s->vp); return 0; }
STREAM_SETTER(SetVolume, 8, (DSStreamObj *o, LONG v), s->vp.volume = v)
STREAM_SETTER(SetPitch, 8, (DSStreamObj *o, LONG v), (s->vp.pitch = v, s->vp.pitchSet = true))
STREAM_SETTER(SetHeadroom, 8, (DSStreamObj *o, DWORD v), s->vp.headroom = v)
STREAM_SETTER(SetMixBins, 8, (DSStreamObj *o, const DSMixBins *m), ParseMixBins(s->vp, m))
STREAM_SETTER(SetMixBinVolumes, 8, (DSStreamObj *o, const DSMixBins *m), ParseMixBins(s->vp, m))
STREAM_SETTER(Set3DVoiceData, 8, (DSStreamObj *o, const Voice3D *d), Apply3D(s->vp, d))
STREAM_SETTER(Use3DVoiceData, 8, (DSStreamObj *o, BOOL u), s->vp.use3D = u != 0)
STREAM_SETTER(SetEG, 8, (DSStreamObj *o, const void *), (void)0)
STREAM_SETTER(SetLFO, 8, (DSStreamObj *o, const void *), (void)0)
STREAM_SETTER(SetFilter, 8, (DSStreamObj *o, const void *), (void)0)
STREAM_SETTER(SetOutputBuffer, 8, (DSStreamObj *o, void *), (void)0)
XSTD(HRESULT, IDirectSoundStream_SetFormat, 8, (DSStreamObj *o, const WAVEFORMATEX *f)) {
    DSLOCK;
    DSStream *s = o->s;
    WORD oldCh = s->vp.fmt.nChannels; DWORD oldTag = s->vp.fmt.wFormatTag, oldBits = s->vp.fmt.wBitsPerSample;
    SetFormatInto(s->vp, f);
    if (s->voice && (oldCh != s->vp.fmt.nChannels || oldTag != s->vp.fmt.wFormatTag || oldBits != s->vp.fmt.wBitsPerSample)) {
        s->voice->DestroyVoice();
        s->voice = nullptr;
    }
    s->EnsureVoice();
    ApplyParams(s->voice, s->vp);
    return 0;
}
XSTD(HRESULT, IDirectSoundStream_GetVoiceProperties, 8, (DSStreamObj *o, DWORD *p)) { DSLOCK; FillVoiceProps(o->s->vp, p); return 0; }

// ---------------------------------------------------------------------------
// DirectSound buffers: one XAudio2 voice each, playing straight out of the buffer memory
struct DSBufferDesc { DWORD dwSize, dwFlags, dwBufferBytes; const WAVEFORMATEX *lpwfxFormat; const DSMixBins *mixbins; DWORD inputMixBin; };
struct DSBuf;
static std::map<std::pair<const void *, DWORD>, std::shared_ptr<std::vector<short>>> g_adpcmCache;
static void SafeDecode(const uint8_t *src, size_t bytes, int ch, short *out) {
    __try { XboxAdpcmDecode(src, bytes, ch, out); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}   // decoded wave data
struct DSBuf : IXAudio2VoiceCallback {
    IXAudio2SourceVoice *voice = nullptr;
    WAVEFORMATEX voiceFmt = {};
    std::vector<uint8_t> mem;
    uint8_t *data = nullptr;
    DWORD bytes = 0;
    DWORD playStart = 0, playLen = 0, loopStart = 0, loopLen = 0;
    VoiceParams vp;
    bool playing = false, looping = false;
    UINT64 startSamples = 0;
    DWORD startFrame = 0;
    std::shared_ptr<std::vector<short>> pcm;   // decoded ADPCM kept alive while queued
    volatile LONG ended = 0;
    void STDMETHODCALLTYPE OnStreamEnd() override { InterlockedExchange(&ended, 1); }
    void STDMETHODCALLTYPE OnBufferEnd(void *) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void *) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void *) override {}
    void STDMETHODCALLTYPE OnVoiceError(void *, HRESULT) override {}

    UINT32 BytesToFrames(DWORD b) const {
        if (vp.adpcm) return (UINT32)(b / (36 * vp.fmt.nChannels) * 65);
        return (UINT32)(b / std::max<WORD>(vp.fmt.nBlockAlign, 1));
    }
    bool EnsureVoice() {
        if (!vp.hasFmt || !AudioInit()) return false;
        WAVEFORMATEX f = PcmOf(vp);
        if (voice && (f.nChannels != voiceFmt.nChannels || f.wBitsPerSample != voiceFmt.wBitsPerSample)) {
            voice->DestroyVoice(); voice = nullptr;
        }
        if (!voice) {
            if (FAILED(g_xa->CreateSourceVoice(&voice, &f, 0, 8.0f, this))) { voice = nullptr; return false; }
            voiceFmt = f;
        }
        // source rate changes are handled by the frequency ratio (relative to the voice's creation rate)
        VoiceParams p = vp;
        p.fmt.nSamplesPerSec = vp.fmt.nSamplesPerSec;
        ApplyParams(voice, p);
        if (vp.fmt.nSamplesPerSec != voiceFmt.nSamplesPerSec) {
            float ratio = vp.pitchSet ? powf(2.f, (vp.pitch + (vp.use3D ? vp.doppler : 0)) / 4096.f) * 48000.f / voiceFmt.nSamplesPerSec
                                      : (float)vp.fmt.nSamplesPerSec / voiceFmt.nSamplesPerSec;
            voice->SetFrequencyRatio(std::min(std::max(ratio, 1.f / 1024), 8.f));
        }
        return true;
    }
    void Apply() {
        if (!voice) return;
        VoiceParams p = vp;
        p.fmt.nSamplesPerSec = voiceFmt.nSamplesPerSec ? voiceFmt.nSamplesPerSec : vp.fmt.nSamplesPerSec;
        ApplyParams(voice, p);
    }
    // Decode/play only the play region: wave banks pack entries at 4-byte alignment, and ADPCM blocks are
    // aligned to the start of each entry, not to the start of the buffer.
    void Submit(DWORD fromByte, bool restart) {
        if (!data || !bytes || !EnsureVoice()) { playing = false; return; }
        voice->Stop();
        voice->FlushSourceBuffers();
        DWORD rs = std::min(playStart, bytes);
        DWORD rlen = playLen ? std::min(playLen, bytes - rs) : bytes - rs;
        const uint8_t *base = data + rs;
        XAUDIO2_BUFFER x = {};
        UINT32 totalFrames;
        if (vp.adpcm) {
            auto key = std::make_pair((const void *)base, rlen);
            auto it = g_adpcmCache.find(key);
            if (it == g_adpcmCache.end()) {
                auto v = std::make_shared<std::vector<short>>(XboxAdpcmFrames(rlen, vp.fmt.nChannels) * vp.fmt.nChannels);
                SafeDecode(base, rlen, vp.fmt.nChannels, v->data());
                it = g_adpcmCache.emplace(key, v).first;
            }
            pcm = it->second;
            x.pAudioData = (const BYTE *)pcm->data();
            x.AudioBytes = (UINT32)(pcm->size() * 2);
            totalFrames = (UINT32)(pcm->size() / vp.fmt.nChannels);
        } else {
            pcm.reset();
            DWORD ba = std::max<WORD>(vp.fmt.nBlockAlign, 1);
            x.pAudioData = base;
            x.AudioBytes = rlen - rlen % ba;
            totalFrames = x.AudioBytes / ba;
        }
        if (!totalFrames) { playing = false; return; }
        UINT32 from = fromByte > rs ? std::min(BytesToFrames(fromByte - rs), totalFrames - 1) : 0;
        x.PlayBegin = from;
        x.PlayLength = totalFrames - from;
        if (looping) {
            // loop region: relative to the play region when it fits inside it, else relative to the buffer
            DWORD ls = loopStart, ll = loopLen;
            if (ls >= rs && ls + ll > rlen) ls -= rs;
            UINT32 lsf = std::min(BytesToFrames(ls), totalFrames - 1);
            UINT32 lef = ll ? std::min(lsf + BytesToFrames(ll), totalFrames) : totalFrames;
            if (lsf < from) lsf = from;   // XAudio2 needs the loop inside the play region
            if (lef <= lsf) lef = totalFrames;
            x.LoopBegin = lsf;
            x.LoopLength = lef - lsf;
            x.LoopCount = XAUDIO2_LOOP_INFINITE;
        }
        x.Flags = XAUDIO2_END_OF_STREAM;
        InterlockedExchange(&ended, 0);
        XAUDIO2_VOICE_STATE st; voice->GetState(&st);
        startSamples = st.SamplesPlayed;
        startFrame = from;
        if (FAILED(voice->SubmitSourceBuffer(&x))) { playing = false; return; }
        Apply();
        voice->Start();
        playing = true;
    }
    bool IsPlaying() {
        if (!playing || !voice) return false;
        XAUDIO2_VOICE_STATE st; voice->GetState(&st);
        if (st.BuffersQueued == 0 || ended) playing = false;
        return playing;
    }
};
static int g_fakeDS[32];
XSTD(HRESULT, DirectSoundCreate, 12, (void *guid, void **out, void *unk)) { AudioInit(); *out = g_fakeDS; return 0; }
XSTD(ULONG, IDirectSound_Release, 4, (void *)) { return 1; }
XSTD(HRESULT, IDirectSound_CommitDeferredSettings, 4, (void *)) { return 0; }
XSTD(HRESULT, IDirectSound_SetI3DL2Listener, 12, (void *, const void *, DWORD)) { return 0; }
XSTD(HRESULT, IDirectSound_SetMixBinHeadroom, 12, (void *, DWORD, DWORD)) { return 0; }
XSTD(HRESULT, IDirectSound_DownloadEffectsImage, 20, (void *ds, const void *img, DWORD size, const void *loc, void **desc)) {
    // DSEFFECTIMAGEDESC: dwEffectCount, dwTotalScratchSize, DSEFFECTMAP[n] (8 dwords, pointers into DSP memory)
    static DWORD d[2 + 32 * 8];
    static uint8_t scratch[32][4096];
    d[0] = 32; d[1] = 0;
    for (int i = 0; i < 32; i++) {
        DWORD *m = d + 2 + i * 8;
        m[0] = (DWORD)scratch[i]; m[1] = 1024; m[2] = (DWORD)scratch[i]; m[3] = 1024;
        m[4] = (DWORD)scratch[i] + 1024; m[5] = 1024; m[6] = (DWORD)scratch[i] + 2048; m[7] = 2048;
    }
    if (desc) *desc = d;
    return 0;
}
XSTD(HRESULT, IDirectSound_CreateSoundBuffer, 16, (void *ds, const DSBufferDesc *d, DSBuf **out, void *unk)) {
    DSLOCK;
    AudioInit();
    auto b = new DSBuf();
    b->bytes = d->dwBufferBytes;
    if (b->bytes) { b->mem.resize(b->bytes); b->data = b->mem.data(); }
    if (d->lpwfxFormat) SetFormatInto(b->vp, d->lpwfxFormat);
    ParseMixBins(b->vp, d->mixbins);
    b->vp.use3D = (d->dwFlags & 0x10) != 0;   // DSBCAPS_CTRL3D
    *out = b;
    return 0;
}
XSTD(HRESULT, IDirectSoundBuffer_SetFormat, 8, (DSBuf *b, const WAVEFORMATEX *f)) { DSLOCK; SetFormatInto(b->vp, f); return 0; }
XSTD(HRESULT, IDirectSoundBuffer_SetBufferData, 12, (DSBuf *b, void *data, DWORD bytes)) {
    DSLOCK;
    b->data = (uint8_t *)data; b->bytes = bytes;
    b->playStart = b->playLen = b->loopStart = b->loopLen = 0;
    return 0;
}
XSTD(HRESULT, IDirectSoundBuffer_SetPlayRegion, 12, (DSBuf *b, DWORD start, DWORD len)) { DSLOCK; b->playStart = start; b->playLen = len; return 0; }
XSTD(HRESULT, IDirectSoundBuffer_SetLoopRegion, 12, (DSBuf *b, DWORD start, DWORD len)) { DSLOCK; b->loopStart = start; b->loopLen = len; return 0; }
XSTD(HRESULT, IDirectSoundBuffer_Play, 16, (DSBuf *b, DWORD r1, DWORD r2, DWORD flags)) {
    DSLOCK;
    bool loop = (flags & 1) != 0;   // DSBPLAY_LOOPING
    bool fromStart = (flags & 2) != 0;
    if (b->IsPlaying() && !fromStart) {
        if (loop != b->looping) { b->looping = loop; if (!loop && b->voice) b->voice->ExitLoop(); }
        return 0;
    }
    b->looping = loop;
    b->Submit(0, true);
    static int logged;
    static bool logAll = getenv("GHOSTPC_FRAMESTATS") != nullptr;
    if (logged < 40 || logAll) {
        logged++;
        Log("dsound: play #%d fmt %x %uch %uHz %ubit bytes %u region %u+%u loop %d(%u+%u) vol %d pitch %d 3d %d -> %s", logged,
            b->vp.fmt.wFormatTag, b->vp.fmt.nChannels, b->vp.fmt.nSamplesPerSec, b->vp.fmt.wBitsPerSample, b->bytes, b->playStart,
            b->playLen, loop, b->loopStart, b->loopLen, b->vp.volume, b->vp.pitch, b->vp.use3D, b->playing ? "playing" : "FAILED");
    }
    return 0;
}
XSTD(HRESULT, IDirectSoundBuffer_Stop, 4, (DSBuf *b)) {
    DSLOCK;
    b->playing = false;
    if (b->voice) { b->voice->Stop(); b->voice->FlushSourceBuffers(); }
    return 0;
}
XSTD(HRESULT, IDirectSoundBuffer_StopEx, 16, (DSBuf *b, LONGLONG ts, DWORD flags)) {
    DSLOCK;
    // DSBSTOPEX_RELEASEWAVEFORM (2): leave the loop and play to the end; otherwise stop now
    if ((flags & 2) && b->IsPlaying()) { b->looping = false; b->voice->ExitLoop(); return 0; }
    b->playing = false;
    if (b->voice) { b->voice->Stop(); b->voice->FlushSourceBuffers(); }
    return 0;
}
XSTD(HRESULT, IDirectSoundBuffer_GetStatus, 8, (DSBuf *b, DWORD *st)) {
    DSLOCK;
    bool p = b->IsPlaying();
    *st = p ? (1 | (b->looping ? 4 : 0)) : 0;   // DSBSTATUS_PLAYING / LOOPING
    return 0;
}
XSTD(HRESULT, IDirectSoundBuffer_GetCurrentPosition, 12, (DSBuf *b, DWORD *play, DWORD *write)) {
    DSLOCK;
    DWORD pos = 0;
    if (b->voice && b->IsPlaying() && b->vp.fmt.nBlockAlign) {
        XAUDIO2_VOICE_STATE st; b->voice->GetState(&st);
        UINT64 frames = st.SamplesPlayed - b->startSamples + b->startFrame;
        DWORD ba = b->vp.adpcm ? 2 * b->vp.fmt.nChannels : b->vp.fmt.nBlockAlign;
        UINT64 total = b->vp.adpcm ? XboxAdpcmFrames(b->bytes, b->vp.fmt.nChannels) : b->bytes / ba;
        if (total) frames %= total;
        pos = b->playStart + (b->vp.adpcm ? (DWORD)(frames / 65 * 36 * b->vp.fmt.nChannels) : (DWORD)(frames * ba));
    }
    if (play) *play = pos;
    if (write) *write = b->bytes ? (pos + 2048) % b->bytes : 0;
    return 0;
}
XSTD(HRESULT, IDirectSoundBuffer_SetCurrentPosition, 8, (DSBuf *b, DWORD pos)) {
    DSLOCK;
    if (b->IsPlaying()) b->Submit(pos, false);
    return 0;
}
XSTD(HRESULT, IDirectSoundBuffer_Lock, 32, (DSBuf *b, DWORD off, DWORD n, void **p1, DWORD *n1, void **p2, DWORD *n2, DWORD fl)) {
    DSLOCK;
    if (fl & 2) { off = 0; n = b->bytes; }   // DSBLOCK_ENTIREBUFFER
    if (!b->bytes) { *p1 = nullptr; *n1 = 0; if (p2) { *p2 = nullptr; *n2 = 0; } return 0; }
    off %= b->bytes;
    if (off + n <= b->bytes) { *p1 = b->data + off; *n1 = n; if (p2) { *p2 = nullptr; *n2 = 0; } }
    else { *p1 = b->data + off; *n1 = b->bytes - off; if (p2) { *p2 = b->data; *n2 = n - *n1; } }
    return 0;
}
XSTD(HRESULT, IDirectSoundBuffer_Unlock, 20, (DSBuf *b, void *, DWORD, void *, DWORD)) { return 0; }
XSTD(HRESULT, IDirectSoundBuffer_SetFrequency, 8, (DSBuf *b, DWORD f)) {
    DSLOCK;
    if (f) { b->vp.pitch = (LONG)(4096.0 * log2(f / 48000.0)); b->vp.pitchSet = true; }
    b->Apply();
    return 0;
}
#define BUF_SETTER(name, nb, decl, body) \
    XSTD(HRESULT, IDirectSoundBuffer_##name, nb, decl) { DSLOCK; body; b->Apply(); return 0; }
BUF_SETTER(SetVolume, 8, (DSBuf *b, LONG v), b->vp.volume = v)
BUF_SETTER(SetPitch, 8, (DSBuf *b, LONG v), (b->vp.pitch = v, b->vp.pitchSet = true))
BUF_SETTER(SetHeadroom, 8, (DSBuf *b, DWORD v), b->vp.headroom = v)
BUF_SETTER(SetMixBins, 8, (DSBuf *b, const DSMixBins *m), ParseMixBins(b->vp, m))
BUF_SETTER(SetMixBinVolumes, 8, (DSBuf *b, const DSMixBins *m), ParseMixBins(b->vp, m))
BUF_SETTER(Set3DVoiceData, 8, (DSBuf *b, const Voice3D *d), Apply3D(b->vp, d))
BUF_SETTER(Use3DVoiceData, 8, (DSBuf *b, BOOL u), b->vp.use3D = u != 0)
BUF_SETTER(SetEG, 8, (DSBuf *b, const void *), (void)0)
BUF_SETTER(SetLFO, 8, (DSBuf *b, const void *), (void)0)
BUF_SETTER(SetFilter, 8, (DSBuf *b, const void *), (void)0)
BUF_SETTER(SetOutputBuffer, 8, (DSBuf *b, void *), (void)0)
XSTD(HRESULT, IDirectSoundBuffer_GetVoiceProperties, 8, (DSBuf *b, DWORD *p)) { DSLOCK; FillVoiceProps(b->vp, p); return 0; }
XSTD(ULONG, IDirectSoundBuffer_Release, 4, (DSBuf *b)) {
    DSLOCK;
    if (b->voice) { b->voice->Stop(); b->voice->DestroyVoice(); }
    delete b;
    return 0;
}

// dsound's operator new (picked up by some XDK-linked code): use the game's CRT heap
static void *__cdecl DsNew(size_t n) { return ((void *(__cdecl *)(size_t))SymAddr("_malloc"))(n); }
XRAW("??2@YAPAXI@Z", DsNew);

