// Custom foley sounds: WAV files that replace or add cues of a foley set (the per-template sound banks,
// Sounds\<set>.xsb). Many units in this build have banks missing cues (the Lurker boss's has only footsteps), so the
// user supplies sounds (often StarCraft's own) as
//
//   Sounds\custom\<foley set>\<mode>.wav        e.g. Sounds\custom\zergSunkenClny\swing.wav
//   Sounds\custom\<foley set>\<mode>~<n>.wav    variants, one picked at random per play
//
// <mode> is the foley mode name (gameFoley_GetModeName: swing, hitdirt, vox_alert, vox_death, special1, ...). Every
// tObject::PlayFoleyMode(mode, at, ...) whose object's foley set (+0x28C; tFoleySet starts with its name) has a custom
// sound for the mode plays it instead of the bank's cue, at the object's (or <at>'s) position: full volume within 5 m of
// the player, fading out at 60 m. PCM WAVs (8/16-bit, any rate, mono or stereo).
// GHOSTPC_LOGFOLEY logs every PlayFoleyMode call (object, set, mode, custom or not) to find which modes a unit uses.
//
// Missing banks: a template whose foley bank is not in the build (the Vulture's playerVulture: no
// Sounds\playerVulture.xsb) leaves its objects without a foley set (cGameMixer::LoadFoleySet fails, +0x28C stays
// null; the game then plays nothing). For those, the set's name comes from the object's template (thing name +0xC4,
// gameTemplate_Find, modifier 10 "foley", through basedon), and its custom sounds play as for any other set.
//
// Loops: a mode played with a handle (the Vulture's engine, lwalk_solid: RidingControlsUpdate keeps the handle, sets
// its pitch every frame with cGameMixer::SetPitch and ends it with cGameMixer::Stop) loops when it is a custom sound;
// its handle is GhostPC's own (LOOP_HANDLE | n), so SetPitch / Stop for it act on that voice (frequency ratio = the
// game's pitch, about 0.7 at rest rising with speed) and never reach the mixer. Loops end on a level change too.
#include "common.h"
#include "audio.h"
#include <map>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <math.h>
#include <intrin.h>

extern UINT g_frame;
extern IXAudio2 *g_xa;
extern IXAudio2MasteringVoice *g_master;

namespace {
struct Wav {
    WAVEFORMATEX fmt;
    std::vector<uint8_t> data;
};
std::map<std::string, std::vector<std::shared_ptr<Wav>>> g_sounds;   // "set/mode" (lower case) -> variants
std::vector<IXAudio2SourceVoice *> g_voices;
const unsigned LOOP_HANDLE = 0xC5700000;                           // GhostPC's loop handles: LOOP_HANDLE | n
std::map<unsigned, IXAudio2SourceVoice *> g_loops;
typedef void(__thiscall *SetPitchFn)(void *, unsigned, float);
typedef void(__thiscall *StopFn)(void *, unsigned, unsigned);
SetPitchFn g_setPitchTramp;   // cGameMixer::SetPitch / Stop, for the game's own handles
StopFn g_stopTramp;

void WriteCode(void *at, const void *bytes, size_t n) {
    DWORD old;
    VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
}

std::string Lower(std::string s) {
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::shared_ptr<Wav> LoadWav(const char *path) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    DWORD size = GetFileSize(h, nullptr), got = 0;
    std::vector<uint8_t> f(size);
    ReadFile(h, f.data(), size, &got, nullptr);
    CloseHandle(h);
    if (got < 12 || memcmp(f.data(), "RIFF", 4) || memcmp(f.data() + 8, "WAVE", 4)) return nullptr;
    auto w = std::make_shared<Wav>();
    bool haveFmt = false;
    for (size_t o = 12; o + 8 <= got;) {
        uint32_t len = *(uint32_t *)(f.data() + o + 4);
        const uint8_t *body = f.data() + o + 8;
        if (o + 8 + len > got) len = (uint32_t)(got - o - 8);
        if (!memcmp(f.data() + o, "fmt ", 4) && len >= 16) {
            memset(&w->fmt, 0, sizeof w->fmt);
            memcpy(&w->fmt, body, 16);
            haveFmt = true;
        } else if (!memcmp(f.data() + o, "data", 4)) {
            w->data.assign(body, body + len);
        }
        o += 8 + len + (len & 1);
    }
    if (!haveFmt || w->fmt.wFormatTag != WAVE_FORMAT_PCM || w->data.empty()) return nullptr;
    return w;
}

void LoadSounds() {
    char dir[MAX_PATH];
    _snprintf(dir, MAX_PATH, "%s\\Sounds\\custom\\*", g_gameDir);
    WIN32_FIND_DATAA sets;
    HANDLE hs = FindFirstFileA(dir, &sets);
    if (hs == INVALID_HANDLE_VALUE) return;
    int n = 0;
    do {
        if (!(sets.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || sets.cFileName[0] == '.') continue;
        char pat[MAX_PATH];
        _snprintf(pat, MAX_PATH, "%s\\Sounds\\custom\\%s\\*.wav", g_gameDir, sets.cFileName);
        WIN32_FIND_DATAA wf;
        HANDLE hw = FindFirstFileA(pat, &wf);
        if (hw == INVALID_HANDLE_VALUE) continue;
        do {
            char path[MAX_PATH];
            _snprintf(path, MAX_PATH, "%s\\Sounds\\custom\\%s\\%s", g_gameDir, sets.cFileName, wf.cFileName);
            std::string mode = wf.cFileName;
            mode = mode.substr(0, mode.rfind('.'));
            if (mode.find('~') != std::string::npos) mode = mode.substr(0, mode.find('~'));
            auto w = LoadWav(path);
            if (!w) {
                Log("customsound: %s is not a PCM WAV, skipped", path);
                continue;
            }
            g_sounds[Lower(std::string(sets.cFileName) + "/" + mode)].push_back(w);
            n++;
        } while (FindNextFileA(hw, &wf));
        FindClose(hw);
    } while (FindNextFileA(hs, &sets));
    FindClose(hs);
    Log("customsound: %d sound(s) for %u cue(s) from Sounds\\custom", n, (unsigned)g_sounds.size());
}

typedef uint8_t *(__cdecl *ActivePlayerFn)();
ActivePlayerFn g_activePlayer;
uint8_t *ActivePlayer() {
    if (!g_activePlayer) g_activePlayer = (ActivePlayerFn)SymAddr("?gameMultiplayer_GetActivePlayer@@YAPAVtPlayer@@XZ");
    __try {
        return g_activePlayer ? g_activePlayer() : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

IXAudio2SourceVoice *Play(const Wav &w, const float *at, bool loop = false) {
    if (!g_xa || !g_master) return nullptr;
    // finished voices are released here (no callbacks)
    for (size_t i = 0; i < g_voices.size();) {
        XAUDIO2_VOICE_STATE st;
        g_voices[i]->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (st.BuffersQueued == 0) {
            g_voices[i]->DestroyVoice();
            g_voices.erase(g_voices.begin() + i);
        } else
            i++;
    }
    if (g_voices.size() >= 32) return nullptr;
    float gain = 1.f;
    const uint8_t *player = ActivePlayer();   // the listener
    if (player && at) {
        const float *l = (const float *)(player + 0xb0);
        float dx = at[0] - l[0], dy = at[1] - l[1], dz = at[2] - l[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        gain = d <= 5.f ? 1.f : d >= 60.f ? 0.f : 1.f - (d - 5.f) / 55.f;
        gain *= gain;
    }
    static bool logFoley = getenv("GHOSTPC_LOGFOLEY") != nullptr;
    if (logFoley) Log("customsound: play gain %.2f (%u Hz, %u ch, %u bytes)", gain, w.fmt.nSamplesPerSec, w.fmt.nChannels, (unsigned)w.data.size());
    if (gain <= 0.01f && !loop) return nullptr;
    IXAudio2SourceVoice *v = nullptr;
    HRESULT hr = g_xa->CreateSourceVoice(&v, &w.fmt, 0, 4.f);   // pitch up to 4x (loops follow the game's pitch)
    if (FAILED(hr)) {
        Log("customsound: CreateSourceVoice failed (%08x)", (unsigned)hr);
        return nullptr;
    }
    XAUDIO2_BUFFER b = {};
    b.Flags = XAUDIO2_END_OF_STREAM;
    b.AudioBytes = (UINT32)w.data.size();
    b.pAudioData = w.data.data();
    if (loop) b.LoopCount = XAUDIO2_LOOP_INFINITE;
    v->SubmitSourceBuffer(&b);
    v->SetVolume(gain * 0.9f);
    v->Start();
    if (!loop) g_voices.push_back(v);
    return v;
}

void StopLoop(unsigned handle) {
    auto it = g_loops.find(handle);
    if (it == g_loops.end()) return;
    static bool logFoley = getenv("GHOSTPC_LOGFOLEY") != nullptr;
    if (logFoley) Log("customsound: loop %08x stopped", handle);
    it->second->Stop();
    it->second->DestroyVoice();
    g_loops.erase(it);
}

// the foley set an object's template names (modifier 10 "foley", e.g. playerVulture.nag -> playerVulture), through its
// basedon chain (modifier 0); cached per template name
std::string TemplateFoley(const char *tmplName) {
    static std::map<std::string, std::string> cache;
    std::string key = Lower(tmplName);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    typedef uint8_t *(__cdecl *FindFn)(const char *);
    static auto find = (FindFn)SymAddr("?gameTemplate_Find@@YAPAVtTemplate@@PBD@Z");
    std::string found, name = tmplName;
    for (int depth = 0; find && depth < 8 && found.empty() && !name.empty(); depth++) {
        uint8_t *t = find(name.c_str());
        if (!t) break;
        unsigned n = *(unsigned *)(t + 0x28);
        uint8_t *mods = *(uint8_t **)(t + 0x2c);
        std::string base;
        for (unsigned i = 0; mods && i < n; i++) {
            unsigned id = *(unsigned *)(mods + 36 * i);
            std::string v((const char *)(mods + 36 * i + 4), strnlen((const char *)(mods + 36 * i + 4), 32));
            if (id == 10 && found.empty()) found = v;
            if (id == 0 && base.empty()) base = v;
        }
        name = base;
    }
    size_t slash = found.find_last_of("\\/");
    if (slash != std::string::npos) found = found.substr(slash + 1);
    if (found.find('.') != std::string::npos) found = found.substr(0, found.find('.'));
    return cache[key] = found;
}

typedef int(__thiscall *PlayFoleyFn)(void *, unsigned, uint8_t *, unsigned *, int, float, void *);
PlayFoleyFn g_playTramp;
typedef const char *(__cdecl *ModeNameFn)(unsigned);
ModeNameFn g_modeName;

int __fastcall PlayFoleyMode(uint8_t *obj, void *, unsigned mode, uint8_t *at, unsigned *handle, int a, float vol, void *b) {
    static bool logFoley = getenv("GHOSTPC_LOGFOLEY") != nullptr;
    const char *set = obj ? *(const char **)(obj + 0x28c) : nullptr;   // tFoleySet: name[32] first
    std::string templateSet;
    if (!set && obj) {   // no bank in the build: the template's foley name, for its custom sounds
        templateSet = TemplateFoley(std::string((const char *)(obj + 0xc4), strnlen((const char *)(obj + 0xc4), 32)).c_str());
        if (!templateSet.empty()) set = templateSet.c_str();
    }
    const char *name = g_modeName ? g_modeName(mode) : nullptr;
    const std::vector<std::shared_ptr<Wav>> *custom = nullptr;
    if (set && name && !g_sounds.empty()) {
        char key[96];
        _snprintf(key, sizeof key, "%.32s/%s", set, name);
        auto it = g_sounds.find(Lower(key));
        if (it != g_sounds.end()) custom = &it->second;
    }
    if (logFoley) {
        static int n;
        if (n++ < 2000) {
            char caller[128];
            SymName((uint32_t)_ReturnAddress(), caller, sizeof caller);
            Log("foley-dbg frame %u object %p set %.32s mode %u %s%s from %s", g_frame, obj, set ? set : "(none)", mode,
                name ? name : "?", custom ? " -> custom" : "", caller);
        }
    }
    if (custom) {
        const uint8_t *src = at ? at : obj;
        const Wav &w = *(*custom)[rand() % custom->size()];
        if (handle) {   // the game keeps it to pitch and stop it: a loop
            if (!g_stopTramp) return 0;   // (only with the Stop hook: the mixer must never see our handles)
            static unsigned next;
            unsigned h = LOOP_HANDLE | (++next & 0xFFFF);
            if (IXAudio2SourceVoice *v = Play(w, (const float *)(src + 0xb0), true)) g_loops[h] = v, *handle = h;
            if (logFoley) Log("customsound: loop %08x started (%s/%s)", h, set, name);
            return 1;
        }
        Play(w, (const float *)(src + 0xb0));
        return 1;
    }
    // The Reaver (GhostPC's reaver_vehicle_player, the tank's control with the Dragoon's foley set): the tank code's
    // engine sounds (moving, mode 4, every frame while driving; lwalk_solid, mode 23, as an engine loop with a handle)
    // are dropped; vehicles.cpp plays the Dragoon's idle hum (special1) instead.
    if (obj && (mode == 4 || (mode == 23 && handle)) && !_stricmp((const char *)(obj + 0xc4), "reaver_vehicle_player"))
        return 1;
    // The Scarab's explosion (GhostPC's exp_reaverScarab, foley expPtssScarab): that bank's only cue is createnoblock
    // (mode 320, played by tThing::UnHibernate), but an explosion only plays create (mode 0, tThing::Finalize), so
    // the sound never played. Its create plays createnoblock.
    if (mode == 0 && set && !_strnicmp(set, "expPtssScarab", 14)) {
        int r = g_playTramp(obj, 320, at, handle, a, vol, b);
        if (logFoley) Log("customsound: %s create -> createnoblock: %d", set, r);
        return r;
    }
    return g_playTramp(obj, mode, at, handle, a, vol, b);
}
void __fastcall SetPitch(void *mixer, void *, unsigned handle, float pitch) {
    auto it = g_loops.find(handle);
    if (it == g_loops.end()) return g_setPitchTramp(mixer, handle, pitch);
    it->second->SetFrequencyRatio(std::min(std::max(pitch, 0.1f), 4.f));
    static bool logFoley = getenv("GHOSTPC_LOGFOLEY") != nullptr;
    static DWORD last;
    if (logFoley && GetTickCount() - last > 1000) last = GetTickCount(), Log("customsound: loop %08x pitch %.2f", handle, pitch);
}
void __fastcall Stop(void *mixer, void *, unsigned handle, unsigned a) {
    if (g_loops.count(handle)) return StopLoop(handle);
    g_stopTramp(mixer, handle, a);
}

// a trampoline for fn's first n bytes (whole instructions, no relative ones) and a jmp from fn to to
void *Detour(uint8_t *fn, const uint8_t *expect, size_t n, void *to) {
    if (!fn || memcmp(fn, expect, n)) return nullptr;
    uint8_t *t = (uint8_t *)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(t, fn, n);
    t[n] = 0xE9;
    *(int32_t *)(t + n + 1) = (int32_t)((fn + n) - (t + n + 5));
    uint8_t jmp[5] = {0xE9};
    *(int32_t *)(jmp + 1) = (int32_t)((uint8_t *)to - (fn + 5));
    WriteCode(fn, jmp, 5);
    return t;
}
}   // namespace

// called every frame: custom loops end when the level changes
void CustomSoundUpdate() {
    extern uint8_t *LevelWorld();
    static uint8_t *last;
    uint8_t *world = LevelWorld();
    if (world == last) return;
    last = world;
    while (!g_loops.empty()) StopLoop(g_loops.begin()->first);
}

// called every frame; installs once the game image is loaded
void CustomSoundInstall() {
    static bool done;
    if (done) return;
    done = true;
    uint8_t *fn = (uint8_t *)SymAddr("?PlayFoleyMode@tObject@@QAEHIPAVtThing@@PAIHM1@Z");
    g_modeName = (ModeNameFn)SymAddr("?gameFoley_GetModeName@@YAPBDI@Z");
    static const uint8_t prologue[] = {0x8b, 0x91, 0x8c, 0x02, 0x00, 0x00};   // mov edx, [ecx + 0x28c]
    if (!fn || memcmp(fn, prologue, 6)) {
        Log("customsound: unexpected tObject::PlayFoleyMode, custom sounds off");
        return;
    }
    LoadSounds();
    if (g_sounds.empty() && !getenv("GHOSTPC_LOGFOLEY")) return;
    uint8_t *t = (uint8_t *)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(t, fn, 6);
    t[6] = 0xE9;
    *(int32_t *)(t + 7) = (int32_t)((fn + 6) - (t + 11));
    g_playTramp = (PlayFoleyFn)t;
    uint8_t jmp[5] = {0xE9};
    *(int32_t *)(jmp + 1) = (int32_t)((uint8_t *)PlayFoleyMode - (fn + 5));
    WriteCode(fn, jmp, 5);
    // cGameMixer::SetPitch: mov eax, [esp+4]; push eax   Stop: mov edx, [esp+4]; mov eax, edx
    static const uint8_t pitchStart[] = {0x8B, 0x44, 0x24, 0x04, 0x50};
    static const uint8_t stopStart[] = {0x8B, 0x54, 0x24, 0x04, 0x8B, 0xC2};
    g_setPitchTramp = (SetPitchFn)Detour((uint8_t *)SymAddr("?SetPitch@cGameMixer@@QAEXIM@Z"), pitchStart, 5, SetPitch);
    g_stopTramp = (StopFn)Detour((uint8_t *)SymAddr("?Stop@cGameMixer@@QAEXII@Z"), stopStart, 6, Stop);
    if (!g_setPitchTramp || !g_stopTramp) Log("customsound: unexpected cGameMixer::SetPitch / Stop, custom loops cannot be pitched or stopped");
}
