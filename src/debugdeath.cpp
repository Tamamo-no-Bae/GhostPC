// Debug (GHOSTPC_LOGDEATH): log calls to tActor::Kill, tActor::Damage and cPlayerControl::Killed with their caller, then
// run the original code. Unlike the HookReg replacements these are pass-through detours: the first whole instructions
// of each function are copied to a trampoline that jumps back after them.
#include "common.h"
#include <math.h>
#include <stdlib.h>

extern UINT g_frame;
extern UINT g_traceRequest;   // d3d_core.cpp: trace the next N frames

namespace {
struct Detour {
    const char *name;   // decorated name in Ghost.map
    int stolen;         // bytes of whole instructions copied to the trampoline (>= 5, no relative operands)
    uint8_t *tramp;
};
Detour g_det[] = {
    {"?Kill@tActor@@QAEHPAVtThing@@W4eDamageSource@@W4eDamageType@@MPAUXGVECTOR3@@3IW4eHitRegionType@@M@Z", 10, nullptr},
    {"?Damage@tActor@@QAEMPAVtThing@@W4eDamageSource@@W4eDamageType@@MPAUXGVECTOR3@@3IW4eHitRegionType@@MPAH@Z", 8, nullptr},
    {"?Killed@cPlayerControl@@UAEXPAVtThing@@W4eDamageSource@@W4eDamageType@@MPAUXGVECTOR3@@3IM@Z", 9, nullptr},
    {"?FireSniperProjectile@cAIBossSniper@@AAEXXZ", 6, nullptr},   // sub esp, 0x18; push esi; push 0
    {"?RenderMeshGroup@cCloakShader@@UAEIPAVcMeshGroup@@PAVcModelInst@@I@Z", 6, nullptr},   // push ebp; mov ebp, esp;
                                                                                           // push ecx; push ebx; push esi
    {"?FireSpike@cAIBossLurker@@QAEXXZ", 6, nullptr},   // push ebp; mov ebp, esp; and esp, -16
    {"?DoBurrowedDamage@cAIBossLurker@@QAEHPAUXGVECTOR3@@MM@Z", 5, nullptr},   // mov eax, 0x183c
    {"?simCombat_FireTemplateProjectile@@YAPAVtProjectile@@PAV1@PAUXGMATRIX@@PAVtThing@@IHHPAVSimpleHit@@W4eHitRegionType@@@Z", 5,
     nullptr},   // push esi; mov esi, [esp + 8]
    {"?UpdateDamage@cEffectDamage@@QAEXPAUXGVECTOR3@@PAVtSector@@PAVtThing@@@Z", 5, nullptr},   // mov eax, 0x185c
    {"?FireGoliathMachineGun@cAIBossGoliath@@UAEXIM@Z", 6, nullptr},   // push ebp; mov ebp, esp; and esp, -16
    {"?FireWeapon@cAIProtossScout@@QAEXM@Z", 6, nullptr},   // sub esp, 0x3c; push esi; mov esi, ecx
};

// stack at entry: [ret][attacker][source][type][amount] ; this in ecx
void __stdcall LogCall(int which, uint8_t *self, const uint32_t *stack) {
    if (which == 4) {   // cloak shader: camera distance to the model's root bone (it skips non-player models past 8 m)
        static int cloakLogs;
        static bool traceCloak = getenv("GHOSTPC_TRACECLOAK") != nullptr;
        if (traceCloak && cloakLogs == 0) g_traceRequest = 2;   // trace the first cloaked frames
        if (cloakLogs++ % 30) return;   // every 30th call
        static const float *cam = (const float *)SymAddr("?rlCamera_pCurCamera@@3PAVtRenderCamera@@A");
        const uint8_t *camera = cam ? *(const uint8_t **)cam : nullptr;
        const float *root = *(const float **)((uint8_t *)stack[2] + 0xa4);
        float d = camera && root ? sqrtf((root[0] - *(float *)(camera + 0x30)) * (root[0] - *(float *)(camera + 0x30)) +
                                         (root[1] - *(float *)(camera + 0x34)) * (root[1] - *(float *)(camera + 0x34)) +
                                         (root[2] - *(float *)(camera + 0x38)) * (root[2] - *(float *)(camera + 0x38)))
                                  : -1.f;
        Log("death-dbg frame %u cloak RenderMeshGroup model %p root %.1f %.1f %.1f camera distance %.1f", g_frame,
            (void *)stack[2], root ? root[0] : 0.f, root ? root[1] : 0.f, root ? root[2] : 0.f, d);
        return;
    }
    static int logged;
    if (logged++ > 200) return;
    if (which == 5 || which == 6) {   // the Lurker boss's spike: projectile / burrowed area damage (at, radius, amount)
        const float *at = which == 6 ? (const float *)stack[1] : nullptr;
        float r = 0, a = 0;
        if (which == 6) memcpy(&r, &stack[2], 4), memcpy(&a, &stack[3], 4);
        Log("death-dbg frame %u %s at %.1f %.1f %.1f radius %g amount %g", g_frame, which == 5 ? "Lurker FireSpike" : "Lurker DoBurrowedDamage",
            at ? at[0] : 0.f, at ? at[1] : 0.f, at ? at[2] : 0.f, r, a);
        return;
    }
    if (which == 10) {   // the Scout's gun
        Log("death-dbg frame %u Scout FireWeapon", g_frame);
        return;
    }
    if (which == 9) {   // the cut Goliath boss's machine gun: gun index, accuracy
        float acc;
        memcpy(&acc, &stack[2], 4);
        Log("death-dbg frame %u Goliath machine gun %u accuracy %g", g_frame, stack[1], acc);
        return;
    }
    if (which == 7 || which == 8) {   // projectiles fired (with the caller) / damage effects (first call per owner thing)
        char caller[128];
        SymName(stack[0], caller, sizeof caller);
        if (which == 8) {
            static void *seen[64];
            static int nSeen;
            for (int i = 0; i < nSeen; i++)
                if (seen[i] == (void *)stack[3]) return;
            if (nSeen < 64) seen[nSeen++] = (void *)stack[3];
            Log("death-dbg frame %u cEffectDamage %p first damage update, thing %p from %s", g_frame, self, (void *)stack[3], caller);
            return;
        }
        Log("death-dbg frame %u FireTemplateProjectile %p from %s", g_frame, (void *)stack[1], caller);
        return;
    }
    if (which == 3) {
        const float *pos = *(float **)(self + 8) ? (const float *)(*(uint8_t **)(self + 8) + 0xb0) : nullptr;
        Log("death-dbg frame %u FireSniperProjectile from %.1f %.1f %.1f", g_frame, pos ? pos[0] : 0.f, pos ? pos[1] : 0.f,
            pos ? pos[2] : 0.f);
        return;
    }
    char caller[128];
    SymName(stack[0], caller, sizeof caller);
    uint8_t *attacker = (uint8_t *)stack[1];
    float amount;
    memcpy(&amount, &stack[4], 4);
    float *pos = which < 2 && self ? (float *)(self + 0xb0) : nullptr;
    Log("death-dbg frame %u %s this %p (type %d%s) attacker %p (type %d) source %u dmgtype %u amount %g pos %.1f %.1f %.1f from %s",
        g_frame, which == 0 ? "Kill" : which == 1 ? "Damage" : "Killed", self, which < 2 && self ? *(int *)(self + 0x10) : -1,
        which < 2 && self && *(int *)(self + 0x10) == 7 ? " player" : "", attacker, attacker ? *(int *)(attacker + 0x10) : -1,
        stack[2], stack[3], amount, pos ? pos[0] : 0.f, pos ? pos[1] : 0.f, pos ? pos[2] : 0.f, caller);
}

#define STUB(i)                                                                                      \
    __declspec(naked) void Stub##i() {                                                               \
        __asm pushad                                                                                 \
        __asm lea eax, [esp + 32]                                                                    \
        __asm push eax                                                                               \
        __asm push ecx                                                                               \
        __asm push i                                                                                 \
        __asm call LogCall                                                                           \
        __asm popad                                                                                  \
        __asm jmp dword ptr [g_det + i * 12 + 8]                                                     \
    }
STUB(0)
STUB(1)
STUB(2)
STUB(3)
STUB(4)
STUB(5)
STUB(6)
STUB(7)
STUB(8)
STUB(9)
STUB(10)
void (*const g_stubs[])() = {Stub0, Stub1, Stub2, Stub3, Stub4, Stub5, Stub6, Stub7, Stub8, Stub9, Stub10};
}   // namespace

void DebugDeathInstall() {
    static bool done;
    if (done) return;
    done = true;
    if (!getenv("GHOSTPC_LOGDEATH")) return;
    static_assert(sizeof(Detour) == 12, "stub offsets assume 12-byte entries");
    for (int i = 0; i < (int)(sizeof g_det / sizeof g_det[0]); i++) {
        uint8_t *fn = (uint8_t *)SymAddr(g_det[i].name);
        if (!fn) { Log("death-dbg: %s not found", g_det[i].name); continue; }
        uint8_t *t = (uint8_t *)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        memcpy(t, fn, g_det[i].stolen);
        t[g_det[i].stolen] = 0xE9;
        *(int32_t *)(t + g_det[i].stolen + 1) = (int32_t)((fn + g_det[i].stolen) - (t + g_det[i].stolen + 5));
        g_det[i].tramp = t;
        DWORD old;
        VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old);
        fn[0] = 0xE9;
        *(int32_t *)(fn + 1) = (int32_t)((uint8_t *)g_stubs[i] - (fn + 5));
        VirtualProtect(fn, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), fn, 5);
        Log("death-dbg: detoured %s", g_det[i].name);
    }
}
