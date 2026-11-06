// The Psi Blade (user's plan): Tanagazj drops it when he dies; picking it up turns Nova into NovaDT (the Dark Templar
// armour) with a psi blade on her hand and the old "K" melee animations.
//
// The build has pieces of a cut Psi Blade: missions take "PSIBLADE 0 | 1" (cChronicle::Load sets chronicle +0x10C bit 0,
// which nothing reads), the string table has "Psi Blade" (LBENTRY_WEAPONPSIBLADE), game.nfx has a player blade effect
// (controller psibladePlayer: psibladeGlow + psibladeBeam on model marker type 3 index 0, Nova's hand marker), and the
// K animations (scripts/convert_nova_k_anims.py -> nova43_K_*.nad) mark their hits with the tags
// cPlayerControl::MeleeCallback already handles (0x1F / 0x20 -> MeleeHitCheck).
//
// PsiBladeGive(player):
//   - model: the instance keeps its bones, mixer and attachments; only its cModel (cModelInst +0) becomes nova_DT43.nod
//     (renderModel_Load). Same 43-bone skeleton and markers (convert_nova_dt.py copies nova.nod's), and the instance
//     block (0x100 + 76 per bone + 4 per shader, cModel::NewInstance) was sized for nova.nod's 9 shaders, DT's 7 fit.
//   - motion set (tObject +0x2A4): nova_psiblade.nms (scripts/make_psiblade_nms.py: attack1 = the K combo, gunmelee =
//     the thrust). gameMotion_LoadSet takes a reference first, then tMotionSet::Free drops the old one's (sets are
//     reference counted: an unbalanced count crashes when the level is freed).
//   - the instance's shader table (cModelInst +0xA8, one cShader per model material, made by cModel::NewInstance) is
//     rebuilt from the new model by cModelInst::Finalize(no matrix) (without it, NovaDT drew with Nova's materials);
//   - the blade: effectManager CreateEffect("psibladeNova", player) + tThing::AddEffect(effect, 1), as
//     cAIBossTanagazj::BladeOn does with its own.
//   - the shock glove (weapon 1) is selected: its melee is attack1.
//   - a 40-point Protoss shield (user; vehicles.cpp PlayerShieldOn: on in combat, recharging 1 a second then and 3
//     out of it), removed with the blade.
// The pickup: GhostPC's powPsiBlade (ghostpc.not: the flame ammo pickup's script with a power crystal model);
// vehicles.cpp's cPowerupFlameAmmo::Activate detour calls PsiBladeGive for a pickup with that name.
// The drop: the thing named "Tanagazj" is watched; when it is dead (+0x360 0x10000000) or gone, powPsiBlade is created
// where it was last seen.
// Losing it (user): dying and restarting restores the level-start snapshot (cSaveData::Restore) on the same player
// object, and a level load makes a new one. On either (levelscript.cpp's load count) the player it was given to is
// reverted if she still exists and still has it: her original cModel back, nova.nms back (balanced references), the
// blade effect removed by name (tThing::RemoveEffect).
// Dying (user: dying kept it): a game started with -level (GhostPC's test runs) does not restart after a death:
// cShell::State_RestorePlayer (shell state 69) revives the player where she fell (tActor::Revive) and only restores the
// level-start snapshot when mainOptions_aLevelName is empty (started from the menus). So the bladed player's death
// (+0x360 0x10000000) is noted and the blade taken away once she is alive again (or on the restore, if first).
// Ammo: the blade is the shock glove's melee, which uses none.
// Debug: GHOSTPC_PSIBLADE=<frame> gives it to player 1 then; GHOSTPC_PSIBLADE_DROP=<frame> drops a pickup 2 m in front
// of her then.
#include "common.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <utility>
#include <vector>

extern UINT g_frame;
bool PlayerShieldOn(uint8_t *player, float max);   // vehicles.cpp: a cProtossShield for her
void PlayerShieldOff(uint8_t *player);
bool PlayerShieldState(uint8_t *player, float *amount, float *max);
// (user) only with the blade; recharge 1 / s in combat, 3 / s out of it; 40 points at first, then 80 (user)
const float BLADE_SHIELD = 80.f;
// Tanagazj's shield (user: 80): his AI makes its cProtossShield (AI +0x5AC) from ACTOR_SHIELD, which his template
// lacks (0 of 0); PsiBladeUpdate gives it 80 (cProtossShield::Create) when it finds him with none
const float TANAGAZJ_SHIELD = 80.f;
uint8_t *LevelWorld();   // levelscript.cpp
UINT LevelLoadCount();   // levelscript.cpp: level loads and restarts
bool LevelPsiBladeStart();   // levelscript.cpp: the level script's psiblade_start

void CustomLoopStop(unsigned handle);   // customsound.cpp
namespace {
const char *DT_MODEL = "nova_DT43.nod", *DT_MODEL_ECHO = "nova_DT43_echo.nod", *BLADE_SET = "nova_psiblade.nms", *BLADE_FX = "psibladeNova";
const UINT DEAD = 0x10000000;

uint8_t *LocalPlayer(int side) {   // cPlayerInfo +0 of pLeft / pRightLocalPlayer (as levelscript.cpp's weapon command)
    uint32_t nm = SymAddr("?gameMotion_apNames@@3PAPADA");
    uint8_t **info = nm ? *(uint8_t ***)(nm + (side ? 0x72c : 0x728)) : nullptr;
    return info ? info[0] : nullptr;
}

uint8_t *CreateAt(const char *tmplName, const float *pos) {
    static auto find = (uint8_t *(__cdecl *)(const char *))SymAddr("?gameTemplate_Find@@YAPAVtTemplate@@PBD@Z");
    static auto sectorOf = (void *(__thiscall *)(uint8_t *, const float *))SymAddr("?FindPosSector@tWorld@@QAEPAVtSector@@PBUXGVECTOR3@@@Z");
    static auto create = (uint8_t *(__cdecl *)(void *, void *, void *, void *, unsigned))SymAddr(
        "?gameThing_CreateFromTemplateAtPos@@YAPAVtThing@@PAVtTemplate@@PAVtWorld@@PAVtSector@@PBUXGMATRIX@@I@Z");
    uint8_t *w = LevelWorld(), *tmpl = find ? find(tmplName) : nullptr;
    if (!w || !tmpl || !sectorOf || !create) {
        Log("psiblade: cannot create %s (world %p template %p)", tmplName, w, tmpl);
        return nullptr;
    }
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, pos[0], pos[1], pos[2], 1};
    return create(tmpl, w, sectorOf(w, pos), m, 0);
}

// Who has the blade (user, co-op: both Nova and Echo can take it; the pickup stays until both have): one record per
// local player, each with her own model / motion set to put back, blade effect, hum, plunge and gun state
struct Given {
    uint8_t *player, *origModel, *dtModel, *bladeSet, *fx, *origSet;
    unsigned hum;
    bool plunge, died;
    UINT strikeUntil;   // (the plunge's landing strike: its turn rate kept at 0 until this frame)
    int bladeGun, bladeGunSt;   // (0, or the gun state the blade is in: 1 GaussRifle, 2 GunCrouch; its state number)
};
Given g_given[2];
int SlotOf(const uint8_t *player) {
    for (int i = 0; i < 2; i++)
        if (player && g_given[i].player == player) return i;
    return -1;
}

void Revert(int i) {
    Given &g = g_given[i];
    static auto loadSet = (uint8_t *(__cdecl *)(const char *))SymAddr("?gameMotion_LoadSet@@YAPAVtMotionSet@@PAD@Z");
    static auto freeSet = (int(__thiscall *)(uint8_t *))SymAddr("?Free@tMotionSet@@QAEHXZ");
    static auto removeFx = (int(__thiscall *)(uint8_t *, const char *))SymAddr("?RemoveEffect@tThing@@QAEHPBD@Z");
    uint8_t *p = g.player;
    if (!p || (p != LocalPlayer(0) && p != LocalPlayer(1))) {   // gone (a new level made new players)
        g = {};
        return;
    }
    uint8_t *inst = *(uint8_t **)(p + 0x2a0);
    bool model = inst && *(uint8_t **)inst == g.dtModel && g.origModel;
    static auto finalize = (void(__thiscall *)(uint8_t *, void *))SymAddr("?Finalize@cModelInst@@QAEXPAUXGMATRIX@@@Z");
    if (model) {
        *(uint8_t **)inst = g.origModel;
        if (finalize) finalize(inst, nullptr);
    }
    bool set = *(uint8_t **)(p + 0x2a4) == g.bladeSet && loadSet && freeSet;
    if (set && g.origSet) {   // (her own set back: Give kept its reference; the blade's is dropped)
        *(uint8_t **)(p + 0x2a4) = g.origSet;
        freeSet(g.bladeSet);
    } else if (set)
        if (uint8_t *nova = loadSet("nova.nms")) {
            *(uint8_t **)(p + 0x2a4) = nova;
            freeSet(g.bladeSet);
        }
    if (g.hum) CustomLoopStop(g.hum);
    PlayerShieldOff(p);
    bool chosen = *(int *)(p + 0x458) == 1;
    int fx = removeFx ? removeFx(p, BLADE_FX) : 0;
    Log("psiblade: taken away from %p (model %s, motion set %s, effect removed %d)", p, model ? "reverted" : "unchanged",
        set ? "reverted" : "unchanged", fx);
    g = {};
    if (chosen) {   // slot 1 is the shock glove again, out of the cycle: on to the next weapon
        static auto setNext = (int(__thiscall *)(uint8_t *, int))SymAddr("?SetNextPlayerWeapon@tPlayer@@QAEHH@Z");
        if (setNext) setNext(p, 0);
    }
}
void RevertAll() {
    for (int i = 0; i < 2; i++)
        if (g_given[i].player) Revert(i);
}
}   // namespace
// the blade effects (vehicles.cpp's tSortList::AddSortEffect detour draws them last)
bool IsPsiBladeFx(const uint8_t *e) { return e && (e == g_given[0].fx || e == g_given[1].fx); }
bool PlayerHasPsiBlade(const uint8_t *player) { return SlotOf(player) >= 0; }   // (vehicles.cpp: the co-op pickup rule)

// A GhostPC database entry unlocked (Misc\ghostpc.nlx, merged into game.nlx by xapi.cpp; flags 24: hidden until
// then): cLogbook::IdFromName, UnInvisEntry, UnlockEntry on the game's logbook. The Psi Blade's here, the
// flamethrower's from vehicles.cpp's pickup (user).
void LogbookUnlock(const char *entry) {
    static uint8_t *logbook = (uint8_t *)SymAddr("?gameLogbook@@3VcLogbook@@A");
    static auto idOf = (unsigned short(__thiscall *)(uint8_t *, const char *))SymAddr("?IdFromName@cLogbook@@QAEGPAD@Z");
    static auto unInvis = (void(__thiscall *)(uint8_t *, unsigned short))SymAddr("?UnInvisEntry@cLogbook@@QAEXG@Z");
    static auto unlock = (void(__thiscall *)(uint8_t *, unsigned short))SymAddr("?UnlockEntry@cLogbook@@QAEXG@Z");
    if (!logbook || !idOf || !unInvis || !unlock) return;
    char name[64];
    strncpy(name, entry, sizeof name - 1), name[sizeof name - 1] = 0;
    unsigned short id = idOf(logbook, name);
    if (id) unInvis(logbook, id), unlock(logbook, id);
    Log("logbook: database entry %s id %u%s", name, id, id ? " unlocked" : " not found");
}
bool PsiBladeGive(uint8_t *player) {
    if (!player) return false;
    static auto loadModel = (uint8_t *(__cdecl *)(const char *, unsigned))SymAddr("?renderModel_Load@@YAPAVcModel@@PBDI@Z");
    static auto loadSet = (uint8_t *(__cdecl *)(const char *))SymAddr("?gameMotion_LoadSet@@YAPAVtMotionSet@@PAD@Z");
    static auto freeSet = (int(__thiscall *)(uint8_t *))SymAddr("?Free@tMotionSet@@QAEHXZ");
    static auto createFx = (uint8_t *(__thiscall *)(void *, const char *, uint8_t *, void *, void *, void *, void *, float))SymAddr(
        "?CreateEffect@cEffectManager@@QAEPAVcEffect@@PADPAVtThing@@PAVcShader@@PAVcEffectDef@@03M@Z");
    static auto addFx = (void(__thiscall *)(uint8_t *, uint8_t *, int))SymAddr("?AddEffect@tThing@@QAEXPAVcEffect@@H@Z");
    static auto setWeapon = (int(__thiscall *)(uint8_t *, int, int, int))SymAddr("?SetPlayerWeapon@tPlayer@@QAEHW4ePlayerWeapons@@HH@Z");
    static void *fxManager = (void *)SymAddr("?effectManager_Manager@@3VcEffectManager@@A");
    if (SlotOf(player) >= 0) return false;   // she has it already
    int slot = !g_given[0].player ? 0 : !g_given[1].player ? 1 : -1;
    if (slot < 0) return false;
    Given &g = g_given[slot];
    uint8_t *inst = *(uint8_t **)(player + 0x2a0);
    // Echo (her own model is nova_ice.nod, player 2's skin) gets her own DT armour (user's textures,
    // scripts/make_echo_dt.py: nova_DT43_echo.nod, and nova_DT43_echo_shield.nod as her shield's shell)
    uint8_t *echoModel = loadModel ? loadModel("nova_ice.nod", 0) : nullptr;   // (looked up each time: a new level reloads models)
    bool echo = inst && echoModel && *(uint8_t **)inst == echoModel;
    const char *dtName = echo ? DT_MODEL_ECHO : DT_MODEL;
    uint8_t *model = loadModel ? loadModel(dtName, 0) : nullptr;
    g = {player, inst ? *(uint8_t **)inst : nullptr, model, nullptr, nullptr};
    uint8_t *cur = inst ? *(uint8_t **)inst : nullptr;   // cModel: +9 bone count, +4 shader count (cModel::NewInstance)
    static auto finalize = (void(__thiscall *)(uint8_t *, void *))SymAddr("?Finalize@cModelInst@@QAEXPAUXGMATRIX@@@Z");
    if (cur && model && model[9] == cur[9] && model[4] <= cur[4]) {
        *(uint8_t **)inst = model;
        if (finalize) finalize(inst, nullptr);   // the instance's shader table (+0xA8) from the new model's materials
    }
    else Log("psiblade: model not swapped (instance %p, %s %p: bones %d / %d, shaders %d / %d)", inst, DT_MODEL, model, model ? model[9] : -1,
             cur ? cur[9] : -1, model ? model[4] : -1, cur ? cur[4] : -1);
    uint8_t *set = loadSet ? loadSet(BLADE_SET) : nullptr, *old = *(uint8_t **)(player + 0x2a4);
    if (set && set != old) {
        g.bladeSet = set;
        g.origSet = old;   // (its reference kept until Revert puts it back: Echo's set may not be nova.nms)
        *(uint8_t **)(player + 0x2a4) = set;
    } else if (!set) Log("psiblade: motion set %s not loaded", BLADE_SET);
    uint8_t *fx = createFx && fxManager ? createFx(fxManager, BLADE_FX, player, nullptr, nullptr, nullptr, nullptr, 0.f) : nullptr;
    if (fx && addFx) addFx(player, fx, 1);
    g.fx = fx;
    PlayerShieldOn(player, BLADE_SHIELD);
    LogbookUnlock("lbentry_weaponpsiblade");   // its database entry (user)
    // (after the model swap: its shell is "<model>_shield", nova_DT43_shield.nod)
    *(int *)(player + 0x458) = 1;   // (the cycle position: the Psi Blade is her chosen weapon now; SetWeaponHook then
                                    // sets the pending weapon +0x45C to it as well)
    int sel = setWeapon ? setWeapon(player, 1, 1, 0) : 0;
    Log("psiblade: given to %p: model %p, motion set %p (was %p), blade effect %p, shock glove selected %d", player, model, set, old, fx, sel);
    return true;
}

// The shield bar (user: only once she has the blade): Nova's health and psi bars are cUIBars (cPlayerInfo +0x24 / +0x28,
// made by cPlayerInfo::InitUIVars with vuiBar_New: 110 x 6, at (42, 78) and (52, 68) in the 640 x 480 HUD, y up, each
// lower bar 10 to the right along the frame's slant) drawn by cMiniMap::DrawPlayerBars: SetPos, fill (+8), colours
// (+0x14, +0x18), Draw(1, 1), then +4 (shown) cleared. Its two calls (cMiniMap::DrawBorder, vuiMiniMap_Render) are
// wrapped to draw a third bar the same way at (62, 58), in Protoss blue, when the player being drawn
// (gameMultiplayer_GetActivePlayer: split screen draws each) has the blade's shield.
typedef void(__thiscall *DrawBarsFn)(void *);
static DrawBarsFn g_drawPlayerBars;
static void __fastcall DrawPlayerBarsHook(void *minimap, void *) {
    g_drawPlayerBars(minimap);
    static auto active = (uint8_t *(__cdecl *)())SymAddr("?gameMultiplayer_GetActivePlayer@@YAPAVtPlayer@@XZ");
    static auto newBar = (uint8_t *(__cdecl *)(float, float, float, float, const char *, const char *, int, float, unsigned, int))SymAddr(
        "?vuiBar_New@@YAPAVcUIBar@@MMMMPAD0W4eVUIBarDirection@@MKH@Z");
    static auto setPos = (void(__thiscall *)(uint8_t *, unsigned, unsigned))SymAddr("?SetPos@cUIBar@@QAEXII@Z");
    static auto draw = (void(__thiscall *)(uint8_t *, float, int))SymAddr("?Draw@cUIBar@@QAEXMH@Z");
    uint8_t *p = active ? active() : nullptr;
    float amount, max;
    if (!p || SlotOf(p) < 0 || !PlayerShieldState(p, &amount, &max) || max <= 0.f || !newBar || !setPos || !draw) return;
    static uint8_t *bar;
    if (!bar && !(bar = newBar(62, 58, 110, 6, nullptr, nullptr, 0, 0.5f, 0x40a0ff, 1))) return;
    setPos(bar, 62, 58);
    float f = amount / max;
    *(float *)(bar + 8) = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
    *(uint32_t *)(bar + 0x14) = 0x60c0ff;   // (the two colours DrawPlayerBars sets on its bars)
    *(uint32_t *)(bar + 0x18) = 0x2050c0;
    *(uint32_t *)(bar + 0x1c) = 0;
    *(int *)(bar + 4) = 1;
    draw(bar, 1.f, 1);
    *(int *)(bar + 4) = 0;
}
// The Protoss vehicles' shield bar (user: under the vehicle health bar, not Nova's): a vehicle's health is drawn by
// cMiniMap::DrawVehicleHealth (from DrawPowersWindow) as a cUIBar at (45, 202) (top, left in the 640 x 480 HUD, as
// Nova's bars are placed), resized to 70 x 16 each time (push 0x10 at +0x6E). When the vehicle being driven (the active
// player is the vehicle) has a shield (the Reaver, the Dragoon: vehicles.cpp) the health bar is drawn 12 tall and a
// 56 x 4 bar of the shield right under it, at (58, 202), in Protoss blue, both inside the frame's box.
static DrawBarsFn g_drawVehicleHealth;
static void __fastcall DrawVehicleHealthHook(void *minimap, void *) {
    static auto active = (uint8_t *(__cdecl *)())SymAddr("?gameMultiplayer_GetActivePlayer@@YAPAVtPlayer@@XZ");
    static uint8_t *height = nullptr;
    if (!height) {
        uint8_t *vh = (uint8_t *)SymAddr("?DrawVehicleHealth@cMiniMap@@AAEXXZ");
        height = vh && vh[0x6d] == 0x6a && vh[0x6e] == 0x10 ? vh + 0x6e : (uint8_t *)1;
    }
    uint8_t *v = active ? active() : nullptr;
    float a0, m0;
    bool shielded = v && SlotOf(v) < 0 && PlayerShieldState(v, &a0, &m0) && m0 > 0.f;
    if (shielded && height != (uint8_t *)1) {
        DWORD o;
        VirtualProtect(height, 1, PAGE_EXECUTE_READWRITE, &o);
        *height = 12;
        g_drawVehicleHealth(minimap);
        *height = 0x10;
        VirtualProtect(height, 1, o, &o);
    } else
        g_drawVehicleHealth(minimap);
    static auto newBar = (uint8_t *(__cdecl *)(float, float, float, float, const char *, const char *, int, float, unsigned, int))SymAddr(
        "?vuiBar_New@@YAPAVcUIBar@@MMMMPAD0W4eVUIBarDirection@@MKH@Z");
    static auto setPos = (void(__thiscall *)(uint8_t *, unsigned, unsigned))SymAddr("?SetPos@cUIBar@@QAEXII@Z");
    static auto draw = (void(__thiscall *)(uint8_t *, float, int))SymAddr("?Draw@cUIBar@@QAEXMH@Z");
    uint8_t *p = active ? active() : nullptr;
    float amount, max;
    if (!p || SlotOf(p) >= 0 || !PlayerShieldState(p, &amount, &max) || max <= 0.f || !newBar || !setPos || !draw) return;
    static uint8_t *bar;
    if (!bar && !(bar = newBar(58, 202, 56, 4, nullptr, nullptr, 0, 0.5f, 0x40a0ff, 1))) return;
    setPos(bar, 58, 202);
    float f = amount / max;
    *(float *)(bar + 8) = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
    *(uint32_t *)(bar + 0x14) = 0x60c0ff;
    *(uint32_t *)(bar + 0x18) = 0x2050c0;
    *(uint32_t *)(bar + 0x1c) = 0;
    *(int *)(bar + 4) = 1;
    draw(bar, 1.f, 1);
    *(int *)(bar + 4) = 0;
}
static bool Patch(uint8_t *at, const uint8_t *expect, size_t n, const uint8_t *with);
static void HookPlayerBars() {
    uint8_t *fn = (uint8_t *)SymAddr("?DrawPlayerBars@cMiniMap@@AAEXXZ");
    // the turrets' health (user: on a turret the health bar was no use): DrawPlayerBars draws the rider's own health
    // (cPlayerInfo::GetOldPlayer: Nova) when the active control is type 10 or 6 (cGunControl / cFlameGunControl), else
    // the active player's (a Goliath, a Vulture: the vehicle's). A turret takes the hits itself (350 points) and its
    // destruction kills her, so the bar stayed full until she died. The type 6 test (cmp al, 6 at +0x6B) is made never
    // to match: a turret shows its own health, as the other vehicles do.
    if (fn && fn[0x6b] == 0x3c && fn[0x6c] == 0x06) {
        static const uint8_t was[] = {0x3c, 0x06}, now[] = {0x3c, 0xff};
        Patch(fn + 0x6b, was, 2, now);
    } else Log("psiblade: unexpected cMiniMap::DrawPlayerBars, turrets keep showing the rider's health");
    const char *callers[] = {"?DrawBorder@cMiniMap@@QAEXEH@Z", "?vuiMiniMap_Render@@YAXXZ"};
    int n = 0;
    for (const char *c : callers)
        for (uint8_t *q = (uint8_t *)SymAddr(c), *end = q ? q + 0x400 : nullptr; q && fn && q < end; q++) {
            if (q[0] != 0xE8 || q + 5 + *(int32_t *)(q + 1) != fn) continue;
            g_drawPlayerBars = (DrawBarsFn)fn;
            DWORD o;
            VirtualProtect(q + 1, 4, PAGE_EXECUTE_READWRITE, &o);
            *(int32_t *)(q + 1) = (int32_t)((uint8_t *)DrawPlayerBarsHook - (q + 5));
            VirtualProtect(q + 1, 4, o, &o);
            n++;
            break;
        }
    if (n != 2) Log("psiblade: cMiniMap::DrawPlayerBars: %d of 2 calls wrapped (the shield bar may not show)", n);
    uint8_t *vh = (uint8_t *)SymAddr("?DrawVehicleHealth@cMiniMap@@AAEXXZ");
    int m = 0;
    for (uint8_t *q = (uint8_t *)SymAddr("?DrawPowersWindow@cMiniMap@@AAEXXZ"), *end = q ? q + 0x600 : nullptr; q && vh && q < end; q++) {
        if (q[0] != 0xE8 || q + 5 + *(int32_t *)(q + 1) != vh) continue;
        g_drawVehicleHealth = (DrawBarsFn)vh;
        DWORD o;
        VirtualProtect(q + 1, 4, PAGE_EXECUTE_READWRITE, &o);
        *(int32_t *)(q + 1) = (int32_t)((uint8_t *)DrawVehicleHealthHook - (q + 5));
        VirtualProtect(q + 1, 4, o, &o);
        m++;
    }
    if (m != 1) Log("psiblade: cMiniMap::DrawVehicleHealth: %d calls wrapped (expected 1; the vehicle shield bar may not show)", m);
}

// ---------------------------------------------------------------------------------------------------------------
// The Psi Blade as a weapon (user: selected like the flamethrower; the blade shows only then, swings with the new
// animations, Tanagazj's damage, and stealth kills; any other weapon keeps Nova's own melee).
// Weapons are tPlayer +0x460[slot] (1 shock glove: unarmed, its melee is attack1; 2 pistol, 3 gauss, 4 flamethrower,
// 5 grenade, 6 sniper); +0x454 the current one, +0x458 the cycle position. The cycle is slots 2-4 (SetNextPlayerWeapon
// wraps to 2, SetPrevPlayerWeapon stops at 2, and the weapon list's GetNext / GetPrevPlayerWeapon likewise), skipping a
// weapon whose +0x5F has bit 2. So:
//   - the bound becomes 1 (SetNext's wrap immediate, SetPrev's compare; GetNext / GetPrev replaced by the same walks
//     over 1-4), and slot 1 gets bit 2 every frame unless its player has the blade: nothing changes without it;
//   - slot 1 is the Psi Blade: its name (+0) becomes LBENTRY_WEAPONPSIBLADE ("Psi Blade") while she has it, and the
//     weapon list draws it with ItemCanisterEMPIcon (user's choice for now; cUIInventory::DrawGun, called from
//     DrawWeaponIcon and DrawWeaponUI, would draw the machine gun's pieces for it);
//   - the blade effect is hidden (cEffect +0x50 bit 2, which both draw paths skip) unless slot 1 is the current weapon;
//   - its swings play the Zealot's swing (ptssZealot "swing", Sounds\custom\ghostpc\psiblade_swing.wav) on the K
//     animations' tag 0x13 (just before each hit; cPlayerControl::MeleeCallback ignores it);
//   - stealth kills: cPlayerControl::Activate's call of SilentStun (the takedown behind an unaware enemy) only goes on
//     with the blade selected (nova_psiblade.nms has the silentkill animations; nova.nms has none, so Nova never did
//     them), playing one of the Zealot's silentkill sounds;
//   - damage: vehicles.cpp's tActor::Damage detour asks PsiBladeMeleeDamage for hits from cPlayerControl::
//     MeleeHitCheck (her melee: amount = cPlayerControl +0x229 per move): with the blade selected, Tanagazj's 10
//     (cAIBossTanagazj::BladeHitCheck: MeleeHitCheck(..., 10) per hit).
bool CustomPlayOnce(const char *key, const float *at);   // customsound.cpp
const float BLADE_DAMAGE = 80.f;   // (Tanagazj's blade as GhostPC tuned it: levelscript.cpp FixTanagazjClass, 80; the game's own 10 was too low, user)
static char g_gloveName[32];   // slot 1's own name, put back when the blade goes

static bool HasBlade(const uint8_t *player) { return SlotOf(player) >= 0; }
// selected = the player's own choice, the cycle position (+0x458): every melee switches the current weapon (+0x454) to
// slot 1 for its swing (cPlayerControl::MeleeStart / Activate -> ChangeToBlade -> SetPlayerWeapon(1, 1, 0)) and back,
// leaving +0x458 on the gun
static bool BladeSelected(const uint8_t *player) { return HasBlade(player) && *(const int *)(player + 0x458) == 1; }
// in the cycle: slot 1 only for a player with the blade (its +0x5F bit is left alone: every melee switches to slot 1,
// and setting its bit 2 to hide it broke Nova's own melee, the kick included, user), the others as the game has them
static bool Visible(const uint8_t *player, int slot) {
    const uint8_t *w = *(uint8_t *const *)(player + 0x460 + 4 * slot);
    if (!w || (w[0x5f] & 2) || (slot == 1 && !HasBlade(player))) return false;
    if (w[0x5f] & 1) {   // (a weapon with ammo needs a maximum for its type, as SetNextPlayerWeapon checks)
        int type = *(const int *)(w + 0x44);
        if (type < 0 || type >= 4 || !*(const int *)(player + 0x434 + 4 * type)) return false;
    }
    return true;
}
// tPlayer::SetNext / SetPrevPlayerWeapon(bool justCheck), replaced: walk the cycle position (+0x458) over 1-4 (the
// game's walk 2-4) to the next visible weapon, then SetPlayerWeapon(it, 0, 0) unless only checking
static int __fastcall SetNextWeapon(uint8_t *player, void *, int justCheck);
static int __fastcall SetPrevWeapon(uint8_t *player, void *, int justCheck);
static int __fastcall GetNextWeapon(uint8_t *player, void *, int cur) {   // (as tPlayer::GetNextPlayerWeapon, over 1-4)
    if (cur < 0) cur = *(int *)(player + 0x454);
    if (cur > 4) cur = 4;
    for (int i = cur + 1; i <= 4; i++)
        if (Visible(player, i)) return i;
    for (int i = 1; i <= cur; i++)
        if (Visible(player, i)) return i;
    return 0;
}
static int __fastcall GetPrevWeapon(uint8_t *player, void *, int cur) {   // (as tPlayer::GetPrevPlayerWeapon, over 1-4)
    if (cur < 0) cur = *(int *)(player + 0x454);
    if (cur)
        for (int i = cur - 1; i >= 1; i--)
            if (Visible(player, i)) return i;
    if (cur <= 4)
        for (int i = 4; i >= cur && i >= 1; i--)
            if (Visible(player, i)) return i;
    return 0;
}
// tPlayer::CountAvailableWeapons (how many the weapon list draws; slots 2-4 in the game), replaced: the same walk's 1-4
static int __fastcall CountWeapons(uint8_t *player, void *) {
    int n = 0;
    for (int i = 1; i <= 4; i++) n += Visible(player, i);
    return n;
}
typedef void(__thiscall *DrawGunFn)(void *, float, float, int, float);
static DrawGunFn g_drawGun;
static void __fastcall DrawGunHook(void *inv, void *, float x, float y, int weapon, float alpha) {
    static auto active = (uint8_t *(__cdecl *)())SymAddr("?gameMultiplayer_GetActivePlayer@@YAPAVtPlayer@@XZ");
    static auto loadTex = (uint8_t *(__cdecl *)(const char *, unsigned, int))SymAddr("?Load@cTexture@@SAPAV1@PBDIW4eTexFormat@@@Z");
    static auto quad = (void(__cdecl *)(unsigned, uint8_t *, unsigned, void *, float *, float, float, float, unsigned))SymAddr(
        "?vui_DrawQuad@@YAXIPAVcTexture@@IPAUXGVECTOR2@@1MMMK@Z");
    uint8_t *p = active ? active() : nullptr;
    if (weapon != 1 || !HasBlade(p) || !loadTex || !quad) return g_drawGun(inv, x, y, weapon, alpha);
    static uint8_t *tex = loadTex("ItemCanisterEMPIcon", 0x20, -1);
    if (!tex) return g_drawGun(inv, x, y, weapon, alpha);
    float at[2] = {x + 34.f, y - 2.f};   // (the machine gun's first piece goes at x + 24.5, y + 7; this one is 20 x 20)
    quad(0, tex, 0xd, nullptr, at, 20.f, 20.f, alpha * 255.f, 0xffff);
}
// its ammo text: cUIInventory::GetWeaponAmmoText(slot, player, text, text 2) would print an ammo count for slot 1 (it
// showed 56); the blade has none, so both texts are empty
typedef int(__thiscall *AmmoTextFn)(void *, int, uint8_t *, char *, char *);
static AmmoTextFn g_ammoText;
static int __fastcall AmmoTextHook(void *inv, void *, int slot, uint8_t *player, char *a, char *b) {
    if (slot != 1 || !HasBlade(player)) return g_ammoText(inv, slot, player, a, b);
    if (a) a[0] = 0;
    if (b) b[0] = 0;
    return 0;
}
typedef void(__thiscall *MeleeCbFn)(uint8_t *, void *, int);
static MeleeCbFn g_meleeCallback;
static void __fastcall MeleeCallbackHook(uint8_t *ctl, void *, void *channel, int tag) {
    uint8_t *p = *(uint8_t **)(ctl + 0x180);
    if (tag == 0x13 && BladeSelected(p)) CustomPlayOnce("ghostpc/psiblade_swing", (float *)(p + 0xb0));
    g_meleeCallback(ctl, channel, tag);
}
typedef int(__thiscall *SilentStunFn)(uint8_t *, uint8_t *);
static SilentStunFn g_silentStun;
static int __fastcall SilentStunHook(uint8_t *ctl, void *, uint8_t *target) {
    uint8_t *p = *(uint8_t **)(ctl + 0x180);
    if (HasBlade(p) && !BladeSelected(p)) return 0;   // (stealth kills with the blade on)
    int r = g_silentStun(ctl, target);
    if (r > 0 && HasBlade(p)) {
        CustomPlayOnce("ghostpc/psiblade_kill", (float *)(p + 0xb0));
        Log("psiblade: stealth kill on %.24s", target ? (char *)(target + 0xc4) : "?");
    }
    return r;
}
float PsiBladeMeleeDamage(uint8_t *src, float amount, void *caller) {   // vehicles.cpp: tActor::Damage
    static uint8_t *hit = (uint8_t *)SymAddr("?MeleeHitCheck@cPlayerControl@@AAEHI@Z");
    if (!hit || (uint8_t *)caller < hit || (uint8_t *)caller > hit + 0xa00) return amount;
    static int logs;
    if (logs < 6) logs++, Log("psiblade: melee hit by %.24s: %.0f -> %.0f%s", src ? (char *)(src + 0xc4) : "?", amount,
                             BladeSelected(src) ? BLADE_DAMAGE : amount, BladeSelected(src) ? " (the blade)" : " (Nova's own melee)");
    return BladeSelected(src) ? BLADE_DAMAGE : amount;
}

// tPlayer::SetPlayerWeapon(slot, a, b): slot 1 is switched to at once (+0x454 = 1) without the pending weapon (+0x45C),
// which every other slot sets; tPlayer::EarlyCompleteWeaponChange (from GaussRifleStop and others) then changes to
// +0x45C when it differs. That is how a melee swing returns to the gun (it switches to slot 1 and leaves +0x45C on the
// gun), but selecting the Psi Blade (cycle position +0x458 = 1) would be undone the same way, or, with +0x45C never set
// (0xCCCCCCCC), crash (MountWeaponCallback indexing +0x460 with it). So when slot 1 is set for a player who chose it,
// +0x45C becomes 1 too.
typedef int(__thiscall *SetWeaponFn)(uint8_t *, int, int, int);
static SetWeaponFn g_setWeapon;
static int __fastcall SetWeaponHook(uint8_t *player, void *, int slot, int a, int b) {
    int r = g_setWeapon(player, slot, a, b);
    if (slot == 1 && BladeSelected(player)) *(int *)(player + 0x45c) = 1;
    return r;
}

static int StepWeapon(uint8_t *player, int justCheck, int dir) {
    int &pos = *(int *)(player + 0x458);
    for (int tries = 0; tries < 4; tries++) {
        pos += dir;
        if (pos > 4) pos = 1;
        if (pos < 1) pos = 4;
        if (Visible(player, pos)) break;
    }
    if (justCheck) return 1;
    static auto setWeapon = (int(__thiscall *)(uint8_t *, int, int, int))SymAddr("?SetPlayerWeapon@tPlayer@@QAEHW4ePlayerWeapons@@HH@Z");
    return setWeapon ? setWeapon(player, pos, 0, 0) : 0;
}
static int __fastcall SetNextWeapon(uint8_t *player, void *, int justCheck) { return StepWeapon(player, justCheck, 1); }
static int __fastcall SetPrevWeapon(uint8_t *player, void *, int justCheck) { return StepWeapon(player, justCheck, -1); }

// the blade's swings (user: its own weapon, forked from melee, not replacing it): cPlayerControl::MeleeAttack (B, and
// the blade's fire press) plays attack1 (mode 0xF) -- Nova's own jabs; with the blade chosen its call of
// tObject::PlayMotion plays attack2 / attack3 instead (nova_psiblade.nms: the K wrist swipe / spin), alternating
typedef int(__thiscall *PlayMotionFn)(uint8_t *, int, unsigned, float, void *, float, float);
static PlayMotionFn g_playMotion;
static int __fastcall MeleePlayMotionHook(uint8_t *obj, void *, int mode, unsigned a, float fps, void *set, float c, float d) {
    static int next;
    if (mode == 0xf && BladeSelected(obj)) mode = (next++ & 1) ? 0x11 : 0x10;
    return g_playMotion(obj, mode, a, fps, set, c, d);
}

typedef int(__thiscall *StartStateFn)(uint8_t *, int);
static StartStateFn g_startState;
static int __fastcall StartStateHook(uint8_t *ctl, void *, int state) {
    if (getenv("GHOSTPC_LOG_STATES") && BladeSelected(*(uint8_t **)(ctl + 0x180)))
        Log("psiblade: StartState %d from %d (aim button %d)", state, ctl[0x5e], *(int *)(ctl + 0x1e0));
    return g_startState(ctl, state);
}

// the blade's gun state (user: right click held with the blade opens the weapon wheel, without aiming; "a gun state
// without a crosshair that doesn't play the aim animation"): right click (the control's aim button, +0x1E0) starts the
// gun states as with any weapon -- GaussRifle (27) standing, GunCrouch crouched -- whose updates open the wheel
// (OpenWeaponSelect) once it is held 250 ms and run it (UpdateWeaponSelect, slow motion) until it is let go. With the
// blade chosen their starts and updates are replaced: no aim motion (0xE2), aim camera or crosshair; she stands (mode 3,
// as StandStart plays it); let go before the wheel, she walks again (crouches, from GunCrouch). Fire swings the blade
// (WeaponFrame). A gun picked on the wheel gets the game's own start (aim camera, pose) once the wheel closes; the blade
// picked while aiming a gun ends the aim (StartState walk / crouch).
typedef int(__thiscall *GunStartFn)(uint8_t *, int);
typedef void(__thiscall *GunUpdateFn)(uint8_t *);
static GunStartFn g_rifleStart, g_crouchStart;
static GunUpdateFn g_rifleUpdate, g_crouchUpdate;
static int g_noGun, g_noGunSt;   // (a control whose player has no blade)
static int &BladeGun(uint8_t *ctl) { int i = SlotOf(*(uint8_t **)(ctl + 0x180)); return i < 0 ? (g_noGun = 0) : g_given[i].bladeGun; }
static int &BladeGunSt(uint8_t *ctl) { int i = SlotOf(*(uint8_t **)(ctl + 0x180)); return i < 0 ? g_noGunSt : g_given[i].bladeGunSt; }
// the game's own start (its aim camera: skipping it left the camera broken and the world black, user), then no aim pose
// (she stands: mode 3, as StandStart plays it) and no crosshair
static int BladeGunStart(uint8_t *ctl, int kind, GunStartFn start, int prev) {
    static auto reticle = (void(__thiscall *)(uint8_t *, int))SymAddr("?SetReticle@cControl@@QAEXH@Z");
    uint8_t *p = *(uint8_t **)(ctl + 0x180);
    int r = start(ctl, prev);
    if (kind == 1 && g_playMotion) g_playMotion(p, 3, 1, 30.f, nullptr, 7.5f, -1.f);
    if (reticle) reticle(ctl, 0);
    BladeGun(ctl) = kind;
    if (getenv("GHOSTPC_LOG_STATES")) Log("psiblade: blade gun state (%s)", kind == 1 ? "standing" : "crouched");
    return r;
}
static void BladeGunUpdate(uint8_t *ctl, int kind, GunStartFn start, GunUpdateFn update) {
    uint8_t *p = *(uint8_t **)(ctl + 0x180);
    bool wheel = (*(uint32_t *)(ctl + 0x68) & 2) != 0;
    if (BladeGun(ctl) != kind) {   // in the state with a gun
        if (!wheel && BladeSelected(p)) {   // the blade picked while aiming: no aiming with it
            if (getenv("GHOSTPC_LOG_STATES")) Log("psiblade: blade picked in a gun state, leaving it");
            g_startState(ctl, kind == 1 ? 2 : 5);
            return;
        }
        update(ctl);
        return;
    }
    if (*(uint32_t *)(ctl + 0x70) & 2) {   // (no crosshair)
        static auto reticle = (void(__thiscall *)(uint8_t *, int))SymAddr("?SetReticle@cControl@@QAEXH@Z");
        if (reticle) reticle(ctl, 0);
    }
    if (wheel) {   // the wheel (UpdateWeaponSelect; it changes the weapon as it turns, and closes when let go)
        update(ctl);
        return;
    }
    if (!BladeSelected(p)) {   // a gun picked on the wheel: the game's own gun state from here
        BladeGun(ctl) = 0;
        if (getenv("GHOSTPC_LOG_STATES")) Log("psiblade: gun %d picked on the wheel, aiming it", *(int *)(p + 0x458));
        if (start) start(ctl, ctl[0x5e]);
        return;
    }
    BladeGunSt(ctl) = ctl[0x5e];
    static auto down = (int(__cdecl *)(unsigned))SymAddr("?stdInput_IsJoyButtonDown@@YAHI@Z");
    static auto hold = (int(__cdecl *)(unsigned))SymAddr("?stdInput_GetJoyButtonHoldTime@@YAHI@Z");
    static auto openSel = (void(__thiscall *)(uint8_t *))SymAddr("?OpenWeaponSelect@cPlayerControl@@AAEXXZ");
    unsigned btn = *(unsigned *)(ctl + 0x1e0);
    if (!down || !hold || !openSel || !down(btn)) {   // let go: walking (crouching) again
        BladeGun(ctl) = 0;
        g_startState(ctl, kind == 1 ? 2 : 5);
    } else if (hold(btn) > 0xfa) {
        openSel(ctl);
        if (getenv("GHOSTPC_LOG_STATES")) Log("psiblade: weapon wheel opened");
    }
}
// a blade owner who chose a gun walks with the blade still current (+0x454 = 1: the blade stays drawn while it is
// chosen, where a gun is holstered, 0); a gun state's start ends with SetPlayerWeapon(+0x454), which put slot 1, the bare
// glove, back in her hands (user: after switching from the blade the gun would not fire until switched again). Holstered
// first, as the game has it when she walks with a gun.
static void HolsterBlade(uint8_t *ctl) {
    uint8_t *p = *(uint8_t **)(ctl + 0x180);
    if (HasBlade(p) && !BladeSelected(p) && *(int *)(p + 0x454) == 1) {
        *(int *)(p + 0x454) = 0;
        if (getenv("GHOSTPC_LOG_STATES")) Log("psiblade: gun chosen, the blade holstered before aiming");
    }
}
static int __fastcall RifleStartHook(uint8_t *ctl, void *, int prev) {
    if (BladeSelected(*(uint8_t **)(ctl + 0x180))) return BladeGunStart(ctl, 1, g_rifleStart, prev);
    BladeGun(ctl) = 0;
    HolsterBlade(ctl);
    return g_rifleStart(ctl, prev);
}
static int __fastcall CrouchStartHook(uint8_t *ctl, void *, int prev) {
    if (BladeSelected(*(uint8_t **)(ctl + 0x180))) return BladeGunStart(ctl, 2, g_crouchStart, prev);
    BladeGun(ctl) = 0;
    HolsterBlade(ctl);
    return g_crouchStart(ctl, prev);
}
static void __fastcall RifleUpdateHook(uint8_t *ctl) { BladeGunUpdate(ctl, 1, g_rifleStart, g_rifleUpdate); }
static void __fastcall CrouchUpdateHook(uint8_t *ctl) { BladeGunUpdate(ctl, 2, g_crouchStart, g_crouchUpdate); }
// The plunge (user: a mid-air attack with the blade): fire while jumping or falling with the blade chosen. The game
// has unused motion modes for it, attackdown (0x1F) and attackdownland (0x20), which nova_psiblade.nms gives the K set's
// JumpDownAttack / JumpDownAttack_land. She goes to the falling state (FallUpdate: air steering, CheckLanding) if she
// was rising, plays attackdown and is driven down at PLUNGE_SPEED; cPlayerControl::Land (detoured) then does the
// game's landing first (fall damage, foley) and the strike: attackdownland in the melee state (0x1E, held for the
// motion's length, as MeleeAttack does) and PLUNGE_DAMAGE to every living actor within PLUNGE_RADIUS (melee damage,
// through tActor::Damage: shields first).
const float HUM_VOLUME = 0.25f;   // (user: the hum was far too loud)
// (radius from her to the target's origin, beyond its own size: 5 m, 4 m up or down; 3.5 / 2.5 missed the Lurker, user)
const float PLUNGE_SPEED = 15.f, PLUNGE_DAMAGE = 120.f, PLUNGE_RADIUS = 5.f, PLUNGE_HEIGHT = 4.f;
static int g_fallState = -1;
static unsigned g_airStates;   // (bit per control state: jumping / falling)
static void FindAirStates() {
    static bool done;
    uint8_t *states = (uint8_t *)SymAddr("?aStates@cPlayerControl@@1PAUcControlState@@A");
    if (done || !states) return;
    const char *names[] = {"?JumpStart@cPlayerControl@@AAEHI@Z", "?JumpUpdate@cPlayerControl@@AAEXXZ", "?FallStart@cPlayerControl@@AAEHI@Z",
                           "?FallUpdate@cPlayerControl@@AAEXXZ", "?ForwardJumpUpdate@cPlayerControl@@AAEXXZ",
                           "?StrafeFallStart@cPlayerControl@@AAEHI@Z", "?StrafeFallUpdate@cPlayerControl@@AAEXXZ"};
    uint32_t fns[7];
    for (int i = 0; i < 7; i++) fns[i] = SymAddr(names[i]);
    for (int st = 0; st < 32; st++)
        for (int k = 0; k < 0x90; k += 4) {
            uint32_t v = *(uint32_t *)(states + st * 0x90 + k);
            for (int i = 0; i < 7; i++)
                if (v && v == fns[i]) {
                    g_airStates |= 1u << st;
                    if (i == 2) g_fallState = st;
                }
        }
    done = g_airStates != 0;
    Log("psiblade: air states %08x, falling %d", g_airStates, g_fallState);
}
static int PlayAction(uint8_t *ctl, int mode) {   // (as MeleeAttack plays its swing)
    uint8_t *p = *(uint8_t **)(ctl + 0x180);
    return g_playMotion ? g_playMotion(p, mode, 0, 30.f, nullptr, 7.5f, -1.f) : -1;
}
static bool PlungeStart(uint8_t *ctl) {
    FindAirStates();
    int st = ctl[0x5e], i = SlotOf(*(uint8_t **)(ctl + 0x180));
    if (i < 0 || g_given[i].plunge || st >= 32 || !(g_airStates & (1u << st)) || g_fallState < 0) return false;
    if (st != g_fallState) g_startState(ctl, g_fallState);
    if (PlayAction(ctl, 0x1f) < 0) Log("psiblade: no attackdown motion");
    CustomPlayOnce("ghostpc/psiblade_swing", (const float *)(*(uint8_t **)(ctl + 0x180) + 0xb0));   // (the dive's swing)
    g_given[i].plunge = true;
    Log("psiblade: plunge from state %d (player %d)", st, i + 1);
    return true;
}
static void PlungeFrame(Given &g) {   // (driven down while it lasts; it ends on landing, or out of the air states)
    if (!g.plunge) return;
    uint8_t *p = g.player, *ctl = p ? *(uint8_t **)(p + 0x3f8) : nullptr;
    int st = ctl ? ctl[0x5e] : -1;
    if (!ctl || st >= 32 || st < 0 || !(g_airStates & (1u << st)) || (*(uint32_t *)(p + 0x360) & DEAD)) {
        g.plunge = false;
        return;
    }
    static auto getVel = (void(__thiscall *)(uint8_t *, float *))SymAddr("?GetVelocity@tThing@@QAEXPAUXGVECTOR3@@@Z");
    static auto setVel = (void(__thiscall *)(uint8_t *, const float *))SymAddr("?SetVelocity@tThing@@QAEXPBUXGVECTOR3@@@Z");
    float v[3];
    if (getVel && setVel && *(void **)(p + 0x158)) {
        getVel(p, v);
        if (v[2] > -PLUNGE_SPEED) v[2] = -PLUNGE_SPEED, setVel(p, v);
    }
}
static void PlungeStrike(uint8_t *ctl) {
    uint8_t *p = *(uint8_t **)(ctl + 0x180);
    int d = PlayAction(ctl, 0x20);
    CustomPlayOnce("ghostpc/psiblade_kill", (const float *)(p + 0xb0));   // (the impact: the Zealot's blade kill sound)
    if (d >= 0) {
        if (d < 650) d = 650;   // (its length, 20 frames: PlayMotion gives 0 for an animation without event tags)   // (MeleeAttack's: hit flags, amount, melee state, controls held for the motion)
        static auto hold = (void(__thiscall *)(uint8_t *, unsigned))SymAddr("?SetHoldTime@cPlayerControl@@QAEXI@Z");
        *(uint16_t *)(ctl + 0x22e) = 0x201;
        ctl[0x229] = 5;
        ctl[0x224] = 0;
        // (no turning in it: MeleeUpdate turns her by control +0x1F0 x the frame time every frame, a turn rate a swing
        // starts at 0; from the air she brought the steering's, and spun around for the whole strike, user)
        *(float *)(ctl + 0x1f0) = 0.f;
        g_startState(ctl, 0x1e);
        int i = SlotOf(p);
        if (i >= 0) g_given[i].strikeUntil = g_frame + 30;
        if (hold) hold(ctl, d);
    }
    static unsigned *nextGuid = (unsigned *)SymAddr("?nextGUID@tThing@@2IA");
    static auto findT = (uint8_t *(__cdecl *)(unsigned))SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    typedef float(__thiscall *DamageFn)(uint8_t *, uint8_t *, int, int, float, float *, float *, unsigned, int, float, int *);
    static auto damage = (DamageFn)SymAddr("?Damage@tActor@@QAEMPAVtThing@@W4eDamageSource@@W4eDamageType@@MPAUXGVECTOR3@@3IW4eHitRegionType@@MPAH@Z");
    int hits = 0;
    float nearest = 1e9f;
    const char *nearName = "none";
    const float *at = (const float *)(p + 0xb0);
    {   // the impact (user: a shockwave on the ground where she lands, like the Dragoon shot's): Misc\ghostpc.nfx
        // psibladeShock, game.nfx's dragoonProjExp0 (a flat blue ring on the ground) widened to the plunge's 5 m, made
        // with cEffectManager::CreateHitEffectFromName as CreateHitEffect makes its own (name, no thing, position,
        // direction, 0, 0, -99999). (CreateEffect by name never showed it; CreateHitEffect's hit type 10, the cut blade
        // dive's landing -- bladeDive sparks and the superdiveshock wall, 6 m tall, 0.5 m up -- was massive and floating.)
        typedef void *(__thiscall *HitFromNameFn)(void *, const char *, void *, float *, float *, int, float, float, void *, void *, float);
        static auto fromName = (HitFromNameFn)SymAddr(
            "?CreateHitEffectFromName@cEffectManager@@QAEPAVcEffect@@PADPAVtThing@@PAUXGVECTOR3@@2HMM0PAVcEffectDef@@M@Z");
        static void *fxManager = (void *)SymAddr("?effectManager_Manager@@3VcEffectManager@@A");
        float pos[3] = {at[0], at[1], at[2]}, up[3] = {0.f, 0.f, 1.f};
        void *fx = fromName && fxManager ? fromName(fxManager, "psibladeShock", nullptr, pos, up, 0, 0.f, -99999.f, nullptr, nullptr, 0.f) : nullptr;
        if (!fx) Log("psiblade: no psibladeShock effect (not in this level's effect list?)");
    }
    // targets: enemies (actors with an AI that is not a prop's animation player; not the players, nor beacons), and in a
    // competitive game (the chronicle's game type 2) the other player too (user: the plunge did nothing to Echo; players
    // are type-7 things with no AI, so the search never took them)
    std::vector<uint8_t *> targets;
    static uint32_t propAI = SymAddr("??_7cAIDemoAnims@@6B@");
    for (unsigned g = 0; nextGuid && findT && g < *nextGuid; g++) {
        uint8_t *t = findT(g);
        if (!t || t == p || *(int *)(t + 0x10) != 0 || (*(uint32_t *)(t + 0x360) & DEAD)) continue;
        uint8_t *ai = *(uint8_t **)(t + 0x3c4);
        if (!ai || IsBadReadPtr(ai, 4) || *(uint32_t *)ai == propAI || t == LocalPlayer(0) || t == LocalPlayer(1)) continue;
        targets.push_back(t);
    }
    static uint8_t *chron = (uint8_t *)SymAddr("?gameChronicle@@3VcChronicle@@A");
    if (chron && *(int *)(chron + 4) == 2)
        if (uint8_t *rival = p == LocalPlayer(0) ? LocalPlayer(1) : LocalPlayer(0))
            if (!(*(uint32_t *)(rival + 0x360) & DEAD)) targets.push_back(rival);
    for (uint8_t *t : targets) {
        if (!damage) break;
        const float *q = (const float *)(t + 0xb0);
        float dir[3] = {q[0] - at[0], q[1] - at[1], q[2] - at[2]};
        float dist = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]);
        if (dist < nearest) nearest = dist, nearName = (const char *)(t + 0xc4);
        if (dist > PLUNGE_RADIUS || fabsf(dir[2]) > PLUNGE_HEIGHT) continue;
        if (dist > 0.01f) dir[0] /= dist, dir[1] /= dist;
        dir[2] = 0.f;
        float pos[3] = {q[0], q[1], q[2] + 1.f};
        damage(t, p, 1, 0, PLUNGE_DAMAGE, pos, dir, 0, 0, 0.f, nullptr);
        hits++;
        Log("psiblade: plunge hits %.24s at %.1f m", (char *)(t + 0xc4), dist);
    }
    Log("psiblade: plunge strike (motion %d ms), %d hit (nearest enemy %.24s at %.1f m)", d, hits, nearName, nearest);

}
typedef void(__thiscall *LandFn)(uint8_t *);
static LandFn g_land;
static void __fastcall LandHook(uint8_t *ctl) {
    uint8_t *p = *(uint8_t **)(ctl + 0x180);
    int i = SlotOf(p);
    bool plunge = i >= 0 && g_given[i].plunge;
    g_land(ctl);
    if (plunge) {
        g_given[i].plunge = false;
        if (!(*(uint32_t *)(p + 0x360) & DEAD)) PlungeStrike(ctl);
    }
}
// a jmp from a function's first n bytes (checked against expect) to hook; returns the trampoline running them, or null
static void *DetourAt(const char *name, const uint8_t *expect, size_t n, void *hook) {
    uint8_t *f = (uint8_t *)SymAddr(name);
    if (!f || memcmp(f, expect, n)) {
        Log("psiblade: unexpected %.60s, not hooked", name);
        return nullptr;
    }
    uint8_t *t = (uint8_t *)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(t, f, n);
    t[n] = 0xE9;
    *(int32_t *)(t + n + 1) = (int32_t)((f + n) - (t + n + 5));
    uint8_t jmp[5] = {0xE9};
    *(int32_t *)(jmp + 1) = (int32_t)((uint8_t *)hook - (f + 5));
    DWORD o;
    VirtualProtect(f, 5, PAGE_EXECUTE_READWRITE, &o);
    memcpy(f, jmp, 5);
    VirtualProtect(f, 5, o, &o);
    FlushInstructionCache(GetCurrentProcess(), f, 5);
    return t;
}
// Quit crash guard (user: the game crashed quitting, with no log or dump; FINDINGS: once a tRenderKeyTrack was freed
// twice from tMotionSet::Free in tObject::Free when the world was freed, so some motion set's reference count went one
// too low): tMotionSet::Free (dec [this]; frees its key tracks at 0) is detoured; a set whose count is already 0 or
// less (freed memory reads 0xDDDDDDDD) is not freed again, and the object being freed (tObject::Free's breadcrumb,
// vehicles.cpp) is logged, to find whose count is off.
typedef int(__thiscall *SetFreeFn)(uint8_t *);
static SetFreeFn g_setFree;
static int __fastcall MotionSetFree(uint8_t *ms) {
    int refs = *(int *)ms;
    if (refs <= 0 || refs > 0x10000) {
        char name[33] = "?";
        if (!IsBadReadPtr(ms + 4, 32)) memcpy(name, ms + 4, 32), name[32] = 0;
        for (char *c = name; *c; c++) if ((unsigned char)*c < 32 || (unsigned char)*c > 126) { *c = 0; break; }
        Log("motionset: %p (%s) freed with %d references: skipped (step: %s)", ms, name, refs, g_crumb);
        return 0;
    }
    return g_setFree(ms);
}

static void Redirect(const char *caller, size_t range, uint8_t *target, void *to, const char *what) {
    int n = 0;
    for (uint8_t *q = (uint8_t *)SymAddr(caller), *end = q ? q + range : nullptr; q && target && q < end; q++)
        if (q[0] == 0xE8 && q + 5 + *(int32_t *)(q + 1) == target) {
            DWORD o;
            VirtualProtect(q + 1, 4, PAGE_EXECUTE_READWRITE, &o);
            *(int32_t *)(q + 1) = (int32_t)((uint8_t *)to - (q + 5));
            VirtualProtect(q + 1, 4, o, &o);
            n++;
            q += 4;
        }
    if (n != 1) Log("psiblade: %s: %d calls redirected in %.40s (expected 1)", what, n, caller);
}
static bool Patch(uint8_t *at, const uint8_t *expect, size_t n, const uint8_t *with) {
    if (!at || memcmp(at, expect, n)) return false;
    DWORD o;
    VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &o);
    memcpy(at, with, n);
    VirtualProtect(at, n, o, &o);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}
// before a level is freed (quitting, a level change: tSequence::TerminateLevel), the blade is taken away like on a
// restart, so the world is freed with Nova's own model, motion set and no shield attached (user: quitting crashed)
typedef int(__thiscall *TerminateFn)(void *, unsigned);
static TerminateFn g_terminate;
static int __fastcall TerminateHook(void *seq, void *, unsigned a) {
    if (g_given[0].player || g_given[1].player) {
        Log("psiblade: level ending, taking the blade away first");
        RevertAll();
    }
    return g_terminate(seq, a);
}

static void HookWeapon() {
    // TerminateLevel: test byte ptr [abs], 1; push esi; push edi (9 bytes, the absolute address already relocated)
    uint8_t *tl = (uint8_t *)SymAddr("?TerminateLevel@tSequence@@QAEHI@Z");
    if (tl && tl[0] == 0xF6 && tl[1] == 0x05 && tl[6] == 1 && tl[7] == 0x56 && tl[8] == 0x57) {
        uint8_t *t = (uint8_t *)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        memcpy(t, tl, 9);
        t[9] = 0xE9;
        *(int32_t *)(t + 10) = (int32_t)((tl + 9) - (t + 14));
        g_terminate = (TerminateFn)t;
        uint8_t jmp[5] = {0xE9};
        *(int32_t *)(jmp + 1) = (int32_t)((uint8_t *)TerminateHook - (tl + 5));
        Patch(tl, tl, 5, jmp);
    } else Log("psiblade: unexpected tSequence::TerminateLevel, the blade stays on at level end");
    uint8_t *next = (uint8_t *)SymAddr("?SetNextPlayerWeapon@tPlayer@@QAEHH@Z"), *prev = (uint8_t *)SymAddr("?SetPrevPlayerWeapon@tPlayer@@QAEHH@Z");
    static const uint8_t sstart[] = {0x8B, 0x91, 0x58, 0x04, 0x00};   // SetNext / SetPrev: mov edx, [ecx + 0x458]
    bool ok = true;
    std::pair<uint8_t *, void *> sets[] = {{next, (void *)SetNextWeapon}, {prev, (void *)SetPrevWeapon}};
    for (auto &r : sets) {
        uint8_t jmp[5] = {0xE9};
        *(int32_t *)(jmp + 1) = r.first ? (int32_t)((uint8_t *)r.second - (r.first + 5)) : 0;
        ok = Patch(r.first, sstart, 5, jmp) && ok;   // (replaced)
    }
    static const uint8_t start[] = {0x53, 0x56, 0x8B, 0x74, 0x24};   // GetNext / GetPrev's first bytes
    uint8_t *gn = (uint8_t *)SymAddr("?GetNextPlayerWeapon@tPlayer@@QAE?AW4ePlayerWeapons@@H@Z");
    uint8_t *gp = (uint8_t *)SymAddr("?GetPrevPlayerWeapon@tPlayer@@QAE?AW4ePlayerWeapons@@H@Z");
    std::pair<uint8_t *, void *> repl[] = {{gn, (void *)GetNextWeapon}, {gp, (void *)GetPrevWeapon}};
    for (auto &r : repl) {
        uint8_t jmp[5] = {0xE9};
        *(int32_t *)(jmp + 1) = r.first ? (int32_t)((uint8_t *)r.second - (r.first + 5)) : 0;
        ok = Patch(r.first, start, 5, jmp) && ok;   // (replaced: the jmp never comes back)
    }
    uint8_t *count = (uint8_t *)SymAddr("?CountAvailableWeapons@tPlayer@@QAEIXZ");
    static const uint8_t cstart[] = {0x53, 0x56, 0x8B, 0xB1, 0x68};   // push ebx; push esi; mov esi, [ecx + 0x468]
    uint8_t cjmp[5] = {0xE9};
    *(int32_t *)(cjmp + 1) = count ? (int32_t)((uint8_t *)CountWeapons - (count + 5)) : 0;
    ok = Patch(count, cstart, 5, cjmp) && ok;
    if (!ok) Log("psiblade: unexpected weapon cycle code, the Psi Blade may not be selectable");
    g_drawGun = (DrawGunFn)SymAddr("?DrawGun@cUIInventory@@QAEXMMW4ePlayerWeapons@@M@Z");
    Redirect("?DrawWeaponIcon@cUIInventory@@QAEXPAVtPlayerWeapon@@IMMMMM@Z", 0x200, (uint8_t *)g_drawGun, (void *)DrawGunHook, "DrawGun");
    Redirect("?DrawWeaponUI@cUIInventory@@QAEXMMMM@Z", 0x400, (uint8_t *)g_drawGun, (void *)DrawGunHook, "DrawGun");
    g_ammoText = (AmmoTextFn)SymAddr("?GetWeaponAmmoText@cUIInventory@@QAEHW4ePlayerWeapons@@PAVtPlayer@@PAD2@Z");
    Redirect("?DrawWeaponIcon@cUIInventory@@QAEXPAVtPlayerWeapon@@IMMMMM@Z", 0x200, (uint8_t *)g_ammoText, (void *)AmmoTextHook, "GetWeaponAmmoText");
    Redirect("?DrawWeaponUI@cUIInventory@@QAEXMMMM@Z", 0x400, (uint8_t *)g_ammoText, (void *)AmmoTextHook, "GetWeaponAmmoText");
    g_playMotion = (PlayMotionFn)SymAddr("?PlayMotion@tObject@@QAEHW4eMotionMode@@IMPAVtMotionSet@@MM@Z");
    Redirect("?MeleeAttack@cPlayerControl@@AAEHXZ", 0x100, (uint8_t *)g_playMotion, (void *)MeleePlayMotionHook, "PlayMotion");
    g_silentStun = (SilentStunFn)SymAddr("?SilentStun@cPlayerControl@@AAEHPAVtActor@@@Z");
    Redirect("?Activate@cPlayerControl@@QAEHXZ", 0x200, (uint8_t *)g_silentStun, (void *)SilentStunHook, "SilentStun");
    // SetPlayerWeapon: mov eax, [esp + 0xC]; test eax, eax; push esi (7 bytes) -> a trampoline
    uint8_t *sw = (uint8_t *)SymAddr("?SetPlayerWeapon@tPlayer@@QAEHW4ePlayerWeapons@@HH@Z");
    static const uint8_t swstart[] = {0x8B, 0x44, 0x24, 0x0C, 0x85, 0xC0, 0x56};
    if (sw && !memcmp(sw, swstart, 7)) {
        uint8_t *t = (uint8_t *)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        memcpy(t, sw, 7);
        t[7] = 0xE9;
        *(int32_t *)(t + 8) = (int32_t)((sw + 7) - (t + 12));
        g_setWeapon = (SetWeaponFn)t;
        uint8_t jmp[5] = {0xE9};
        *(int32_t *)(jmp + 1) = (int32_t)((uint8_t *)SetWeaponHook - (sw + 5));
        Patch(sw, swstart, 5, jmp);
    } else Log("psiblade: unexpected tPlayer::SetPlayerWeapon, selecting the blade may not stick");
    // StartState: push ebx; mov ebx, [esp + 8]; push esi; mov esi, ecx (8 bytes) -> a trampoline
    uint8_t *ss = (uint8_t *)SymAddr("?StartState@cPlayerControl@@UAEHI@Z");
    static const uint8_t ssstart[] = {0x53, 0x8B, 0x5C, 0x24, 0x08, 0x56, 0x8B, 0xF1};
    if (ss && !memcmp(ss, ssstart, 8)) {
        uint8_t *t = (uint8_t *)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        memcpy(t, ss, 8);
        t[8] = 0xE9;
        *(int32_t *)(t + 9) = (int32_t)((ss + 8) - (t + 13));
        g_startState = (StartStateFn)t;
        uint8_t jmp[5] = {0xE9};
        *(int32_t *)(jmp + 1) = (int32_t)((uint8_t *)StartStateHook - (ss + 5));
        Patch(ss, ssstart, 5, jmp);
    } else Log("psiblade: unexpected cPlayerControl::StartState");
    // the blade's gun state: GaussRifleStart / GunCrouchStart (sub esp, 0x10; push ebx; push esi), GaussRifleUpdate /
    // GunCrouchUpdate (sub esp, 0x18; push esi; mov esi, ecx)
    static const uint8_t gstart[] = {0x83, 0xEC, 0x10, 0x53, 0x56}, gupdate[] = {0x83, 0xEC, 0x18, 0x56, 0x8B, 0xF1};
    g_rifleStart = (GunStartFn)DetourAt("?GaussRifleStart@cPlayerControl@@AAEHI@Z", gstart, 5, (void *)RifleStartHook);
    g_crouchStart = (GunStartFn)DetourAt("?GunCrouchStart@cPlayerControl@@AAEHI@Z", gstart, 5, (void *)CrouchStartHook);
    g_rifleUpdate = (GunUpdateFn)DetourAt("?GaussRifleUpdate@cPlayerControl@@AAEXXZ", gupdate, 6, (void *)RifleUpdateHook);
    g_crouchUpdate = (GunUpdateFn)DetourAt("?GunCrouchUpdate@cPlayerControl@@AAEXXZ", gupdate, 6, (void *)CrouchUpdateHook);
    // the plunge's landing: cPlayerControl::Land (sub esp, 0x3C; push esi; mov esi, ecx)
    static const uint8_t lstart[] = {0x83, 0xEC, 0x3C, 0x56, 0x8B, 0xF1};
    g_land = (LandFn)DetourAt("?Land@cPlayerControl@@QAEXXZ", lstart, 6, (void *)LandHook);
    // the quit crash guard: tMotionSet::Free (push ecx; push ebp; mov ebp, ecx; dec dword ptr [ebp])
    static const uint8_t fstart[] = {0x51, 0x55, 0x8B, 0xE9, 0xFF, 0x4D, 0x00};
    g_setFree = (SetFreeFn)DetourAt("?Free@tMotionSet@@QAEHXZ", fstart, 7, (void *)MotionSetFree);
    // MeleeCallback: push esi; mov esi, ecx; mov ecx, [esp + 0xC] (7 bytes) -> a trampoline
    uint8_t *mc = (uint8_t *)SymAddr("?MeleeCallback@cPlayerControl@@AAEXPAVcKeyChannel@@W4eRenderKeyTag@@@Z");
    static const uint8_t mstart[] = {0x56, 0x8B, 0xF1, 0x8B, 0x4C, 0x24, 0x0C};
    if (mc && !memcmp(mc, mstart, 7)) {
        uint8_t *t = (uint8_t *)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        memcpy(t, mc, 7);
        t[7] = 0xE9;
        *(int32_t *)(t + 8) = (int32_t)((mc + 7) - (t + 12));
        g_meleeCallback = (MeleeCbFn)t;
        uint8_t jmp[5] = {0xE9};
        *(int32_t *)(jmp + 1) = (int32_t)((uint8_t *)MeleeCallbackHook - (mc + 5));
        Patch(mc, mstart, 5, jmp);
    } else Log("psiblade: unexpected cPlayerControl::MeleeCallback, no swing sound");
}
// Diagnosis (user: in the Vehicle Duel, after Echo was killed by a Vulture her whole HUD was gone, also after restarts):
// what the HUD passes read per player -- cUISelect::Render: her control (tPlayer +0x3F8), its type (+0x10) and flags
// (+0x70: bit 7 draws the HUD), tPlayer +0x51C bit 1; vuiMiniMap_Render: the control, type 4, flags bit 0, the dead bit
// (+0x360 0x10000000) -- logged whenever any of them changes (at most 300 lines a run).
static void HudTrace() {
    static struct { uint8_t *p, *ctl; int type; unsigned flags, b51c, dead; } last[2];
    static int lines;
    for (int side = 0; side < 2 && lines < 300; side++) {
        uint8_t *p = LocalPlayer(side);
        uint8_t *ctl = p ? *(uint8_t **)(p + 0x3f8) : nullptr;
        int type = ctl ? *(int *)(ctl + 0x10) : -1;
        unsigned flags = ctl ? ctl[0x70] : 0, b51c = p ? *(uint32_t *)(p + 0x51c) : 0,
                 dead = p ? (*(uint32_t *)(p + 0x360) & DEAD) != 0 : 0;
        auto &l = last[side];
        if (l.p == p && l.ctl == ctl && l.type == type && l.flags == flags && l.b51c == b51c && l.dead == dead) continue;
        l = {p, ctl, type, flags, b51c, dead};
        lines++;
        Log("hudtrace: player %d %p: control %p type %d flags70 %02x +51c %08x dead %u (frame %u)", side + 1, p, ctl, type,
            flags, b51c, dead, g_frame);
    }
}
// every frame: slot 1 hidden from the cycle unless its player has the blade, its name, and the blade effect shown only
// while it is the current weapon
static void WeaponFrame() {
    HudTrace();
    for (int side = 0; side < 2; side++) {
        uint8_t *p = LocalPlayer(side);
        uint8_t *w = p ? *(uint8_t **)(p + 0x464) : nullptr;
        if (!w) continue;
        if (!g_gloveName[0]) memcpy(g_gloveName, w, 31);
        if (HasBlade(p)) {
            if (strcmp((char *)w, "LBENTRY_WEAPONPSIBLADE")) strcpy((char *)w, "LBENTRY_WEAPONPSIBLADE");
        } else if (g_gloveName[0] && strcmp((char *)w, g_gloveName)) strcpy((char *)w, g_gloveName);
    }
    // per owner (user, co-op: both can have the blade):
    //   - the blade's hum (user: as the Zealot's with its blades out): a loop of Sounds\custom\ghostpc\psiblade_hum.wav
    //     (protoss.xwb wave 0xC7, a looping wave the user picked) while the blade is drawn, not while cloaked or dead;
    //   - the blade effect shown only while the blade is drawn, hidden while she is cloaked too (user; vehicles.cpp
    //     ThingCloaked: her tCloakPower working);
    //   - the attack (user: left click attacks with the blade): this build's fire button (the right trigger, pad button
    //     7; left click) only acts once a gun is drawn, so with the blade chosen a press of it starts
    //     cPlayerControl::MeleeAttack itself (walking 2, sneaking 4, crouched 5, or the blade's gun state), or the plunge
    //     in the air. Each player's own pad: her cPlayerInfo (tPlayer +0x520) +0x10 is its index in aGamepads (0x1E4
    //     each), as gameMultiplayer's loops select it (stdInput_pCurrentGamepad is only the one being updated).
    extern unsigned CustomLoopStart(const char *key, const float *at, float volume);
    extern void CustomLoopStop(unsigned handle);
    extern bool ThingCloaked(uint8_t *thing, bool player);
    static uint8_t *humWorld;
    if (LevelWorld() != humWorld) {   // (loops end with the level)
        humWorld = LevelWorld();
        for (Given &g : g_given) g.hum = 0;
    }
    static uint8_t *pads = (uint8_t *)SymAddr("?aGamepads@@3PAUtGamepad@@A");
    static unsigned *frame = (unsigned *)SymAddr("?stdInput_curUpdateFrame@@3IA");
    static auto attack = (int(__thiscall *)(uint8_t *))SymAddr("?MeleeAttack@cPlayerControl@@AAEHXZ");
    for (Given &g : g_given) {
        uint8_t *p = g.player;
        if (!p) continue;
        bool drawn = BladeSelected(p) && !ThingCloaked(p, true);
        bool want = drawn && !(*(uint32_t *)(p + 0x360) & DEAD);
        if (want && !g.hum) g.hum = CustomLoopStart("ghostpc/psiblade_hum", (const float *)(p + 0xb0), HUM_VOLUME);
        else if (!want && g.hum) CustomLoopStop(g.hum), g.hum = 0;
        if (g.fx) {
            uint32_t &flags = *(uint32_t *)(g.fx + 0x50);
            if (drawn) flags &= ~2u;
            else flags |= 2u;
        }
        PlungeFrame(g);
        if (g_frame < g.strikeUntil)
            if (uint8_t *c = *(uint8_t **)(p + 0x3f8))
                if (c[0x5e] == 0x1e) *(float *)(c + 0x1f0) = 0.f;
        uint8_t *ctl = *(uint8_t **)(p + 0x3f8), *info = *(uint8_t **)(p + 0x520);
        if (!BladeSelected(p) || !ctl || !info || !pads || !frame || !attack) continue;
        uint8_t *pad = pads + 0x1e4 * *(unsigned short *)(info + 0x10);
        if (*(unsigned *)(pad + 0x68 + 24 * 7) != *frame || *(float *)(pad + 0x64 + 24 * 7) <= 0.5f) continue;
        int st = ctl[0x5e];
        int r = (st == 2 || st == 4 || st == 5 || (g.bladeGun && st == g.bladeGunSt && !(*(uint32_t *)(ctl + 0x68) & 2))) ? attack(ctl)
                : PlungeStart(ctl) ? 2 : -1;
        if (getenv("GHOSTPC_LOG_STATES")) Log("psiblade: fire pressed in state %d -> MeleeAttack %d", st, r);
    }
    if (getenv("GHOSTPC_LOG_STATES") && g_frame == 100)   // (debug: which pad buttons the control maps, once)
        if (uint8_t *p1 = LocalPlayer(0))
            if (uint8_t *ctl = *(uint8_t **)(p1 + 0x3f8))
                Log("psiblade: control buttons: +0x1d0 %d, +0x1e4 %d (weapon), +0x1ec %d (melee)", *(int *)(ctl + 0x1d0),
                    *(int *)(ctl + 0x1e4), *(int *)(ctl + 0x1ec));
    if (getenv("GHOSTPC_LOG_STATES")) {   // (debug: the pad buttons pressed this frame, by index)
        static uint8_t **pad = (uint8_t **)SymAddr("?stdInput_pCurrentGamepad@@3PAUtGamepad@@A");
        static unsigned *frame = (unsigned *)SymAddr("?stdInput_curUpdateFrame@@3IA");
        for (int b = 0; pad && *pad && frame && b < 24; b++)
            if (*(unsigned *)(*pad + 0x68 + 24 * b) == *frame && *(float *)(*pad + 0x64 + 24 * b) > 0.5f) Log("pad: button %d pressed", b);
    }
    static int lastState = -1;   // (debug log: the bladed player's control state, with its name from aStates)
    static bool logStates = getenv("GHOSTPC_LOG_STATES") != nullptr;
    uint8_t *who = !logStates ? nullptr : g_given[0].player ? g_given[0].player : LocalPlayer(0);
    if (who)
        if (uint8_t *ctl = *(uint8_t **)(who + 0x3f8)) {
            int st = ctl[0x5e];
            if (st != lastState) {
                lastState = st;
                static uint8_t *states = (uint8_t *)SymAddr("?aStates@cPlayerControl@@1PAUcControlState@@A");
                const char *name = "?";
                for (int k = 0; states && k < 0x90; k += 4) {   // (the state's name: a pointer to text in its entry)
                    const char *c = *(const char **)(states + st * 0x90 + k);
                    if (!IsBadReadPtr(c, 8) && c[0] >= 'A' && c[0] <= 'z' && c[1] >= 'A' && c[1] <= 'z') { name = c; break; }
                }
                Log("psiblade: control state %d %.24s (weapon %d, chosen %d)", st, name, *(int *)(who + 0x454), *(int *)(who + 0x458));
            }
        }
    static int lastWeapon = -1;   // (debug log: the bladed player's weapon changes)
    uint8_t *g0 = g_given[0].player;
    if (logStates && g0 && *(int *)(g0 + 0x454) + 16 * *(int *)(g0 + 0x458) != lastWeapon) {
        lastWeapon = *(int *)(g0 + 0x454) + 16 * *(int *)(g0 + 0x458);
        Log("psiblade: current weapon %d, chosen %d", *(int *)(g0 + 0x454), *(int *)(g0 + 0x458));
    }
}

void PsiBladeUpdate() {
    static bool hooked;
    if (!hooked) hooked = true, HookPlayerBars(), HookWeapon();
    WeaponFrame();
    static unsigned *nextGuid = (unsigned *)SymAddr("?nextGUID@tThing@@2IA");
    static auto findT = (uint8_t *(__cdecl *)(unsigned))SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    if (!nextGuid || !findT) return;
    static unsigned lastNext, boss, scanned;
    static float bossAt[3];
    static bool dropped;
    static UINT loads;
    if (*nextGuid < lastNext || LevelLoadCount() != loads) {   // a new level, or a restart
        boss = scanned = 0, dropped = false;
        RevertAll();
    }
    lastNext = *nextGuid;
    loads = LevelLoadCount();
    for (int i = 0; i < 2; i++) {
        Given &g = g_given[i];
        if (!g.player) continue;
        if (*(uint32_t *)(g.player + 0x360) & DEAD) g.died = true;
        else if (g.died) {   // revived where she fell (a -level start, a co-op revive), or a restart the load count missed
            Log("psiblade: player %d died and was revived%s", i + 1, LevelPsiBladeStart() ? " (keeps the blade: psiblade_start)" : "");
            if (LevelPsiBladeStart()) g.died = false;
            else Revert(i);
        }
    }
    // psiblade_start (the Psi Blade arena): every local player has the blade from the start, given once she is alive
    // in the level (30 frames in: after the level script's start positions); a restart takes it away above and it is
    // given again
    static UINT levelFrame0, lastLoads = ~0u;
    if (LevelLoadCount() != lastLoads) lastLoads = LevelLoadCount(), levelFrame0 = g_frame;
    if (LevelPsiBladeStart() && g_frame - levelFrame0 > 30)
        for (int i = 0; i < 2; i++)
            if (uint8_t *p = LocalPlayer(i))
                if (SlotOf(p) < 0 && !(*(uint32_t *)(p + 0x360) & DEAD) && PsiBladeGive(p))
                    Log("psiblade: player %d starts with the blade (psiblade_start)", i + 1);
    static int giveAt = getenv("GHOSTPC_PSIBLADE") ? atoi(getenv("GHOSTPC_PSIBLADE")) : -1;
    static int dropAt = getenv("GHOSTPC_PSIBLADE_DROP") ? atoi(getenv("GHOSTPC_PSIBLADE_DROP")) : -1;
    if ((int)g_frame == giveAt) PsiBladeGive(LocalPlayer(0));
    static int giveAt2 = getenv("GHOSTPC_PSIBLADE2") ? atoi(getenv("GHOSTPC_PSIBLADE2")) : -1;   // (debug: player 2, co-op)
    if ((int)g_frame == giveAt2) PsiBladeGive(LocalPlayer(1));
    // debug (GHOSTPC_LOCKDOWN="<frame> <name>"): locks down every thing whose name starts with <name> (tObject::Lockdown,
    // 10 minutes), e.g. the Zoo's Scout before a test near it
    static int lockAt = -1;
    static char lockName[32];
    if (lockAt == -1) {
        lockAt = -2;
        if (const char *e = getenv("GHOSTPC_LOCKDOWN")) sscanf(e, "%d %31s", &lockAt, lockName);
    }
    // debug (GHOSTPC_KILL="<frame> <name>"): kills every live actor whose name starts with <name> (tActor::Kill, 1000
    // explosion damage), e.g. a mounted vehicle, to hear and log its death
    static int killAt = -1;
    static char killName[32];
    if (killAt == -1) {
        killAt = -2;
        if (const char *e = getenv("GHOSTPC_KILL")) sscanf(e, "%d %31s", &killAt, killName);
    }
    if ((int)g_frame == killAt) {
        typedef int(__thiscall *KillFn)(uint8_t *, uint8_t *, int, int, float, float *, float *, unsigned, int, float);
        static auto kill = (KillFn)SymAddr("?Kill@tActor@@QAEHPAVtThing@@W4eDamageSource@@W4eDamageType@@MPAUXGVECTOR3@@3IW4eHitRegionType@@M@Z");
        for (unsigned g = 1; kill && g < *nextGuid && g < 200000; g++)
            if (uint8_t *t = findT(g))
                if (!_strnicmp((char *)(t + 0xc4), killName, strlen(killName)) && !(*(uint32_t *)(t + 0x360) & DEAD))
                    Log("psiblade: debug kill %.24s -> %d", (char *)(t + 0xc4),
                        kill(t, nullptr, 3, 0, 1000.f, (float *)(t + 0xb0), (float *)(t + 0x90), 0, 0, 0.f));
    }
    if ((int)g_frame == lockAt) {
        static auto lockdown = (int(__thiscall *)(uint8_t *, unsigned))SymAddr("?Lockdown@tObject@@QAEHI@Z");
        for (unsigned g = 1; lockdown && g < *nextGuid && g < 200000; g++)
            if (uint8_t *t = findT(g))
                if (!_strnicmp((char *)(t + 0xc4), lockName, strlen(lockName)))
                    Log("psiblade: debug lockdown %.24s -> %d", (char *)(t + 0xc4), lockdown(t, 600000));
    }
    if ((int)g_frame == dropAt)
        if (uint8_t *p = LocalPlayer(0)) {
            const float *q = (const float *)(p + 0xb0), *f = (const float *)(p + 0x90);
            float at[3] = {q[0] + f[0] * 2, q[1] + f[1] * 2, q[2] + 0.3f};
            Log("psiblade: debug drop at %.1f %.1f %.1f -> %p", at[0], at[1], at[2], CreateAt("powPsiBlade", at));
        }
    {   // debug (GHOSTPC_RESTART=<frame>): restart the level then, as the pause menu's Restart ends up doing
        // (cShell::State_RestartMission -> tSequence::RestartSinglePlayer on mainSequence: its pending action +0x5C = 6)
        static int restartAt = getenv("GHOSTPC_RESTART") ? atoi(getenv("GHOSTPC_RESTART")) : -1;
        static auto restart = (void(__thiscall *)(void *))SymAddr("?RestartSinglePlayer@tSequence@@QAEXXZ");
        static void *seq = (void *)SymAddr("?mainSequence@@3VtSequence@@A");
        if ((int)g_frame == restartAt && restart && seq) {
            Log("psiblade: debug restart");
            restart(seq);
        }
    }
    {   // debug (GHOSTPC_SPAWN="<frame> <template> [metres] [right]"): creates that template in front of player 1 (default 4 m), optionally to her right
        static int spawnAt = -2;
        static char spawnName[64];
        static float spawnDist = 4.f, spawnSide = 0.f;
        if (spawnAt == -2) {
            spawnAt = -1;
            if (const char *e = getenv("GHOSTPC_SPAWN")) sscanf(e, "%d %63s %f %f", &spawnAt, spawnName, &spawnDist, &spawnSide);
        }
        if ((int)g_frame == spawnAt)
            if (uint8_t *p = LocalPlayer(0)) {
                const float *q = (const float *)(p + 0xb0), *f = (const float *)(p + 0x90);
                float at[3] = {q[0] + f[0] * spawnDist + f[1] * spawnSide, q[1] + f[1] * spawnDist - f[0] * spawnSide, q[2] + 0.1f};
                Log("psiblade: debug spawn %s at %.1f %.1f %.1f -> %p", spawnName, at[0], at[1], at[2], CreateAt(spawnName, at));
            }
    }
    // Tanagazj: found by name among the new things, then watched until he dies
    for (; !boss && scanned < *nextGuid; scanned++)
        if (uint8_t *t = findT(scanned))
            if (!_stricmp((char *)(t + 0xc4), "Tanagazj")) boss = scanned;
    static unsigned reported;
    if (boss && reported != boss)   // (his health and shield, once: the AI's cProtossShield at +0x5AC, +8 amount, +0xC max)
        if (uint8_t *t = findT(boss))
            if (uint8_t *ai = *(uint8_t **)(t + 0x3c4)) {
                reported = boss;
                static auto create = (void(__thiscall *)(uint8_t *, uint8_t *, float, float))SymAddr("?Create@cProtossShield@@QAEXPAVtActor@@MM@Z");
                if (create && *(float *)(ai + 0x5ac + 0xc) <= 0.f) {   // (his shield, if he has none: Tanagazj_shield.nod as its shell)
                    create(ai + 0x5ac, t, TANAGAZJ_SHIELD, TANAGAZJ_SHIELD);
                    *(float *)(ai + 0x5ac + 8) = TANAGAZJ_SHIELD;   // (Create starts it empty)
                }
                Log("psiblade: Tanagazj health %.0f of %.0f, shield %.0f of %.0f", *(float *)(t + 0x36c), *(float *)(t + 0x370),
                    *(float *)(ai + 0x5ac + 8), *(float *)(ai + 0x5ac + 0xc));
            }
    if (!boss || dropped) return;
    uint8_t *t = findT(boss);
    if (t && _stricmp((char *)(t + 0xc4), "Tanagazj")) t = nullptr;   // the GUID was reused
    if (t && !(*(uint32_t *)(t + 0x360) & DEAD)) {
        memcpy(bossAt, t + 0xb0, 12);
        // his shield's shell is not drawn while he is cloaked (user: cloaked, he was solid white; vehicles.cpp)
        extern bool ThingCloaked(uint8_t *thing, bool player);
        extern void ShellCloak(uint8_t *shield, uint8_t *thing, bool cloaked);
        if (uint8_t *ai = *(uint8_t **)(t + 0x3c4)) ShellCloak(ai + 0x5ac, t, ThingCloaked(t, false));
        return;
    }
    dropped = true;
    if (t)   // (his shell back on his model, if he died cloaked)
        if (uint8_t *ai = *(uint8_t **)(t + 0x3c4)) {
            extern void ShellCloak(uint8_t *shield, uint8_t *thing, bool cloaked);
            ShellCloak(ai + 0x5ac, t, false);
        }
    float at[3] = {bossAt[0], bossAt[1], bossAt[2] + 0.3f};
    Log("psiblade: Tanagazj %s at %.1f %.1f %.1f: Psi Blade dropped -> %p", t ? "died" : "is gone", bossAt[0], bossAt[1], bossAt[2],
        CreateAt("powPsiBlade", at));
}
