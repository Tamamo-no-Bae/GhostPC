// The smoke grenade (user: it only made particles; it should cancel aggro on Nova and briefly stun those hit).
//
// The grenade (ePlayerItems 1, projectile template smokeGrenade) explodes into smke_Canister: 16 s of smoke particles and
// a 20 s volume fog (fogGrenadeSmoke), no damage and no AI effect. GhostPC tracks each smokeGrenade projectile (new
// things get increasing GUIDs, so only the GUIDs created since the last frame are looked at); when it is gone, a cloud
// sits at its last position for 20 s:
//   - the burst: every living AI actor within SMOKE_R drops its target (cAIInstance::Reset: target +0x2D4 cleared, out
//     of the combat group, state NONE); walkers (zerglings, marines, ...: vtable +0x258 is cAIBaseWalker::SilentStun)
//     are also stunned: SilentStun starts the knockdown (walker mode 0x23, AI state KNOCKDOWN) and SetProneDuration
//     keeps them down for SMOKE_STUN_MS (milliseconds: AnimationCallback ends it at stdTime +0x44 + duration);
//   - while it lasts, every SMOKE_SCAN frames: an AI whose target is a player loses it again when either of them is in
//     the cloud (no line of sight through the smoke).
// GHOSTPC_LOG_SMOKE logs what it does.
#include "common.h"
#include <math.h>
#include <stdlib.h>

extern UINT g_frame;

namespace {
const float SMOKE_R = 7.f, SMOKE_H = 5.f;   // cloud radius / half height (fogGrenadeSmoke spreads about (20, 20, 4))
const UINT SMOKE_MS = 20000, SMOKE_STUN_MS = 2500, SMOKE_SCAN = 10;
const UINT DEAD = 0x10000000;   // tThing +0x360 (set when killed)

struct Cloud { float p[3]; UINT until; };
struct Nade { unsigned guid; float p[3]; };
Cloud g_clouds[8];
int g_nClouds;
Nade g_nades[16];
int g_nNades;
unsigned g_scanned;

typedef uint8_t *(__cdecl *FindFn)(unsigned);
FindFn g_find;
unsigned *g_nextGuid;
UINT *g_now;
void(__thiscall *g_reset)(uint8_t *);
uint8_t *g_silentStun;
bool g_log;

bool Init() {
    static int state;   // 0 not yet, 1 ok, -1 missing
    if (state) return state > 0;
    g_find = (FindFn)SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    g_nextGuid = (unsigned *)SymAddr("?nextGUID@tThing@@2IA");
    uint8_t *t = (uint8_t *)SymAddr("?stdTime@@3VtStdTime@@A");
    g_now = t ? (UINT *)(t + 0x44) : nullptr;
    g_reset = (void(__thiscall *)(uint8_t *))SymAddr("?Reset@cAIInstance@@QAEXXZ");
    g_silentStun = (uint8_t *)SymAddr("?SilentStun@cAIBaseWalker@@MAEHPAUXGVECTOR3@@I@Z");
    g_log = getenv("GHOSTPC_LOG_SMOKE") != nullptr;
    state = g_find && g_nextGuid && g_now && g_reset && g_silentStun ? 1 : -1;
    if (state < 0) Log("smoke: symbols missing, the smoke grenade stays visual only");
    return state > 0;
}

bool InCloud(const Cloud &c, const float *q) {
    float dx = q[0] - c.p[0], dy = q[1] - c.p[1], dz = q[2] - c.p[2];
    return dx * dx + dy * dy < SMOKE_R * SMOKE_R && fabsf(dz) < SMOKE_H;
}

bool InAnyCloud(const float *q) {
    for (int i = 0; i < g_nClouds; i++)
        if (InCloud(g_clouds[i], q)) return true;
    return false;
}

uint8_t *AIOf(uint8_t *t) {   // a living AI actor's cAIInstance (tActor::GetAI: +0x3C4), else null
    if (*(int *)(t + 0x10) != 0 || (*(uint32_t *)(t + 0x360) & DEAD)) return nullptr;
    return *(uint8_t **)(t + 0x3c4);
}

void Burst(const Cloud &c) {
    int dropped = 0, stunned = 0;
    for (unsigned g = 1; g < *g_nextGuid && g < 200000; g++) {
        uint8_t *t = g_find(g);
        if (!t || !InCloud(c, (const float *)(t + 0xb0))) continue;
        uint8_t *ai = AIOf(t);
        if (!ai) continue;
        g_reset(ai);
        dropped++;
        void **vt = *(void ***)ai;
        if (vt[0x258 / 4] == g_silentStun) {   // a walker that keeps the base stun
            ((int(__thiscall *)(uint8_t *, float *, unsigned))g_silentStun)(ai, nullptr, SMOKE_STUN_MS);
            stunned++;
        }
        if (g_log) Log("smoke: %.24s dropped its target%s", (char *)(t + 0xc4), vt[0x258 / 4] == g_silentStun ? ", stunned" : "");
    }
    Log("smoke: cloud at %.1f %.1f %.1f: %d AIs lost their target, %d stunned", c.p[0], c.p[1], c.p[2], dropped, stunned);
}

void Blind() {   // AIs targeting a player lose them when either is in a cloud
    for (unsigned g = 1; g < *g_nextGuid && g < 200000; g++) {
        uint8_t *t = g_find(g);
        uint8_t *ai = t ? AIOf(t) : nullptr;
        unsigned target = ai ? *(unsigned *)(ai + 0x2d4) : 0;
        if (!target) continue;
        uint8_t *p = g_find(target);
        if (!p || *(int *)(p + 0x10) != 7) continue;   // only a player target
        if (!InAnyCloud((const float *)(t + 0xb0)) && !InAnyCloud((const float *)(p + 0xb0))) continue;
        g_reset(ai);
        if (g_log) Log("smoke: %.24s lost the player in the smoke", (char *)(t + 0xc4));
    }
}
}   // namespace

void SmokeUpdate() {
    if (!Init()) return;
    unsigned next = *g_nextGuid;
    if (next < g_scanned) g_scanned = 0, g_nNades = 0, g_nClouds = 0;   // a new level
    for (; g_scanned < next; g_scanned++)   // new things: smoke grenades in flight
        if (uint8_t *t = g_find(g_scanned))
            if (!_stricmp((char *)(t + 0xc4), "smokeGrenade") && g_nNades < 16) {
                g_nades[g_nNades].guid = g_scanned;
                memcpy(g_nades[g_nNades++].p, t + 0xb0, 12);
            }
    for (int i = 0; i < g_nNades;) {
        uint8_t *t = g_find(g_nades[i].guid);
        if (t && !_stricmp((char *)(t + 0xc4), "smokeGrenade")) {
            memcpy(g_nades[i].p, t + 0xb0, 12);
            i++;
            continue;
        }
        Cloud c;   // gone: it burst where it was last seen
        memcpy(c.p, g_nades[i].p, 12);
        c.until = *g_now + SMOKE_MS;
        if (g_nClouds == 8) memmove(g_clouds, g_clouds + 1, sizeof(Cloud) * 7), g_nClouds--;
        g_clouds[g_nClouds++] = c;
        Burst(c);
        g_nades[i] = g_nades[--g_nNades];
    }
    for (int i = 0; i < g_nClouds;)
        if ((int)(g_clouds[i].until - *g_now) <= 0) g_clouds[i] = g_clouds[--g_nClouds];
        else i++;
    if (g_nClouds && g_frame % SMOKE_SCAN == 0) Blind();
}
