// Minimal level scripting for levels that have no compiled mission script (the GhostPC test levels): an optional
// Levels\<level>.gpc next to the level, one command per line ('#' starts a comment):
//
//   notify <x0> <y0> <z0> <x1> <y1> <z1> <ai class> <message>
//   props_start <motion mode name>
//   player_start <x> <y> <z> [yaw]
//   props_player_only
//   unpin <ai class>
//   sunken_los
//   fly_path <ai class> <track> [speed]
//
// When the player enters the box, or as soon as an actor whose AI is <ai class> has been damaged (shot from outside the
// box), cAIInstance::ScriptNotify(message, 0, 0, 0, 0, 0) is sent to every actor whose AI is <ai class> -- what a mission script does to start a boss (cAIBossTanagazj: 101 = phase 1, which also
// targets the player and shows the boss bar; 102/103 = later phases, 200 = end). Sent once it reaches an actor.
//
// props_start: the DEMOANIMS props (they play their next animation each time they are shot) start with the given
// motion mode, or the next one their motion set has. cAIDemoAnims::PlayNextAnimation increments the mode counter
// (this+0x374) and plays the first mode from there that exists; InitInstance starts it at 1, so props open with mode 2
// "run" (gameMotion_Startup: 1 walk, 2 run, 3 stand, 4 idle). Applied to each prop after its first ClassUpdate (see
// StartProps).
//
// props_player_only: DEMOANIMS props move to their next animation only when the player shoots them, not when other
// units' fire hits them (the Scout kept cycling the Reaver out of its stand pose). cAIDemoAnims::Damaged (vtable
// +0x190) is wrapped while a level asks for it.
//
// unpin: releases actors whose AI is <ai class> (tThing::SetPinned(0)) 30 frames after the level loads. The Ultralisk
// boss pins itself in InitInstance and never unpins (its mission script must have); pinned, it still walks, but its
// charge steers its physics velocity (SteerVelocity), which a pinned body ignores, so it froze in the charge.
//
// sunken_los: Sunken Colonies strike only targets they can see. Their tongue attack (ASUpdateAcquired ->
// TongueAttack) reaches anything within their 80 m sight (struck at up to 60 m) with no line-of-sight test, through
// walls into the zoo's hallways; this adds one from 8 m above the colony to 1 m above its target (+0x2D4; a target
// past 60 m is struck 60 m along the way, which can be a visible spot in the room next to an unseen target).
//
// fly_path: starts flyers (cAIBaseFlyer: the Mutalisk) whose AI is <ai class> on a flight path 30 frames after the
// level loads, what a mission script's ScriptActor::AIPlayPath does: cAIBaseFlyer::StartPathMove(track, 0, speed) plays
// the .nad track 3D\Anims\<track>.nad (world coordinates, turned by the actor's facing) as its movement and puts it in
// its follow-path state, the only one the Mutalisk attacks from. A path whose last key matches its first loops.
//
// player_start: the game creates the player at the world origin; this moves her there 5 frames after the level loads,
// so a level can use its origin for something else (the zoo's sniper arena: his animations are in world coordinates).
#include "common.h"
#include <array>
#include <utility>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

extern UINT g_frame;

namespace {
struct Notify {
    float lo[3], hi[3];
    std::string cls;
    unsigned msg;
    bool fired;
};
std::vector<Notify> g_cmds;
uint8_t *g_loadedWorld;
UINT g_loadFrame;
unsigned g_propsMode;     // props_start motion mode (0 = none / applied)
std::vector<uint8_t *> g_propsDone;   // props already restarted (StartProps)
bool g_propsPlayerOnly;   // props_player_only: props cycle only when the player damages them
static bool g_sunkenLOS;   // sunken_los
std::vector<std::string> g_unpin;   // unpin <ai class>: actors to release after the level loads
struct FlyPath {
    std::string cls, track;
    float speed;
};
static std::vector<FlyPath> g_flyPaths;   // fly_path
float g_start[4];         // player_start x y z yaw
bool g_hasStart;
float g_start2[4];        // player2_start x y z yaw: player 2's start (else 1 m beside player 1)
bool g_hasStart2;
std::vector<std::array<float, 4>> g_respawns;   // respawn x y z yaw: competitive revive points (shell.cpp)
uint8_t *g_lastAI;        // last AI instance a notify reached (GHOSTPC_TANAGAZJ_DEBUG)
UINT g_lastAIFrame;

typedef uint8_t *(__cdecl *ActivePlayerFn)();
ActivePlayerFn g_activePlayer;   // resolved in LevelScriptUpdate (no initialised statics next to __try)
uint8_t *GuardedPlayer() {
    __try { return g_activePlayer ? g_activePlayer() : nullptr; } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

uint8_t *CurrentWorld() {
    static uint8_t *seq = (uint8_t *)SymAddr("?mainSequence@@3VtSequence@@A");
    return seq ? *(uint8_t **)(seq + 0x10) : nullptr;    // tSequence +0x10: the loaded tWorld
}

void Load(const char *level) {
    g_cmds.clear();
    g_propsMode = 0;
    g_propsDone.clear();
    g_propsPlayerOnly = false;
    g_sunkenLOS = false;
    g_unpin.clear();
    g_flyPaths.clear();
    g_hasStart = g_hasStart2 = false;
    g_respawns.clear();
    char path[MAX_PATH];
    _snprintf(path, MAX_PATH, "%s\\Levels\\%s.gpc", g_gameDir, level);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (char *c = strchr(line, '#')) *c = 0;
        Notify n = {};
        char cls[128];
        float st[4] = {};
        if (sscanf(line, " player_start %f %f %f %f", &st[0], &st[1], &st[2], &st[3]) >= 3) {
            memcpy(g_start, st, sizeof st);
            g_hasStart = true;
            continue;
        }
        if (sscanf(line, " player2_start %f %f %f %f", &st[0], &st[1], &st[2], &st[3]) >= 3) {
            memcpy(g_start2, st, sizeof st);
            g_hasStart2 = true;
            continue;
        }
        if (sscanf(line, " respawn %f %f %f %f", &st[0], &st[1], &st[2], &st[3]) >= 3) {
            g_respawns.push_back({st[0], st[1], st[2], st[3]});
            continue;
        }
        char track[128];
        float speed = 1.f;
        if (sscanf(line, " fly_path %127s %127s %f", cls, track, &speed) >= 2) {
            g_flyPaths.push_back({cls, track, speed});
            continue;
        }
        if (sscanf(line, " unpin %127s", cls) == 1) {
            g_unpin.push_back(cls);
            continue;
        }
        if (!strncmp(line + strspn(line, " 	"), "sunken_los", 10)) {
            g_sunkenLOS = true;
            continue;
        }
        if (!strncmp(line + strspn(line, " 	"), "props_player_only", 17)) {
            g_propsPlayerOnly = true;
            continue;
        }
        if (sscanf(line, " props_start %127s", cls) == 1) {
            typedef unsigned(__cdecl *MatchFn)(const char *);
            auto match = (MatchFn)SymAddr("?gameMotion_MatchMode@@YA?AW4eMotionMode@@PBD@Z");
            g_propsMode = match ? match(cls) : 0;
            if (!g_propsMode) Log("levelscript: props_start: unknown motion mode %s", cls);
            continue;
        }
        if (sscanf(line, " notify %f %f %f %f %f %f %127s %u", &n.lo[0], &n.lo[1], &n.lo[2], &n.hi[0], &n.hi[1], &n.hi[2],
                   cls, &n.msg) == 8) {
            n.cls = cls;
            g_cmds.push_back(n);
        }
    }
    fclose(f);
    Log("levelscript: %s: %u command(s)%s", path, (unsigned)g_cmds.size(), g_propsMode ? ", props_start" : "");
}

// sends msg to every actor with an AI of the given vtable; returns how many received it
typedef int(__thiscall *ScriptNotifyFn)(void *, unsigned, unsigned, unsigned, float, float, float);
int NotifyAll(uint8_t *world, uint32_t vtable, ScriptNotifyFn notify, unsigned msg, bool follow) {
    int sent = 0;
    __try {
        uint32_t sectors = *(uint32_t *)(world + 0xdc);   // tWorld: sector count, array of 0x110-byte tSectors
        uint8_t *sector = *(uint8_t **)(world + 0xe0);
        for (uint32_t s = 0; s < sectors; s++, sector += 0x110) {
            for (uint8_t *t = *(uint8_t **)(sector + 0x24); t; t = *(uint8_t **)(t + 0x28)) {   // things in sector
                uint8_t *ai = *(uint8_t **)(t + 0x3c4);                                        // tActor AI instance
                if (ai && !IsBadReadPtr(ai, 4) && *(uint32_t *)ai == vtable) {
                    notify(ai, msg, 0, 0, 0.f, 0.f, 0.f);
                    if (follow) g_lastAI = ai;
                    sent++;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("levelscript: fault while walking the sector thing lists");
    }
    return sent;
}
// cAIDemoAnims::Damaged(attacker, source, type, amount, &flags, pos, dir, region, scale): plays the next animation
typedef float(__thiscall *DamagedFn)(void *, void *, int, int, float, void *, void *, void *, int, float);
DamagedFn g_demoDamaged;
float __fastcall DemoAnimsDamaged(void *ai, void *, void *attacker, int src, int type, float amount, void *flags,
                                  void *pos, void *dir, int region, float scale) {
    if (g_propsPlayerOnly && attacker != GuardedPlayer()) return 0.f;   // not the player: keep the pose
    return g_demoDamaged(ai, attacker, src, type, amount, flags, pos, dir, region, scale);
}

// moves the player as cCueRow::WarpPlayerIntoPlace does (tThing::JumpToMatrix; MoveToPos leaves the physics body
// behind). p = x, y, z, yaw in degrees (0 = +Y, 90 = -X); matrix rows right, forward, up, position.
void WarpPlayer(uint8_t *player, const float *p) {
    typedef void(__thiscall *JumpFn)(void *, void *, void *, float *, int);
    float c = cosf(p[3] * 0.0174533f), sn = sinf(p[3] * 0.0174533f);
    float m[16] = {c, sn, 0, 0, -sn, c, 0, 0, 0, 0, 1, 0, p[0], p[1], p[2], 1};
    if (auto jump = (JumpFn)SymAddr("?JumpToMatrix@tThing@@QAEXPAVtWorld@@PAVtSector@@PAUXGMATRIX@@H@Z"))
        jump(player, *(void **)(player + 0x14), *(void **)(player + 0x24), m, 0);
}

// props_start: restart each DEMOANIMS prop's cycle at the given mode once it has had its first ClassUpdate (which plays
// the next animation and sets its next think about an hour ahead, +0x14). AIs far from the player update late, so a
// one-off restart missed the Reaver. Returns how many props are still waiting for their first update.
int StartProps(uint8_t *world, unsigned mode) {
    uint32_t vt = SymAddr("??_7cAIDemoAnims@@6B@");
    typedef void(__thiscall *NextFn)(void *);
    auto next = (NextFn)SymAddr("?PlayNextAnimation@cAIDemoAnims@@QAEXXZ");
    if (!vt || !next) return 0;
    UINT now = *(UINT *)(SymAddr("?stdTime@@3VtStdTime@@A") + 0x44);   // game time (ms)
    int waiting = 0;
    __try {
        uint32_t sectors = *(uint32_t *)(world + 0xdc);
        uint8_t *sector = *(uint8_t **)(world + 0xe0);
        for (uint32_t s = 0; s < sectors; s++, sector += 0x110) {
            for (uint8_t *t = *(uint8_t **)(sector + 0x24); t; t = *(uint8_t **)(t + 0x28)) {
                uint8_t *ai = *(uint8_t **)(t + 0x3c4);
                if (!ai || IsBadReadPtr(ai, 4) || *(uint32_t *)ai != vt) continue;
                bool done = false;
                for (uint8_t *d : g_propsDone) done |= d == ai;
                if (done) continue;
                if ((int)(*(UINT *)(ai + 0x14) - now) < 1000000) {   // first ClassUpdate still to come
                    waiting++;
                    continue;
                }
                *(uint32_t *)(ai + 0x374) = mode - 1;   // PlayNextAnimation increments first
                next(ai);
                g_propsDone.push_back(ai);
                Log("levelscript: props_start: prop %p restarted at motion mode %u", ai, mode);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("levelscript: fault while restarting the props' animations");
    }
    return waiting;
}

// unpin: release every actor whose AI has the given vtable; returns how many
// fly_path: StartPathMove (vtable +0x1C8) on each flyer of the class; returns how many started
static int StartFlyPaths(uint8_t *world, uint32_t vtable, const char *track, float speed) {
    typedef int(__thiscall *StartPathFn)(void *, const char *, const char *, float);
    uint32_t start = SymAddr("?StartPathMove@cAIBaseFlyer@@MAEHPBD0M@Z");
    if (!vtable || !start || ((uint32_t *)vtable)[0x1c8 / 4] != start) return -1;   // not a cAIBaseFlyer
    int n = 0;
    __try {
        uint32_t sectors = *(uint32_t *)(world + 0xdc);
        uint8_t *sector = *(uint8_t **)(world + 0xe0);
        for (uint32_t s = 0; s < sectors; s++, sector += 0x110)
            for (uint8_t *t = *(uint8_t **)(sector + 0x24); t; t = *(uint8_t **)(t + 0x28)) {
                uint8_t *ai = *(uint8_t **)(t + 0x3c4);
                if (ai && !IsBadReadPtr(ai, 4) && *(uint32_t *)ai == vtable && ((StartPathFn)start)(ai, track, nullptr, speed)) n++;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}
int UnpinActors(uint8_t *world, uint32_t vtable) {
    typedef void(__thiscall *PinFn)(void *, int);
    auto setPinned = (PinFn)SymAddr("?SetPinned@tThing@@QAEXH@Z");
    if (!setPinned) return 0;
    int n = 0;
    __try {
        uint32_t sectors = *(uint32_t *)(world + 0xdc);
        uint8_t *sector = *(uint8_t **)(world + 0xe0);
        for (uint32_t s = 0; s < sectors; s++, sector += 0x110)
            for (uint8_t *t = *(uint8_t **)(sector + 0x24); t; t = *(uint8_t **)(t + 0x28)) {
                uint8_t *ai = *(uint8_t **)(t + 0x3c4);
                if (ai && !IsBadReadPtr(ai, 4) && *(uint32_t *)ai == vtable) setPinned(t, 0), n++;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

// true when an actor with an AI of the given vtable is below its maximum health (tActor +0x36C health, +0x370 maximum,
// as tActor::Damage uses them)
bool AnyDamaged(uint8_t *world, uint32_t vtable) {
    __try {
        uint32_t sectors = *(uint32_t *)(world + 0xdc);
        uint8_t *sector = *(uint8_t **)(world + 0xe0);
        for (uint32_t s = 0; s < sectors; s++, sector += 0x110) {
            for (uint8_t *t = *(uint8_t **)(sector + 0x24); t; t = *(uint8_t **)(t + 0x28)) {
                uint8_t *ai = *(uint8_t **)(t + 0x3c4);
                if (ai && !IsBadReadPtr(ai, 4) && *(uint32_t *)ai == vtable &&
                    *(float *)(t + 0x36c) < *(float *)(t + 0x370))
                    return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}
}   // namespace
// for shell.cpp (competitive respawns): the level script's respawn points and the player warp
const std::vector<std::array<float, 4>> &LevelRespawns() { return g_respawns; }
void LevelWarpPlayer(uint8_t *player, const float *p) { WarpPlayer(player, p); }
uint8_t *LevelWorld() { return CurrentWorld(); }

// cAIBossTanagazj was never registered in this build and has two bugs, fixed here when it is registered:
//  1. it overrides HasShield (vtable +0x53C) but not GetShield (+0x538), so cAIInstance::UpdateShield used the base
//     class's shield pointer and crashed in cProtossShield::Update. Its shield is created at this+0x5AC (InitInstance),
//     like cAIProtossZealot::GetShield returning this+0x580.
//  2. PreUpdateAIState calls cAIInstance::UpdateAIState as its "base" call, which calls PreUpdateAIState again
//     (endless recursion, stack overflow); the base class it derives from is cAIBaseWalker::PreUpdateAIState.
static __declspec(naked) void TanagazjGetShield() {
    __asm lea eax, [ecx + 0x5ac]
    __asm ret
}
static void WriteCode(void *at, const void *bytes, size_t n) {
    DWORD old;
    // the page can also hold code or data of other sections (Xbox images pack them), so keep it executable
    VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
}
static void FixTanagazjClass() {
    uint32_t *vt = (uint32_t *)SymAddr("??_7cAIBossTanagazj@@6B@");
    uint8_t *pre = (uint8_t *)SymAddr("?PreUpdateAIState@cAIBossTanagazj@@UAEXXZ");
    uint8_t *walkerPre = (uint8_t *)SymAddr("?PreUpdateAIState@cAIBaseWalker@@MAEXXZ");
    uint8_t *update = (uint8_t *)SymAddr("?UpdateAIState@cAIInstance@@UAEXXZ");
    if (!vt || !pre || !walkerPre || !update) return;
    uint32_t getShield = (uint32_t)TanagazjGetShield;
    WriteCode(&vt[0x538 / 4], &getShield, 4);
    uint8_t *call = pre + 4;   // push ecx; push esi; mov esi, ecx; call cAIInstance::UpdateAIState
    if (call[0] == 0xE8 && call + 5 + *(int32_t *)(call + 1) == update) {
        int32_t rel = (int32_t)(walkerPre - (call + 5));
        WriteCode(call + 1, &rel, 4);
    } else {
        Log("levelscript: unexpected code in cAIBossTanagazj::PreUpdateAIState, not patched");
    }
    // Debug (GHOSTPC_DEBUG_TANAGAZJ_SHIMMER): his UpdateCloak hides him outright while cloaked (tThing +0x48 bit 2,
    // which tThing::Draw skips) unless the player has thermal sight; skip that write to see the cloak shader draw him
    if (getenv("GHOSTPC_DEBUG_TANAGAZJ_SHIMMER")) {
        uint8_t *upd = (uint8_t *)SymAddr("?UpdateCloak@cAIBossTanagazj@@QAEXXZ");
        static const uint8_t orHide[] = {0x83, 0x4e, 0x48, 0x02}, nops[] = {0x90, 0x90, 0x90, 0x90};
        if (upd && !memcmp(upd + 0xc8, orHide, 4)) WriteCode(upd + 0xc8, nops, 4);
        else Log("levelscript: unexpected code in cAIBossTanagazj::UpdateCloak");
    }
    // His blade hit (BladeHitCheck: MeleeHitCheck along the blade, markers 3 -> 5) does 10 damage, a third of a plain
    // Zealot's blade (cAIProtossZealot::BladeHitCheck: 30); placeholder tuning of the unfinished boss. Raised to 80
    // (the user's choice for the boss).
    uint8_t *hit = (uint8_t *)SymAddr("?BladeHitCheck@cAIBossTanagazj@@QAEHXZ");
    const float bladeDamage = 80.f;
    if (hit && hit[0x19] == 0x68 && *(uint32_t *)(hit + 0x1a) == 0x41200000)   // push 10.0f
        WriteCode(hit + 0x1a, &bladeDamage, 4);
    else
        Log("levelscript: unexpected code in cAIBossTanagazj::BladeHitCheck, damage not patched");
}

// The Probe (template AI PROTOSSPROBE = cAIDetector, a cAIBaseFlyer; PROTOSSPROBEOLD/PROTOSSPROBE2 are older walker
// versions) follows Nova and aims its desired yaw at her (cAIBaseFlyer::Face, +0x3A4), but never turns:
// cAIBaseFlyer::UpdateFacing only turns with AI flag 0x10000 (+0xC) at the turn rate +0x3A8, and cAIDetector, unlike
// cAIProtossObserver::Clear (flag 0x10000, 300 degrees/s), sets neither. Its InitInstance (vtable +0x20) gets them.
typedef void(__thiscall *InitInstanceFn)(void *);
static InitInstanceFn g_detectorInit;
static void __fastcall DetectorInitInstance(uint8_t *ai, void *) {
    g_detectorInit(ai);
    *(uint32_t *)(ai + 0xc) |= 0x10000;
    if (*(float *)(ai + 0x3a8) <= 0.f) *(float *)(ai + 0x3a8) = 300.f;
}
static void FixDetectorClass() {
    uint32_t *vt = (uint32_t *)SymAddr("??_7cAIDetector@@6B@");
    uint32_t init = SymAddr("?InitInstance@cAIDetector@@UAEXXZ");
    if (!vt || !init || vt[0x20 / 4] != init) {
        Log("levelscript: unexpected cAIDetector vtable, Probe facing not fixed");
        return;
    }
    g_detectorInit = (InitInstanceFn)init;
    uint32_t wrapper = (uint32_t)DetectorInitInstance;
    WriteCode(&vt[0x20 / 4], &wrapper, 4);
}

// The level script is found by the loaded level's name. mainOptions_aLevelName only holds it for "-level" starts;
// levels started from the menu (the mission list) left it empty, so their .gpc never loaded (no triggers, no
// player_start). gameWorld_LoadWorld(name, ...) gets the name either way: a pass-through detour records it.
static char g_worldName[MAX_PATH];
static uint8_t *g_loadWorldTramp;
static UINT g_worldLoads;   // counts gameWorld_LoadWorld calls: a restart can reuse the same tWorld object
static void __cdecl RecordWorldName(const char *name) {
    if (!name) return;
    const char *base = name;
    for (const char *c = name; *c; c++)
        if (*c == '\\' || *c == '/' || *c == ':') base = c + 1;
    strncpy(g_worldName, base, MAX_PATH - 1);
    g_worldLoads++;
    if (char *dot = strrchr(g_worldName, '.')) *dot = 0;
}
static __declspec(naked) void LoadWorldStub() {
    __asm push dword ptr [esp + 4]
    __asm call RecordWorldName
    __asm add esp, 4
    __asm jmp dword ptr [g_loadWorldTramp]
}
static void HookLoadWorld() {
    uint8_t *fn = (uint8_t *)SymAddr("?gameWorld_LoadWorld@@YAPAVtWorld@@PBDI@Z");
    static const uint8_t prologue[] = {0x81, 0xec, 0x2c, 0x02, 0x00, 0x00};   // sub esp, 0x22c
    if (!fn || memcmp(fn, prologue, 6)) {
        Log("levelscript: unexpected gameWorld_LoadWorld, level names from -level only");
        return;
    }
    uint8_t *t = (uint8_t *)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(t, fn, 6);
    t[6] = 0xE9;
    *(int32_t *)(t + 7) = (int32_t)((fn + 6) - (t + 11));
    g_loadWorldTramp = t;
    uint8_t jmp[5] = {0xE9};
    *(int32_t *)(jmp + 1) = (int32_t)((uint8_t *)LoadWorldStub - (fn + 5));
    WriteCode(fn, jmp, 5);
}
// Restarting the level after dying (tSequence::DoRestartSinglePlayer) does not load the world again: it restores the
// snapshot taken at the level start (cSaveData::Restore), with the player back at the world origin and the AIs as
// they were (pinned, untriggered). Counted as a load, so the script runs again (player_start, triggers, unpin).
static uint8_t *g_restoreTramp;
static void __cdecl CountRestore() { g_worldLoads++; }
static __declspec(naked) void RestoreStub() {
    __asm push ecx
    __asm call CountRestore
    __asm pop ecx
    __asm jmp dword ptr [g_restoreTramp]
}
static void HookSaveRestore() {
    uint8_t *fn = (uint8_t *)SymAddr("?Restore@cSaveData@@QAEXXZ");
    static const uint8_t prologue[] = {0x83, 0xec, 0x40, 0x53, 0x56};   // sub esp, 0x40; push ebx; push esi
    if (!fn || memcmp(fn, prologue, 5)) {
        Log("levelscript: unexpected cSaveData::Restore, restarts keep the level script state");
        return;
    }
    uint8_t *t = (uint8_t *)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(t, fn, 5);
    t[5] = 0xE9;
    *(int32_t *)(t + 6) = (int32_t)((fn + 5) - (t + 10));
    g_restoreTramp = t;
    uint8_t jmp[5] = {0xE9};
    *(int32_t *)(jmp + 1) = (int32_t)((uint8_t *)RestoreStub - (fn + 5));
    WriteCode(fn, jmp, 5);
}

// props_player_only support: wrap cAIDemoAnims::Damaged (vtable +0x190); it only filters while a level asks for it
static void HookDemoAnimsDamaged() {
    uint32_t *vt = (uint32_t *)SymAddr("??_7cAIDemoAnims@@6B@");
    uint32_t dmg = SymAddr("?Damaged@cAIDemoAnims@@UAEMPAVtThing@@W4eDamageSource@@W4eDamageType@@MAAIPAUXGVECTOR3@@4W4eHitRegionType@@M@Z");
    if (!vt || !dmg || vt[0x190 / 4] != dmg) {
        Log("levelscript: unexpected cAIDemoAnims vtable, props_player_only unavailable");
        return;
    }
    g_demoDamaged = (DamagedFn)dmg;
    uint32_t wrapper = (uint32_t)DemoAnimsDamaged;
    WriteCode(&vt[0x190 / 4], &wrapper, 4);
}

// Tanagazj draws his blade only from ASUpdateAcquired/ASUpdateCombatRanged (phase 3); fights that stay in his other
// combat states (e.g. Nova close when the fight starts) never reached them, so he fought bare-handed. While he is in
// phase 3 (+0x594) with a target (+0x2D4) and no blade (+0x548), outside his idle state (2, which puts it away),
// BladeOn is called.
static void KeepTanagazjBlade(uint8_t *world) {
    static uint32_t vt;
    typedef void(__thiscall *BladeFn)(void *);
    static BladeFn bladeOn;
    if (!vt) vt = SymAddr("??_7cAIBossTanagazj@@6B@"), bladeOn = (BladeFn)SymAddr("?BladeOn@cAIBossTanagazj@@QAEXXZ");
    if (!vt || !bladeOn) return;
    __try {
        uint32_t sectors = *(uint32_t *)(world + 0xdc);
        uint8_t *sector = *(uint8_t **)(world + 0xe0);
        for (uint32_t i = 0; i < sectors; i++, sector += 0x110)
            for (uint8_t *t = *(uint8_t **)(sector + 0x24); t; t = *(uint8_t **)(t + 0x28)) {
                uint8_t *ai = *(uint8_t **)(t + 0x3c4);
                if (ai && !IsBadReadPtr(ai, 4) && *(uint32_t *)ai == vt && *(int *)(ai + 0x594) == 3 &&
                    *(uint32_t *)(ai + 0x2d4) && !*(int *)(ai + 0x548) && *(int *)(ai + 0x18) != 2)   // 2 = idle
                    bladeOn(ai);
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The Ultralisk boss only leaves CHASE for AQUIRED (where it picks its melee/charge/gore attacks) when LOSCheck
// succeeds: a ray (simCollision_LineOfSight) from model marker type 4 (its head) to the target's feet. ultralisk.nod in
// this build has no markers, so the ray started at the fallback, 1 m above its origin: inside its own 16 x 7 m body.
// It never saw Nova, chased her while standing on top of her and never attacked. LOSCheck is replaced: the ray starts
// 5 m ahead of its origin along its facing (tThing +0x90; the model's front is 4.5 m ahead) at 3 m, and ends 1 m above
// the target's feet. Same result convention (1 = clear).
typedef int(__cdecl *LineOfSightFn)(float *, float *, int);
static LineOfSightFn g_lineOfSight;
static int __fastcall UltraliskLOSCheck(uint8_t *ai, void *, uint8_t *target) {
    if (!target || !g_lineOfSight) return 0;
    uint8_t *thing = *(uint8_t **)(ai + 8);
    const float *p = (const float *)(thing + 0xb0), *f = (const float *)(thing + 0x90), *t = (const float *)(target + 0xb0);
    float from[3] = {p[0] + f[0] * 5.f, p[1] + f[1] * 5.f, p[2] + 3.f}, to[3] = {t[0], t[1], t[2] + 1.f};
    return g_lineOfSight(from, to, 4) == 0;
}
static void FixUltraliskLOS() {
    uint8_t *los = (uint8_t *)SymAddr("?LOSCheck@cAIBossUltralisk@@QAEHPAVtThing@@@Z");
    g_lineOfSight = (LineOfSightFn)SymAddr("?simCollision_LineOfSight@@YAHPAUXGVECTOR3@@0W4eRaycastDbgID@@@Z");
    static const uint8_t prologue[] = {0x8b, 0x44, 0x24, 0x04};   // mov eax, [esp + 4]
    if (!los || !g_lineOfSight || memcmp(los, prologue, 4)) {
        Log("levelscript: unexpected code in cAIBossUltralisk::LOSCheck, not patched");
        return;
    }
    uint8_t jmp[5] = {0xE9};
    *(int32_t *)(jmp + 1) = (int32_t)((uint8_t *)UltraliskLOSCheck - (los + 5));
    WriteCode(los, jmp, 5);
}

typedef float(__thiscall *DamageFn)(void *, void *, int, int, float, float *, float *, unsigned, int, float, int *);
static DamageFn g_damage;   // tActor::Damage, resolved on first use (no initialised statics next to __try)
// Its tusk attack (AnimationCallback: MeleeHitCheck along model markers 3 -> 5, 25 damage) needs markers the model
// lacks, so it never hit (only the stomp's knockback did). That call is replaced by a sphere check like the Zealot's
// blade: the target within 4 m of a point 5 m ahead of the Ultralisk (its head, ~2 m up) takes 100 damage (the user's choice; the game passes 25).
// Returns 0, so the game skips its own hit handling (foley, depot damage via the hit thing).
static int __fastcall UltraliskTuskHit(uint8_t *ai, void *, int, int, int, int, int src, int type, float damage, int,
                                       void *, void *) {
    uint8_t *thing = *(uint8_t **)(ai + 8), *player = GuardedPlayer();
    if (!player || !g_damage) return 0;
    const float *p = (const float *)(thing + 0xb0), *f = (const float *)(thing + 0x90), *t = (const float *)(player + 0xb0);
    float hx = p[0] + f[0] * 5.f - t[0], hy = p[1] + f[1] * 5.f - t[1], hz = p[2] + 2.f - (t[2] + 1.f);
    if (hx * hx + hy * hy + hz * hz > 16.f) return 0;
    // the attack's hit tag comes on several consecutive frames: one hit per swing
    static UINT lastHit;
    UINT now = *(UINT *)(SymAddr("?stdTime@@3VtStdTime@@A") + 0x44);   // game time (ms)
    if (lastHit && now - lastHit < 600) return 0;
    lastHit = now;
    float dir[3] = {f[0], f[1], f[2]};
    g_damage(player, thing, src, type, 100.f, (float *)t, dir, 0, 0, 1.f, nullptr);   // the user: 100 (game: 25)
    return 0;
}
static void FixUltraliskTusks() {
    uint8_t *cb = (uint8_t *)SymAddr("?AnimationCallback@cAIBossUltralisk@@UAEXPAVcKeyChannel@@W4eRenderKeyTag@@I@Z");
    uint8_t *melee = (uint8_t *)SymAddr("?MeleeHitCheck@cAIInstance@@QAEHEGEGW4eDamageSource@@W4eDamageType@@MIPAUXGVECTOR3@@PAVSimpleHit@@@Z");
    if (!g_damage)
        g_damage = (DamageFn)SymAddr(
            "?Damage@tActor@@QAEMPAVtThing@@W4eDamageSource@@W4eDamageType@@MPAUXGVECTOR3@@3IW4eHitRegionType@@MPAH@Z");
    uint8_t *call = cb ? cb + 0x20e : nullptr;   // call MeleeHitCheck (0x4F3F6E in the original image)
    if (!call || !melee || !g_damage || call[0] != 0xE8 || call + 5 + *(int32_t *)(call + 1) != melee) {
        Log("levelscript: unexpected code in cAIBossUltralisk::AnimationCallback, tusks not patched");
        return;
    }
    int32_t rel = (int32_t)((uint8_t *)UltraliskTuskHit - (call + 5));
    WriteCode(call + 1, &rel, 4);
}

// Its charge (controller mode SPECIAL1: wind-up, then stage +0x584 == 2 for 1.5 s) moves it with
// tThing::SteerVelocity(direction * speed (+0x37C) * 1.5), which does not move it here (the walker's own movement owns
// the body), so it charged in place at range and never closed in. While a charge is in stage 2 it is moved towards
// its target at that speed, the way tObject::MoveRootWorld moves things (SetPhysPos + MoveToPos), stopping 5 m short.
static void DriveUltraliskCharge(uint8_t *world) {
    static uint32_t vt;
    typedef uint8_t *(__cdecl *FindFn)(unsigned);
    typedef void(__thiscall *PosFn)(void *, const float *);
    static FindFn find;
    static PosFn physPos, moveTo;
    static UINT lastTime;
    if (!vt) {
        vt = SymAddr("??_7cAIBossUltralisk@@6B@");
        find = (FindFn)SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
        physPos = (PosFn)SymAddr("?SetPhysPos@tThing@@QAEXPBUXGVECTOR3@@@Z");
        moveTo = (PosFn)SymAddr("?MoveToPos@tThing@@QAEXPBUXGVECTOR3@@@Z");
    }
    UINT now = *(UINT *)(SymAddr("?stdTime@@3VtStdTime@@A") + 0x44);   // game time (ms)
    float dt = lastTime && now > lastTime && now - lastTime < 200 ? (now - lastTime) / 1000.f : 0.f;
    lastTime = now;
    if (!vt || !find || !physPos || !moveTo || dt <= 0) return;
    __try {
        uint32_t sectors = *(uint32_t *)(world + 0xdc);
        uint8_t *sector = *(uint8_t **)(world + 0xe0);
        for (uint32_t i = 0; i < sectors; i++, sector += 0x110)
            for (uint8_t *t = *(uint8_t **)(sector + 0x24); t; t = *(uint8_t **)(t + 0x28)) {
                uint8_t *ai = *(uint8_t **)(t + 0x3c4);
                if (!ai || IsBadReadPtr(ai, 4) || *(uint32_t *)ai != vt || *(int *)(ai + 0x584) != 2) continue;
                uint8_t *target = find(*(unsigned *)(ai + 0x2d4));
                if (!target) continue;
                const float *p = (const float *)(t + 0xb0), *q = (const float *)(target + 0xb0);
                float dx = q[0] - p[0], dy = q[1] - p[1], d = sqrtf(dx * dx + dy * dy);
                float step = *(float *)(t + 0x37c) * 1.5f * dt;
                if (d < 5.f + step) continue;
                float np[3] = {p[0] + dx / d * step, p[1] + dy / d * step, p[2]};
                physPos(t, np);
                moveTo(t, np);
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Its stomp (COMBATRANGED: the ground attack, attack2) could repeat every 2 s (StopAttackRanged: next allowed at
// now + 2000 ms) and only knocked the target back: AnimationCallback hits a target on the ground (within 0.5 m of his
// height, at any distance) with tActor::Knockback (+0x564 set) or tActor::Hurlback (+0x56C set), no damage.
// The user's tuning: a 5 s cooldown and 20 damage with either.
typedef int(__thiscall *KnockbackFn)(void *, float *, int, int);
static KnockbackFn g_knockback;
static int __fastcall UltraliskStompKnockback(uint8_t *target, void *, float *from, int a, int b) {
    int r = g_knockback(target, from, a, b);
    if (g_damage && from) {   // from = the Ultralisk's position (tThing +0xB0)
        uint8_t *ultra = (uint8_t *)from - 0xb0;
        float dir[3] = {0, 0, 1};
        g_damage(target, ultra, 1, 0, 20.f, (float *)(target + 0xb0), dir, 0, 0, 1.f, nullptr);
    }
    return r;
}
typedef int(__thiscall *HurlbackFn)(void *, void *, float, float *, float, unsigned);
static HurlbackFn g_hurlback;
static int __fastcall UltraliskStompHurlback(uint8_t *target, void *, uint8_t *ultra, float force, float *dir, float up,
                                             unsigned flags) {
    int r = g_hurlback(target, ultra, force, dir, up, flags);
    if (g_damage && ultra) {
        float d[3] = {0, 0, 1};
        g_damage(target, ultra, 1, 0, 20.f, (float *)(target + 0xb0), d, 0, 0, 1.f, nullptr);
    }
    return r;
}
static void FixUltraliskStomp() {
    uint8_t *stop = (uint8_t *)SymAddr("?StopAttackRanged@cAIBossUltralisk@@UAEIXZ");
    uint8_t *cb = (uint8_t *)SymAddr("?AnimationCallback@cAIBossUltralisk@@UAEXPAVcKeyChannel@@W4eRenderKeyTag@@I@Z");
    g_knockback = (KnockbackFn)SymAddr("?Knockback@tActor@@QAEHPAUXGVECTOR3@@HH@Z");
    if (stop && stop[5] == 0x05 && *(uint32_t *)(stop + 6) == 2000) {   // add eax, 2000
        const uint32_t cooldown = 5000;
        WriteCode(stop + 6, &cooldown, 4);
    } else
        Log("levelscript: unexpected code in cAIBossUltralisk::StopAttackRanged, stomp cooldown not patched");
    uint8_t *call = cb ? cb + 0x117 : nullptr;   // call tActor::Knockback (0x4F3E77 in the original image)
    if (call && g_knockback && call[0] == 0xE8 && call + 5 + *(int32_t *)(call + 1) == (uint8_t *)g_knockback) {
        int32_t rel = (int32_t)((uint8_t *)UltraliskStompKnockback - (call + 5));
        WriteCode(call + 1, &rel, 4);
    } else
        Log("levelscript: unexpected code in cAIBossUltralisk::AnimationCallback, stomp damage not patched");
    g_hurlback = (HurlbackFn)SymAddr("?Hurlback@tActor@@QAEHPAVtThing@@MPAUXGVECTOR3@@MI@Z");
    call = cb ? cb + 0x13f : nullptr;   // call tActor::Hurlback (0x4F3E9F in the original image)
    if (call && g_hurlback && call[0] == 0xE8 && call + 5 + *(int32_t *)(call + 1) == (uint8_t *)g_hurlback) {
        int32_t rel = (int32_t)((uint8_t *)UltraliskStompHurlback - (call + 5));
        WriteCode(call + 1, &rel, 4);
    } else
        Log("levelscript: unexpected code in cAIBossUltralisk::AnimationCallback, stomp hurlback damage not patched");
}

// Plays foley mode <mode> (by name) of <obj>'s foley set at <at> (or <obj>): through tObject::PlayFoleyMode, so the
// custom sounds (customsound.cpp) apply.
static void PlayFoley(void *obj, const char *mode, void *at = nullptr) {
    typedef int(__thiscall *PlayFoleyFn)(void *, unsigned, void *, unsigned *, int, float, void *);
    typedef int(__cdecl *MatchFn)(const char *);
    static PlayFoleyFn play;
    static MatchFn match;
    if (!play) play = (PlayFoleyFn)SymAddr("?PlayFoleyMode@tObject@@QAEHIPAVtThing@@PAIHM1@Z");
    if (!match) match = (MatchFn)SymAddr("?gameFoley_MatchMode@@YAHPBD@Z");
    int m = obj && play && match ? match(mode) : -1;
    if (m >= 0) play(obj, m, at, nullptr, 0, 1.f, nullptr);
}
// Finds a virtual function in a vtable and puts a wrapper in its slot; returns the original (0 if not found).
static uint32_t WrapVirtual(const char *vtable, const char *fn, void *wrapper) {
    uint32_t *vt = (uint32_t *)SymAddr(vtable), f = SymAddr(fn);
    for (int i = 0; vt && f && i < 0x400; i++)
        if (vt[i] == f) {
            uint32_t w = (uint32_t)wrapper;
            WriteCode(&vt[i], &w, 4);
            return f;
        }
    Log("levelscript: %s not in %s", fn, vtable);
    return 0;
}

// The Sunken Colony's tongue attack (cAIZergSunkenColony): CMStartSpecial1 plays its attack animation and schedules the
// spike at +0x554 (now + 50 ms per metre to the target); CMUpdateSpecial1 fires proj_SunkenColony at the target point
// once that time passes and clears +0x554 to 0. But "time passed" is checked as now > +0x554, which 0 also passes: it
// fired again on every update until the animation ended (several 150-damage spikes per attack). The wrapper skips the
// update while the spike is spent and the animation still runs (once it ends the original leaves the attack).
// The spike (sunkencolonySpike.nod, hanging 36 m below its origin) rises by its "stand" animation's root bone (up
// 15.66 m at frames 11-14, held to 20, back down by 23, 30 fps), but root motion is taken out of the pose for the
// thing to move by and a projectile never moves by it: the spike stayed under the floor, only its dust burst showed.
// DriveSunkenSpikes moves each spike along that root track itself. The spike was also invisible at any height: see
// FixStillProjectiles. And the attack was silent: nothing plays the Sunken's foley (zergSunkenClny: swing, hitdirt,
// hitflesh, ...) for it, so the colony plays "swing" when its attack starts (CMStartSpecial1) and "hitdirt" at the spike
// (from the colony's set) when the spike comes up.
typedef void(__thiscall *UpdateFn)(void *);
static UpdateFn g_sunkenUpdate;
static void __fastcall SunkenUpdateSpecial1(uint8_t *ai) {
    UINT now = *(UINT *)(SymAddr("?stdTime@@3VtStdTime@@A") + 0x44);
    if (*(UINT *)(ai + 0x554) == 0 && now <= *(UINT *)(ai + 0x550)) return;   // spike spent, animation still running
    g_sunkenUpdate(ai);
}
struct SunkenSpike {
    unsigned guid;
    UINT start;
    float base[3];
};
static SunkenSpike g_spikes[16];
typedef uint8_t *(__cdecl *FireTemplateFn)(void *, void *, void *, unsigned, int, int, void *, int);
static FireTemplateFn g_fireTemplate;
static uint8_t *__cdecl SunkenFireSpike(void *tmpl, void *mat, void *owner, unsigned guid, int a, int b, void *hit, int region) {
    uint8_t *p = g_fireTemplate(tmpl, mat, owner, guid, a, b, hit, region);
    if (p && owner) PlayFoley(owner, "hitdirt", p);
    if (p) {
        static int next;
        SunkenSpike &s = g_spikes[next++ % 16];
        s.guid = *(unsigned *)(p + 0x34);
        s.start = *(UINT *)(SymAddr("?stdTime@@3VtStdTime@@A") + 0x44);
        memcpy(s.base, p + 0xb0, 12);
    }
    return p;
}
typedef void(__thiscall *ThingPosFn)(void *, const float *);
static bool GuardedMoveTo(ThingPosFn moveTo, uint8_t *thing, const float *pos) {
    __try {
        moveTo(thing, pos);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static void DriveSunkenSpikes() {
    typedef uint8_t *(__cdecl *FindFn)(unsigned);
    static FindFn find;
    static ThingPosFn moveTo;
    if (!find) find = (FindFn)SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    if (!moveTo) moveTo = (ThingPosFn)SymAddr("?MoveToPos@tThing@@QAEXPBUXGVECTOR3@@@Z");
    if (!find || !moveTo) return;
    UINT now = *(UINT *)(SymAddr("?stdTime@@3VtStdTime@@A") + 0x44);
    for (auto &s : g_spikes) {
        if (!s.guid) continue;
        // animation frame (30 fps), from frame 11 where the rise begins: its damage (sunkColDam) hits at once, and the
        // 0.37 s lead-in left the tentacle coming up after the damage
        float f = (now - s.start) * 0.03f + 11.f;
        uint8_t *p = find(s.guid);
        if (!p || f > 24.f) {
            s.guid = 0;
            continue;
        }
        float up = f < 11.f ? 0.f : f < 14.f ? 15.66f * (f - 11.f) / 3.f : f < 20.f ? 15.66f
                 : f < 23.f ? 15.66f * (23.f - f) / 3.f : 0.f;
        float pos[3] = {s.base[0], s.base[1], s.base[2] + up};
        if (!GuardedMoveTo(moveTo, p, pos)) s.guid = 0;
        static bool dbg = getenv("GHOSTPC_SUNKEN_DEBUG") != nullptr;
        if (dbg)
            Log("sunken-dbg: spike %p frame %.1f up %.2f pos %.2f %.2f %.2f flags48 %08x model %p rot %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f", p, f, up, ((float *)(p + 0xb0))[0],
                ((float *)(p + 0xb0))[1], ((float *)(p + 0xb0))[2], *(uint32_t *)(p + 0x48), *(void **)(p + 0x2a0), ((float *)(p + 0x80))[0], ((float *)(p + 0x80))[1],
                ((float *)(p + 0x80))[2], ((float *)(p + 0x90))[0], ((float *)(p + 0x90))[1], ((float *)(p + 0x90))[2],
                ((float *)(p + 0xa0))[0], ((float *)(p + 0xa0))[1], ((float *)(p + 0xa0))[2]);
    }
}
// tProjectile::PrePhysicsUpdateProjectile turns every projectile to face along its velocity each update
// (tThing::LookAt3d / LookDir3d with the normalized velocity). A projectile that stands still (velocity 0 0 0, like the
// Sunken Colony's spike) got the normalized zero vector: an all-zero rotation, so its model was drawn collapsed to a
// point. The two calls skip a zero (or NaN) direction and keep the orientation it was fired with.
typedef void(__thiscall *LookFn)(void *, float *);
static LookFn g_lookAt, g_lookDir;
static bool UsableDir(const float *v) {
    float l = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    return l > 1e-6f && l < 1e6f;   // false for NaN too
}
static void __fastcall ProjectileLookAt(void *thing, void *, float *v) { if (UsableDir(v)) g_lookAt(thing, v); }
static void __fastcall ProjectileLookDir(void *thing, void *, float *v) { if (UsableDir(v)) g_lookDir(thing, v); }
static void FixStillProjectiles() {
    uint8_t *fn = (uint8_t *)SymAddr("?PrePhysicsUpdateProjectile@tProjectile@@QAEXXZ");
    g_lookAt = (LookFn)SymAddr("?LookAt3d@tThing@@QAEXPAUXGVECTOR3@@@Z");
    g_lookDir = (LookFn)SymAddr("?LookDir3d@tThing@@QAEXPAUXGVECTOR3@@@Z");
    struct { int off; void *target, *wrapper; } calls[] = {{0x29d, (void *)g_lookAt, (void *)ProjectileLookAt},
                                                           {0x4e7, (void *)g_lookDir, (void *)ProjectileLookDir}};
    for (auto &c : calls) {
        uint8_t *call = fn ? fn + c.off : nullptr;
        if (!call || !c.target || call[0] != 0xE8 || call + 5 + *(int32_t *)(call + 1) != (uint8_t *)c.target) {
            Log("levelscript: unexpected code in tProjectile::PrePhysicsUpdateProjectile, still projectiles not fixed");
            continue;
        }
        int32_t rel = (int32_t)((uint8_t *)c.wrapper - (call + 5));
        WriteCode(call + 1, &rel, 4);
    }
}
typedef void(__thiscall *TongueFn)(void *, float *);
static TongueFn g_tongue;
static void __fastcall SunkenTongueAttack(uint8_t *ai, void *, float *at) {
    if (g_sunkenLOS && g_lineOfSight && at) {
        typedef uint8_t *(__cdecl *FindFn)(unsigned);
        static FindFn find;
        if (!find) find = (FindFn)SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
        uint8_t *target = find && *(unsigned *)(ai + 0x2d4) ? find(*(unsigned *)(ai + 0x2d4)) : nullptr;
        const float *t = target ? (const float *)(target + 0xb0) : at;
        const float *p = (const float *)(*(uint8_t **)(ai + 8) + 0xb0);
        float from[3] = {p[0], p[1], p[2] + 8.f}, to[3] = {t[0], t[1], t[2] + 1.f};
        if (g_lineOfSight(from, to, 4) != 0) return;   // blocked
    }
    g_tongue(ai, at);
}
static UpdateFn g_sunkenStart;
static void __fastcall SunkenStartSpecial1(uint8_t *ai) {
    g_sunkenStart(ai);
    PlayFoley(*(void **)(ai + 8), "swing");
}
static void FixSunkenColony() {
    g_sunkenStart = (UpdateFn)WrapVirtual("??_7cAIZergSunkenColony@@6B@", "?CMStartSpecial1@cAIZergSunkenColony@@UAEXXZ",
                                          (void *)SunkenStartSpecial1);
    // sunken_los: the TongueAttack calls in ASUpdateAcquired
    uint8_t *acq = (uint8_t *)SymAddr("?ASUpdateAcquired@cAIZergSunkenColony@@UAEXXZ");
    g_tongue = (TongueFn)SymAddr("?TongueAttack@cAIZergSunkenColony@@QAEXPAUXGVECTOR3@@@Z");
    if (!g_lineOfSight) g_lineOfSight = (LineOfSightFn)SymAddr("?simCollision_LineOfSight@@YAHPAUXGVECTOR3@@0W4eRaycastDbgID@@@Z");
    int tongueCalls = 0;
    for (int i = 0; acq && g_tongue && i < 0x300; i++)
        if (acq[i] == 0xE8 && acq + i + 5 + *(int32_t *)(acq + i + 1) == (uint8_t *)g_tongue) {
            int32_t rel = (int32_t)((uint8_t *)SunkenTongueAttack - (acq + i + 5));
            WriteCode(acq + i + 1, &rel, 4);
            tongueCalls++;
        }
    if (tongueCalls != 2) Log("levelscript: %d TongueAttack calls in cAIZergSunkenColony::ASUpdateAcquired (2 expected)", tongueCalls);
    uint32_t *vt = (uint32_t *)SymAddr("??_7cAIZergSunkenColony@@6B@");
    uint32_t upd = SymAddr("?CMUpdateSpecial1@cAIZergSunkenColony@@UAEXXZ");
    int slot = -1;
    for (int i = 0; vt && upd && i < 0x400 && slot < 0; i++)
        if (vt[i] == upd) slot = i;
    if (slot < 0) {
        Log("levelscript: cAIZergSunkenColony::CMUpdateSpecial1 not in its vtable, repeated spikes not fixed");
    } else {
        g_sunkenUpdate = (UpdateFn)upd;
        uint32_t wrapper = (uint32_t)SunkenUpdateSpecial1;
        WriteCode(&vt[slot], &wrapper, 4);
    }
    uint8_t *call = upd ? (uint8_t *)upd + 0xfd : nullptr;   // call simCombat_FireTemplateProjectile (0x4E64ED originally)
    g_fireTemplate = (FireTemplateFn)SymAddr(
        "?simCombat_FireTemplateProjectile@@YAPAVtProjectile@@PAVtTemplate@@PAUXGMATRIX@@PAVtThing@@IHHPAVSimpleHit@@W4eHitRegionType@@@Z");
    if (call && g_fireTemplate && call[0] == 0xE8 && call + 5 + *(int32_t *)(call + 1) == (uint8_t *)g_fireTemplate) {
        int32_t rel = (int32_t)((uint8_t *)SunkenFireSpike - (call + 5));
        WriteCode(call + 1, &rel, 4);
    } else
        Log("levelscript: unexpected code in cAIZergSunkenColony::CMUpdateSpecial1, spike not raised");
}

// The Lurker boss (cAIBossLurker) plays no foley but its footsteps: its bank (zergLurker) has nothing else and its code
// asks for nothing else. Its events play these modes (custom sounds, Sounds\custom\zergLurker):
//   vox_alert  its roar (call for help: CMStartSpecial4, controller mode 51), 3 s in when it rears up (the user's
//              timing; the animation's own call-for-help tag comes later)
//   vox_death  when killed
//   special1   burrowing (CMStartBurrow), special2 coming up (CMStartUnburrow, and its burst attack out of the ground,
//              CMStartSpecial1, which spawns lurkerunburrow; StarCraft uses one sound for both)
//   swing      firing a spike (FireSpike, from its AnimationCallback), hitdirt where the spike ends (hit or expired)
static UpdateFn g_lurkerBurrow, g_lurkerUnburrow, g_lurkerFireSpike;
static UpdateFn g_lurkerRoar;
static struct { uint8_t *ai; UINT at; } g_lurkerRoars[4];
typedef void(__thiscall *KilledFn)(void *, void *, int, int, float, unsigned, void *, void *, int, float, int *);
static KilledFn g_lurkerKilled;
static struct { unsigned guid; uint8_t *owner; float pos[3]; bool seen, hit; } g_lurkerSpikes[8];
static void __fastcall LurkerRoar(uint8_t *ai) {
    g_lurkerRoar(ai);
    UINT now = *(UINT *)(SymAddr("?stdTime@@3VtStdTime@@A") + 0x44);
    for (auto &r : g_lurkerRoars)
        if (!r.ai || r.ai == ai) {
            r.ai = ai, r.at = now + 3000;
            return;
        }
}
static void __fastcall LurkerKilled(uint8_t *ai, void *, void *a, int b, int c, float d, unsigned e, void *f, void *g, int h,
                                    float i, int *j) {
    PlayFoley(*(void **)(ai + 8), "vox_death");
    g_lurkerKilled(ai, a, b, c, d, e, f, g, h, i, j);
}
static void __fastcall LurkerBurrow(uint8_t *ai) {
    g_lurkerBurrow(ai);
    PlayFoley(*(void **)(ai + 8), "special1");
}
static UpdateFn g_lurkerBurst;
static void __fastcall LurkerBurst(uint8_t *ai) {
    g_lurkerBurst(ai);
    PlayFoley(*(void **)(ai + 8), "special2");
}
static void __fastcall LurkerUnburrow(uint8_t *ai) {
    g_lurkerUnburrow(ai);
    PlayFoley(*(void **)(ai + 8), "special2");
}
static void __fastcall LurkerFireSpike(uint8_t *ai) {
    *(unsigned *)(ai + 0x59c) = 0;
    g_lurkerFireSpike(ai);
    unsigned guid = *(unsigned *)(ai + 0x59c);   // the spike projectile it fired (0: none)
    if (!guid) return;
    uint8_t *thing = *(uint8_t **)(ai + 8);
    PlayFoley(thing, "swing");
    static int next;
    auto &s = g_lurkerSpikes[next++ % 8];
    s.guid = guid, s.owner = thing, s.seen = false, s.hit = false;
}
static void TrackLurkerRoars() {
    UINT now = *(UINT *)(SymAddr("?stdTime@@3VtStdTime@@A") + 0x44);
    for (auto &r : g_lurkerRoars)
        if (r.ai && now >= r.at) {
            if (*(int *)(r.ai + 0x374) == 0x33) PlayFoley(*(void **)(r.ai + 8), "vox_alert");   // still roaring
            r.ai = nullptr;
        }
}
// Its burrowed attacks (DoBurrowedDamage: the burst out of the ground from its AnimationCallback, 4 m, and the
// unburrow, CMStartUnburrow, 2 m) did 25 damage: 90 (the user's tuning). The amount is an immediate push before each
// call (25.0f = 0x41C80000).
static void FixLurkerDamage() {
    static const char *const fns[] = {"?CMStartUnburrow@cAIBossLurker@@UAEXXZ",
                                      "?AnimationCallback@cAIBossLurker@@UAEXPAVcKeyChannel@@W4eRenderKeyTag@@I@Z"};
    uint32_t target = SymAddr("?DoBurrowedDamage@cAIBossLurker@@QAEHPAUXGVECTOR3@@MM@Z");
    int n = 0;
    for (const char *name : fns) {
        uint8_t *fn = (uint8_t *)SymAddr(name);
        for (uint8_t *call = fn; fn && target && call < fn + 0x400; call++) {
            if (call[0] != 0xE8 || (uint32_t)(call + 5 + *(int32_t *)(call + 1)) != target) continue;
            for (uint8_t *q = call - 0x18; q < call; q++)
                if (q[0] == 0x68 && *(uint32_t *)(q + 1) == 0x41c80000) {
                    uint32_t v = 0x42b40000;   // 90.0f
                    WriteCode(q + 1, &v, 4);
                    n++;
                    break;
                }
            break;
        }
    }
    if (n != 2) Log("levelscript: Lurker burrowed damage: %d of 2 call sites changed", n);
}
// Aries (cAIBossSniper::FireSniperProjectile) fires cAIInstance::FireInstantProjectile with 25 damage a shot
// (push 25.0f at +0x16); raised to 80.
static void FixSniperDamage() {
    uint8_t *fn = (uint8_t *)SymAddr("?FireSniperProjectile@cAIBossSniper@@AAEXXZ");
    if (!fn || fn[0x16] != 0x68 || *(uint32_t *)(fn + 0x17) != 0x41c80000) {
        Log("levelscript: unexpected cAIBossSniper::FireSniperProjectile, Aries keeps 25 damage a shot");
        return;
    }
    uint32_t v = 0x42a00000;   // 80.0f
    WriteCode(fn + 0x17, &v, 4);
}
static void TrackLurkerSpikes() {
    TrackLurkerRoars();
    typedef uint8_t *(__cdecl *FindFn)(unsigned);
    static FindFn find;
    if (!find) find = (FindFn)SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    if (!find) return;
    for (auto &s : g_lurkerSpikes) {
        if (!s.guid) continue;
        if (uint8_t *p = find(s.guid)) {
            memcpy(s.pos, p + 0xb0, 12);
            // the spike (proj_lurker) runs along the floor through Nova without colliding with her (it went on for
            // another 70 m): passing within 1 m of her (and 2 m of her feet's height) it hits her once, for 75 damage
            // (the user's tuning; proj_lurker has 25), as the Lurker's (source 3, as its DoBurrowedDamage)
            if (!s.hit && s.owner) {
                uint8_t *player = GuardedPlayer();
                const float *pp = player ? (const float *)(player + 0xb0) : nullptr;
                float dx = pp ? pp[0] - s.pos[0] : 99.f, dy = pp ? pp[1] - s.pos[1] : 99.f, dz = pp ? pp[2] - s.pos[2] : 99.f;
                if (dx * dx + dy * dy < 1.f && fabsf(dz) < 2.f) {
                    if (!g_damage)
                        g_damage = (DamageFn)SymAddr("?Damage@tActor@@QAEMPAVtThing@@W4eDamageSource@@W4eDamageType@@MPAUXGVECTOR3@@3IW4eHitRegionType@@MPAH@Z");
                    float at[3] = {pp[0], pp[1], pp[2] + 0.5f}, dir[3] = {0, 0, 1};
                    if (g_damage) g_damage(player, s.owner, 3, 0, 75.f, at, dir, 0, 0, 1.f, nullptr);
                    s.hit = true;
                }
            }
            static bool dbg = getenv("GHOSTPC_LOGDEATH") != nullptr;
            if (dbg && (!s.seen || g_frame % 3 == 0)) {
                const float *v = (const float *)(p + 0x100);
                Log("lurker-dbg: frame %u spike %u at %.2f %.2f %.2f (+0x100 %.2f %.2f %.2f)", g_frame, s.guid, s.pos[0], s.pos[1],
                    s.pos[2], v[0], v[1], v[2]);
            }
            s.seen = true;
            continue;
        }
        static bool dbgGone = getenv("GHOSTPC_LOGDEATH") != nullptr;
        if (dbgGone) Log("lurker-dbg: frame %u spike %u gone at %.2f %.2f %.2f", g_frame, s.guid, s.pos[0], s.pos[1], s.pos[2]);
        // gone (hit something or expired): the hit sound where it was last, from its Lurker's set; tObject::PlayFoleyMode
        // plays at a thing's position, so the Lurker's is borrowed for the call
        if (s.seen && s.owner) {
            float keep[3];
            memcpy(keep, s.owner + 0xb0, 12);
            memcpy(s.owner + 0xb0, s.pos, 12);
            PlayFoley(s.owner, "hitdirt");
            memcpy(s.owner + 0xb0, keep, 12);
        }
        s.guid = 0;
    }
}
// GHOSTPC_FLY_DEBUG: logs cAIBaseFlyer::CMUpdatePathMove calls (position before / after) for the Mutalisk
static UpdateFn g_pathMove;
static void __fastcall DebugPathMove(uint8_t *ai) {
    float *p = (float *)(*(uint8_t **)(ai + 8) + 0xb0), b[3] = {p[0], p[1], p[2]};
    g_pathMove(ai);
    static int n;
    uint8_t *model = *(uint8_t **)(*(uint8_t **)(ai + 8) + 0x2a0);
    uint8_t *mixer = model ? *(uint8_t **)(model + 0xac) : nullptr;
    if (n++ % 30 == 0)
        Log("fly-dbg: path move %.1f %.1f %.1f -> %.1f %.1f %.1f, mixer %p track %p rate %g t0x4c %u", b[0], b[1], b[2], p[0], p[1],
            p[2], mixer, mixer ? *(void **)(mixer + 0xf0 + 0x40) : nullptr, mixer ? *(float *)(mixer + 0xf0 + 0x50) : 0.f,
            *(UINT *)(SymAddr("?stdTime@@3VtStdTime@@A") + 0x4c));
}
// The Overlord (cAIZergOverlord, a cAIDetector flyer) that loses sight of its target goes back to patrolling (state 5) and its fly
// controller (cAIBaseFlyer::CMUpdateFly, mode 3) steers it straight at the target: no navigation, so it sank to the
// floor and flew into the wall between them. The wrapper keeps it at or above the altitude it started at and undoes a
// step that a wall blocks (simCollision_LineOfSight from the old to the new position).
static UpdateFn g_overlordFly;
static struct { uint8_t *ai; float z; } g_overlordHome[8];
static void __fastcall OverlordFly(uint8_t *ai) {
    typedef void(__thiscall *PosFn)(void *, const float *);
    static PosFn physPos, moveTo;
    if (!physPos) physPos = (PosFn)SymAddr("?SetPhysPos@tThing@@QAEXPBUXGVECTOR3@@@Z");
    if (!moveTo) moveTo = (PosFn)SymAddr("?MoveToPos@tThing@@QAEXPBUXGVECTOR3@@@Z");
    uint8_t *thing = *(uint8_t **)(ai + 8);
    float *p = (float *)(thing + 0xb0), before[3] = {p[0], p[1], p[2]};
    float home = before[2];
    for (auto &h : g_overlordHome)
        if (h.ai == ai || !h.ai) {
            if (!h.ai) h.ai = ai, h.z = before[2];
            home = h.z;
            break;
        }
    g_overlordFly(ai);
    float after[3] = {p[0], p[1], p[2] < home ? home : p[2]};
    if (g_lineOfSight && g_lineOfSight(before, after, 4) != 0) memcpy(after, before, sizeof after);   // a wall: stay
    if (after[0] != p[0] || after[1] != p[1] || after[2] != p[2]) {
        if (physPos) physPos(thing, after);
        if (moveTo) moveTo(thing, after);
    }
}
// A flyer that loses its target (or comes out of lockdown) goes back to patrolling (state 5,
// cAIInstance::ASStartPatrolling, then the class's ASUpdatePatrolling): it steers (controller mode 3 at +0x2E0)
// toward its patrol point (+0x25C) until it is within 3 m, then hovers. A flyer placed without a patrol route never
// gets that point (it stays the allocator's 0xCCCCCCCC fill, about -1e8 on each axis), so it never arrived and flew
// off toward that far corner, low or into the wall in between. The Scout's AI (the Scout and the Wraith) and the
// Overlord, without a patrol point, now patrol back to where they were placed (tThing +0x350, what UnOverridePatrol
// jumps them back to). Other flyers (Shuttle, Observer, ...) are left as they were.
static void __fastcall FlyerStartPatrol(uint8_t *ai) {
    typedef void(__thiscall *RecoFn)(void *, int, unsigned, float *, unsigned, unsigned);
    uint32_t *vt = *(uint32_t **)ai;
    ((RecoFn)vt[0x4e8 / 4])(ai, 5, 0, nullptr, 0, 0);   // what ASStartPatrolling did: RECOSet(patrolling)
    static uint32_t scoutVt = SymAddr("??_7cAIProtossScout@@6B@"), overlordVt = SymAddr("??_7cAIZergOverlord@@6B@");
    float *pt = (float *)(ai + 0x25c);
    uint8_t *thing = *(uint8_t **)(ai + 8);
    if (!thing || ((uint32_t)vt != scoutVt && (uint32_t)vt != overlordVt)) return;
    if (!(fabsf(pt[0]) < 1e6f && fabsf(pt[1]) < 1e6f && fabsf(pt[2]) < 1e6f)) {
        memcpy(pt, thing + 0x350, 12);
        static int logged;
        if (logged++ < 8) Log("levelscript: flyer %p has no patrol point, patrols back to %.1f %.1f %.1f", ai, pt[0], pt[1], pt[2]);
    }
    // patrolling home (the point given above, kept from an earlier patrol): the fly controller steers at +0x2E0,
    // which still held its last goal (a search lookout), so it never got within 3 m of home
    if (!memcmp(pt, thing + 0x350, 12)) memcpy(ai + 0x2e0, pt, 12);
}
static void FixFlyerPatrol() {
    uint8_t *fn = (uint8_t *)SymAddr("?ASStartPatrolling@cAIInstance@@UAEXXZ");
    // mov eax,[ecx]; push 0 (x4); push 5; call [eax+0x4E8]; ret
    static const uint8_t orig[] = {0x8b, 0x01, 0x6a, 0x00, 0x6a, 0x00, 0x6a, 0x00, 0x6a, 0x00, 0x6a, 0x05,
                                   0xff, 0x90, 0xe8, 0x04, 0x00, 0x00, 0xc3};
    if (!fn || memcmp(fn, orig, sizeof orig)) {
        Log("levelscript: unexpected cAIInstance::ASStartPatrolling, flyer patrol not fixed");
        return;
    }
    uint8_t jmp[5] = {0xE9};
    *(int32_t *)(jmp + 1) = (int32_t)((uint8_t *)FlyerStartPatrol - (fn + 5));
    WriteCode(fn, jmp, 5);
}
static void FixOverlordFlight() {
    if (!g_lineOfSight) g_lineOfSight = (LineOfSightFn)SymAddr("?simCollision_LineOfSight@@YAHPAUXGVECTOR3@@0W4eRaycastDbgID@@@Z");
    g_overlordFly = (UpdateFn)WrapVirtual("??_7cAIZergOverlord@@6B@", "?CMUpdateFly@cAIBaseFlyer@@MAEXXZ", (void *)OverlordFly);
}
// The cut Goliath boss (cAIBossGoliath, BOSSGOLIATH) is a frame-to-frame boss like the sniper: its mission script
// sent it to positions stored as its placement's track frames (cAIBaseBossGoliath::FightToFrame, vtable +0x8F4;
// ScriptNotify 100/200-205 map to frames 9/5/3/2/1/7/6) and it fights from them, firing rockets at frames 1-3 and 5-7
// (IsRocketFrame); ClassUpdate reports each arrival (scriptMessage_AIArrived). Without that script it only idled.
// The driver stands in for it: once the boss has a target (the level script's notify), it sends it to a random other
// frame 1-9 a few seconds after each arrival (or 15 s after a command that never arrived).
struct GoliathBoss {
    uint8_t *ai;
    UINT next;   // game time of the next command; ~0u while walking to a frame
    UINT sent;
    int frame;
};
static GoliathBoss g_goliathBosses[4];
typedef float(__cdecl *ArrivedFn)(void *, unsigned);
static ArrivedFn g_aiArrived;
static float __cdecl GoliathArrived(void *actor, unsigned frame) {
    UINT now = *(UINT *)(SymAddr("?stdTime@@3VtStdTime@@A") + 0x44);
    uint8_t *ai = actor ? *(uint8_t **)((uint8_t *)actor + 0x3c4) : nullptr;
    for (auto &b : g_goliathBosses)
        if (b.ai && b.ai == ai) b.next = now + 3000 + rand() % 2500;   // fight from here for a few seconds
    return g_aiArrived(actor, frame);
}
static void DriveGoliathBoss(uint8_t *world) {
    static uint32_t vt;
    if (!vt) vt = SymAddr("??_7cAIBossGoliath@@6B@");
    if (!vt || !g_aiArrived) return;
    UINT now = *(UINT *)(SymAddr("?stdTime@@3VtStdTime@@A") + 0x44);
    if (g_frame % 15 == 0) {   // pick up the bosses that have a target
        __try {
            uint32_t sectors = *(uint32_t *)(world + 0xdc);
            uint8_t *sector = *(uint8_t **)(world + 0xe0);
            for (uint32_t i = 0; i < sectors; i++, sector += 0x110)
                for (uint8_t *t = *(uint8_t **)(sector + 0x24); t; t = *(uint8_t **)(t + 0x28)) {
                    uint8_t *ai = *(uint8_t **)(t + 0x3c4);
                    if (!ai || IsBadReadPtr(ai, 4) || *(uint32_t *)ai != vt || !*(unsigned *)(ai + 0x2d4)) continue;
                    bool known = false;
                    for (auto &b : g_goliathBosses) known |= b.ai == ai;
                    if (known) continue;
                    for (auto &b : g_goliathBosses)
                        if (!b.ai) {
                            b = {ai, now + 1500, 0, 0};
                            break;
                        }
                }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    typedef int(__thiscall *FightToFrameFn)(void *, unsigned);
    for (auto &b : g_goliathBosses) {
        if (!b.ai) continue;
        uint8_t *thing = *(uint8_t **)(b.ai + 8);
        if (!thing || *(float *)(thing + 0x36c) <= 0.f) {   // dead
            b.ai = nullptr;
            continue;
        }
        if (b.next == ~0u && now - b.sent > 15000) b.next = now;   // never arrived: try another frame
        if (b.next == ~0u || now < b.next) continue;
        int f = 1 + rand() % 9;
        if (f == b.frame) f = f % 9 + 1;
        auto fight = (FightToFrameFn)(*(uint32_t **)b.ai)[0x8f4 / 4];
        __try {
            fight(b.ai, f);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        b.frame = f, b.sent = now, b.next = ~0u;
        static bool dbg = getenv("GHOSTPC_TANAGAZJ_DEBUG") != nullptr;
        if (dbg) Log("goliath-boss: FightToFrame(%d)", f);
    }
}
// Its machine guns (FireGoliathMachineGun, vtable +0x9B0: an instant hit from each of 3 gun markers, 5 damage)
// aim at the target with a random yaw and pitch of +-10 deg x (1 - accuracy); the boss fires at accuracy 0.5,
// +-5 deg, which at its 15-25 m fighting range almost never hit Nova (1 of 36 shots in a test). The spread is cut
// to 40%: accuracy 0.5 -> 0.8, +-2 deg.
typedef void(__thiscall *MachineGunFn)(void *, unsigned, float);
static MachineGunFn g_goliathMG;
static void __fastcall GoliathMachineGun(void *ai, void *, unsigned gun, float accuracy) {
    g_goliathMG(ai, gun, 1.f - (1.f - accuracy) * 0.4f);
}
static void FixGoliathBoss() {
    g_goliathMG = (MachineGunFn)WrapVirtual("??_7cAIBossGoliath@@6B@", "?FireGoliathMachineGun@cAIBossGoliath@@UAEXIM@Z",
                                            (void *)GoliathMachineGun);
    if (getenv("GHOSTPC_NO_GOLIATH_ROCKETS")) {   // debug: the cut Goliath boss fires no rockets (FireGoliathLaunchers)
        uint8_t *fl = (uint8_t *)SymAddr("?FireGoliathLaunchers@cAIBossGoliath@@UAEXM@Z");
        static const uint8_t ret4[] = {0xC2, 0x04, 0x00};
        if (fl) WriteCode(fl, ret4, 3);
    }
    uint8_t *cu = (uint8_t *)SymAddr("?ClassUpdate@cAIBossGoliath@@UAEXXZ");
    uint8_t *arrived = (uint8_t *)SymAddr("?scriptMessage_AIArrived@@YAMPAVtActor@@I@Z");
    for (int i = 0; cu && arrived && i < 0x800; i++)
        if (cu[i] == 0xE8 && cu + i + 5 + *(int32_t *)(cu + i + 1) == arrived) {
            g_aiArrived = (ArrivedFn)arrived;
            int32_t rel = (int32_t)((uint8_t *)GoliathArrived - (cu + i + 5));
            WriteCode(cu + i + 1, &rel, 4);
            return;
        }
    Log("levelscript: cAIBossGoliath::ClassUpdate arrival call not found, Goliath boss driver off");
}
// The restored Vulture (cAITerranVulture) noticed Nova through walls: its IsTargettable override (vtable +0x158, the
// check cAITerranVulture::Search makes before taking a target) tests only range and direction, where the base
// cAIInstance version also asks the sight checks. The wrapper adds a line of sight from 1.5 m above the Vulture to
// 1 m above the target.
typedef int(__thiscall *TargettableFn)(void *, uint8_t *);
static TargettableFn g_vultureTargettable;
static int __fastcall VultureTargettable(uint8_t *ai, void *, uint8_t *target) {
    int r = g_vultureTargettable(ai, target);
    if (!r || !target || !g_lineOfSight) return r;
    const float *p = (const float *)(*(uint8_t **)(ai + 8) + 0xb0), *t = (const float *)(target + 0xb0);
    float from[3] = {p[0], p[1], p[2] + 1.5f}, to[3] = {t[0], t[1], t[2] + 1.f};
    return g_lineOfSight(from, to, 4) == 0 ? r : 0;
}
static void FixVultureSight() {
    if (!g_lineOfSight) g_lineOfSight = (LineOfSightFn)SymAddr("?simCollision_LineOfSight@@YAHPAUXGVECTOR3@@0W4eRaycastDbgID@@@Z");
    g_vultureTargettable = (TargettableFn)WrapVirtual("??_7cAITerranVulture@@6B@", "?IsTargettable@cAITerranVulture@@UAEHPAVtActor@@@Z",
                                                      (void *)VultureTargettable);
}
// The Scout's AI (cAIProtossScout; the Wraith uses it too) flies attack runs made for open terrain: ASStartAcquired
// sends it to a point 60 m beyond itself from the target and 40 m above it (+0x2E0, controller mode 4), then
// ASStartCombatRanged makes a firing pass to 2 m past the target, 10 m above it. Indoors those points are outside the
// walls and at the wall tops: it flew into the wall, climbed to the top and stuck there. Both goals are replaced:
// acquired = a standoff point 9-14 m from the target on the Scout's side, 7 m above it (its gun reaches 20 m), with a
// clear line to the
// target and a clear flight from the Scout (other directions / shorter distances if not); combat = a sideways slide
// around the target at that distance and height, firing. Out of range or out of sight, the next standoff point
// brings it closer (it may still climb when that is what gets it a clear line).
static UpdateFn g_scoutAcquired, g_scoutCombat;
static UINT GameTime() { return *(UINT *)(SymAddr("?stdTime@@3VtStdTime@@A") + 0x44); }
// its room (the user's rule: it never leaves it): within 30 m of where it was placed (tThing +0x350), in a clear line
// from there, and at most 4 m above it
static bool ScoutInRoom(uint8_t *thing, const float *pt) {
    float *home = (float *)(thing + 0x350);
    float dx = pt[0] - home[0], dy = pt[1] - home[1];
    return dx * dx + dy * dy < 30.f * 30.f && pt[2] <= home[2] + 4.f && g_lineOfSight(home, (float *)pt, 4) == 0;
}
static bool ScoutPointClear(uint8_t *thing, const float *scout, const float *target, const float *pt) {
    float t[3] = {target[0], target[1], target[2] + 1.5f};
    return g_lineOfSight(t, (float *)pt, 4) == 0 && g_lineOfSight((float *)scout, (float *)pt, 4) == 0 &&
           ScoutInRoom(thing, pt);
}
// a goal around the target: direction (dx, dy) from it turned by the given angles, distances tried in order
static bool ScoutGoal(uint8_t *ai, const float *angles, int nAngles, float baseYaw, bool keepDist, float *out) {
    typedef uint8_t *(__cdecl *FindFn)(unsigned);
    static FindFn find;
    if (!find) find = (FindFn)SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    uint8_t *target = find && *(unsigned *)(ai + 0x2d4) ? find(*(unsigned *)(ai + 0x2d4)) : nullptr;
    if (!target || !g_lineOfSight) return false;
    const float *s = (const float *)(*(uint8_t **)(ai + 8) + 0xb0), *t = (const float *)(target + 0xb0);
    float dx = s[0] - t[0], dy = s[1] - t[1], d = sqrtf(dx * dx + dy * dy);
    if (d < 0.1f) dx = 1, dy = 0, d = 1;
    // its gun reaches 20 m (FireWeapon: FireInstantProjectileAtPos range 20): 9-14 m out and 7 m up stays within ~16
    float dist = keepDist ? d : (d < 9.f ? 9.f : d > 14.f ? 14.f : d);
    if (dist > 14.f) dist = 14.f;
    float base = atan2f(dy, dx) + baseYaw;
    for (float scale : {1.f, 0.75f, 0.5f, 0.35f})
        for (int i = 0; i < nAngles; i++) {
            float a = base + angles[i], r = dist * scale;
            float pt[3] = {t[0] + cosf(a) * r, t[1] + sinf(a) * r, t[2] + 7.f};
            if (ScoutPointClear(*(uint8_t **)(ai + 8), s, t, pt)) {
                memcpy(out, pt, sizeof pt);
                return true;
            }
        }
    return false;
}
static void ScoutSetGoal(uint8_t *ai, const float *g) {
    memcpy(ai + 0x2e0, g, 12);
    memcpy(ai + 0x40, g, 12);   // ASStartAcquired keeps a copy here
}
static void __fastcall ScoutStartAcquired(uint8_t *ai) {
    g_scoutAcquired(ai);
    if (*(int *)(ai + 0x18) != 11) return;   // it went idle (no valid target)
    static const float angles[] = {0.f, 0.7f, -0.7f, 1.4f, -1.4f, 2.1f, -2.1f, 3.14159f};
    float g[3];
    if (ScoutGoal(ai, angles, 8, 0.f, false, g)) {
        ScoutSetGoal(ai, g);
        return;
    }
    // no standoff point in its room (the target is outside it): hover where it is and let the acquired state time out
    // (ASUpdateAcquired goes idle once the game time passes +0x3D4); the game's own goal, 60 m out and 40 m up, is
    // what took it high above the room
    ScoutSetGoal(ai, (float *)(*(uint8_t **)(ai + 8) + 0xb0));
    *(UINT *)(ai + 0x3d4) = GameTime() - 1;
}
static void __fastcall ScoutStartCombat(uint8_t *ai) {
    g_scoutCombat(ai);
    if (*(int *)(ai + 0x18) != 12) return;
    float side = rand() & 1 ? 0.6f : -0.6f;   // slide ~35 deg around the target
    float angles[] = {side, -side, side * 0.5f, -side * 0.5f};
    float g[3];
    if (ScoutGoal(ai, angles, 4, 0.f, true, g)) ScoutSetGoal(ai, g);
    else ScoutSetGoal(ai, (float *)(*(uint8_t **)(ai + 8) + 0xb0));   // nothing in its room: fire from where it is
}
// Per Scout (and Wraith): its search (curious) state, where its target was when it was locked down, and when it last
// fought or searched (DriveScoutsHome: 2 minutes without, it flies back to where it was placed).
static struct ScoutSearch {
    uint8_t *ai;
    UINT start, seen, lastActive;
    float goal[3], lockPos[3];
    bool hasLock;
} g_scoutSearch[8];
static ScoutSearch &ScoutSearchOf(uint8_t *ai) {
    for (auto &s : g_scoutSearch)
        if (s.ai == ai) return s;
    for (auto &s : g_scoutSearch)
        if (!s.ai) return s = {}, s.ai = ai, s;
    return g_scoutSearch[0] = {}, g_scoutSearch[0].ai = ai, g_scoutSearch[0];
}
// Lockdown on a Scout (cAIProtossScout::Lockdown, vtable +0x1A4) just killed it (tActor::Kill: it exploded, and once
// the game crashed); its UnLockdown slot (+0x1A8) was an empty stub. It now locks down like the Observer
// (cAIProtossObserver::Lockdown): patrol overridden, model frozen (cModelInst +0x14 bit 0x20000), velocity zeroed,
// lockdown state (54), hover controller (mode 2), shield off; UnLockdown undoes it. If it was fighting (acquired or
// combat), it then searches where its target was when it was locked down (curious, state 4), else it hovers where
// it is (idle).
static void __fastcall ScoutLockdown(uint8_t *ai) {
    uint32_t *vt = *(uint32_t **)ai;
    typedef void(__thiscall *VoidFn)(void *);
    typedef void(__thiscall *IntFn)(void *, int);
    typedef void(__thiscall *ZeroFn)(void *);
    typedef void(__thiscall *ShieldOffFn)(void *, int);
    typedef void(__thiscall *ActiveFn)(void *, int);
    static ZeroFn zero = (ZeroFn)SymAddr("?ZeroVelocity@tThing@@QAEXXZ");
    static ActiveFn physActive = (ActiveFn)SymAddr("?SetPhysActive@tThing@@QAEXH@Z");
    static ShieldOffFn off = (ShieldOffFn)SymAddr("?Off@cProtossShield@@QAEXH@Z");
    typedef uint8_t *(__cdecl *FindFn)(unsigned);
    static FindFn find = (FindFn)SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    int st = *(int *)(ai + 0x18);
    uint8_t *target = find && *(unsigned *)(ai + 0x2d4) && (st == 11 || st == 12) ? find(*(unsigned *)(ai + 0x2d4)) : nullptr;
    ScoutSearch &rec = ScoutSearchOf(ai);
    rec.hasLock = target != nullptr;
    if (target) memcpy(rec.lockPos, target + 0xb0, 12);
    ((VoidFn)vt[0x8dc / 4])(ai);   // OverridePatrol
    uint8_t *thing = *(uint8_t **)(ai + 8);
    if (thing) {
        if (uint8_t *model = *(uint8_t **)(thing + 0x2a0)) *(uint32_t *)(model + 0x14) |= 0x20000;
        if (zero) zero(thing);
        if (physActive) physActive(thing, 0);   // it hangs where it is (with physics on it sank to the floor)
    }
    ((IntFn)vt[0x5b0 / 4])(ai, 0x36);   // SetState: lockdown
    ((IntFn)vt[0x8d0 / 4])(ai, 2);      // StartControllerMode: hover
    if (off) off(ai + 0x3b4, 0);        // its shield (GetShield: +0x3B4)
}
// The Scout's curious state (4; per-state start/update tables at vtable +0x5D0 / +0x6C8, so its slots are +0x5E0 /
// +0x6D8) was never written: both are the empty cAIInstance stub, so a curious Scout kept whatever flight it had (the
// fly controller steers its velocity straight at +0x2E0, which could be a point on the floor or past a wall). It now searches like a walker does: it flies to a
// lookout over the curious point (cAIInstance::ALSetCurious: +0x40), 3-7 m above it with a clear line to it and a
// clear flight from the Scout (backed off toward the Scout if needed), hovers there facing it, and goes back to its
// target only after seeing it for 1 s (from 1.5 s in, the same checks as cAIProtossScout::Search: vtable +0x15C,
// +0x158); after 10 s without that it goes idle (whose Search carries on). Lockdown ends in this state.
static void __fastcall ScoutStartCurious(uint8_t *ai) {
    typedef void(__thiscall *IntFn)(void *, int);
    uint32_t *vt = *(uint32_t **)ai;
    uint8_t *thing = *(uint8_t **)(ai + 8);
    if (!thing || !g_lineOfSight) return;
    float *s = (float *)(thing + 0xb0), *c = (float *)(ai + 0x40);
    ScoutSearch &st = ScoutSearchOf(ai);
    st.start = GameTime(), st.seen = 0;
    memcpy(st.goal, s, 12);   // nothing clear in its room: hover where it is
    float dx = s[0] - c[0], dy = s[1] - c[1], d = sqrtf(dx * dx + dy * dy);
    if (d > 0.1f) dx /= d, dy /= d;
    else dx = 1, dy = 0;
    float look[3] = {c[0], c[1], c[2] + 1.f};
    bool found = false;
    for (float back : {2.f, 5.f, 8.f, 11.f})
        for (float up : {7.f, 5.f, 3.f}) {
            if (found || back > d + 2.f) continue;
            float pt[3] = {c[0] + dx * back, c[1] + dy * back, c[2] + up};
            if (!g_lineOfSight(look, pt, 4) && !g_lineOfSight(s, pt, 4) && ScoutInRoom(thing, pt))
                memcpy(st.goal, pt, 12), found = true;
        }
    memcpy(ai + 0x2e0, st.goal, 12);
    ((IntFn)vt[0x8d0 / 4])(ai, found ? 3 : 2);   // StartControllerMode: fly / hover
    *(UINT *)(ai + 0x14) = st.start + 100;
}
static void __fastcall ScoutUpdateCurious(uint8_t *ai) {
    typedef void(__thiscall *IntFn)(void *, int);
    typedef int(__thiscall *SeeFn)(void *, void *, int);
    typedef int(__thiscall *TargettableFn)(void *, void *);
    typedef int(__thiscall *SetTargetFn)(void *, void *);
    typedef void(__thiscall *LookFn)(void *, float *);
    typedef uint8_t *(__cdecl *PlayerFn)();
    static auto setTarget = (SetTargetFn)SymAddr("?SetTarget@cAIInstance@@QAEHPAVtActor@@@Z");
    static auto lookAt = (LookFn)SymAddr("?LookAt@tThing@@QAEXPAUXGVECTOR3@@@Z");
    static auto activePlayer = (PlayerFn)SymAddr("?gameMultiplayer_GetActivePlayer@@YAPAVtPlayer@@XZ");
    uint32_t *vt = *(uint32_t **)ai;
    uint8_t *thing = *(uint8_t **)(ai + 8);
    UINT now = GameTime();
    *(UINT *)(ai + 0x14) = now + 250;
    if (!thing) return;
    ScoutSearch &st = ScoutSearchOf(ai);
    const float *s = (const float *)(thing + 0xb0);
    float dx = st.goal[0] - s[0], dy = st.goal[1] - s[1], dz = st.goal[2] - s[2];
    if (*(int *)(ai + 0x378) != 2 && dx * dx + dy * dy + dz * dz < 2.f * 2.f) {
        ((IntFn)vt[0x8d0 / 4])(ai, 2);   // there: hover and face the spot
        if (lookAt) lookAt(thing, (float *)(ai + 0x40));
    }
    uint8_t *player = activePlayer ? activePlayer() : nullptr;
    if (player && now - st.start > 1500 && ((SeeFn)vt[0x15c / 4])(ai, player, 0) && ((TargettableFn)vt[0x158 / 4])(ai, player)) {
        if ((st.seen += 250) >= 1000) {
            if (setTarget) setTarget(ai, player);
            ((IntFn)vt[0x5b0 / 4])(ai, 11);   // acquired
            return;
        }
    } else
        st.seen = 0;
    if (now - st.start > 10000) ((IntFn)vt[0x5b0 / 4])(ai, 2);   // idle
}
static void __fastcall ScoutUnLockdown(uint8_t *ai) {
    uint32_t *vt = *(uint32_t **)ai;
    typedef void(__thiscall *IntFn)(void *, int);
    typedef void(__thiscall *PosFn)(void *, float *);
    typedef void(__thiscall *ShieldOnFn)(void *);
    static ShieldOnFn on = (ShieldOnFn)SymAddr("?On@cProtossShield@@QAEXXZ");
    typedef void(__thiscall *ActiveFn)(void *, int);
    static ActiveFn physActive = (ActiveFn)SymAddr("?SetPhysActive@tThing@@QAEXH@Z");
    uint8_t *thing = *(uint8_t **)(ai + 8);
    if (thing) {
        if (uint8_t *model = *(uint8_t **)(thing + 0x2a0)) *(uint32_t *)(model + 0x14) &= ~0x20000u;
        if (physActive) physActive(thing, 1);
    }
    if (*(uint32_t *)(ai + 0xc) & 0x200000) ((IntFn)vt[0x8e0 / 4])(ai, 0);   // UnOverridePatrol
    ScoutSearch &rec = ScoutSearchOf(ai);
    if (rec.hasLock) ((PosFn)vt[0x374 / 4])(ai, rec.lockPos);   // ALSetCurious
    ((IntFn)vt[0x5b0 / 4])(ai, rec.hasLock ? 4 : 2);
    rec.hasLock = false;
    if (on) on(ai + 0x3b4);
}
// In combat (state 12) its gun line follows the target (see below). While patrolling (state 5) its CMUpdateFly
// (vtable +0x918) stops it at a wall.
static UpdateFn g_scoutFly;
static void __fastcall ScoutFly(uint8_t *ai) {
    uint8_t *thing = *(uint8_t **)(ai + 8);
    float *p = (float *)(thing + 0xb0), before[3] = {p[0], p[1], p[2]};
    g_scoutFly(ai);
    // it never goes down to within 3 m of the floor (its combat passes dipped to 0-2 m and it sat on the ground):
    // below 3 m above its target's feet (without a target: while the ground is within 3 m under it), its physics
    // body (tThing +0x158 hkEntity, its motion at +8: linear velocity +0x40, set through vtable +0x50, as
    // tThing::ZeroVelocity does) climbs at 4 m/s instead of sinking. Moving the actor itself did not stick: the body
    // put it back.
    typedef uint8_t *(__cdecl *FindTgtFn)(unsigned);
    static FindTgtFn findTgt = (FindTgtFn)SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    uint8_t *tgt = findTgt && *(unsigned *)(ai + 0x2d4) ? findTgt(*(unsigned *)(ai + 0x2d4)) : nullptr;
    float below[3] = {p[0], p[1], p[2] - 3.f};
    bool low = tgt ? p[2] < ((float *)(tgt + 0xb0))[2] + 3.f : g_lineOfSight && g_lineOfSight(p, below, 4) != 0;
    uint8_t *body = *(uint8_t **)(thing + 0x158), *motion = body ? *(uint8_t **)(body + 8) : nullptr;
    if (low && motion) {
        __declspec(align(16)) float v[4];
        memcpy(v, motion + 0x40, 16);
        if (v[2] < 4.f) {
            v[2] = 4.f;
            typedef void(__thiscall *SetVelFn)(void *, const float *);
            ((SetVelFn)(*(uint32_t **)motion)[0x50 / 4])(motion, v);
        }
    }
    if (*(int *)(ai + 0x18) == 12) {
        // its gun (FireWeapon: instant hits at a point moving from +0x3E4 to +0x3F0 over the pass) swept a line laid
        // for the original fly-over, which rarely crossed Nova once it circles her: the line now runs across the
        // target's current position at chest height, 0.75 m to either side
        typedef uint8_t *(__cdecl *FindFn)(unsigned);
        static FindFn find;
        if (!find) find = (FindFn)SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
        if (uint8_t *target = find && *(unsigned *)(ai + 0x2d4) ? find(*(unsigned *)(ai + 0x2d4)) : nullptr) {
            const float *t = (const float *)(target + 0xb0);
            float dx = t[0] - p[0], dy = t[1] - p[1], d = sqrtf(dx * dx + dy * dy);
            float sx = d > 0.1f ? -dy / d * 0.75f : 0.75f, sy = d > 0.1f ? dx / d * 0.75f : 0.f;
            float *a = (float *)(ai + 0x3e4), *b = (float *)(ai + 0x3f0);
            a[0] = t[0] + sx, a[1] = t[1] + sy, a[2] = t[2] + 1.f;
            b[0] = t[0] - sx, b[1] = t[1] - sy, b[2] = t[2] + 1.f;
        }
        return;
    }
    if (*(int *)(ai + 0x18) != 5) return;
    if (g_lineOfSight && g_lineOfSight(before, p, 4) != 0) {   // a wall: stay, and stop
        typedef void(__thiscall *PosFn)(void *, const float *);
        typedef void(__thiscall *ZeroFn)(void *);
        static PosFn physPos = (PosFn)SymAddr("?SetPhysPos@tThing@@QAEXPBUXGVECTOR3@@@Z");
        static PosFn moveTo = (PosFn)SymAddr("?MoveToPos@tThing@@QAEXPBUXGVECTOR3@@@Z");
        static ZeroFn zero = (ZeroFn)SymAddr("?ZeroVelocity@tThing@@QAEXXZ");
        if (physPos) physPos(thing, before);
        if (moveTo) moveTo(thing, before);
        if (zero) zero(thing);
    }
}
// The Scout's exhaust: its eight "scoutThruster" effects (game.nfx controller scoutThrusters) sit on model markers of
// type 17, index 1-8, but scout.nod has no markers at all, so all eight drew at the model's origin as one flame in
// the middle of its body. The model gets those markers (cModel +0x128 list / +0x12C count, 0x50-byte tModelMarker:
// local matrix, +0x40 bone, +0x41 type, +0x42 index) at its four tail vents (from its vertices: x +-0.8, upper
// pair z 4.4 / lower pair z 2.8, at the tail end y -8.0), two effects per vent. Allocated with the game's allocator,
// as the model frees its own list.
static void AddScoutMarkers(uint8_t *world) {
    typedef void *(__cdecl *MallocFn)(unsigned, const char *, unsigned);
    typedef int(__cdecl *OpenFn)();
    static MallocFn alloc;
    static OpenFn open;
    if (!alloc) alloc = (MallocFn)SymAddr("?stdMemory_Malloc@@YAPAXIPADI@Z");
    if (!open) open = (OpenFn)SymAddr("?stdMemory_Open@@YAHXZ");
    if (!alloc || !open) return;
    static const float vents[4][3] = {{-0.8f, -8.0f, 4.4f}, {0.8f, -8.0f, 4.4f}, {-0.8f, -8.1f, 2.8f}, {0.8f, -8.1f, 2.8f}};
    __try {
        uint32_t sectors = *(uint32_t *)(world + 0xdc);
        uint8_t *sector = *(uint8_t **)(world + 0xe0);
        for (uint32_t i = 0; i < sectors; i++, sector += 0x110)
            for (uint8_t *t = *(uint8_t **)(sector + 0x24); t; t = *(uint8_t **)(t + 0x28)) {
                uint8_t *inst = *(uint8_t **)(t + 0x2a0);
                uint8_t *model = inst ? *(uint8_t **)inst : nullptr;
                if (!model || *(unsigned *)(model + 0x12c) || _stricmp((const char *)(model + 0x8c), "scout.nod")) continue;
                open();
                uint8_t *m = (uint8_t *)alloc(8 * 0x50, "new", 0);
                if (!m) return;
                memset(m, 0, 8 * 0x50);
                for (int k = 0; k < 8; k++) {
                    float *mat = (float *)(m + k * 0x50);
                    mat[0] = mat[5] = -1.f;   // turned 180 deg about z: the flames point back, out of the vents
                    mat[10] = mat[15] = 1.f;
                    memcpy(mat + 12, vents[k % 4], 12);
                    m[k * 0x50 + 0x41] = 17;                            // type
                    *(uint16_t *)(m + k * 0x50 + 0x42) = (uint16_t)(k + 1);   // index 1-8: 1/5, 2/6, 3/7, 4/8 share a vent
                }
                *(uint8_t **)(model + 0x128) = m;
                *(unsigned *)(model + 0x12c) = 8;
                Log("levelscript: scout.nod: 8 thruster markers at its tail vents");
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
// Losing its target, the Scout (and Wraith) goes idle and hovers where it is (its idle update runs Search every
// second). 2 minutes after it last fought or searched (states 4, 11, 12, 54), idle away from where it was placed, it
// patrols back there (FlyerStartPatrol gives it that patrol point) and hovers. Timed here, every 15 frames: an AI far
// from the player updates rarely, so a timer in its own idle update kept starting over.
static UINT g_scoutHomeAfter = 120000;   // GHOSTPC_SCOUT_HOME_MS (tests)
static void DriveScoutsHome(uint8_t *world) {
    static uint32_t vt;   // (no initialised statics next to __try)
    if (!vt) vt = SymAddr("??_7cAIProtossScout@@6B@");
    if (!vt) return;
    UINT now = GameTime();
    uint8_t *found[8];
    int n = 0;
    __try {
        uint32_t sectors = *(uint32_t *)(world + 0xdc);
        uint8_t *sector = *(uint8_t **)(world + 0xe0);
        for (uint32_t i = 0; i < sectors; i++, sector += 0x110)
            for (uint8_t *t = *(uint8_t **)(sector + 0x24); t && n < 8; t = *(uint8_t **)(t + 0x28))
                if (uint8_t *ai = *(uint8_t **)(t + 0x3c4))
                    if (!IsBadReadPtr(ai, 4) && *(uint32_t *)ai == vt && *(float *)(t + 0x36c) > 0.f) found[n++] = ai;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    typedef void(__thiscall *IntFn)(void *, int);
    for (int i = 0; i < n; i++) {
        uint8_t *ai = found[i], *thing = *(uint8_t **)(ai + 8);
        ScoutSearch &rec = ScoutSearchOf(ai);
        int st = *(int *)(ai + 0x18);
        if (!rec.lastActive || st == 4 || st == 11 || st == 12 || st == 54) rec.lastActive = now;
        if (st != 2 || now - rec.lastActive < g_scoutHomeAfter) continue;
        rec.lastActive = now;
        const float *p = (const float *)(thing + 0xb0), *home = (const float *)(thing + 0x350);
        float dx = p[0] - home[0], dy = p[1] - home[1], dz = p[2] - home[2];
        if (dx * dx + dy * dy + dz * dz < 3.f * 3.f) continue;   // already home
        Log("levelscript: scout %p idle %u s away from its post, flying back", ai, g_scoutHomeAfter / 1000);
        ((IntFn)(*(uint32_t **)ai)[0x5b0 / 4])(ai, 5);   // patrolling
    }
}
static void FixScoutFlight() {
    if (const char *e = getenv("GHOSTPC_SCOUT_HOME_MS")) g_scoutHomeAfter = atoi(e);
    if (!g_lineOfSight) g_lineOfSight = (LineOfSightFn)SymAddr("?simCollision_LineOfSight@@YAHPAUXGVECTOR3@@0W4eRaycastDbgID@@@Z");
    const char *vt = "??_7cAIProtossScout@@6B@";
    g_scoutAcquired = (UpdateFn)WrapVirtual(vt, "?ASStartAcquired@cAIProtossScout@@UAEXXZ", (void *)ScoutStartAcquired);
    g_scoutCombat = (UpdateFn)WrapVirtual(vt, "?ASStartCombatRanged@cAIProtossScout@@UAEXXZ", (void *)ScoutStartCombat);
    g_scoutFly = (UpdateFn)WrapVirtual(vt, "?CMUpdateFly@cAIBaseFlyer@@MAEXXZ", (void *)ScoutFly);
    if (uint32_t *v = (uint32_t *)SymAddr(vt)) {
        if (v[0x1a4 / 4] == SymAddr("?Lockdown@cAIProtossScout@@UAEXXZ")) {
            uint32_t lock = (uint32_t)ScoutLockdown, unlock = (uint32_t)ScoutUnLockdown;
            WriteCode(&v[0x1a4 / 4], &lock, 4);
            WriteCode(&v[0x1a8 / 4], &unlock, 4);
        } else
            Log("levelscript: unexpected cAIProtossScout vtable, lockdown not fixed");
        uint32_t stub = SymAddr("?ASStartCurious@cAIInstance@@UAEXXZ");   // the empty stub both curious slots hold
        if (stub && v[0x5e0 / 4] == stub && v[0x6d8 / 4] == stub) {
            uint32_t start = (uint32_t)ScoutStartCurious, update = (uint32_t)ScoutUpdateCurious;
            WriteCode(&v[0x5e0 / 4], &start, 4);
            WriteCode(&v[0x6d8 / 4], &update, 4);
        } else
            Log("levelscript: unexpected cAIProtossScout vtable, curious state not added");
    }
    // a combat pass that times out went idle (state 2) and stayed there with the target in view: it goes back to
    // acquired (11) instead, whose start drops to idle itself when the target is no longer valid
    uint8_t *upd = (uint8_t *)SymAddr("?ASUpdateCombatRanged@cAIProtossScout@@UAEXXZ");
    static const uint8_t idle[] = {0x6a, 0x02, 0x8b, 0xce, 0xff, 0x90, 0xb0, 0x05, 0x00, 0x00};   // push 2; ...SetState
    static const uint8_t acquired = 0x0b;
    if (upd && !memcmp(upd + 0x56, idle, sizeof idle)) WriteCode(upd + 0x57, &acquired, 1);
    else Log("levelscript: unexpected cAIProtossScout::ASUpdateCombatRanged, combat timeout not changed");
}
// The Purifier's beam aimed above Nova and so never hit her (no damage). FirePurifierGun (cAIProtossPurifier vtable)
// turns on its "purifierBeam" effect controller (game.nfx: purifLight, a cEffectLight, and purifBeam, a "smoke"
// cEffectParticleSmoke of type elec from gun marker 2/0, damage 3 per particle that touches) and hands the controller
// the target's GUID; the controller passes it to each part (SetTargetThing(guid), effect vtable +0x18), but both keep
// cEffect's, which does nothing: the particles only flew along the gun's aim, a little high. The smoke effect can
// steer its particles to a target (cEffectParticleSmoke::UpdatePosition with flag +0x104 & 0x2000000 homes them on
// +0x140): after each call, each part gets the target's chest (1 m above its feet) through SetTargetPos (vtable +0x14,
// which sets both). The controller's parts: +0xA8 (12-byte entries, effect first), count byte +0xC4.
typedef void(__thiscall *FireGunFn)(void *, float);
static FireGunFn g_purifierFire;
static float g_beamUp = 1.f;   // GHOSTPC_BEAM_UP: target height above the feet (tests)
static void __fastcall PurifierFire(uint8_t *ai, void *, float dt) {
    g_purifierFire(ai, dt);
    typedef uint8_t *(__cdecl *FindFn)(unsigned);
    static FindFn find = (FindFn)SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    uint8_t *target = find && *(unsigned *)(ai + 0x2d4) ? find(*(unsigned *)(ai + 0x2d4)) : nullptr;
    uint8_t *ctrl = *(uint8_t **)(ai + 0x570);
    if (!target || !ctrl || !*(uint8_t **)(ctrl + 0xa8)) return;
    const float *t = (const float *)(target + 0xb0);
    float aim[3] = {t[0], t[1], t[2] + g_beamUp};
    uint8_t *parts = *(uint8_t **)(ctrl + 0xa8);
    typedef void(__thiscall *PosFn)(void *, float *, int);
    for (unsigned i = 0; i < *(uint8_t *)(ctrl + 0xc4); i++)
        if (uint8_t *e = *(uint8_t **)(parts + i * 12)) ((PosFn)(*(uint32_t **)e)[0x14 / 4])(e, aim, 1);
    static bool dbg = getenv("GHOSTPC_LOGDEATH") != nullptr;
    static int n;
    if (dbg && n++ % 15 == 0) Log("purifier-dbg: beam aimed at %.2f %.2f %.2f", aim[0], aim[1], aim[2]);
}
static void FixPurifierBeam() {
    if (const char *e = getenv("GHOSTPC_BEAM_UP")) g_beamUp = (float)atof(e);
    g_purifierFire = (FireGunFn)WrapVirtual("??_7cAIProtossPurifier@@6B@", "?FirePurifierGun@cAIProtossPurifier@@UAEXM@Z",
                                            (void *)PurifierFire);
}
static void InstallFlyDebug() {
    if (!getenv("GHOSTPC_FLY_DEBUG")) return;
    g_pathMove = (UpdateFn)WrapVirtual("??_7cAIZergMutalisk@@6B@", "?CMUpdatePathMove@cAIBaseFlyer@@MAEXXZ", (void *)DebugPathMove);
}
static void FixLurkerSounds() {
    const char *vt = "??_7cAIBossLurker@@6B@";
    g_lurkerKilled = (KilledFn)WrapVirtual(
        vt, "?Killed@cAIBossLurker@@UAEXPAVtThing@@W4eDamageSource@@W4eDamageType@@MIPAUXGVECTOR3@@3W4eHitRegionType@@MAAH@Z",
        (void *)LurkerKilled);
    g_lurkerBurrow = (UpdateFn)WrapVirtual(vt, "?CMStartBurrow@cAIBossLurker@@UAEXXZ", (void *)LurkerBurrow);
    g_lurkerUnburrow = (UpdateFn)WrapVirtual(vt, "?CMStartUnburrow@cAIBossLurker@@UAEXXZ", (void *)LurkerUnburrow);
    g_lurkerBurst = (UpdateFn)WrapVirtual(vt, "?CMStartSpecial1@cAIBossLurker@@UAEXXZ", (void *)LurkerBurst);
    uint8_t *cb = (uint8_t *)SymAddr("?AnimationCallback@cAIBossLurker@@UAEXPAVcKeyChannel@@W4eRenderKeyTag@@I@Z");
    uint8_t *fire = (uint8_t *)SymAddr("?FireSpike@cAIBossLurker@@QAEXXZ");
    uint8_t *call = cb ? cb + 0x125 : nullptr;   // call FireSpike (0x4EEA45 originally)
    if (call && fire && call[0] == 0xE8 && call + 5 + *(int32_t *)(call + 1) == fire) {
        g_lurkerFireSpike = (UpdateFn)fire;
        int32_t rel = (int32_t)((uint8_t *)LurkerFireSpike - (call + 5));
        WriteCode(call + 1, &rel, 4);
    } else
        Log("levelscript: unexpected code in cAIBossLurker::AnimationCallback, spike sounds off");
    g_lurkerRoar = (UpdateFn)WrapVirtual(vt, "?CMStartSpecial4@cAIBossLurker@@UAEXXZ", (void *)LurkerRoar);
}

// AI classes whose code is in Ghost.exe but which aiClass_Startup never registers, so templates naming them get no AI:
// Tanagazj's template asks for BOSSTANAGAZJ (cAIBossTanagazj, a complete boss AI). Registered like aiClass_Startup
// does it: a 0x28-byte cAIClass (name[32], +0x20 prototype instance, +0x24 0) in aiClass_aClasses and the name
// hashtable. cAIBossTanagazj::NewInstance builds a full instance without using its prototype's data.
// Tanagazj's boss AI is cut content: registered here with its two bugs fixed (FixTanagazjClass), so his template
// gets the AI. GHOSTPC_NO_TANAGAZJ_AI keeps him without one, as in the original build.
static void RegisterMissingAIClasses() {
    static bool done;
    if (done) return;
    static bool detectorFixed;
    if (!detectorFixed) detectorFixed = true, FixDetectorClass(), HookDemoAnimsDamaged(), HookLoadWorld(), HookSaveRestore(), FixUltraliskLOS(), FixUltraliskTusks(), FixUltraliskStomp(),
                          FixSunkenColony(), FixStillProjectiles(),
                          FixLurkerSounds(), InstallFlyDebug(),
                          FixOverlordFlight(), FixFlyerPatrol(), FixLurkerDamage(), FixSniperDamage(), FixPurifierBeam(),
                          FixGoliathBoss(), FixVultureSight(),
                          FixScoutFlight();   // all levels
    static bool enabled = getenv("GHOSTPC_NO_TANAGAZJ_AI") == nullptr;
    if (!enabled) { done = true; return; }
    uint32_t *num = (uint32_t *)SymAddr("?aiClass_numClasses@@3IA");
    void **table = (void **)SymAddr("?aiClass_pHashtable@@3PAVtHashtable@@A");
    uint8_t **classes = (uint8_t **)SymAddr("?aiClass_aClasses@@3PAPAVcAIClass@@A");
    if (!num || !table || !classes || !*num || !*table) return;   // not started yet
    done = true;
    typedef void *(__cdecl *FindFn)(void *, const char *);
    typedef int(__cdecl *AddFn)(void *, const char *, void *);
    typedef void *(__cdecl *MallocFn)(unsigned, const char *, unsigned);
    typedef int(__cdecl *OpenFn)();
    typedef void *(__thiscall *NewInstanceFn)(void *);
    auto find = (FindFn)SymAddr("?stdHashtable_Find@@YAPAXPAVtHashtable@@PBD@Z");
    auto add = (AddFn)SymAddr("?stdHashtable_Add@@YAHPAVtHashtable@@PBDPAX@Z");
    auto alloc = (MallocFn)SymAddr("?stdMemory_Malloc@@YAPAXIPADI@Z");
    auto open = (OpenFn)SymAddr("?stdMemory_Open@@YAHXZ");
    static const struct { const char *name, *newInstance; } missing[] = {
        {"BOSSTANAGAZJ", "?NewInstance@cAIBossTanagazj@@UAEPAVcAIInstance@@XZ"},
        // the cut Goliath boss (cAIBossGoliath: boss bar, launchers and machine guns, run-MG / blowback / knockback
        // modes); no template names it, the Enemy Zoo gives it to Goliath_222c with a map-only ACTOR_AI override
        {"BOSSGOLIATH", "?NewInstance@cAIBossGoliath@@UAEPAVcAIInstance@@XZ"},
        // the TerranVulture template asks for TERRANVULTURE (cAITerranVulture: guns, spider mines, chase / charge /
        // flee), a complete vehicle AI that nothing registers, so the Vulture had no AI in any level
        {"TERRANVULTURE", "?NewInstance@cAITerranVulture@@UAEPAVcAIInstance@@XZ"},
    };
    for (auto &m : missing) {
        auto make = (NewInstanceFn)SymAddr(m.newInstance);
        if (!find || !add || !alloc || !open || !make || find(*table, m.name)) continue;
        if (*num >= 128) { Log("levelscript: AI class table full"); return; }   // aiClass_aClasses holds 128
        open();
        uint8_t *cls = (uint8_t *)alloc(0x28, "new", 0);
        memset(cls, 0, 0x28);
        strncpy((char *)cls, m.name, 0x1f);
        *(void **)(cls + 0x20) = make(nullptr);
        classes[*num] = cls;
        add(*table, (const char *)cls, cls);
        ++*num;
        Log("levelscript: registered missing AI class %s", m.name);
    }
    FixTanagazjClass();
}

// Debug (GHOSTPC_TANAGAZJ_DEBUG): notify boxes fire wherever the player is, the AI that received a notify is shot once
// by the player (tActor::Damage, which starts the boss's combat) and its state is logged. GHOSTPC_TANAGAZJ_DEBUG=shoot
// instead shoots Tanagazj without forcing the notify (tests the damage trigger), =watch forces the notify without
// shooting, =log only logs (play normally). Logged: AI state (+0x18), and for
// cAIBossTanagazj the phase (+0x594) and blade flag (+0x548, set by BladeOn).
static int DebugAIMode() {   // 0 off, 1 force the notify and shoot, 2 shoot only, 3 force the notify only ("watch"),
                             // 4 only log the AI a notify reached ("log": play normally)
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("GHOSTPC_TANAGAZJ_DEBUG");
        mode = !e ? 0 : !strcmp(e, "shoot") ? 2 : !strcmp(e, "watch") ? 3 : !strcmp(e, "log") ? 4 : 1;
    }
    return mode;
}
static bool DebugAI() { return DebugAIMode() != 0; }
static UINT DebugWarpFrame() {
    static int f = -1;
    if (f < 0) f = getenv("GHOSTPC_TANAGAZJ_DEBUG_WARPFRAME") ? atoi(getenv("GHOSTPC_TANAGAZJ_DEBUG_WARPFRAME")) : 40;
    return (UINT)f;
}
typedef int(__thiscall *SpotLOSFn)(void *, unsigned short, void *);
static SpotLOSFn g_spotLOS;
static uint32_t g_sniperVt;
static int g_debugLockAt = -2;
static UINT g_debugLogFrames;
static void DebugAIUpdate(uint8_t *player) {
    if (!g_lastAI) return;
    if (!g_debugLogFrames) {
        const char *e = getenv("GHOSTPC_TANAGAZJ_DEBUG_FRAMES");
        g_debugLogFrames = e ? atoi(e) : 900;
    }
    if (g_debugLockAt == -2) {
        const char *e = getenv("GHOSTPC_TANAGAZJ_DEBUG_LOCKDOWN");
        g_debugLockAt = e ? atoi(e) : -1;
    }
    int lockAt = g_debugLockAt;
    if (!g_damage) {
        g_damage = (DamageFn)SymAddr(
            "?Damage@tActor@@QAEMPAVtThing@@W4eDamageSource@@W4eDamageType@@MPAUXGVECTOR3@@3IW4eHitRegionType@@MPAH@Z");
        g_spotLOS = (SpotLOSFn)SymAddr("?HasLOSFromSpot@cAIBossSniper@@AAEHGPAVtActor@@@Z");
        g_sniperVt = SymAddr("??_7cAIBossSniper@@6B@");
    }
    __try {
        uint8_t *thing = *(uint8_t **)(g_lastAI + 8);
        UINT age = g_frame - g_lastAIFrame;
        // GHOSTPC_TANAGAZJ_DEBUG_SHOOTDELAY: frames after the notify (or the shoot-only pick) to shoot it, default 60
        static int shootDelay = -1;
        if (shootDelay < 0) {
            const char *e = getenv("GHOSTPC_TANAGAZJ_DEBUG_SHOOTDELAY");
            shootDelay = e ? atoi(e) : 60;
        }
        // GHOSTPC_TANAGAZJ_DEBUG_LOCKDOWN=<frames after the pick>: tObject::Lockdown(5000 ms) on the watched AI's actor
        if (lockAt >= 0 && age == (UINT)lockAt) {
            typedef int(__thiscall *LockFn)(void *, unsigned);
            if (auto lock = (LockFn)SymAddr("?Lockdown@tObject@@QAEHI@Z")) Log("ai-dbg: lockdown -> %d", lock(thing, 5000));
        }
        // GHOSTPC_DEBUG_MARKERS=<type>: log the watched AI model's markers of that type (index 0-15, model space)
        if (age == 60 && getenv("GHOSTPC_DEBUG_MARKERS")) {
            typedef int(__thiscall *MarkerFn)(void *, unsigned char, unsigned short, float *, void *);
            auto pos = (MarkerFn)SymAddr("?GetMarkerPos@cModelInst@@QAEHEGPAUXGVECTOR3@@PAUXGMATRIX@@@Z");
            uint8_t *model = *(uint8_t **)(thing + 0x2a0);
            int want = atoi(getenv("GHOSTPC_DEBUG_MARKERS"));   // -1: every type 0-39, index 0-9 (found ones only)
            for (int type = want < 0 ? 0 : want; type <= (want < 0 ? 39 : want); type++)
                for (int i = 0; pos && model && i < (want < 0 ? 10 : 16); i++) {
                    float v[3] = {0, 0, 0};
                    int ok = pos(model, (unsigned char)type, (unsigned short)i, v, nullptr);
                    if (ok || want >= 0) Log("ai-dbg: marker type %d index %d -> %d at %.2f %.2f %.2f", type, i, ok, v[0], v[1], v[2]);
                }
            const float *tp = (const float *)(thing + 0xb0);
            Log("ai-dbg: thing at %.2f %.2f %.2f", tp[0], tp[1], tp[2]);
            if (model) {   // where the cModel keeps its name (printable runs in its first 0x140 bytes)
                uint8_t *cm = *(uint8_t **)model;
                for (int o = 0; o < 0x140; o += 4) {
                    const char *c = (const char *)(cm + o);
                    int n = 0;
                    while (n < 32 && c[n] >= 32 && c[n] < 127) n++;
                    if (n >= 4 && c[n] == 0) Log("ai-dbg: cModel +0x%x '%s'", o, c);
                }
                Log("ai-dbg: cModel %p markers %p count %u", cm, *(void **)(cm + 0x128), *(unsigned *)(cm + 0x12c));
            }
        }
        if (age == (UINT)shootDelay && (DebugAIMode() == 1 || DebugAIMode() == 2)) {
            float *pos = (float *)(thing + 0xb0);
            float dir[3] = {0, 0, 1};
            if (g_damage) {
                float r = g_damage(thing, player, 0, 0, 1.f, pos, dir, 0, 0, 1.f, nullptr);
                Log("ai-dbg: damaged the AI's actor (returned %g)", r);
            }
        }
        // GHOSTPC_TANAGAZJ_DEBUG_FRAMES: how long the watched AI is logged (frames after the pick, default 900)
        if (age % 30 == 0 && (age <= g_debugLogFrames || DebugAIMode() == 4)) {
            const float *p = (const float *)(thing + 0xb0);
            UINT now = *(UINT *)(SymAddr("?stdTime@@3VtStdTime@@A") + 0x44);   // game time (ms)
            const float *pp = (const float *)(player + 0xb0);
            Log("ai-dbg: frame %u cm %d state %d phase %d blade %d cloak %d/%d dist %.1f pos %.1f %.1f %.1f (sniper: phase %u spot %u next %u) (ultra: stage %d) fwd %.2f %.2f sight %.1f cm378 %d goal %.1f %.1f %.1f patrol %.1f %.1f %.1f #%u",
                g_frame, *(int *)(g_lastAI + 0x374), *(int *)(g_lastAI + 0x18), *(int *)(g_lastAI + 0x594), *(int *)(g_lastAI + 0x548),
                *(UINT *)(g_lastAI + 0x568) ? (int)(*(UINT *)(g_lastAI + 0x568) - now) : 0,
                (int)(*(UINT *)(g_lastAI + 0x56c) - now),
                sqrtf((p[0] - pp[0]) * (p[0] - pp[0]) + (p[1] - pp[1]) * (p[1] - pp[1])), p[0], p[1], p[2],
                *(uint16_t *)(g_lastAI + 0x560), *(uint16_t *)(g_lastAI + 0x562), *(uint16_t *)(g_lastAI + 0x558),
                *(int *)(g_lastAI + 0x584), ((float *)(thing + 0x90))[0], ((float *)(thing + 0x90))[1],
                *(float *)(g_lastAI + 0x330), *(int *)(g_lastAI + 0x378), *(float *)(g_lastAI + 0x2e0), *(float *)(g_lastAI + 0x2e4),
                *(float *)(g_lastAI + 0x2e8), *(float *)(g_lastAI + 0x25c), *(float *)(g_lastAI + 0x260),
                *(float *)(g_lastAI + 0x264), *(unsigned *)(g_lastAI + 0x278));
            // effects attached to the actor (tThing +0x140, linked through cEffect +0x58)
            int n = 0;
            for (uint8_t *e = *(uint8_t **)(thing + 0x140); e && n < 12; e = *(uint8_t **)(e + 0x58), n++) {
                char cls[128];
                SymName(*(uint32_t *)e, cls, sizeof cls);
                Log("ai-dbg:   effect %p %s flags %08x", e, cls, *(uint32_t *)(e + 0x50));
            }
            if (*(uint32_t *)g_lastAI == g_sniperVt) {
                // sniper: his track frames (tThing +0x134, 28 bytes: rotation, position, uint32) and line of sight
                // from each spot to the player (cAIBossSniper::HasLOSFromSpot, frame[spot + 1])
                const float *fr = *(const float **)(thing + 0x134);
                if (age == 30)
                    for (int i = 0; fr && i < 4; i++)
                        Log("ai-dbg:   frame %d pos %.2f %.2f %.2f", i, fr[i * 7 + 3], fr[i * 7 + 4], fr[i * 7 + 5]);
                if (fr && g_spotLOS)
                    Log("ai-dbg:   LOS from spots: %d %d %d", g_spotLOS(g_lastAI, 0, player), g_spotLOS(g_lastAI, 1, player),
                        g_spotLOS(g_lastAI, 2, player));
                else
                    Log("ai-dbg:   no track frames");
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("ai-dbg: fault");
        g_lastAI = nullptr;
    }
}

// Debug (GHOSTPC_PROBE_DEBUG): every 30 frames, log the flyer facing state of each cAIDetector (the Probe's AI) and
// cAIProtossObserver: AI flags (+0xC; cAIBaseFlyer::UpdateFacing turns only with 0x10000, or tThing +0x3C8 bit 0),
// current/desired yaw (+0x3A0/+0x3A4), turn rate (+0x3A8), controller mode (+0x378), state (+0x18)
static void DebugProbe(uint8_t *world) {
    static uint32_t det, obs, demo;
    static UINT calls;
    typedef int(__thiscall *ModeFn)(void *);
    static ModeFn moveMode, actionMode;
    if (!det) {
        det = SymAddr("??_7cAIDetector@@6B@"), obs = SymAddr("??_7cAIProtossObserver@@6B@");
        demo = SymAddr("??_7cAIDemoAnims@@6B@");
        moveMode = (ModeFn)SymAddr("?GetMoveMode@tObject@@QAE?AW4eMotionMode@@XZ");
        actionMode = (ModeFn)SymAddr("?GetActionMode@tObject@@QAE?AW4eMotionMode@@XZ");
    }
    if (!det || ++calls % 30) return;
    __try {
        uint32_t sectors = *(uint32_t *)(world + 0xdc);
        uint8_t *sector = *(uint8_t **)(world + 0xe0);
        for (uint32_t i = 0; i < sectors; i++, sector += 0x110)
            for (uint8_t *t = *(uint8_t **)(sector + 0x24); t; t = *(uint8_t **)(t + 0x28)) {
                uint8_t *ai = *(uint8_t **)(t + 0x3c4);
                if (ai && !IsBadReadPtr(ai, 4) && *(uint32_t *)ai == demo && moveMode && actionMode) {
                    Log("probe-dbg: frame %u prop %p counter %u move mode %d action mode %d", g_frame, ai,
                        *(uint32_t *)(ai + 0x374), moveMode(t), actionMode(t));
                    continue;
                }
                if (!ai || IsBadReadPtr(ai, 4) || (*(uint32_t *)ai != det && *(uint32_t *)ai != obs)) continue;
                Log("probe-dbg: frame %u %s state %d flags %08x thing3c8 %08x mode %d yaw %.1f -> %.1f rate %.0f", g_frame,
                    *(uint32_t *)ai == det ? "detector" : "observer", *(int *)(ai + 0x18), *(uint32_t *)(ai + 0xc),
                    *(uint32_t *)(t + 0x3c8), *(int *)(ai + 0x378), *(float *)(ai + 0x3a0), *(float *)(ai + 0x3a4),
                    *(float *)(ai + 0x3a8));
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// called once per frame (ShellWatch)
// Crash checks: the per-unit tables of the fixes above keep AI / actor pointers across restarts (cSaveData::Restore)
// and level changes. Every 60 frames and at each (re)load, each entry is looked up among the actors in the world's
// sectors (their AIs, tActor +0x3C4); an entry that is not there any more is logged and dropped.
static uint8_t *g_liveThings[16384], *g_liveAIs[16384];
static int WalkLive(uint8_t *world, int *nAIs) {   // -1 on a fault
    int nt = 0, na = 0;
    __try {
        uint32_t sectors = *(uint32_t *)(world + 0xdc);
        uint8_t *sector = *(uint8_t **)(world + 0xe0);
        for (uint32_t i = 0; i < sectors && i < 100000; i++, sector += 0x110)
            for (uint8_t *t = *(uint8_t **)(sector + 0x24); t && nt < 16384; t = *(uint8_t **)(t + 0x28)) {
                g_liveThings[nt++] = t;
                if (uint8_t *ai = *(uint8_t **)(t + 0x3c4)) g_liveAIs[na++] = ai;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    *nAIs = na;
    return nt;
}
static void CheckStaleTables(uint8_t *world, const char *why) {
    int na = 0, nt = WalkLive(world, &na);
    if (nt < 0) {
        Log("crashcheck: fault while walking the sectors (%s)", why);
        return;
    }
    auto check = [&](uint8_t *&p, bool thing, const char *table) {
        if (!p) return;
        uint8_t **live = thing ? g_liveThings : g_liveAIs;
        int n = thing ? nt : na;
        for (int i = 0; i < n; i++)
            if (live[i] == p) return;
        Log("crashcheck: stale %s entry %p dropped (%s, frame %u)", table, p, why, g_frame);
        p = nullptr;
    };
    for (auto &b : g_goliathBosses) check(b.ai, false, "goliath boss");
    for (auto &r : g_lurkerRoars) check(r.ai, false, "lurker roar");
    for (auto &l : g_lurkerSpikes)
        if (l.guid) {
            check(l.owner, true, "lurker spike owner");
            if (!l.owner) l.guid = 0;
        }
    for (auto &o : g_overlordHome) check(o.ai, false, "overlord altitude");
    for (auto &c : g_scoutSearch) check(c.ai, false, "scout search");
    check(g_lastAI, false, "debug watch");
}
// the per-unit tables start over with each level load and restart
static void ResetUnitTables() {
    memset(g_goliathBosses, 0, sizeof g_goliathBosses);
    memset(g_lurkerRoars, 0, sizeof g_lurkerRoars);
    memset(g_lurkerSpikes, 0, sizeof g_lurkerSpikes);
    memset(g_spikes, 0, sizeof g_spikes);
    memset(g_overlordHome, 0, sizeof g_overlordHome);
    memset(g_scoutSearch, 0, sizeof g_scoutSearch);
}
// debug (GHOSTPC_LOG_POS): every 30 frames, the local players' positions (tThing +0xB0) and camera yaw (tSimCamera
// +0x20C, cPlayerInfo +4), in level-script form
static void LogPositions() {
    static bool on = getenv("GHOSTPC_LOG_POS") != nullptr;
    if (!on || g_frame % 30) return;
    uint32_t names = SymAddr("?gameMotion_apNames@@3PAPADA");
    for (int i = 0; i < 2; i++) {
        uint8_t **info = *(uint8_t ***)(names + 0x728 + 4 * i);
        uint8_t *p = info ? info[0] : nullptr, *cam = info ? info[1] : nullptr;
        if (!p) continue;
        const float *q = (const float *)(p + 0xb0);
        Log("pos: player %d %.2f %.2f %.2f yaw %.0f", i + 1, q[0], q[1], q[2], cam ? *(float *)(cam + 0x20c) : 0.f);
    }
}
// debug (GHOSTPC_WARP="x y z yaw [frame]", GHOSTPC_WARP2 likewise): move player 1 there <frame> (default 60) frames
// after the load; WARP2 a second move (e.g. after a cutscene the first one triggered); player 2 1.5 m to her right
static void DebugWarp() {
    static float w[2][5];
    static int n[2] = {-1, -1};
    static const char *vars[2] = {"GHOSTPC_WARP", "GHOSTPC_WARP2"};
    UINT f = g_frame - g_loadFrame;
    for (int i = 0; i < 2; i++) {
        if (n[i] < 0) {
            const char *e = getenv(vars[i]);
            n[i] = e ? sscanf(e, "%f %f %f %f %f", &w[i][0], &w[i][1], &w[i][2], &w[i][3], &w[i][4]) : 0;
            if (n[i] < 5) w[i][4] = 60;
        }
        if (n[i] < 4 || f != (UINT)w[i][4]) continue;
        uint32_t names = SymAddr("?gameMotion_apNames@@3PAPADA");
        uint8_t **l = *(uint8_t ***)(names + 0x728), **r = *(uint8_t ***)(names + 0x72c);
        // GHOSTPC_WARP_SWAP=1: player 2 on the second warp's spot, player 1 beside her; =2: on both
        static int swap = getenv("GHOSTPC_WARP_SWAP") ? atoi(getenv("GHOSTPC_WARP_SWAP")) : 0;
        if ((swap == 2 || (swap == 1 && i == 1)) && r && r[0]) std::swap(l, r);
        // GHOSTPC_WARP2_ONLY=1: the second warp moves player 2 alone, onto its spot (e.g. into player 1's line of fire)
        static bool p2only = getenv("GHOSTPC_WARP2_ONLY") != nullptr;
        if (i == 1 && p2only) {
            if (r && r[0]) WarpPlayer(r[0], w[i]);
            if (r && r[1]) *(float *)(r[1] + 0x20c) = w[i][3];
            Log("warp: player 2 to %.1f %.1f %.1f yaw %.0f (frame %u)", w[i][0], w[i][1], w[i][2], w[i][3], f);
            continue;
        }
        if (l && l[0]) {
            WarpPlayer(l[0], w[i]);
            if (l[1]) *(float *)(l[1] + 0x20c) = w[i][3];   // her camera (cPlayerInfo +4 tSimCamera, yaw +0x20C) too
        }
        if (r && r[0]) {
            float a = w[i][3] * 0.0174533f, p2[4] = {w[i][0] + 1.5f * cosf(a), w[i][1] + 1.5f * sinf(a), w[i][2], w[i][3]};
            WarpPlayer(r[0], p2);
            if (r[1]) *(float *)(r[1] + 0x20c) = w[i][3];
        }
        Log("warp: player 1 to %.1f %.1f %.1f yaw %.0f (frame %u)", w[i][0], w[i][1], w[i][2], w[i][3], f);
    }
}
static void CalldownTest(uint8_t *world) {
    LogPositions();
    DebugWarp();
    // debug (GHOSTPC_CALLDOWN_TEST=<n>): 60 frames after the load, list the Overlords (model overlord*.nod) and move
    // player 1 (and player 2) next to a ground unit near the n-th listed (-1: the nearest), facing it
    const char *e = g_frame - g_loadFrame == 60 ? getenv("GHOSTPC_CALLDOWN_TEST") : nullptr;
    if (e && g_frame - g_loadFrame == 60) {
        int pick = -1, k = 0;
        sscanf(e, "%d", &pick);
        uint32_t names = SymAddr("?gameMotion_apNames@@3PAPADA");
        uint8_t **l = *(uint8_t ***)(names + 0x728), **r = *(uint8_t ***)(names + 0x72c);
        uint8_t *p1 = l ? l[0] : nullptr;
        float best = 1e30f, ov[3] = {};
        __try {
            uint32_t sectors = *(uint32_t *)(world + 0xdc);
            uint8_t *sector = *(uint8_t **)(world + 0xe0);
            for (uint32_t i = 0; i < sectors; i++, sector += 0x110)
                for (uint8_t *t = *(uint8_t **)(sector + 0x24); t; t = *(uint8_t **)(t + 0x28)) {
                    uint8_t *inst = *(uint8_t **)(t + 0x2a0);
                    uint8_t *model = inst ? *(uint8_t **)inst : nullptr;
                    if (!model || _strnicmp((const char *)(model + 0x8c), "overlord", 8)) continue;
                    const float *q = (const float *)(t + 0xb0), *pp = p1 ? (const float *)(p1 + 0xb0) : q;
                    float d = (q[0] - pp[0]) * (q[0] - pp[0]) + (q[1] - pp[1]) * (q[1] - pp[1]);
                    Log("calldown test: %s at %.1f %.1f %.1f (%.0f m from player 1 at %.1f %.1f %.1f)",
                        (const char *)(model + 0x8c), q[0], q[1], q[2], sqrtf(d), pp[0], pp[1], pp[2]);
                    if (pick >= 0 ? k == pick : d < best) best = d, memcpy(ov, q, sizeof ov);
                    k++;
                }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        // where to stand: beside the ground unit (an AI actor 3+ m below the Overlord) nearest to it, 8-40 m away
        // (its feet are on the floor; a guessed height dropped Nova out of the world)
        float ground[3] = {}, gbest = 1e30f;
        __try {
            uint32_t sectors = *(uint32_t *)(world + 0xdc);
            uint8_t *sector = *(uint8_t **)(world + 0xe0);
            for (uint32_t i = 0; best < 1e29f && i < sectors; i++, sector += 0x110)
                for (uint8_t *t = *(uint8_t **)(sector + 0x24); t; t = *(uint8_t **)(t + 0x28)) {
                    if (!*(uint8_t **)(t + 0x3c4)) continue;   // AI actors only
                    const float *q = (const float *)(t + 0xb0);
                    float d = (q[0] - ov[0]) * (q[0] - ov[0]) + (q[1] - ov[1]) * (q[1] - ov[1]);
                    if (q[2] > ov[2] - 3 || d < 64 || d > 1600 || d >= gbest) continue;
                    gbest = d, memcpy(ground, q, sizeof ground);
                }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (p1 && gbest < 1e29f) {
            float dx = ov[0] - ground[0], dy = ov[1] - ground[1], len = sqrtf(dx * dx + dy * dy);
            dx /= len, dy /= len;
            float at[4] = {ground[0] - dy * 1.5f, ground[1] + dx * 1.5f, ground[2] + 0.2f, atan2f(-dx, dy) * 57.29578f};
            WarpPlayer(p1, at);
            Log("calldown test: player 1 to %.1f %.1f %.1f yaw %.0f (%.0f m from the Overlord, beside a ground unit)",
                at[0], at[1], at[2], at[3], len);
            if (r && r[0]) {
                float at2[4] = {ground[0] + dy * 1.5f, ground[1] - dx * 1.5f, ground[2] + 0.2f, at[3]};
                WarpPlayer(r[0], at2);
            }
        } else
            Log("calldown test: no ground unit near the Overlord");
    }
}

void LevelScriptUpdate() {
    g_crumb = "levelscript: start";
    RegisterMissingAIClasses();
    if (!g_activePlayer) g_activePlayer = (ActivePlayerFn)SymAddr("?gameMultiplayer_GetActivePlayer@@YAPAVtPlayer@@XZ");
    uint8_t *world = CurrentWorld();
    static uint8_t *lastWorld;
    if (world != lastWorld) {
        Log("crashcheck: world %p -> %p at frame %u", lastWorld, world, g_frame);
        lastWorld = world;
    }
    if (!world) return;
    static UINT seenLoads;
    if (world != g_loadedWorld || g_worldLoads != seenLoads) {   // a level was (re)loaded (restarts reuse the tWorld)
        Log("crashcheck: level %s at frame %u (load/restore count %u)", world != g_loadedWorld ? "load" : "restart",
            g_frame, g_worldLoads);
        g_crumb = "levelscript: stale check (load)";
        CheckStaleTables(world, world != g_loadedWorld ? "level load" : "restart");
        if (getenv("GHOSTPC_LOG_STARTS")) {   // debug: the level's player starts (tWorld +0xF0/+0xF4 and +0x100/+0x104,
                                              // 0x60-byte tPlayerStart: matrix +0x10, number +0x50, order +0x54)
            for (int list = 0; list < 2; list++) {
                unsigned n = *(unsigned *)(world + (list ? 0x100 : 0xf0));
                uint8_t *s = *(uint8_t **)(world + (list ? 0x104 : 0xf4));
                Log("starts: list %d: %u", list, n);
                for (unsigned i = 0; s && i < n && i < 64; i++, s += 0x60) {
                    const float *m = (const float *)(s + 0x10);
                    Log("starts:   number %u order %u at %.1f %.1f %.1f", *(unsigned *)(s + 0x50), *(unsigned *)(s + 0x54),
                        m[12], m[13], m[14]);
                }
            }
        }
        ResetUnitTables();   // its units are new, or back at the level start
        g_loadedWorld = world;
        seenLoads = g_worldLoads;
        g_loadFrame = g_frame;
        g_lastAI = nullptr;
        static const char *level = (const char *)SymAddr("?mainOptions_aLevelName@@3PADA");
        g_crumb = "levelscript: load script";
        Load(g_worldName[0] ? g_worldName : level ? level : "");
    }
    if (g_frame % 60 == 0) g_crumb = "levelscript: stale check", CheckStaleTables(world, "periodic");
    g_crumb = "levelscript: Tanagazj blade";
    if (g_frame % 10 == 0) KeepTanagazjBlade(world);
    g_crumb = "levelscript: Ultralisk charge";
    DriveUltraliskCharge(world);
    g_crumb = "levelscript: Sunken spikes";
    DriveSunkenSpikes();
    g_crumb = "levelscript: Lurker spikes";
    TrackLurkerSpikes();
    g_crumb = "levelscript: Goliath boss";
    DriveGoliathBoss(world);
    g_crumb = "levelscript: Scout markers";
    if (g_frame % 30 == 0) AddScoutMarkers(world);
    g_crumb = "levelscript: Scouts home";
    if (g_frame % 15 == 0) DriveScoutsHome(world);
    CalldownTest(world);
    g_crumb = "levelscript: commands";
    static bool probeDebug = getenv("GHOSTPC_PROBE_DEBUG") != nullptr;
    if (probeDebug) DebugProbe(world);
    if (!g_unpin.empty() && g_frame - g_loadFrame >= 30) {
        for (auto &c : g_unpin)
            Log("levelscript: unpin %s: %d actor(s)", c.c_str(), UnpinActors(world, SymAddr(("??_7" + c + "@@6B@").c_str())));
        g_unpin.clear();
    }
    if (!g_flyPaths.empty() && g_frame - g_loadFrame >= 30) {
        for (auto &f : g_flyPaths) {
            int n = StartFlyPaths(world, SymAddr(("??_7" + f.cls + "@@6B@").c_str()), f.track.c_str(), f.speed);
            if (n < 0) Log("levelscript: fly_path: %s is not a flyer AI class", f.cls.c_str());
            else Log("levelscript: fly_path %s %s: %d actor(s) started", f.cls.c_str(), f.track.c_str(), n);
        }
        g_flyPaths.clear();
    }
    if (g_propsMode && g_frame % 15 == 0 && !StartProps(world, g_propsMode))
        g_propsMode = 0;   // every prop restarted
    if (g_cmds.empty() && !g_hasStart) return;
    uint8_t *player = GuardedPlayer();
    if (!player) return;
    // the player is (re)created at the world origin: move her once she is there (not the previous level's player).
    // In co-op both: player 1 (pLeftLocalPlayer -> cPlayerInfo +0 tPlayer; GuardedPlayer is the current local player,
    // which may be either) to the start, player 2 (pRightLocalPlayer) 1 m to her side.
    uint32_t names = SymAddr("?gameMotion_apNames@@3PAPADA");
    uint8_t **left = names ? *(uint8_t ***)(names + 0x728) : nullptr, **right = names ? *(uint8_t ***)(names + 0x72c) : nullptr;
    uint8_t *p1 = left && left[0] ? left[0] : player;
    const float *pp0 = (const float *)(p1 + 0xb0);
    if (g_hasStart && g_frame - g_loadFrame >= 5 && fabsf(pp0[0]) < 3.f && fabsf(pp0[1]) < 3.f) {
        WarpPlayer(p1, g_start);
        Log("levelscript: player_start: moved the player to %.1f %.1f %.1f (yaw %g)", g_start[0], g_start[1], g_start[2],
            g_start[3]);
        if (right && right[0] && right[0] != p1) {
            float a = g_start[3] * 0.0174533f, p2[4] = {g_start[0] + cosf(a), g_start[1] + sinf(a), g_start[2], g_start[3]};
            if (g_hasStart2) memcpy(p2, g_start2, sizeof p2);
            WarpPlayer(right[0], p2);
            Log("levelscript: player_start: player 2 to %.1f %.1f", p2[0], p2[1]);
        }
        g_hasStart = false;
    }
    if (g_hasStart || g_frame - g_loadFrame < 10) return;   // boxes wait until she is where the level starts her
    {   // debug (GHOSTPC_TEST_GOD): the player's health (tThing +0x36C) is kept at what it was when this first ran
        static int god = getenv("GHOSTPC_TEST_GOD") ? 1 : 0;
        static float full;
        float &hp = *(float *)(player + 0x36c);
        if (god && full <= 0.f) full = hp;
        if (god && hp > 0.f && hp < full) hp = full;
    }
    if (DebugAI()) {
        // GHOSTPC_TANAGAZJ_DEBUG_POS="x y z [yaw]": move the player there (yaw in degrees, 0 = +Y, 90 = -X) just before
        // the forced notify, to watch the fight
        static float dbgPos[4];
        static int hasPos = -1;
        if (hasPos < 0) {
            const char *e = getenv("GHOSTPC_TANAGAZJ_DEBUG_POS");
            hasPos = e && sscanf(e, "%f %f %f %f", &dbgPos[0], &dbgPos[1], &dbgPos[2], &dbgPos[3]) >= 3;
        }
        // GHOSTPC_TANAGAZJ_DEBUG_WARPFRAME: frames after the level load to warp at (default 40); the forced notify and
        // the shoot-only pick follow 10 frames later
        if (hasPos && g_frame - g_loadFrame == DebugWarpFrame()) {
            WarpPlayer(player, dbgPos);
            Log("ai-dbg: moved the player to %.1f %.1f %.1f", dbgPos[0], dbgPos[1], dbgPos[2]);
        }
        // GHOSTPC_TANAGAZJ_DEBUG_POS2="frame x y z [yaw]": a second move, <frame> frames after the level load (to leave)
        static float dbgPos2[5];
        static int hasPos2 = -1;
        if (hasPos2 < 0) {
            const char *e = getenv("GHOSTPC_TANAGAZJ_DEBUG_POS2");
            hasPos2 = e && sscanf(e, "%f %f %f %f %f", &dbgPos2[0], &dbgPos2[1], &dbgPos2[2], &dbgPos2[3], &dbgPos2[4]) >= 4;
        }
        if (hasPos2 && g_frame - g_loadFrame == (UINT)dbgPos2[0]) {
            WarpPlayer(player, dbgPos2 + 1);
            Log("ai-dbg: moved the player to %.1f %.1f %.1f", dbgPos2[1], dbgPos2[2], dbgPos2[3]);
        }
        if (DebugAIMode() == 2 && g_frame - g_loadFrame == DebugWarpFrame() + 10 && !g_lastAI) {
            // pick Tanagazj (or GHOSTPC_TANAGAZJ_DEBUG_CLASS), the one nearest the player; DebugAIUpdate shoots it next frame
            const char *cls = getenv("GHOSTPC_TANAGAZJ_DEBUG_CLASS");
            uint32_t vt = SymAddr(cls ? ("??_7" + std::string(cls) + "@@6B@").c_str() : "??_7cAIBossTanagazj@@6B@");
            uint32_t sectors = *(uint32_t *)(world + 0xdc);
            uint8_t *sector = *(uint8_t **)(world + 0xe0);
            const float *pp = (const float *)(player + 0xb0);
            float best = 1e30f;
            for (uint32_t i = 0; i < sectors; i++, sector += 0x110)
                for (uint8_t *t = *(uint8_t **)(sector + 0x24); t; t = *(uint8_t **)(t + 0x28))
                    if (uint8_t *ai = *(uint8_t **)(t + 0x3c4))
                        if (!IsBadReadPtr(ai, 4) && *(uint32_t *)ai == vt) {
                            const float *q = (const float *)(t + 0xb0);
                            float d = (q[0] - pp[0]) * (q[0] - pp[0]) + (q[1] - pp[1]) * (q[1] - pp[1]);
                            if (d < best) best = d, g_lastAI = ai, g_lastAIFrame = g_frame - 59;
                        }
        }
        DebugAIUpdate(player);
    }
    const float *p = (const float *)(player + 0xb0);
    for (auto &n : g_cmds) {
        bool inBox = p[0] >= n.lo[0] && p[0] <= n.hi[0] && p[1] >= n.lo[1] && p[1] <= n.hi[1] && p[2] >= n.lo[2] &&
                     p[2] <= n.hi[2];
        if ((DebugAIMode() == 1 || DebugAIMode() == 3) && g_frame - g_loadFrame > DebugWarpFrame() + 10) inBox = true;   // debug: fire wherever the player is once the level has settled
        if (n.fired) continue;
        uint32_t vtable = SymAddr(("??_7" + n.cls + "@@6B@").c_str());
        if (!inBox && !(vtable && AnyDamaged(world, vtable))) continue;
        auto fn = (ScriptNotifyFn)SymAddr(("?ScriptNotify@" + n.cls + "@@UAEHIIIMMM@Z").c_str());
        if (!vtable || !fn) {
            Log("levelscript: unknown AI class %s", n.cls.c_str());
            n.fired = true;
            continue;
        }
        // retried every frame while the player is in the box until an actor received it (AIs may not exist yet)
        // debug: the AI to follow (GHOSTPC_TANAGAZJ_DEBUG_CLASS limits it to one class, default all)
        const char *only = getenv("GHOSTPC_TANAGAZJ_DEBUG_CLASS");
        bool follow = !only || n.cls == only;
        int sent = NotifyAll(world, vtable, fn, n.msg, follow);
        if (sent) n.fired = true;
        if (sent && follow) g_lastAIFrame = g_frame;
        if (sent || g_frame % 150 == 0)
            Log("levelscript: frame %u: player %s, ScriptNotify(%u) sent to %d %s", g_frame,
                inBox ? "in box" : "shot the actor", n.msg, sent, n.cls.c_str());
    }
}
