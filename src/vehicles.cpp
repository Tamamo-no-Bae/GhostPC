// Player vehicles (the Vulture_player, Goliath_player, ... templates; tThing::PreFinalize gives a placed player-type
// thing its control by template name, see FINDINGS: player vehicles).
//
// PreFinalize matches the thing's name (+0xC4) with strstr against lowercase names ("goliath", "arclite", "turret",
// "vulture_player", ...): the original levels place lowercase names (1_2_1a: "turret_player"), but the OpenNOD SDK
// writes Global.not's spelling ("Vulture_player"), so every vehicle in our maps got the fallback cHideControl (the
// "Hide" prompt, a cover camera, no driving). Its nine strstr calls go to a case-insensitive one.
//
// Vulture (cVultureControl): the prototype drives it with buttons, not the stick. Its button slots (stdInput: 24-byte
// entries from tGamepad +0x64; slots 0..7 = White, X, Y, Black, A, B, LT, RT, filled by XReadGamepad) are brake X
// (+0x1B8 = 1: RidingControlsUpdate writes the NSI vehicle's throttle [+0x224] +0x20 = the X value, positive, which
// holds the bike), accelerate A (+0x1B4 = 4: minus the A value, which drives it forward), dismount B (+0x1BC = 5), gun
// RT (+0x1C8 = 7), camera White (+0x1D0 = 0), look back Black (+0x1CC = 3); the left stick only steers (tGamepad +0x54;
// its forward axis is not read). While a local player rides a Vulture (RidingVulture), input.cpp turns the left
// stick's forward / back (W / S) into A / X.
#include "common.h"
#include <math.h>
#include <stdlib.h>
#include <ctype.h>
#include <intrin.h>
#include <vector>
#include <map>
#include <algorithm>

extern UINT g_frame;
void LevelWarpPlayer(uint8_t *player, const float *p);   // levelscript.cpp
uint8_t *LevelWorld();                                    // levelscript.cpp: the loaded tWorld
unsigned CustomLoopStart(const char *key, const float *at, float volume);   // customsound.cpp
void CustomLoopStop(unsigned handle);
extern bool g_ghostpcStep;   // customsound.cpp: set while GhostPC plays a Dragoon's own footstep

namespace {

char *__cdecl StrStrI(const char *hay, const char *needle) {
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) h++, n++;
        if (!*n) return (char *)hay;
    }
    return *needle ? nullptr : (char *)hay;
}
// PreFinalize's name checks: case-insensitive, and GhostPC's vehicles (ghostpc.not) get their base's control:
// reaver_vehicle_player counts as "arclite" (the Siege Tank's cTankControl, which vehicles.cpp turns into the Reaver's
// controls), dragoon_vehicle_player as "goliath" (cGoliathControl, the Dragoon's)
char *__cdecl ControlNameMatch(const char *hay, const char *needle) {
    if (!_stricmp(needle, "arclite") && StrStrI(hay, "reaver_vehicle_player")) return (char *)hay;
    if (!_stricmp(needle, "goliath") && StrStrI(hay, "dragoon_vehicle_player")) return (char *)hay;
    return StrStrI(hay, needle);
}

// debug (GHOSTPC_LOG_VEHICLE): once a second while a Vulture's control is updated, what it sees: its state (+0x5E:
// 1 debug, 2 riding, 3 dead), its start delay (+0x16C against stdTime +0x44), the current gamepad, the throttle /
// reverse / gun slots and the left stick
float Pos(uint8_t *ctl, int k) { uint8_t *t = *(uint8_t **)(ctl + 0x180); return t ? ((float *)(t + 0xb0))[k] : 0.f; }
float Vel(uint8_t *ctl, int k) {
    static auto getVel = (void(__thiscall *)(void *, float *))SymAddr("?GetVelocity@tThing@@QAEXPAUXGVECTOR3@@@Z");
    uint8_t *t = *(uint8_t **)(ctl + 0x180);
    float v[3] = {};
    if (t && getVel) getVel(t, v);
    return v[k];
}
typedef void(__thiscall *UpdateFn)(void *);
UpdateFn g_vultureUpdate;
void __fastcall VultureUpdateLog(uint8_t *ctl, void *) {
    static uint8_t **pad = (uint8_t **)SymAddr("?stdInput_pCurrentGamepad@@3PAUtGamepad@@A");
    static uint8_t *time = (uint8_t *)SymAddr("?stdTime@@3VtStdTime@@A");
    static DWORD last;
    if (GetTickCount() - last > 1000) {
        last = GetTickCount();
        uint8_t *p = pad ? *pad : nullptr;
        auto slot = [&](int off) { return p ? *(float *)(p + 0x64 + 24 * *(int *)(ctl + off)) : -1.f; };
        uint8_t *nsi = *(uint8_t **)(ctl + 0x224);
        Log("vehicle: vulture control %p state %u delay %u now %u pad %p throttle %.2f reverse %.2f gun %.2f stick %.2f %.2f"
            " -> nsi %p steer %.2f throttle %.2f speed %.2f pos %.2f %.2f %.2f vel %.2f %.2f %.2f",
            ctl, ctl[0x5e], *(uint32_t *)(ctl + 0x16c), time ? *(uint32_t *)(time + 0x44) : 0, p, slot(0x1b8), slot(0x1b4),
            slot(0x1c8), p ? *(float *)(p + 0x54) : 0.f, p ? *(float *)(p + 0x58) : 0.f, nsi, nsi ? *(float *)(nsi + 0x1c) : 0.f,
            nsi ? *(float *)(nsi + 0x20) : 0.f, *(float *)(ctl + 0x1f8), Pos(ctl, 0), Pos(ctl, 1), Pos(ctl, 2),
            Vel(ctl, 0), Vel(ctl, 1), Vel(ctl, 2));
    }
    g_vultureUpdate(ctl);
}

// debug (GHOSTPC_MOUNT=<frame>): at that frame (after the level load), mount the active player on the nearest placed
// Vulture (or the nearest vehicle whose template name contains GHOSTPC_MOUNT_NAME) (tPlayer::MountVehicle, as the action button does) - scripted presses of B mount, hide or punch depending on
// where she stands. With GHOSTPC_LOG_VEHICLE, the active player and its control are logged once a second.
void DebugMount() {
    static int at = getenv("GHOSTPC_MOUNT") ? atoi(getenv("GHOSTPC_MOUNT")) : -1;
    static bool log = getenv("GHOSTPC_LOG_VEHICLE") != nullptr;
    static auto active = (uint8_t *(__cdecl *)())SymAddr("?gameMultiplayer_GetActivePlayer@@YAPAVtPlayer@@XZ");
    static uint32_t vulture = SymAddr("??_7cVultureControl@@6B@");
    static uint8_t *names = (uint8_t *)SymAddr("?gameMotion_apNames@@3PAPADA");
    if (!names || !*(void **)(names + 0x720)) return;   // no current player info yet (startup, menus)
    if (getenv("GHOSTPC_DROP") && at >= 0 && (int)g_frame > at && (g_frame - at) % 60 == 0) {
        static auto findT = (uint8_t *(__cdecl *)(unsigned))SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
        unsigned *next = (unsigned *)SymAddr("?nextGUID@tThing@@2IA");
        for (unsigned g = 1; next && g < *next && g < 100000; g++)
            if (uint8_t *t = findT(g))
                if (*(int *)(t + 0x10) == 7 && strstr((char *)(t + 0xc4), "Goliath"))
                    Log("vehicle:   Goliath at %.2f %.2f %.2f", ((float *)(t + 0xb0))[0], ((float *)(t + 0xb0))[1], ((float *)(t + 0xb0))[2]);
    }
    if (log && g_frame % 30 == 0 && active) {
        uint8_t *p = active();
        uint8_t *ctl = p ? *(uint8_t **)(p + 0x3f8) : nullptr;
        Log("vehicle: frame %u active player %p control %p vtable %08x (vulture %08x) pos %.1f %.1f %.1f", g_frame, p, ctl,
            ctl ? *(uint32_t *)ctl : 0, vulture, p ? ((float *)(p + 0xb0))[0] : 0.f, p ? ((float *)(p + 0xb0))[1] : 0.f,
            p ? ((float *)(p + 0xb0))[2] : 0.f);
        static auto getCam = (uint8_t *(__thiscall *)(uint8_t *))SymAddr("?GetCamera@cControl@@QAEPAVtSimCamera@@XZ");
        if (uint8_t *cam = ctl && getCam ? getCam(ctl) : nullptr) {   // (the camera's eye and look points)
            const float *e = (const float *)(cam + 0x240), *l = (const float *)(cam + 0x220);
            Log("vehicle:   camera eye %.2f %.2f %.2f, +0x220 %.2f %.2f %.2f, control type %d camera %d", e[0], e[1], e[2], l[0], l[1], l[2],
                *(int *)(ctl + 0x10), *(int *)(ctl + 0x1d0));
        }
    }
    if (at < 0 || (int)g_frame != at || !active) return;
    static auto find = (uint8_t *(__cdecl *)(unsigned))SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    static auto mount = (int(__thiscall *)(void *, void *))SymAddr("?MountVehicle@tPlayer@@QAEHPAV1@@Z");
    unsigned *next = (unsigned *)SymAddr("?nextGUID@tThing@@2IA");
    uint8_t *nova = active(), *best = nullptr;
    float bd = 1e30f;
    for (unsigned g = 1; nova && find && next && g < *next && g < 100000; g++) {
        uint8_t *t = find(g);
        if (!t || t == nova || *(int *)(t + 0x10) != 7) continue;
        uint8_t *ctl = *(uint8_t **)(t + 0x3f8);
        char sym[96] = "";
        if (ctl) SymName(*(uint32_t *)ctl, sym, sizeof sym);
        Log("vehicle:   guid %u thing %p '%.32s' control %p %s pos %.2f %.2f %.2f", g, t, (char *)(t + 0xc4), ctl, sym,
            ((float *)(t + 0xb0))[0], ((float *)(t + 0xb0))[1], ((float *)(t + 0xb0))[2]);
        // debug (GHOSTPC_DROP=<metres>): move the Goliath down by that much, to see where it settles
        static float drop = getenv("GHOSTPC_DROP") ? (float)atof(getenv("GHOSTPC_DROP")) : 0.f;
        if (drop != 0.f && strstr((char *)(t + 0xc4), "Goliath")) {
            float p[4] = {((float *)(t + 0xb0))[0], ((float *)(t + 0xb0))[1], ((float *)(t + 0xb0))[2] - drop, 0};
            LevelWarpPlayer(t, p);
            Log("vehicle:   Goliath moved down %.2f to %.2f", drop, p[2]);
        }
        static const char *want = getenv("GHOSTPC_MOUNT_NAME");   // e.g. Arclite, Goliath (default: the Vulture)
        if (!ctl || (want ? !StrStrI((char *)(t + 0xc4), want) : *(uint32_t *)ctl != vulture)) continue;
        float *a = (float *)(t + 0xb0), *b = (float *)(nova + 0xb0);
        float d = (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]);
        if (d < bd) bd = d, best = t;
    }
    Log("vehicle: GHOSTPC_MOUNT frame %u: player %p vulture %p (%.1f m, next GUID %u) -> %d", g_frame, nova, best, sqrtf(bd),
        next ? *next : 0,
        best && mount && !getenv("GHOSTPC_NOMOUNT") ? mount(nova, best) : -1);
}

}   // namespace

// Goliath chaingun sound: StandControlsUpdate plays foley mode 5 ("swing", the gun's loud firing burst) when RT goes
// down, keeps its handle (+0x1CC) and plays it again only after RT is released (it stops it then). The cue is a
// one-shot (playerGoliath.xsb sound 41 = global.xwb wave 0xEF, ADPCM 24 kHz, 12492 bytes = 0.94 s, no loop flag: about
// 270 ms of full-volume rapid fire, then a tail that fades out by 0.65 s), so a held trigger went quiet after the first
// burst and only the bullets' sound was left. The Goliath bosses play that same wave with every shot of each gun (from
// their fire code, logged in the Zoo: wave 0xEF restarted about every 50 ms). The player's chainguns fire every 50 ms
// (FireChaingun: +0x1D4 = now + 50) but only play it when the handle is free, so the handle is cleared one frame after
// each play: every shot plays it again, the earlier plays ringing on under it. The mixer keeps an instance until Stop
// (cGameMixer::FindInstance still finds it seconds later), so each one is stopped once it has played out.
void GoliathGunSound() {
    static uint8_t *names = (uint8_t *)SymAddr("?gameMotion_apNames@@3PAPADA");
    static uint32_t goliath = SymAddr("??_7cGoliathControl@@6B@");
    static uint8_t **mixer = (uint8_t **)SymAddr("?gameMixer_pMixer@@3PAVcGameMixer@@A");
    typedef void(__thiscall *StopFn)(uint8_t *, unsigned, unsigned);
    static auto stop = (StopFn)SymAddr("?Stop@cGameMixer@@QAEXII@Z");
    static unsigned seen[2];
    static DWORD since[2];
    static std::vector<std::pair<unsigned, DWORD>> playing;   // handles let ring out, and when they started
    const DWORD LENGTH_MS = 950;
    if (!names || !goliath || !mixer || !*mixer || !stop || !*(uint8_t **)(names + 0x720)) return;
    DWORD now = GetTickCount();
    for (size_t k = 0; k < playing.size();)
        if (now - playing[k].second >= LENGTH_MS) stop(*mixer, playing[k].first, 0), playing.erase(playing.begin() + k);
        else k++;
    for (int local = 0; local < 2; local++) {
        uint8_t **info = *(uint8_t ***)(names + (local ? 0x72c : 0x728));
        uint8_t *player = info ? info[0] : nullptr;
        uint8_t *ctl = player ? *(uint8_t **)(player + 0x3f8) : nullptr;
        if (!ctl || *(uint32_t *)ctl != goliath || !_stricmp((char *)(player + 0xc4), "dragoon_vehicle_player")) {
            seen[local] = 0;
            continue;
        }
        unsigned &h = *(unsigned *)(ctl + 0x1cc);
        if (!h || (h & 0xFFF00000) == 0xC5700000) { seen[local] = 0; continue; }   // none, or a GhostPC custom loop
        if (h != seen[local]) { seen[local] = h; since[local] = now; continue; }
        playing.emplace_back(h, since[local]);   // (if RT is released now, the game stops only the newest play)
        h = 0;
        seen[local] = 0;
    }
}

// Remote units (tPlayerItemSpiderMine::Fire creates the unit and tPlayer::MountVehicle hands it the player):
// - Spider Mine (cSpiderMineControl): walks, RT detonates, B burrows ("landmine mode": ExitSpiderMine creates a
//   "spidermine" AI mine where it stood). Quitting it leaves a pickup, but a burrowed mine could never be taken back:
//   Nova now picks up a mine she burrowed by walking over it, as she does a quit one (MinePickup).
// - Firefly (cFireflyControl): X = DoDistraction (sound, "ffDistract" effect, aiAwareness_Critical: AI within 30 m
//   come to look), also on dismount. Its DoSilentKill (kill the closest live actor within 3 m, credited to the
//   Firefly, plus a blast push) is complete but nothing calls it: restored on RT (FireflyUpdateInput), not while
//   Nova's own body is within reach.
// - Mantis (cMantisControl): RT on the ground = hover (EnterHoverMode: narrower field of view, chase camera); while
//   hovering RT rises, LT sinks, the left stick moves, B exits; it lands when the ground is close. A spy camera: it
//   has no attack in the code.
static const int ITEM_SPIDERMINE = 4;   // ePlayerItems = the mission INVENTORY slots (canister 0, smokegrenade 1,
                                        // stickygrenade 2, hellfire 3, spidermine 4, firefly 5, mantis 6)
static uint8_t *LocalPlayer(int local, uint8_t **ctlOut = nullptr) {
    static uint8_t *names = (uint8_t *)SymAddr("?gameMotion_apNames@@3PAPADA");
    if (ctlOut) *ctlOut = nullptr;
    if (!names || !*(uint8_t **)(names + 0x720)) return nullptr;
    uint8_t **info = *(uint8_t ***)(names + (local ? 0x72c : 0x728));
    uint8_t *player = info ? info[0] : nullptr;
    if (player && ctlOut) *ctlOut = *(uint8_t **)(player + 0x3f8);
    return player;
}
static float ThingDist(const uint8_t *a, const uint8_t *b) {
    const float *p = (const float *)(a + 0xb0), *q = (const float *)(b + 0xb0);
    float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

typedef int(__thiscall *UpdateInputFn)(uint8_t *, int);
static UpdateInputFn g_fireflyInput;
static int __fastcall FireflyUpdateInput(uint8_t *ctl, void *, int a) {
    int r = g_fireflyInput(ctl, a);
    static uint8_t **pad = (uint8_t **)SymAddr("?stdInput_pCurrentGamepad@@3PAUtGamepad@@A");
    static unsigned *frame = (unsigned *)SymAddr("?stdInput_curUpdateFrame@@3IA");
    static auto kill = (void(__thiscall *)(uint8_t *))SymAddr("?DoSilentKill@cFireflyControl@@AAEXXZ");
    static DWORD next;
    if (r || !pad || !*pad || !frame || !kill) return r;
    if (*(unsigned *)(*pad + 0x68 + 24 * 7) != *frame) return r;   // RT pressed this frame
    uint8_t *fly = *(uint8_t **)(ctl + 0x180), *rider = *(uint8_t **)(ctl + 0x1a0);
    if (!fly || GetTickCount() < next) return r;
    if (rider && ThingDist(fly, rider) < 3.5f) return r;   // Nova's own body would be the closest actor
    next = GetTickCount() + 1000;
    kill(ctl);
    if (getenv("GHOSTPC_LOG_VEHICLE")) Log("vehicle: Firefly silent kill");
    return r;
}

// burrowed mines Nova may pick up again: GUID, and when it was seen first
struct OwnedMine { unsigned guid; DWORD since; };
static std::vector<OwnedMine> g_mines;
static void MinePickup() {
    static uint32_t mineVt = SymAddr("??_7cSpiderMineControl@@6B@");
    static uint32_t novaVt = SymAddr("??_7cPlayerControl@@6B@");
    static auto findT = (uint8_t *(__cdecl *)(unsigned))SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    static unsigned *nextGuid = (unsigned *)SymAddr("?nextGUID@tThing@@2IA");
    static auto getAmount = (unsigned(__thiscall *)(uint8_t *, int))SymAddr("?GetPlayerItemAmount@tPlayer@@QAEIW4ePlayerItems@@@Z");
    static auto setAmount = (void(__thiscall *)(uint8_t *, int, unsigned))SymAddr("?SetPlayerItemAmount@tPlayer@@QAEXW4ePlayerItems@@I@Z");
    static auto removeT = (void(__thiscall *)(uint8_t *, int))SymAddr("?Remove@tThing@@QAEXH@Z");
    static float last[3];
    static unsigned scanFrom, scanUntil;   // after a ride ends: GUIDs made since it began, for a few frames
    static bool riding;
    static uint8_t *lastWorld;
    if (!findT || !nextGuid || !getAmount || !setAmount || !removeT) return;
    if (LevelWorld() != lastWorld) { lastWorld = LevelWorld(); g_mines.clear(); riding = false; scanUntil = 0; }
    uint8_t *ctl;
    uint8_t *player = LocalPlayer(0, &ctl);
    if (!player || !ctl) return;
    uint32_t vt = *(uint32_t *)ctl;
    if (vt == mineVt) {   // riding a Spider Mine (not a Firefly / Mantis): remember where it is
        if (!riding) scanFrom = *nextGuid;
        riding = true;
        memcpy(last, player + 0xb0, sizeof last);
        return;
    }
    if (riding) { riding = false; scanUntil = g_frame + 15; }
    if (scanUntil && g_frame <= scanUntil) {   // the burrowed mine: a new "spidermine" where the mine was
        for (unsigned g = scanFrom; g < *nextGuid && g < scanFrom + 5000; g++) {
            uint8_t *t = findT(g);
            if (!t || _stricmp((char *)(t + 0xc4), "spidermine")) continue;
            const float *p = (const float *)(t + 0xb0);
            float dx = p[0] - last[0], dy = p[1] - last[1];
            bool known = false;
            for (auto &m : g_mines) known |= m.guid == g;
            if (!known && dx * dx + dy * dy < 4.f) {
                g_mines.push_back({g, GetTickCount()});
                Log("vehicle: burrowed spider mine %u at %.1f %.1f %.1f (Nova can pick it up)", g, p[0], p[1], p[2]);
            }
        }
    }
    if (vt != novaVt) return;
    for (size_t k = 0; k < g_mines.size();) {
        uint8_t *t = findT(g_mines[k].guid);
        if (!t || _stricmp((char *)(t + 0xc4), "spidermine")) { g_mines.erase(g_mines.begin() + k); continue; }
        const float *p = (const float *)(t + 0xb0), *q = (const float *)(player + 0xb0);
        float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
        if (GetTickCount() - g_mines[k].since > 2000 && dx * dx + dy * dy < 1.2f * 1.2f && fabsf(dz) < 1.5f) {
            unsigned n = getAmount(player, ITEM_SPIDERMINE);
            setAmount(player, ITEM_SPIDERMINE, n + 1);
            removeT(t, 0);
            Log("vehicle: Nova picked up burrowed spider mine %u (%u -> %u)", g_mines[k].guid, n, getAmount(player, ITEM_SPIDERMINE));
            g_mines.erase(g_mines.begin() + k);
            continue;
        }
        k++;
    }
}

// debug (GHOSTPC_ITEM="<frame> <item>"): give Nova 4 of an item (4 spider mine, 5 firefly, 6 mantis), select it and
// use it, to launch a remote unit without the pickups
static void DebugItem() {
    static int at = -1, item;
    static bool init;
    if (!init) {
        init = true;
        if (const char *e = getenv("GHOSTPC_ITEM")) sscanf(e, "%d %d", &at, &item);
    }
    if (at < 0 || (int)g_frame != at) return;
    static auto setAmount = (void(__thiscall *)(uint8_t *, int, unsigned))SymAddr("?SetPlayerItemAmount@tPlayer@@QAEXW4ePlayerItems@@I@Z");
    static auto select = (int(__thiscall *)(uint8_t *, int))SymAddr("?SetPlayerItem@tPlayer@@QAEHW4ePlayerItems@@@Z");
    // the launch itself (ActivateCurrentPlayerItem needs Nova's throw): tPlayerItemSpiderMine::Fire, which the
    // Firefly and Mantis items use too, at Nova's matrix (tThing +0x80); the items are tPlayer +0x4C4[item]
    static auto fire = (int(__thiscall *)(uint8_t *, void *))SymAddr("?Fire@tPlayerItemSpiderMine@@UAEHPAUXGMATRIX@@@Z");
    uint8_t *player = LocalPlayer(0);
    if (!player || !setAmount || !select || !fire || item < 0 || item >= 10) return;
    setAmount(player, item, 4);
    int s = select(player, item);
    if (item <= 3) {   // a grenade: thrown the way the grenade button does (tPlayerItemGrenade::Activate -> FireGrenade)
        static auto activate = (int(__thiscall *)(uint8_t *))SymAddr("?ActivateCurrentPlayerItem@tPlayer@@QAEHXZ");
        Log("vehicle: debug item %d: select %d, thrown %d", item, s, activate ? activate(player) : -1);
        return;
    }
    uint8_t *it = *(uint8_t **)(player + 0x4c4 + 4 * item);
    int r = it ? fire(it, player + 0x80) : -1;
    Log("vehicle: debug item %d: select %d, item %p, fire %d", item, s, it, r);
}

// The Reaver (GhostPC's reaver_vehicle_player, game/Misc/Templates/ghostpc.not: the Siege Tank's player template with
// the Reaver's model and motion set). ControlNameMatch gives it cTankControl; cTankControl::UpdateControls
// (vtable slot 10) is wrapped and, for a Reaver only:
// - X (siege mode) and RT (the tank's shells) are hidden from the tank's code (their stdInput slots cleared around
//   the call), so it only drives;
// - its ground speed is held to REAVER_SPEED: its walk (Reaver_walk.nad, 39 frames, an in-place crawl whose body
//   segments slide about 1.5-1.8 m per cycle) suggests about 1.3 m/s;
// - its motions follow what it does: walk (1) when moving, turnleft / turnright (132 / 133) when turning in place,
//   stand (3) otherwise, recoilstand (48, the attack) when it fires;
// - RT launches a Scarab (proj_reaverScarab: proj_scarab's photon glow, slowed down, homing, 1200 damage, a fading
//   trail) at the nearest live AI unit ahead within 40 m, one every REAVER_RELOAD ms (3 s).
static const float REAVER_SPEED = 1.1f;   // its walk at 0.75 scale (segments slide ~1.5 model units per cycle), sped up
static const float REAVER_WALK_FPS = 36.f;   // its walk animation's rate (30 = as made; user: a little too slow)
static const DWORD REAVER_RELOAD = 3000;
static const float REAVER_TURN = 0.6f;   // rad/s (about 35 deg/s) at full stick
static UpdateFn g_tankUpdate;
static unsigned g_reaverIdle;   // the driven Reaver's idle loop (customsound.cpp handle), 0 when none
static float g_reaverThrottle;  // the driven Reaver's stick, forward + / back - (ReaverCollide)
static const float REAVER_SCALE = 0.75f;   // its MODELSCALE (ghostpc.not): hull, mount zone, entry points and camera
static bool IsReaver(uint8_t *thing) { return thing && !_stricmp((char *)(thing + 0xc4), "reaver_vehicle_player"); }
static void PlayMotionMode(uint8_t *thing, int mode, bool restart = false, float fps = 30.f) {
    static auto getMode = (int(__thiscall *)(uint8_t *))SymAddr("?GetMoveMode@tObject@@QAE?AW4eMotionMode@@XZ");
    static auto play = (int(__thiscall *)(uint8_t *, int, unsigned, float, void *, float, float))SymAddr(
        "?PlayMotion@tObject@@QAEHW4eMotionMode@@IMPAVtMotionSet@@MM@Z");
    if (!getMode || !play) return;
    int cur = getMode(thing);
    if (!restart && cur == mode) return;
    int r = play(thing, mode, 1, fps, nullptr, 7.5f, -1.f);   // (third: frames per second)
    if (getenv("GHOSTPC_LOG_VEHICLE")) Log("vehicle: Reaver motion %d (was %d) -> %d, now %d", mode, cur, r, getMode(thing));
}
// the live AI unit the Reaver is aimed at: within range and a 60 degree cone ahead, the one closest to straight ahead
// (nearer ones winning near-ties), or none
static uint8_t *ReaverTarget(uint8_t *reaver, float range) {
    static auto findT = (uint8_t *(__cdecl *)(unsigned))SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    static unsigned *nextGuid = (unsigned *)SymAddr("?nextGUID@tThing@@2IA");
    static auto getAI = (void *(__thiscall *)(uint8_t *))SymAddr("?GetAI@tActor@@QAEPAVcAIInstance@@XZ");
    if (!findT || !nextGuid || !getAI) return nullptr;
    const float *p = (const float *)(reaver + 0xb0), *fwd = (const float *)(reaver + 0x90);
    uint8_t *best = nullptr;
    float bestScore = -1e9f;
    for (unsigned g = 1; g < *nextGuid && g < 200000; g++) {
        uint8_t *t = findT(g);
        if (!t || t == reaver || *(int *)(t + 0x10) != 0) continue;   // actors (type 0)
        if (*(uint32_t *)(t + 0x360) & 0x10000000) continue;         // dead (set when killed; 0x20000 is not:
                                                                       // live zerglings and hydralisks carry it)
        if (!getAI(t)) continue;
        const float *q = (const float *)(t + 0xb0);
        float dx = q[0] - p[0], dy = q[1] - p[1], dz = q[2] - p[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        float flat = sqrtf(dx * dx + dy * dy);
        if (d < 0.5f || d > range || flat < 0.1f) continue;
        float c = (dx * fwd[0] + dy * fwd[1]) / flat;   // cosine of its angle off the Reaver's heading
        if (c < 0.5f) continue;
        float score = c - d / 400.f;
        if (score > bestScore) best = t, bestScore = score;
    }
    return best;
}
// Scarabs in flight and their targets (GUIDs). The game's homing (PROJECT_FLAGS 0x100, target at +0x37C) turns too
// slowly for a 12 m/s Scarab: it overshot moving units and circled them. SteerScarabs points each one straight at its
// target's body (0.5 m above its feet) every frame, so it hits it as SC1's Scarabs do; one whose target died flies on.
struct Scarab { unsigned guid, target; };
static std::vector<Scarab> g_scarabs;
static const float SCARAB_SPEED = 12.f;
static void SteerScarabs() {
    static auto findT = (uint8_t *(__cdecl *)(unsigned))SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    static auto setVel = (void(__thiscall *)(uint8_t *, const float *))SymAddr("?SetVelocity@tThing@@QAEXPBUXGVECTOR3@@@Z");
    static auto lookDir = (void(__thiscall *)(uint8_t *, const float *))SymAddr("?LookDir3d@tThing@@QAEXPAUXGVECTOR3@@@Z");
    static uint8_t *world;
    if (LevelWorld() != world) world = LevelWorld(), g_scarabs.clear();
    if (!findT || !setVel) return;
    for (size_t k = 0; k < g_scarabs.size();) {
        uint8_t *proj = findT(g_scarabs[k].guid);
        if (!proj) { g_scarabs.erase(g_scarabs.begin() + k); continue; }
        uint8_t *t = g_scarabs[k].target ? findT(g_scarabs[k].target) : nullptr;
        k++;
        if (!t || (*(uint32_t *)(t + 0x360) & 0x10000000)) continue;   // no target, or dead
        const float *p = (const float *)(proj + 0xb0), *q = (const float *)(t + 0xb0);
        float d[3] = {q[0] - p[0], q[1] - p[1], q[2] + 0.5f - p[2]};   // low: zerglings are under a metre tall
        float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len < 0.01f) continue;
        float v[3] = {d[0] / len * SCARAB_SPEED, d[1] / len * SCARAB_SPEED, d[2] / len * SCARAB_SPEED};
        setVel(proj, v);
        if (lookDir) lookDir(proj, d);
    }
}
static unsigned g_scarabGuid, g_scarabFrame;   // debug: the last Scarab, followed in the log
static void FollowScarab() {
    static auto findT = (uint8_t *(__cdecl *)(unsigned))SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    if (!g_scarabGuid || !findT || (g_frame - g_scarabFrame) % 10) return;
    uint8_t *t = findT(g_scarabGuid);
    if (!t) { Log("vehicle: Scarab %u gone after %u frames", g_scarabGuid, g_frame - g_scarabFrame); g_scarabGuid = 0; return; }
    const float *q = (const float *)(t + 0xb0);
    Log("vehicle: Scarab at %.1f %.1f %.1f", q[0], q[1], q[2]);
}
static void DragoonFired(uint8_t *thing);
static void FireScarab(uint8_t *reaver) {
    static auto find = (uint8_t *(__cdecl *)(const char *))SymAddr("?gameTemplate_Find@@YAPAVtTemplate@@PBD@Z");
    static auto lookAt = (void(__cdecl *)(float *, const float *, const float *, float))SymAddr(
        "?MatrixLookAt@@YAXPAUXGMATRIX@@PBUXGVECTOR3@@1M@Z");
    static auto fireT = (uint8_t *(__cdecl *)(uint8_t *, float *, uint8_t *, unsigned, int, int, void *, int))SymAddr(
        "?simCombat_FireTemplateProjectile@@YAPAVtProjectile@@PAVtTemplate@@PAUXGMATRIX@@PAVtThing@@IHHPAVSimpleHit@@W4eHitRegionType@@@Z");
    static auto start = (void(__thiscall *)(uint8_t *))SymAddr("?StartWithAccelerate@tProjectile@@QAEXXZ");
    uint8_t *tmpl = find ? find("proj_reaverScarab") : nullptr;
    if (!tmpl || !lookAt || !fireT || !start) { Log("vehicle: Reaver: no proj_reaverScarab (ghostpc.not)"); return; }
    const float *p = (const float *)(reaver + 0xb0), *fwd = (const float *)(reaver + 0x90);
    // from its head (the model reaches 8.8 m ahead of its origin at native size), about a metre up
    float ahead = 8.8f * REAVER_SCALE + 0.5f;
    float from[3] = {p[0] + fwd[0] * ahead, p[1] + fwd[1] * ahead, p[2] + 1.f};
    uint8_t *target = ReaverTarget(reaver, 40.f);
    float to[3] = {from[0] + fwd[0] * 10.f, from[1] + fwd[1] * 10.f, from[2]};
    if (target) memcpy(to, target + 0xb0, sizeof to), to[2] = from[2];
    float m[16] = {};
    lookAt(m, from, to, 0.f);
    m[12] = from[0], m[13] = from[1], m[14] = from[2], m[15] = 1.f;
    uint8_t *proj = fireT(tmpl, m, reaver, *(unsigned *)(reaver + 0x34), 0, 0, nullptr, 1);
    if (!proj) return;
    if (target && (*(uint32_t *)(proj + 0x360) & 0x100)) *(unsigned *)(proj + 0x37c) = *(unsigned *)(target + 0x34);
    start(proj);
    g_scarabs.push_back({*(unsigned *)(proj + 0x34), target ? *(unsigned *)(target + 0x34) : 0});
    DragoonFired(reaver);   // (its shield comes on)
    if (getenv("GHOSTPC_LOG_VEHICLE")) {
        Log("vehicle: Reaver fires a Scarab at %s (%p) from %.1f %.1f %.1f to %.1f %.1f %.1f, flags %08x, target body %p flags %08x",
            target ? (char *)(target + 0xc4) : "nothing", target, from[0], from[1], from[2], to[0], to[1], to[2],
            *(uint32_t *)(proj + 0x360), target ? *(void **)(target + 0x158) : nullptr, target ? *(uint32_t *)(target + 0x360) : 0);
        g_scarabGuid = *(unsigned *)(proj + 0x34), g_scarabFrame = g_frame;
    }
}
// The tank's chase camera presets (avTankCameraPosOffset / avTankLookPosOffset, one per camera, White cycles them) suit
// a 12 m tank. tSimCamera::BeginChase copies a preset into the camera (target offsets +0x1B8 position, +0x1D0 look)
// when the tank's state starts or its camera changes; while a Reaver is driven, a tank preset
// found there is swapped for the Reaver's (the camera eases to it).
static void ReaverPreset(int k, float pos[3], float look[3]);
static void ReaverCamera(uint8_t *ctl) {
    static auto getCam = (uint8_t *(__thiscall *)(uint8_t *))SymAddr("?GetCamera@cControl@@QAEPAVtSimCamera@@XZ");
    static auto setPos = (void(__thiscall *)(uint8_t *, const float *, int))SymAddr("?SetChaseCameraPosOffset@tSimCamera@@QAEXPAUXGVECTOR3@@H@Z");
    static auto setLook = (void(__thiscall *)(uint8_t *, const float *, int))SymAddr("?SetChaseLookPosOffset@tSimCamera@@QAEXPAUXGVECTOR3@@H@Z");
    static const float *tankPos = (const float *)SymAddr("?avTankCameraPosOffset@@3PAUXGVECTOR3@@A");
    float rPos[3][3], rLook[3][3];
    for (int k = 0; k < 3; k++) ReaverPreset(k, rPos[k], rLook[k]);
    uint8_t *cam = getCam && setPos && setLook && tankPos ? getCam(ctl) : nullptr;
    if (!cam) return;
    const float *target = (const float *)(cam + 0x1b8);
    for (int k = 0; k < 3; k++)
        if (!memcmp(target, tankPos + 3 * k, 12)) {
            setPos(cam, rPos[k], 1);
            setLook(cam, rLook[k], 1);
            return;
        }
}
// camera preset k (0 chase, 1 wider chase, 2 at its head) at the Reaver's scale. At native size; under the field's
// 12 m walls (times 0.75: 8.6 / 9.8 m) but above the Reaver's back (8.5 m), looking ahead, so the aim path and its ring
// in front of it can be seen.
static void ReaverPreset(int k, float pos[3], float look[3]) {
    static const float nPos[3][3] = {{0, -18, 11.5f}, {0, -24, 13}, {0, 7, 7.5f}};
    static const float nLook[3][3] = {{0, 12, 2}, {0, 12, 2.5f}, {0, 18, 6.5f}};
    for (int i = 0; i < 3; i++) pos[i] = nPos[k][i] * REAVER_SCALE, look[i] = nLook[k][i] * REAVER_SCALE;
}
// cTankControl::UIRenderCallback (vtable slot 26) draws the shell's trajectory arc (vuiPlayer_pGrenadeArc,
// cGrenadeArc::Set: a matrix, a velocity in its frame - the tank's (0, 50, 0) - and display settings; Render steps it
// until it hits something and draws rings along it and a circle where it lands) from the model's type-2 marker. The
// Reaver's marker points down, so its arc went under the map. For the Reaver the arc is the Scarab's path instead: from
// its head (where Scarabs launch) straight at the unit the next one goes for (0.5 m above its feet, as SteerScarabs
// flies), or with nothing in reach at the ground 20 m ahead; the tank's display settings.
static UpdateFn g_tankUI;
static void __fastcall TankUIRender(uint8_t *ctl, void *) {
    uint8_t *thing = *(uint8_t **)(ctl + 0x180);
    if (!IsReaver(thing)) return g_tankUI(ctl);
    // (no tank shell arc; the Reaver's crosshair is drawn by VuiTargetDraw)
}
static uint8_t *ControlCamera(uint8_t *ctl);
static void __fastcall TankUpdateControls(uint8_t *ctl, void *) {
    uint8_t *thing = *(uint8_t **)(ctl + 0x180);
    if (!IsReaver(thing)) return g_tankUpdate(ctl);
    static uint8_t **padP = (uint8_t **)SymAddr("?stdInput_pCurrentGamepad@@3PAUtGamepad@@A");
    static unsigned *frame = (unsigned *)SymAddr("?stdInput_curUpdateFrame@@3IA");
    static auto getVel = (void(__thiscall *)(uint8_t *, float *))SymAddr("?GetVelocity@tThing@@QAEXPAUXGVECTOR3@@@Z");
    static auto setVel = (void(__thiscall *)(uint8_t *, const float *))SymAddr("?SetVelocity@tThing@@QAEXPBUXGVECTOR3@@@Z");
    static DWORD nextFire, recoilUntil;
    static float lastYaw;
    uint8_t *pad = padP ? *padP : nullptr;
    uint8_t saveX[24], saveRT[24], saveB[24];
    float stickX = 0.f, stickY = 0.f;
    bool firePressed = false, exitPressed = false;
    if (pad) {   // stdInput slots: 24 bytes from tGamepad +0x64; 1 = X, 5 = B, 7 = RT (+4: the frame it was pressed)
        memcpy(saveX, pad + 0x64 + 24 * 1, 24), memcpy(saveRT, pad + 0x64 + 24 * 7, 24), memcpy(saveB, pad + 0x64 + 24 * 5, 24);
        firePressed = frame && *(unsigned *)(saveRT + 4) == *frame;
        exitPressed = frame && *(unsigned *)(saveB + 4) == *frame;
        memset(pad + 0x64 + 24 * 1, 0, 24), memset(pad + 0x64 + 24 * 7, 0, 24), memset(pad + 0x64 + 24 * 5, 0, 24);
        // the left stick (+0x54 X, +0x58 Y): the tank drives toward the stick's direction relative to the camera, so
        // the mouse steered it and it slid sideways; the Reaver drives itself below (tank controls)
        stickX = *(float *)(pad + 0x54), stickY = *(float *)(pad + 0x58);
        *(float *)(pad + 0x54) = 0.f, *(float *)(pad + 0x58) = 0.f;
    }
    g_tankUpdate(ctl);
    if (pad) {
        memcpy(pad + 0x64 + 24 * 1, saveX, 24), memcpy(pad + 0x64 + 24 * 7, saveRT, 24), memcpy(pad + 0x64 + 24 * 5, saveB, 24);
        *(float *)(pad + 0x54) = stickX, *(float *)(pad + 0x58) = stickY;
    }
    if (LocalPlayer(0) != thing && LocalPlayer(1) != thing) return;   // (no longer driven)
    if (getenv("GHOSTPC_LOG_CAM")) {   // debug: camera floats that look like world positions near the Reaver
        static unsigned startFrame;
        static uint8_t *last;
        if (thing != last) last = thing, startFrame = g_frame;
        unsigned k = g_frame - startFrame;
        uint8_t *cam = ControlCamera(ctl);
        if (cam && (k < 12 || k % 10 == 0) && k < 80) {
            char buf[2048];
            int n = _snprintf(buf, sizeof buf, "cam %u:", k);
            static float prev[0x180];
            for (int i = 0; i < 0x180 && n < 1900; i++) {
                float f = ((const float *)cam)[i];
                if (k && f == f && fabsf(f) < 1e5f && fabsf(f - prev[i]) > 0.05f)
                    n += _snprintf(buf + n, sizeof buf - n, " %x=%.2f", i * 4, f);
                prev[i] = f;
            }
            Log("%s", buf);
        }
    }
    // B backs out to the console at any time (the tank's exit needs a free spot beside the vehicle to put its rider:
    // FindDismountPos; Nova is at the console). tPlayer::DismountVehicle as the tank calls it (no placement by it;
    // cTankControl::Dismount puts her at the entry marker = the console spot); nothing after it uses the control.
    static auto dismountV = (int(__thiscall *)(uint8_t *, uint8_t *, int, int, int))SymAddr("?DismountVehicle@tPlayer@@QAEHPAV1@HHH@Z");
    if (exitPressed && dismountV && *(uint8_t **)(ctl + 0x1a0)) {
        dismountV(*(uint8_t **)(ctl + 0x1a0), thing, 0, 1, 0);
        return;
    }
    // the target marker (vuiTarget_SetTarget, which the tank's drive code points at its own target): on the unit the
    // next Scarab will go for (ReaverTarget), so the player can aim with it; none when nothing is in reach
    static auto setTarget = (void(__cdecl *)(uint8_t *, uint8_t *))SymAddr("?vuiTarget_SetTarget@@YAXPAVtPlayer@@PAVtThing@@@Z");
    if (setTarget) setTarget(thing, ReaverTarget(thing, 40.f));
    // no "Transform Tank" prompt (action 25, HUD_ACTION_TANKMODE): the prompts are the control's action bytes (+0x16 X,
    // read by GetStealthActionButton, vtable slot 0; +0x17 jump, +0x1A action, ... DrawActionIcons)
    for (int k : {0x16, 0x17, 0x18, 0x19, 0x1a, 0x5b, 0x5c, 0x5d})
        if (ctl[k] == 25) ctl[k] = 0;
    ReaverCamera(ctl);
    // tank controls: stick forward / back crawls along its heading at REAVER_SPEED, left / right turns it slowly in
    // place or while moving (REAVER_TURN); the mouse only moves the camera
    float v[3] = {};
    if (getVel && setVel) {
        getVel(thing, v);
        const float *hf = (const float *)(thing + 0x90);
        float fl = sqrtf(hf[0] * hf[0] + hf[1] * hf[1]);
        float throttle = fabsf(stickY) > 0.2f ? -stickY : 0.f;   // (stdInput stores the stick's Y negated)
        float turn = fabsf(stickX) > 0.2f ? stickX : 0.f;
        // moving and turning are done on its matrix (tThing::SetPhysMatrix), with no horizontal velocity left for the
        // wheels to fight: a velocity set every frame was braked by the raycast wheels and the Reaver lurched (0 to
        // 0.12 m per frame, bobbing); tThing::SetRotVel did nothing (the wheels and angular damper hold a spin)
        static auto setPhysMatrix = (void(__thiscall *)(uint8_t *, const float *))SymAddr("?SetPhysMatrix@tThing@@QAEXPBUXGMATRIX@@@Z");
        static const float *dt = (const float *)(SymAddr("?stdTime@@3VtStdTime@@A") + 0x30);
        v[0] = v[1] = 0.f;
        setVel(thing, v);
        float step = dt && *dt > 0.f && *dt < 0.25f ? *dt : 0.f;
        // blocked (a wall, another vehicle): the physics pushed it back out last frame. Pushing on scraped it along
        // the wall, tilting it; it stops pushing and tries again every half second.
        static float pushedFrom[2], pushed;
        static DWORD blockedUntil;
        DWORD tnow = GetTickCount();
        if (pushed > 0.f) {
            const float *pp = (const float *)(thing + 0xb0);
            float dx = pp[0] - pushedFrom[0], dy = pp[1] - pushedFrom[1];
            if ((dx * hf[0] + dy * hf[1]) / (fl > 0.01f ? fl : 1.f) * (throttle < 0.f ? -1.f : 1.f) < 0.3f * pushed)
                blockedUntil = tnow + 500;
        }
        pushed = 0.f;
        if (tnow < blockedUntil) throttle = 0.f;
        // (also standing still: left alone, the borrowed suspension rocked it about 0.1 m back and forth every half
        // second and never settled; handing the physics its matrix every frame keeps it steady)
        if (setPhysMatrix && fl > 0.01f) {
            float a = -turn * REAVER_TURN * step, c = cosf(a), sn = sinf(a);
            float m[16];
            memcpy(m, thing + 0x80, sizeof m);   // rows: right, forward, up, position
            for (int r = 0; r < 3; r++) {
                float x = m[4 * r], y = m[4 * r + 1];
                m[4 * r] = x * c - y * sn, m[4 * r + 1] = x * sn + y * c;
            }
            if (throttle != 0.f) {
                pushedFrom[0] = m[12], pushedFrom[1] = m[13], pushed = fabsf(throttle) * REAVER_SPEED * step;
                m[12] += hf[0] / fl * throttle * REAVER_SPEED * step, m[13] += hf[1] / fl * throttle * REAVER_SPEED * step;
            }
            setPhysMatrix(thing, m);
            v[0] = hf[0] / fl * throttle * REAVER_SPEED, v[1] = hf[1] / fl * throttle * REAVER_SPEED;   // (for the motions)
        }
        float h = sqrtf(v[0] * v[0] + v[1] * v[1]);
        g_reaverThrottle = throttle;   // (ReaverCollide: what it drives into)
        if (getenv("GHOSTPC_LOG_REAVERPHYS")) {   // debug: its motion every frame
            float w[3];
            getVel(thing, w);
            const float *r = (const float *)(thing + 0x80), *f = (const float *)(thing + 0x90), *pp = (const float *)(thing + 0xb0);
            Log("reaverphys %u pos %.3f %.3f %.3f yaw %.2f pitch %.3f roll %.3f vel %.2f %.2f %.2f", g_frame, pp[0], pp[1], pp[2],
                atan2f(f[1], f[0]) * 57.29578f, f[2], r[2], w[0], w[1], w[2]);
        }
        // motions
        const float *fwd = (const float *)(thing + 0x90);
        float yaw = atan2f(fwd[1], fwd[0]) * 57.29578f;
        static uint8_t *lastThing;
        if (thing != lastThing) lastThing = thing, lastYaw = yaw;   // first frame in this Reaver: no turn
        float dyaw = yaw - lastYaw;
        if (dyaw > 180.f) dyaw -= 360.f;
        if (dyaw < -180.f) dyaw += 360.f;
        lastYaw = yaw;
        DWORD now = GetTickCount();
        if (firePressed && now >= nextFire && h <= 0.2f) {   // (user: it cannot fire while it moves; turning is fine)
            nextFire = now + REAVER_RELOAD, recoilUntil = now + 1300;
            PlayMotionMode(thing, 48, true);   // recoilstand
            FireScarab(thing);
        } else if (now >= recoilUntil) {
            // turning, moving or not: turn left / right (they crawl as the walk does, with the head and body bent into
            // the turn; user: moving and turning showed the straight walk), at the walk's rate when moving
            if (fabsf(dyaw) > 0.3f) PlayMotionMode(thing, dyaw > 0 ? 132 : 133, false, h > 0.2f ? REAVER_WALK_FPS : 30.f);
            else if (h > 0.2f) PlayMotionMode(thing, 1, false, REAVER_WALK_FPS);   // walk
            else PlayMotionMode(thing, 3);                                // stand
        }
        // sounds (customsound.cpp drops the tank's own engine sounds for the Reaver):
        // - the whole time it is driven: a loop of protoss.xwb wave 0xC6 (6.2 s, looped in the bank; no cue plays it;
        //   user's pick), Sounds\custom\ghostpc\reaver_idle.wav;
        // - moving or turning, on top: the Dragoon's hum (its foley set's special1, mode 152), which its idle
        //   animation's keyframes play in pairs, 0.45 s then 0.8 s apart (logged in the Zoo), on that rhythm
        bool moving = h > 0.2f || fabsf(dyaw) > 0.3f;
        static DWORD nextHum;
        static bool second;
        static auto foley = (int(__thiscall *)(uint8_t *, unsigned, uint8_t *, unsigned *, int, float, void *))SymAddr(
            "?PlayFoleyMode@tObject@@QAEHIPAVtThing@@PAIHM1@Z");
        if (!g_reaverIdle) g_reaverIdle = CustomLoopStart("ghostpc/reaver_idle", (const float *)(thing + 0xb0), 1.f);
        // (user: the Dragoon sound is too loud on the Reaver; PlayFoleyMode ignores its float argument, so the cue is
        // played with a handle and that instance's volume set with cGameMixer::SetVolume; the mixer keeps an instance
        // with a handle until it is stopped, so the previous one is stopped before each play and when it stops moving)
        static const float REAVER_MOVE_VOLUME = 0.35f;
        static unsigned humHandle;
        static void *mixer = *(void **)SymAddr("?gameMixer_pMixer@@3PAVcGameMixer@@A");
        static auto setVolume = (void(__thiscall *)(void *, unsigned, float))SymAddr("?SetVolume@cGameMixer@@QAEXIM@Z");
        static auto stopSound = (void(__thiscall *)(void *, unsigned, unsigned))SymAddr("?Stop@cGameMixer@@QAEXII@Z");
        mixer = *(void **)SymAddr("?gameMixer_pMixer@@3PAVcGameMixer@@A");
        if (moving) {
            if (foley && now >= nextHum) {
                nextHum = now + (second ? 800 : 450);
                second = !second;
                if (humHandle && mixer && stopSound) stopSound(mixer, humHandle, 0);
                humHandle = 0;
                foley(thing, 152, nullptr, &humHandle, 0, 1.f, nullptr);
                if (humHandle && mixer && setVolume) setVolume(mixer, humHandle, REAVER_MOVE_VOLUME);
                static int logs;
                if (logs < 3) logs++, Log("vehicles: Reaver move sound handle %08x, volume %.2f", humHandle, REAVER_MOVE_VOLUME);
            }
        } else {
            if (humHandle && mixer && stopSound && now >= nextHum + 800) stopSound(mixer, humHandle, 0), humHandle = 0;
            if (!humHandle) nextHum = 0, second = false;
        }
    }
}

// The tank's rigid body (simPhysics_SetupTankRB) is a convex hull of the model's collision model, <model>.noc, loaded
// with renderModel_Load at +0xCA. The Reaver has none: its load is wrapped and, when it gives no model or one with no
// vertices (what simPhysics_CreateConvexHull needs), the Siege Tank's siegeTank_black.noc is used (6 x 8.4 x 4 m,
// about the half-scale Reaver's size). Without it the hull is null and the tank's physics setup crashes.
typedef uint8_t *(__cdecl *ModelLoadFn)(const char *, unsigned);
static ModelLoadFn g_modelLoad;
static uint8_t *g_hullFallback;   // the siegeTank_black.noc handed to a model without its own collision model
// simPhysics_CreateConvexHull(model, scale) at SetupTankRB +0xD7: the borrowed tank hull (6 x 8.4 x 4 m, z 1.2-4) is
// widened and lengthened to the Reaver's footprint (native 12 x 17 m, times REAVER_SCALE); scaling its height too lifted the hull off its
// wheels (the Reaver jumped 4 m at setup)
typedef void *(__cdecl *HullFn)(uint8_t *, float *);
static HullFn g_createHull;
// The Reaver's own hull (user: the wall collision was at the wrong spot - the scaled tank hull ran from 5.1 m behind
// its centre to 7.5 m ahead, past its head and short of its tail): a prism over the outline of reaver.nod's vertices
// (native: head +8.8 m, 2.7 m wide each side; widest 6.2 m at +2 m; tail -8.3 m, narrow), times REAVER_SCALE, between
// the tank hull's heights (1.16 .. 4 m: the suspension carries it; a taller hull lifted it off its wheels), built with
// the game's simPhysics_CreateConvexHull(points, count)
static const float REAVER_OUTLINE[][2] = {{2.7f, 8.7f}, {5.3f, 6.f}, {6.2f, 2.f}, {5.6f, -2.f}, {1.2f, -8.3f}};
static void *__cdecl TankHull(uint8_t *model, float *scale) {
    static auto fromPoints = (void *(__cdecl *)(float *, unsigned short))SymAddr(
        "?simPhysics_CreateConvexHull@@YAPAVhkConvexVerticesShape@@PAUXGVECTOR3@@G@Z");
    if (model && model == g_hullFallback && fromPoints) {
        float pts[5 * 2 * 2][3];
        int n = 0;
        for (auto &o : REAVER_OUTLINE)
            for (float side : {-1.f, 1.f})
                for (float z : {1.16f, 4.f})
                    pts[n][0] = o[0] * side * REAVER_SCALE, pts[n][1] = o[1] * REAVER_SCALE, pts[n][2] = z, n++;
        return fromPoints(&pts[0][0], (unsigned short)n);
    }
    return g_createHull(model, scale);
}
static bool ModelHasVerts(const uint8_t *m) {
    if (!m) return false;
    unsigned n = m[5], total = 0;
    for (unsigned i = 0; i < n; i++) total += *(const unsigned *)(m + 0x28 + 0x18 * i);
    return total != 0;
}
static uint8_t *__cdecl TankHullLoad(const char *name, unsigned flags) {
    uint8_t *m = g_modelLoad(name, flags);
    if (ModelHasVerts(m)) return m;
    uint8_t *f = g_hullFallback = g_modelLoad("siegeTank_black.noc", flags);
    Log("vehicles: no collision model %s (%p), using siegeTank_black.noc (%p)", name, m, f);
    return f;
}

// The tank's suspension (SetupSuspension, simControlTank.obj) reads four wheel points from its model: markers of type
// 0x22, numbers 1-4 (cModelInst::GetMarkerPos, world positions after UpdateSkeleton with the object's matrix; it
// converts them back to the vehicle's frame). The Reaver's model has none and the suspension came back null (the Havok
// vehicle then crashes). Its four reads are wrapped: a missing marker gets a corner of the Reaver's footprint (model
// space, through the same matrix).
typedef int(__thiscall *MarkerPosFn)(void *, unsigned char, unsigned short, float *, float *);
static MarkerPosFn g_markerPos;
static int __fastcall TankWheelMarker(void *inst, void *, unsigned char type, unsigned short idx, float *out, float *m) {
    int r = g_markerPos(inst, type, idx, out, m);
    if (r || type != 0x22 || idx < 1 || idx > 4) return r;
    static const float corner[4][2] = {{-3.f, 4.2f}, {3.f, 4.2f}, {-3.f, -4.2f}, {3.f, -4.2f}};   // model units
    // (wider points made the full-size Reaver bounce and stall). Height: the body rests about 1 m lower per unit the
    // points go up; at 0.6 it floated 0.29 m (user: floating), 0.85 puts its feet on the ground
    static float z = getenv("GHOSTPC_REAVER_Z") ? (float)atof(getenv("GHOSTPC_REAVER_Z")) : 0.85f;
    static float ws = getenv("GHOSTPC_REAVER_WHEELS") ? (float)atof(getenv("GHOSTPC_REAVER_WHEELS")) : 1.f;
    float x = corner[idx - 1][0] * ws, y = corner[idx - 1][1] * ws;
    if (m) {   // row vectors (D3D): p * m
        out[0] = x * m[0] + y * m[4] + z * m[8] + m[12];
        out[1] = x * m[1] + y * m[5] + z * m[9] + m[13];
        out[2] = x * m[2] + y * m[6] + z * m[10] + m[14];
    } else
        out[0] = x, out[1] = y, out[2] = z;
    return 1;
}

// The tank's own motion calls (Mount / Dismount / FireArclite / Siege / DriveControlsUpdate: tObject::PlayMotion on
// the tank, or on Nova when she mounts) put the Reaver back in the tank's modes every frame, restarting its walk.
// They go through TankPlayMotion, which leaves the Reaver to TankUpdateControls.
typedef int(__thiscall *PlayMotionFn)(uint8_t *, int, unsigned, float, void *, float, float);
static PlayMotionFn g_playMotion;
static int __fastcall TankPlayMotion(uint8_t *obj, void *, int mode, unsigned a, float b, void *set, float c, float d) {
    if (IsReaver(obj)) return 1;
    return g_playMotion(obj, mode, a, b, set, c, d);
}

// The Reaver is a robot worked from a console (user's design), as the Spider Mine is worked remotely: Nova uses a
// Protoss floor console (AR_hackConsole_floor, an unused Global.not prop) and the Reaver is mounted from there
// (tPlayer::MountVehicle, as the action button does for a vehicle); her body stays at the console and the camera goes
// to the Reaver; B backs out and she is at the console again.
// The tank's Mount / Dismount (cTankControl vtable 53 / 54) put the rider at the vehicle's entry marker (type 0x0F,
// cModelInst::GetMarkerMatrix: +0x10 the direction she faces, +0x30 the position), hide her on mount and play her
// climb-in / -out motions. The Reaver's model has no entry markers: GetMarkerMatrix is detoured and, while it is worked
// from a console, its entry marker is where Nova stood (so the tank code leaves her there and puts her back there, its
// camera following); otherwise its sides, 7 m out, facing it. TankMount / TankDismount show her and stand her up.
static std::vector<uint8_t *> g_reavers;    // Reaver things in the world (ScanReavers, once a second)
static std::vector<uint8_t *> g_consoles;   // its consoles (AR_hackConsole_floor)
static std::vector<uint8_t *> g_dragoons;   // Dragoons (dragoon_vehicle_player), for their entry points
static uint8_t *g_remoteReaver;             // the Reaver being worked from a console, and where Nova stood
static float g_consoleMat[16];
static bool IsReaverConsole(uint8_t *t) { return t && !_stricmp((char *)(t + 0xc4), "AR_hackConsole_floor"); }
// Every listed thing's GUID and model instance, taken when it was found. Quitting a level frees its things before the
// world pointer changes (LevelWorld): the lists then held freed things and DragoonShields read one (crash on quitting,
// user). PruneThings (start of every VehiclesUpdate) keeps only things gameThing_FindGUID still returns; the marker
// detours compare model instances with the cached ones instead of reading the things.
static std::map<uint8_t *, std::pair<unsigned, void *>> g_known;   // thing -> (GUID, model instance)
static bool StillThere(uint8_t *t) {
    static auto findT = (uint8_t *(__cdecl *)(unsigned))SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    auto it = g_known.find(t);
    return it != g_known.end() && findT && findT(it->second.first) == t;
}
static uint8_t *ThingOfInst(const std::vector<uint8_t *> &list, void *inst) {
    for (uint8_t *t : list) {
        auto it = g_known.find(t);
        if (it != g_known.end() && it->second.second == inst) return t;
    }
    return nullptr;
}
static void PruneThings() {
    for (auto *list : {&g_reavers, &g_consoles, &g_dragoons})
        list->erase(std::remove_if(list->begin(), list->end(), [](uint8_t *t) { return !StillThere(t); }), list->end());
    if (g_remoteReaver && !StillThere(g_remoteReaver)) g_remoteReaver = nullptr;
}
static void ScanReavers() {
    static auto findT = (uint8_t *(__cdecl *)(unsigned))SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    static unsigned *nextGuid = (unsigned *)SymAddr("?nextGUID@tThing@@2IA");
    static uint8_t *world;
    if (LevelWorld() != world)
        world = LevelWorld(), g_reavers.clear(), g_consoles.clear(), g_dragoons.clear(), g_known.clear(), g_remoteReaver = nullptr;
    if (!findT || !nextGuid || g_frame % 30) return;
    std::vector<uint8_t *> found, consoles, dragoons;
    for (unsigned g = 1; g < *nextGuid && g < 200000; g++)
        if (uint8_t *t = findT(g)) {
            if (IsReaver(t)) found.push_back(t);
            else if (IsReaverConsole(t)) consoles.push_back(t);
            else if (!_stricmp((char *)(t + 0xc4), "dragoon_vehicle_player")) dragoons.push_back(t);
        }
    g_reavers.swap(found);
    g_consoles.swap(consoles);
    g_dragoons.swap(dragoons);
    g_known.clear();
    for (auto *list : {&g_reavers, &g_consoles, &g_dragoons})
        for (uint8_t *t : *list) g_known[t] = {*(unsigned *)(t + 0x34), *(void **)(t + 0x2a0)};
    static bool log = getenv("GHOSTPC_LOG_VEHICLE") != nullptr;
    for (uint8_t *t : g_reavers)
        if (log) Log("vehicle: Reaver at %.2f %.2f %.2f heading %.0f", ((float *)(t + 0xb0))[0], ((float *)(t + 0xb0))[1],
                     ((float *)(t + 0xb0))[2], atan2f(((float *)(t + 0x90))[1], ((float *)(t + 0x90))[0]) * 57.29578f);
}
// a parked Reaver rocks on its borrowed suspension as a driven one did (TankUpdateControls): its matrix is handed to
// the physics every frame, which keeps it steady
static void HoldParkedReavers() {
    static auto setPhysMatrix = (void(__thiscall *)(uint8_t *, const float *))SymAddr("?SetPhysMatrix@tThing@@QAEXPBUXGMATRIX@@@Z");
    if (!setPhysMatrix) return;
    for (uint8_t *t : g_reavers) {
        if (t == LocalPlayer(0) || t == LocalPlayer(1) || (*(uint32_t *)(t + 0x360) & 0x10000000)) continue;
        float m[16];
        memcpy(m, t + 0x80, sizeof m);
        setPhysMatrix(t, m);
        if (getenv("GHOSTPC_LOG_REAVERPHYS"))
            Log("reaverphys %u parked pos %.3f %.3f %.3f pitch %.3f", g_frame, m[12], m[13], m[14], m[6]);
    }
}
typedef int(__thiscall *MarkerMatrixFn)(void *, unsigned char, unsigned short, float *, float *);
static MarkerMatrixFn g_markerMatrix;
static float g_dragoonAim[3];                         // (the Dragoon's aim point, DragoonAimPoint)
static void DragoonMuzzle(uint8_t *thing, float *from);
static int __fastcall ReaverMarkerMatrix(void *inst, void *, unsigned char type, unsigned short idx, float *out, float *m) {
    int r = g_markerMatrix(inst, type, idx, out, m);
    // The crosshair (cUITarget::DrawTurretReticle for a Goliath-type control) is drawn at the vehicle model's sight
    // marker (type 2, number 5) + 8 m along its forward axis (+0x10) + 2.8 m along its up axis (+0x20: the Goliath's
    // sight is above its guns), without checking that the marker exists; the Dragoon has none, so it landed off screen
    // (user: no crosshair). For a Dragoon the marker points from its gun at the aim point and sits so that the
    // crosshair falls exactly on the aim point, where the shot goes (with the marker at the gun it showed 2.8 m above
    // the shot's path, user).
    if (!r && type == 2 && idx == 5 && out)
        if (uint8_t *t = ThingOfInst(g_dragoons, inst)) {
            float from[3];
            DragoonMuzzle(t, from);
            float d[3] = {g_dragoonAim[0] - from[0], g_dragoonAim[1] - from[1], g_dragoonAim[2] - from[2]};
            float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (l < 0.1f) memcpy(d, t + 0x90, 12), l = 1.f;
            for (float &c : d) c /= l;
            float rgt[3] = {d[1], -d[0], 0.f}, rl = sqrtf(rgt[0] * rgt[0] + rgt[1] * rgt[1]);
            if (rl < 0.01f) rgt[0] = 1.f, rgt[1] = 0.f, rl = 1.f;
            rgt[0] /= rl, rgt[1] /= rl;
            float up[3] = {rgt[1] * d[2] - rgt[2] * d[1], rgt[2] * d[0] - rgt[0] * d[2], rgt[0] * d[1] - rgt[1] * d[0]};
            float at[3];
            for (int k = 0; k < 3; k++) at[k] = g_dragoonAim[k] - d[k] * 8.f - up[k] * 2.8f;
            float mm[16] = {rgt[0], rgt[1], rgt[2], 0, d[0], d[1], d[2], 0, up[0], up[1], up[2], 0, at[0], at[1], at[2], 1};
            memcpy(out, mm, sizeof mm);
            static bool logged;
            if (!logged && getenv("GHOSTPC_LOG_VEHICLE"))
                logged = true, Log("vehicle: Dragoon sight marker supplied (crosshair) at %.1f %.1f %.1f toward %.1f %.1f %.1f", from[0],
                                   from[1], from[2], g_dragoonAim[0], g_dragoonAim[1], g_dragoonAim[2]);
            return 1;
        }
    if (!r && type == 2 && idx == 5 && out)   // the Reaver's crosshair (VuiTargetDraw): on the unit its next Scarab goes for
        if (uint8_t *t = ThingOfInst(g_reavers, inst)) {
            static auto targetPos = (void(__thiscall *)(uint8_t *, float *, unsigned))SymAddr("?GetTargetPos@tActor@@UAEXPAUXGVECTOR3@@I@Z");
            const float *p = (const float *)(t + 0xb0), *f = (const float *)(t + 0x90);
            float fl = sqrtf(f[0] * f[0] + f[1] * f[1]);
            if (fl < 0.01f) return r;
            float hx = f[0] / fl, hy = f[1] / fl, ahead = 8.8f * REAVER_SCALE + 0.5f;
            float from[3] = {p[0] + hx * ahead, p[1] + hy * ahead, p[2] + 1.f};   // (where Scarabs launch)
            float aim[3] = {from[0] + hx * 20.f, from[1] + hy * 20.f, p[2]};       // (none: the ground 20 m ahead)
            if (uint8_t *tg = ReaverTarget(t, 40.f)) {
                const float *q = (const float *)(tg + 0xb0);
                aim[0] = q[0], aim[1] = q[1], aim[2] = q[2] + 1.f;
                if (targetPos) targetPos(tg, aim, 0);
            }
            float d[3] = {aim[0] - from[0], aim[1] - from[1], aim[2] - from[2]};
            float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (l < 0.1f) return r;
            for (float &c : d) c /= l;
            float rgt[3] = {d[1], -d[0], 0.f}, rl = sqrtf(rgt[0] * rgt[0] + rgt[1] * rgt[1]);
            if (rl < 0.01f) return r;
            rgt[0] /= rl, rgt[1] /= rl;
            float up[3] = {rgt[1] * d[2] - rgt[2] * d[1], rgt[2] * d[0] - rgt[0] * d[2], rgt[0] * d[1] - rgt[1] * d[0]};
            float at[3];
            for (int k = 0; k < 3; k++) at[k] = aim[k] - d[k] * 8.f - up[k] * 2.8f;   // (crosshair = marker + 8 fwd + 2.8 up)
            float mm[16] = {rgt[0], rgt[1], rgt[2], 0, d[0], d[1], d[2], 0, up[0], up[1], up[2], 0, at[0], at[1], at[2], 1};
            memcpy(out, mm, sizeof mm);
            return 1;
        }
    if (r || type != 0x0f || idx > 1 || !out) return r;
    if (uint8_t *t = ThingOfInst(g_reavers, inst)) {
        if (t == g_remoteReaver) {   // worked from a console: where Nova stands
            memcpy(out, g_consoleMat, sizeof g_consoleMat);
            return 1;
        }
        const float *right = (const float *)(t + 0x80), *fwd = (const float *)(t + 0x90), *pos = (const float *)(t + 0xb0);
        float side = idx ? 1.f : -1.f;
        for (int k = 0; k < 3; k++) {
            out[0 + k] = fwd[k] * side;
            out[4 + k] = -right[k] * side;   // face the Reaver
            out[8 + k] = k == 2 ? 1.f : 0.f;
            out[12 + k] = pos[k] + right[k] * side * 7.f * REAVER_SCALE;
        }
        out[3] = out[7] = out[11] = 0.f, out[15] = 1.f;
        return 1;
    }
    if (uint8_t *t = ThingOfInst(g_dragoons, inst)) {   // a Dragoon's missing entry markers: its sides, 2.4 m out
        const float *right = (const float *)(t + 0x80), *fwd = (const float *)(t + 0x90), *pos = (const float *)(t + 0xb0);
        float side = idx ? 1.f : -1.f;
        for (int k = 0; k < 3; k++) {
            out[0 + k] = fwd[k] * side;
            out[4 + k] = -right[k] * side;
            out[8 + k] = k == 2 ? 1.f : 0.f;
            out[12 + k] = pos[k] + right[k] * side * 2.4f;
        }
        out[3] = out[7] = out[11] = 0.f, out[15] = 1.f;
        if (getenv("GHOSTPC_LOG_VEHICLE")) Log("vehicle: Dragoon entry point %u supplied", idx);
        return 1;
    }
    return r;
}
// the console ride's start and end (cTankControl::Mount / Dismount, vtable 53 / 54), for a Reaver: Nova stays visible at
// the console (the tank hides its rider) and stands there instead of climbing in / out
static UpdateFn g_tankMount, g_tankDismount;
static void NovaAtConsole(uint8_t *nova) {
    static auto hide = (void(__thiscall *)(uint8_t *, int, int))SymAddr("?HidePlayer@tPlayer@@QAEXHH@Z");
    static const unsigned *now = (const unsigned *)(SymAddr("?stdTime@@3VtStdTime@@A") + 0x44);
    if (!nova) return;
    if (hide) hide(nova, 0, 0);
    PlayMotionMode(nova, 3, true);   // stand
    if (uint8_t *nc = *(uint8_t **)(nova + 0x3f8))
        if (now) *(unsigned *)(nc + 0x16c) = *now;   // (no wait for the climb-out motion)
}
// The camera jumps to the Reaver and back at once. On mount Nova's chase offsets (camera +0x1B8 position, +0x1D0 look)
// are kept, the Reaver's are set (tSimCamera::BeginChase) and the camera is put there now: UpdateChase(0, ...). Its
// first argument starts a lerp (StartLerp, at the second argument's speed in m/s; +0x1A4 = 1 while it runs): the
// tank's StartState (inside Mount) lerps at 10, the Spider Mine's Mount at 5 (it starts next to Nova, so it looks
// instant); from a console 13 m and more away the camera flew over, through the ground. That lerp is cancelled. On
// dismount it goes back to Nova (BeginThirdPerson(1, 0, 0), as the Spider Mine's Dismount) with her offsets.
static float g_novaCamPos[3], g_novaCamLook[3];
static auto g_getCam = (uint8_t *(__thiscall *)(uint8_t *))nullptr;
static uint8_t *ControlCamera(uint8_t *ctl) {
    if (!g_getCam) g_getCam = (uint8_t *(__thiscall *)(uint8_t *))SymAddr("?GetCamera@cControl@@QAEPAVtSimCamera@@XZ");
    return g_getCam ? g_getCam(ctl) : nullptr;
}
static void __fastcall TankMount(uint8_t *ctl, void *) {
    uint8_t *thing = *(uint8_t **)(ctl + 0x180), *nova = *(uint8_t **)(ctl + 0x1a0);
    if (!IsReaver(thing) || !nova) return g_tankMount(ctl);
    memcpy(g_consoleMat, nova + 0x80, sizeof g_consoleMat);
    g_remoteReaver = thing;
    g_tankMount(ctl);
    NovaAtConsole(nova);
    static auto beginChase = (void(__thiscall *)(uint8_t *, float *, int, float *, int))SymAddr("?BeginChase@tSimCamera@@QAEXPAUXGVECTOR3@@H0H@Z");
    static auto updateChase = (void(__thiscall *)(uint8_t *, int, float, float *, int, int))SymAddr("?UpdateChase@tSimCamera@@QAEXHMPAUXGVECTOR3@@HH@Z");
    if (uint8_t *cam = ControlCamera(ctl)) {
        memcpy(g_novaCamPos, cam + 0x1b8, 12), memcpy(g_novaCamLook, cam + 0x1d0, 12);
        float pos[3], look[3];
        ReaverPreset(0, pos, look);
        if (beginChase) beginChase(cam, pos, 0, look, 0);
        *(int *)(cam + 0x1a4) = 0;   // no lerp (the tank's StartState started one)
        if (updateChase) updateChase(cam, 0, 0.f, nullptr, 0, 0);
    }
    Log("vehicles: Reaver worked from the console at %.1f %.1f %.1f", g_consoleMat[12], g_consoleMat[13], g_consoleMat[14]);
}
static void __fastcall TankDismount(uint8_t *ctl, void *) {
    uint8_t *thing = *(uint8_t **)(ctl + 0x180), *nova = *(uint8_t **)(ctl + 0x1a0);
    if (!IsReaver(thing)) return g_tankDismount(ctl);
    g_tankDismount(ctl);
    NovaAtConsole(nova);
    static auto thirdPerson = (void(__thiscall *)(uint8_t *, int, int, int))SymAddr("?BeginThirdPerson@tSimCamera@@QAEXHHH@Z");
    static auto beginChase = (void(__thiscall *)(uint8_t *, float *, int, float *, int))SymAddr("?BeginChase@tSimCamera@@QAEXPAUXGVECTOR3@@H0H@Z");
    if (uint8_t *cam = ControlCamera(ctl)) {
        if (thirdPerson) thirdPerson(cam, 1, 0, 0);
        if (beginChase) beginChase(cam, g_novaCamPos, 0, g_novaCamLook, 0);
    }
    if (nova) {
        const float *p = (const float *)(nova + 0xb0);
        Log("vehicles: back at the Reaver's console: Nova at %.1f %.1f %.1f", p[0], p[1], p[2]);
    }
    g_remoteReaver = nullptr;
}

// Damage burn points (marker type 27, where a ctrldamage effect such as ProtBuildDam puts its burns: cEffectCtrlDamage
// makes one per marker, cModelInst::GetNumMarkers; each smoke places itself with cModelInst::GetMarkerPos). The Dragoon
// and Reaver models have none, so the blue burn sat at their origin, under them (user). Both functions are detoured:
// for those two vehicles type 27 has two points on top of the body (model space; the Reaver's at its 0.75 scale, from
// the top of reaver.nod's vertices).
static const float DRAGOON_BURN[2][3] = {{0, 0, 2.1f}, {0, -2, 2.6f}};
static const float REAVER_BURN[2][3] = {{0, 1.5f, 8.4f}, {0, -3, 7.1f}};
static const float (*BurnPoints(void *inst))[3] {
    if (ThingOfInst(g_dragoons, inst)) return DRAGOON_BURN;
    if (ThingOfInst(g_reavers, inst)) return REAVER_BURN;
    return nullptr;
}
static uint8_t *BurnThing(void *inst) {
    if (uint8_t *t = ThingOfInst(g_dragoons, inst)) return t;
    return ThingOfInst(g_reavers, inst);
}
typedef unsigned char(__thiscall *NumMarkersFn)(void *, unsigned char);
static NumMarkersFn g_numMarkers;
static unsigned char __fastcall BurnNumMarkers(void *inst, void *, unsigned char type) {
    unsigned char n = g_numMarkers(inst, type);
    return n == 0 && type == 27 && BurnPoints(inst) ? 2 : n;
}
static MarkerPosFn g_markerPosAll;
static int __fastcall BurnMarkerPos(void *inst, void *, unsigned char type, unsigned short idx, float *out, float *m) {
    int r = g_markerPosAll(inst, type, idx, out, m);
    if (r || type != 27 || idx > 1 || !out) return r;
    uint8_t *t = BurnThing(inst);
    const float(*pts)[3] = BurnPoints(inst);
    if (!t || !pts) return r;
    const float *right = (const float *)(t + 0x80), *fwd = (const float *)(t + 0x90), *up = (const float *)(t + 0xa0),
                *pos = (const float *)(t + 0xb0), *q = pts[idx];
    for (int k = 0; k < 3; k++) out[k] = pos[k] + right[k] * q[0] + fwd[k] * q[1] + up[k] * q[2];
    static DWORD logged;
    if (getenv("GHOSTPC_LOG_VEHICLE") && GetTickCount() - logged > 2000)
        logged = GetTickCount(), Log("vehicle: %.24s burn point %u at %.1f %.1f %.1f", (char *)(t + 0xc4), idx, out[0], out[1], out[2]);
    return 1;
}

// The Reaver's crosshair (user: the tank's shell-arc rings, drawn under the ground: the arc display steps along its
// path until it collides and on this level it never met the ground). vuiTarget_Draw draws a vehicle crosshair only
// for a control with +0x70 bit 2 set, by control type (+0x10): the tank's (5) is DrawTankReticle, lines along its
// cannon markers (2 #0, 2 #4), which the Reaver's model lacks. Its one call (vui_Update) goes through VuiTargetDraw:
// while a Reaver is driven, its control is shown as Goliath-type (4) with the flag set for that call, so it gets the
// Dragoon's crosshair (DrawTurretReticle), placed by its sight marker (ReaverMarkerMatrix: on the Scarab's target).
typedef void(__cdecl *VoidFn)();
static VoidFn g_vuiTargetDraw;
static void __cdecl VuiTargetDraw() {
    static auto active = (uint8_t *(__cdecl *)())SymAddr("?gameMultiplayer_GetActivePlayer@@YAPAVtPlayer@@XZ");
    uint8_t *p = active ? active() : nullptr;
    uint8_t *ctl = p ? *(uint8_t **)(p + 0x3f8) : nullptr;
    if (!ctl || !IsReaver(p)) return g_vuiTargetDraw();
    int type = *(int *)(ctl + 0x10);
    uint32_t flags = *(uint32_t *)(ctl + 0x70);
    *(int *)(ctl + 0x10) = 4, *(uint32_t *)(ctl + 0x70) = flags | 2;
    g_vuiTargetDraw();
    *(int *)(ctl + 0x10) = type, *(uint32_t *)(ctl + 0x70) = flags;
}

// a trampoline for fn's first n bytes (whole instructions, none relative) and a jmp from fn to `to`
static void *DetourFn(uint8_t *fn, const uint8_t *expect, size_t n, void *to) {
    if (!fn || memcmp(fn, expect, n)) return nullptr;
    uint8_t *t = (uint8_t *)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(t, fn, n);
    t[n] = 0xE9;
    *(int32_t *)(t + n + 1) = (int32_t)((fn + n) - (t + n + 5));
    DWORD o;
    VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &o);
    fn[0] = 0xE9;
    *(int32_t *)(fn + 1) = (int32_t)((uint8_t *)to - (fn + 5));
    VirtualProtect(fn, 5, o, &o);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
    return t;
}

// Shadows (user: the Vulture's is cut off, the Dragoon's fades with distance): tThing::DrawFullShadow renders the thing
// into a shadow texture through a light camera (tRenderCamera::BuildLightProjection) whose width is 2 x GetRadius
// (+0x50, when +0x4C is set) and whose depth is 2.5 x +0x58 (or +0x50), and projects it on the ground.
// Diagnosis (GHOSTPC_LOG_SHADOW): per thing, once a second, its radius fields, model bounds and shadow light.
typedef int(__thiscall *FullShadowFn)(uint8_t *, void *, uint8_t *, float);
static FullShadowFn g_fullShadow;
static int __fastcall FullShadowHook(uint8_t *thing, void *, void *cam, uint8_t *light, float f) {
    static bool log = getenv("GHOSTPC_LOG_SHADOW") != nullptr;
    if (log && thing && *(int *)(thing + 0x10) == 7) {
        static std::map<uint8_t *, DWORD> last;
        DWORD now = GetTickCount();
        if (now - last[thing] > 1000) {
            last[thing] = now;
            uint8_t *inst = *(uint8_t **)(thing + 0x2a0), *model = inst ? *(uint8_t **)inst : nullptr;
            const float *b = model ? (const float *)(model + 0xb0) : nullptr;
            const float *p = (const float *)(thing + 0xb0);
            Log("shadow: %.24s at %.1f %.1f: +4c %08x radius %.2f +54 %.2f +58 %.2f, model bounds %.2f %.2f %.2f .. %.2f %.2f %.2f, "
                "light %p type %d f %.2f", (char *)(thing + 0xc4), p[0], p[1], *(uint32_t *)(thing + 0x4c), *(float *)(thing + 0x50),
                *(float *)(thing + 0x54), *(float *)(thing + 0x58), b ? b[0] : 0.f, b ? b[1] : 0.f, b ? b[2] : 0.f, b ? b[3] : 0.f,
                b ? b[4] : 0.f, b ? b[5] : 0.f, light, light ? *(int *)(light + 4) : -1, f);
        }
    }
    // A thing's shadow camera covers its model, not its radius (vehicles first; characters too, user) (the Vulture is a player-based thing:
    // radius 0.5 m, its model 2.6 x 4.9 m, so only the middle metre of its shadow was drawn, user): for the call, the
    // radius is the model's footprint (cModel +0xB0 tight bounds: the farthest x / y from its origin, plus 15%) and
    // the depth its height, when larger
    {   // (every thing: characters too, user -- Nova's radius is 0.5 m, her model 1.7 m arm to arm)
        uint8_t *inst = *(uint8_t **)(thing + 0x2a0), *model = inst ? *(uint8_t **)inst : nullptr;
        if (model) {
            const float *b = (const float *)(model + 0xb0);
            float r = 0.f;
            for (int k : {0, 1, 3, 4}) r = std::max(r, fabsf(b[k]));
            r *= 1.15f;
            float h = b[5] - b[2];
            uint32_t had4c = *(uint32_t *)(thing + 0x4c);
            float r50 = *(float *)(thing + 0x50), r58 = *(float *)(thing + 0x58);
            if (r > r50) {
                *(uint32_t *)(thing + 0x4c) = had4c ? had4c : 1;
                *(float *)(thing + 0x50) = r;
                if (h > r58) *(float *)(thing + 0x58) = h;
                int ret = g_fullShadow(thing, cam, light, f);
                *(uint32_t *)(thing + 0x4c) = had4c, *(float *)(thing + 0x50) = r50, *(float *)(thing + 0x58) = r58;
                return ret;
            }
        }
    }
    return g_fullShadow(thing, cam, light, f);
}
// Shadow distance (user: the Dragoon's shadow disappeared at a distance): tThing::FindIdealShadowLight and DrawShadow
// give a thing no shadow while its model instance's detail (+0x24) is under 0.5; cModelInst::SetDistanceDetail sets it
// to +0x2C / (1 + 0.2 (distance - 5 m)), so shadows ended about 10 m from the camera, for a 4 m vehicle in plain
// sight. Both run with the detail raised fourfold (shadows to about 40 m; vehicles first, then every character too,
// user); it is put back after, so the model's own level of detail is unchanged.
typedef int(__thiscall *IdealLightFn)(uint8_t *, void *, unsigned);
typedef void(__thiscall *DrawShadowFn)(uint8_t *, void *);
static IdealLightFn g_idealLight;
static DrawShadowFn g_drawShadow;
struct DetailBoost {
    float *d = nullptr, old = 0.f;
    explicit DetailBoost(uint8_t *thing) {
        uint8_t *inst = thing ? *(uint8_t **)(thing + 0x2a0) : nullptr;
        if (!inst) return;
        d = (float *)(inst + 0x24), old = *d;
        *d = std::min(1.f, old * 4.f);
    }
    ~DetailBoost() { if (d) *d = old; }
};
static int __fastcall IdealLightHook(uint8_t *thing, void *, void *lights, unsigned n) {
    DetailBoost boost(thing);
    return g_idealLight(thing, lights, n);
}
static void __fastcall DrawShadowHook(uint8_t *thing, void *, void *cam) {
    DetailBoost boost(thing);
    g_drawShadow(thing, cam);
}
// (diagnosis, GHOSTPC_LOG_SHADOW: every type-7 thing's shadow state once a second: +0x48 shadow flags (8 full, 4 drop),
// the chosen light +0x2DC, the two fade slots +0x2E0 / +0x2E4 with their fades +0x2E8 / +0x2EC, its up vector +0xA0)
static void ShadowStateLog() {
    // debug (GHOSTPC_FULL_DETAIL): every model at full detail (renderOptions +0x40, the lowest detail cModelInst::SetDetail
    // allows, held at 1: level of detail 0 at any distance)
    static float *minDetail = getenv("GHOSTPC_FULL_DETAIL") ? (float *)(SymAddr("?renderOptions@@3VtRenderOptions@@A") + 0x40) : nullptr;
    if (minDetail && SymAddr("?renderOptions@@3VtRenderOptions@@A")) *minDetail = 1.f;
    static bool log = getenv("GHOSTPC_LOG_SHADOW") != nullptr;
    static DWORD last;
    if (!log || GetTickCount() - last < 1000) return;
    last = GetTickCount();
    static auto findT = (uint8_t *(__cdecl *)(unsigned))SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    static unsigned *next = (unsigned *)SymAddr("?nextGUID@tThing@@2IA");
    for (unsigned g = 1; findT && next && g < *next && g < 100000; g++)
        if (uint8_t *t = findT(g))
            if (*(int *)(t + 0x10) == 7) {
                const float *p = (const float *)(t + 0xb0);
                uint8_t *inst = *(uint8_t **)(t + 0x2a0);
                Log("shadowstate: %.24s at %.1f %.1f: flags48 %08x flags360 %08x light %p inst+24 %.3f inst+20 %.3f inst+28 %.3f",
                    (char *)(t + 0xc4), p[0], p[1], *(uint32_t *)(t + 0x48), *(uint32_t *)(t + 0x360), *(void **)(t + 0x2dc),
                    inst ? *(float *)(inst + 0x24) : -1.f, inst ? *(float *)(inst + 0x20) : -1.f, inst ? *(float *)(inst + 0x28) : -1.f);
            }
}

// Levels of detail (user: far off, the Hydralisk lost everything from the waist down): cModelInst::SetDetail picks a
// model's level as (1 - detail) x 4 (instance +0x28, 0-3) from its distance; the Xbox's reduced meshes of some
// skinned models drop parts (hydralisk.nod LOD 3 has no triangles on its lower body and tail). src/lodcaps.h
// (scripts/make_lod_caps.py) lists those models with the highest level each may use: renderModel_Load records their
// cModels, and SetDetail holds their instances at that level; every other model keeps all four.
#include "lodcaps.h"
typedef uint8_t *(__cdecl *ModelLoadAnyFn)(const char *, unsigned);
typedef void(__thiscall *SetDetailFn)(uint8_t *, float);
static ModelLoadAnyFn g_modelLoadFn;
static SetDetailFn g_setDetail;
static std::map<uint8_t *, int> g_lodCap;   // cModel -> the highest level it may use
static uint8_t *__cdecl LodCapLoad(const char *name, unsigned flags) {
    uint8_t *m = g_modelLoadFn(name, flags);
    if (m && name && !g_lodCap.count(m)) {
        const char *base = name;
        for (const char *c = name; *c; c++)
            if (*c == '\\' || *c == '/') base = c + 1;
        for (auto &cap : LOD_CAPS) {
            size_t n = strlen(cap.model) - 4;   // (without ".nod"; the name may come with or without it)
            if (!_strnicmp(base, cap.model, n) && (!base[n] || !_stricmp(base + n, ".nod"))) {
                g_lodCap[m] = cap.maxLod;
                static int logs;
                if (logs++ < 20) Log("vehicles: %s capped at level of detail %d", cap.model, cap.maxLod);
                break;
            }
        }
    }
    return m;
}
static void __fastcall LodCapDetail(uint8_t *inst, void *, float detail) {
    g_setDetail(inst, detail);
    uint8_t *model = inst ? *(uint8_t **)inst : nullptr;
    if (!model || g_lodCap.empty()) return;
    auto it = g_lodCap.find(model);
    if (it != g_lodCap.end() && inst[0x28] > it->second) inst[0x28] = (uint8_t)it->second;
}

// Using the console: cPlayerControl::UpdateActionButton is wrapped; when it found nothing to do and Nova stands within
// 1.5 m of a Reaver console, facing it, the nearest live Reaver (within 150 m) becomes her activate target with the
// prompt action 5 with the label HACK_CONSOLE ("Access Terminal", the game's hack consoles' text): DrawActionWindow
// shows action 5 with the control's label (+0x1B, a game.nls key, as UpdateActionButton copies a thing's +0x108);
// action 7 showed "Door" (_PROP). B then mounts it (cPlayerControl::Activate -> MountVehicle).
// (The Reaver itself offers nothing: the game's own 1.5 m search around her feet never reaches its hull.)
typedef void(__thiscall *UpdateActionFn)(uint8_t *);
static UpdateActionFn g_updateAction;
// Nova using a Reaver console: who, which Reaver, and when her "use" motion ends
struct ConsoleUse { uint8_t *nova, *reaver; DWORD at; };
static ConsoleUse g_consoleUse;
static void ConsoleUseUpdate() {
    static uint8_t *world;
    if (LevelWorld() != world) world = LevelWorld(), g_consoleUse = {};
    if (!g_consoleUse.nova || GetTickCount() < g_consoleUse.at) return;
    ConsoleUse u = g_consoleUse;
    g_consoleUse = {};
    static auto mount = (int(__thiscall *)(void *, void *))SymAddr("?MountVehicle@tPlayer@@QAEHPAV1@@Z");
    if (u.nova != LocalPlayer(0) && u.nova != LocalPlayer(1)) return;   // (no longer on foot)
    if (!mount || (*(uint32_t *)(u.reaver + 0x360) & 0x10000000)) return;
    int r = mount(u.nova, u.reaver);
    if (getenv("GHOSTPC_LOG_VEHICLE")) Log("vehicle: console used, MountVehicle -> %d", r);
}
static void __fastcall ReaverUpdateAction(uint8_t *ctl, void *) {
    g_updateAction(ctl);
    static auto setTarget = (void(__thiscall *)(uint8_t *, uint8_t *))SymAddr("?SetActivateTarget@cPlayerControl@@QAEXPAVtThing@@@Z");
    uint8_t *nova = *(uint8_t **)(ctl + 0x180);
    if (getenv("GHOSTPC_LOG_VEHICLE") && g_frame % 30 == 0)
        Log("vehicle: action button: %u Reavers, %u consoles, busy %u", (unsigned)g_reavers.size(), (unsigned)g_consoles.size(),
            *(unsigned *)(ctl + 0x238));
    if (!setTarget || !nova || g_reavers.empty() || g_consoles.empty()) return;
    if (g_consoleUse.nova) { ctl[0x1a] = 0; return; }   // using it (no prompt)
    // (the game's own pick, +0x238 = its GUID, may be the console itself: thingflags 2244 has the vehicle bit 4, and
    // the prompt then read "Door", its description _PROP; that one is replaced, anything else is left alone)
    static auto findT = (uint8_t *(__cdecl *)(unsigned))SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    bool consolePick = false;
    if (unsigned busy = *(unsigned *)(ctl + 0x238)) {
        if (!findT || !IsReaverConsole(findT(busy))) return;
        consolePick = true;
    }
    // (user: one step out of "Access Terminal" range the game's own "Door" showed; when the game picked the console
    // and the terminal prompt below does not take over, the prompt is cleared)
    struct NoDoor {
        uint8_t *ctl;
        bool on, taken = false;
        ~NoDoor() { if (on && !taken) ctl[0x1a] = 0; }
    } noDoor{ctl, consolePick};
    const float *np = (const float *)(nova + 0xb0), *nf = (const float *)(nova + 0x90);
    for (uint8_t *c : g_consoles) {
        const float *cp = (const float *)(c + 0xb0);
        float dx = cp[0] - np[0], dy = cp[1] - np[1], d = sqrtf(dx * dx + dy * dy);
        if (getenv("GHOSTPC_LOG_VEHICLE") && g_frame % 30 == 0)
            Log("vehicle: console %p at %.1f %.1f %.1f: Nova %.2f m away, facing %.2f", c, cp[0], cp[1], cp[2], d,
                d > 0.1f ? (dx * nf[0] + dy * nf[1]) / d : 1.f);
        if (d > 1.5f || fabsf(cp[2] - np[2]) > 1.5f) continue;
        if (d > 0.1f && (dx * nf[0] + dy * nf[1]) / d < 0.3f) continue;   // facing it
        uint8_t *best = nullptr;
        float bd = 150.f;
        for (uint8_t *t : g_reavers) {
            if (*(uint32_t *)(t + 0x360) & 0x10000000) continue;   // dead
            float e = ThingDist(t, c);
            if (e < bd) bd = e, best = t;
        }
        if (!best) return;
        noDoor.taken = true;
        setTarget(ctl, best);
        ctl[0x1a] = 5, ctl[0x5b] = 0;   // "Access Terminal"
        strcpy((char *)ctl + 0x1b, "HACK_CONSOLE");
        // B (cPlayerControl::Activate, which WalkUpdate calls right after UpdateActionButton, acts by the prompt: 5 = use
        // a switch, 6 = MountVehicle): Nova turns to the console and plays her "use" motion (mode 0x87, nova_use.nad,
        // as Activate does for a switch); the Reaver is mounted when it ends (ConsoleUse). The prompt is cleared on
        // that frame so the game's Activate does nothing.
        static uint8_t **pad = (uint8_t **)SymAddr("?stdInput_pCurrentGamepad@@3PAUtGamepad@@A");
        static unsigned *frame = (unsigned *)SymAddr("?stdInput_curUpdateFrame@@3IA");
        if (pad && *pad && frame && *frame && *(unsigned *)(*pad + 0x68 + 24 * 5) == *frame &&
            *(float *)(*pad + 0x64 + 24 * 5) > 0.5f) {   // (pressed this frame, and held: at level start both read 0)
            static auto lookAt = (void(__thiscall *)(uint8_t *, float *))SymAddr("?LookAt@tThing@@QAEXPAUXGVECTOR3@@@Z");
            static auto zeroVel = (void(__thiscall *)(uint8_t *))SymAddr("?ZeroVelocity@tThing@@QAEXXZ");
            static auto play = (int(__thiscall *)(uint8_t *, int, unsigned, float, void *, float, float))SymAddr(
                "?PlayMotion@tObject@@QAEHW4eMotionMode@@IMPAVtMotionSet@@MM@Z");
            float at[3] = {cp[0], cp[1], np[2]};
            if (lookAt) lookAt(nova, at);
            if (zeroVel) zeroVel(nova);
            int ms = play ? play(nova, 0x87, 0, 30.f, nullptr, 7.5f, -1.f) : 0;
            g_consoleUse = {nova, best, GetTickCount() + (DWORD)(ms > 0 && ms < 4000 ? ms : 0)};
            if (getenv("GHOSTPC_LOG_VEHICLE")) Log("vehicle: console use: motion %d ms", ms);
            ctl[0x1a] = 0;
        }
        return;
    }
}

// ghostpc.not (GhostPC's templates: the Reaver, its Scarab) is opened next to "global"; a level that places one of
// them lists it too (gameScene_Load opens it; gameTemplate_OpenFile skips a file already open). Reopened whenever its
// templates cannot be found (after a level closed it).
static void GhostTemplates() {
    static auto find = (uint8_t *(__cdecl *)(const char *))SymAddr("?gameTemplate_Find@@YAPAVtTemplate@@PBD@Z");
    static auto open = (void *(__cdecl *)(const char *))SymAddr("?gameTemplate_OpenFile@@YAPAVtTemplateFile@@PBD@Z");
    static void **table = (void **)SymAddr("?gameTemplate_pHashtable@@3PAVtHashtable@@A");
    static int tries;
    if (!find || !open || !table || !*table || g_frame % 30) return;
    if (!find("Arclite_player") || find("proj_reaverScarab")) return;   // global not loaded yet, or ours is
    if (tries++ > 3) return;
    void *f = open("ghostpc");
    Log("vehicles: templates ghostpc.not %s", f && find("proj_reaverScarab") ? "loaded" : "NOT loaded");
}

// debug (GHOSTPC_SCARAB=<frame>): Nova fires a Reaver's Scarab then, and every 3 s after
static void DebugScarab() {
    static int at = getenv("GHOSTPC_SCARAB") ? atoi(getenv("GHOSTPC_SCARAB")) : -1;
    if (at < 0 || (int)g_frame < at || ((int)g_frame - at) % 90) return;
    if (uint8_t *nova = LocalPlayer(0)) FireScarab(nova);
}

// The Dragoon (GhostPC's dragoon_vehicle_player, ghostpc.not: the Goliath's player template with the Dragoon's model,
// motion set, foley, size, health, shields and armor). ControlNameMatch gives it cGoliathControl, a walker (no
// raycast vehicle): it turns toward the left stick's direction (relative to the camera) and walks once it faces within
// about 60 degrees, its ground speed (+0x268) ramping to 8 m/s; leg IK plants its feet (Dragoon.nms has the same IK
// tracks, special1-8). For a Dragoon only:
// - its speed is held to the Dragoon's run speed (ACTOR_RUNSPEED 5.6) and its walk plays at its own pace: the Goliath
//   plays walk at speed x 5 frames/s, the Dragoon's walk suits 30 frames/s at its 2.4 m/s walk (x 12.5);
// - RT fires the Dragoon's own shot, proj_dragoon (what cAIProtossDragoon fires: 50 m/s, exp_dragoonProj), once per
//   attack motion (attack1, mode 15, as StartAttackRanged plays it), from its fire marker (type 2, as the AI's
//   FireProjectile) at the Goliath's lock-on target (or straight ahead); the Goliath's chaingun / rockets do nothing;
// - the Goliath's other motion calls (recoilstand, stand, walk) wait while the attack plays.
static const float DRAGOON_SPEED = 5.6f;
static const float DRAGOON_WALK_RATE = 2.5f;   // its walk frames/s per m/s over the Goliath's
static DWORD g_dragoonAttackUntil, g_dragoonNextShot;
static bool IsDragoon(uint8_t *t) { return t && !_stricmp((char *)(t + 0xc4), "dragoon_vehicle_player"); }
static PlayMotionFn g_goliathPlayMotion;
static int __fastcall GoliathPlayMotion(uint8_t *obj, void *, int mode, unsigned a, float fps, void *set, float c, float d) {
    if (IsDragoon(obj)) {
        if (GetTickCount() < g_dragoonAttackUntil || mode == 48) return 1;   // attacking; recoilstand: the shot plays attack1
        if (mode == 1 || mode == 2) fps *= DRAGOON_WALK_RATE;
    }
    return g_goliathPlayMotion(obj, mode, a, fps, set, c, d);
}
// Aiming (user: inconsistent, the shot went off to the right: a unit within 12 degrees of the view's middle was taken
// without the player seeing which). Every frame while driven: the aim point is where the middle of the view (the
// camera's line of sight, CameraView) meets the ground at the Dragoon's feet, at most 60 m out (or 60 m along it,
// looking up); a live AI unit within 5 degrees of the line of sight and 50 m is locked
// instead (the game's target marker on it; the shot is led for its 50 m/s flight, at its body, tActor::GetTargetPos).
// The aim path (AimPath: rings and a circle where it lands) runs from its fire marker to that point: the cursor.
static uint8_t *g_dragoonTarget;
// the camera's actual view (what is on screen): its matrix at +0x210 (rows right, forward +0x220, up, position +0x240),
// which eases after the target position / look point (+0x8 / +0x74): orbiting it with the mouse, the view trailed the
// look point by up to about 40 degrees, so aiming by the look point missed what the player saw
static bool CameraView(uint8_t *ctl, float *pos, float *dir) {
    uint8_t *cam = ControlCamera(ctl);
    if (!cam) return false;
    memcpy(pos, cam + 0x240, 12);
    memcpy(dir, cam + 0x220, 12);
    float l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (l < 0.01f) return false;
    for (int k = 0; k < 3; k++) dir[k] /= l;
    return true;
}
static void DragoonMuzzle(uint8_t *thing, float *from) {
    static auto markerPos = (int(__thiscall *)(void *, unsigned char, unsigned short, float *, float *))SymAddr(
        "?GetMarkerPos@cModelInst@@QAEHEGPAUXGVECTOR3@@PAUXGMATRIX@@@Z");
    const float *p = (const float *)(thing + 0xb0), *fwd = (const float *)(thing + 0x90);
    from[0] = p[0] + fwd[0] * 1.6f, from[1] = p[1] + fwd[1] * 1.6f, from[2] = p[2] + 1.8f;
    void *inst = *(void **)(thing + 0x2a0);
    if (inst && markerPos) markerPos(inst, 2, 0, from, (float *)(thing + 0x80));
}
// the point a shot fired now goes to (g_dragoonAim), led when a unit is locked
static void DragoonAimPoint(uint8_t *ctl, uint8_t *thing) {
    static auto targetPos = (void(__thiscall *)(uint8_t *, float *, unsigned))SymAddr("?GetTargetPos@tActor@@UAEXPAUXGVECTOR3@@I@Z");
    static auto getVel = (void(__thiscall *)(uint8_t *, float *))SymAddr("?GetVelocity@tThing@@QAEXPAUXGVECTOR3@@@Z");
    const float *p = (const float *)(thing + 0xb0);
    float from[3];
    DragoonMuzzle(thing, from);
    if (uint8_t *t = g_dragoonTarget) {
        const float *q = (const float *)(t + 0xb0);
        float aim[3] = {q[0], q[1], q[2] + 1.f}, v[3] = {};
        if (targetPos) targetPos(t, aim, 0);
        if (getVel) getVel(t, v);
        float dt = sqrtf((aim[0] - from[0]) * (aim[0] - from[0]) + (aim[1] - from[1]) * (aim[1] - from[1])) / 50.f;
        g_dragoonAim[0] = aim[0] + v[0] * dt, g_dragoonAim[1] = aim[1] + v[1] * dt, g_dragoonAim[2] = aim[2];
        return;
    }
    const float *fwd = (const float *)(thing + 0x90);
    g_dragoonAim[0] = from[0] + fwd[0] * 20.f, g_dragoonAim[1] = from[1] + fwd[1] * 20.f, g_dragoonAim[2] = p[2];
    float c[3], ray[3];
    if (!CameraView(ctl, c, ray)) return;
    float t = ray[2] < -0.01f ? (p[2] - c[2]) / ray[2] : 1e9f;   // where it meets the ground (its feet's height)
    float pt[3] = {c[0] + ray[0] * t, c[1] + ray[1] * t, c[2] + ray[2] * t};
    float dx = pt[0] - p[0], dy = pt[1] - p[1];
    if (t > 1e8f || dx * dx + dy * dy > 60.f * 60.f)   // (no ground within 60 m: 60 m along the line of sight)
        for (int k = 0; k < 3; k++) pt[k] = c[k] + ray[k] * 60.f;
    memcpy(g_dragoonAim, pt, sizeof pt);
}
static uint8_t *DragoonTarget(uint8_t *ctl, uint8_t *thing) {
    static auto findT = (uint8_t *(__cdecl *)(unsigned))SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    static unsigned *nextGuid = (unsigned *)SymAddr("?nextGUID@tThing@@2IA");
    static auto getAI = (void *(__thiscall *)(uint8_t *))SymAddr("?GetAI@tActor@@QAEPAVcAIInstance@@XZ");
    float c[3], ray[3];
    if (!findT || !nextGuid || !getAI || !CameraView(ctl, c, ray)) return nullptr;
    uint8_t *best = nullptr;
    float bestScore = 0.9962f;   // cos 5 degrees
    for (unsigned g = 1; g < *nextGuid && g < 200000; g++) {
        uint8_t *t = findT(g);
        if (!t || t == thing || *(int *)(t + 0x10) != 0 || (*(uint32_t *)(t + 0x360) & 0x10000000) || !getAI(t)) continue;
        if (ThingDist(t, thing) > 50.f) continue;
        const float *q = (const float *)(t + 0xb0);
        float d[3] = {q[0] - c[0], q[1] - c[1], q[2] + 0.8f - c[2]};
        float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dl < 1.f) continue;
        float score = (d[0] * ray[0] + d[1] * ray[1] + d[2] * ray[2]) / dl - dl / 4000.f;   // (nearer wins near-ties)
        if (score > bestScore) best = t, bestScore = score;
    }
    return best;
}
static void DragoonFired(uint8_t *thing);   // (its shield comes on)
// the shot: attack1, then proj_dragoon from its fire marker to the aim point
static void DragoonFire(uint8_t *ctl, uint8_t *thing, uint8_t *target) {
    DWORD now = GetTickCount();
    if (now < g_dragoonNextShot) return;
    static auto find = (uint8_t *(__cdecl *)(const char *))SymAddr("?gameTemplate_Find@@YAPAVtTemplate@@PBD@Z");
    static auto lookAt = (void(__cdecl *)(float *, const float *, const float *, float))SymAddr(
        "?MatrixLookAt@@YAXPAUXGMATRIX@@PBUXGVECTOR3@@1M@Z");
    static auto fireT = (uint8_t *(__cdecl *)(uint8_t *, float *, uint8_t *, unsigned, int, int, void *, int))SymAddr(
        "?simCombat_FireTemplateProjectile@@YAPAVtProjectile@@PAVtTemplate@@PAUXGMATRIX@@PAVtThing@@IHHPAVSimpleHit@@W4eHitRegionType@@@Z");
    static auto start = (void(__thiscall *)(uint8_t *))SymAddr("?StartWithAccelerate@tProjectile@@QAEXXZ");
    uint8_t *tmpl = find ? find("proj_dragoon") : nullptr;
    if (!tmpl || !lookAt || !fireT || !start || !g_goliathPlayMotion) return;
    int ms = g_goliathPlayMotion(thing, 15, 1, 30.f, nullptr, 7.5f, -1.f);   // attack1
    if (ms <= 0 || ms > 5000) ms = 1000;
    g_dragoonAttackUntil = g_dragoonNextShot = now + ms;
    float from[3], to[3];
    DragoonMuzzle(thing, from);
    DragoonAimPoint(ctl, thing);
    memcpy(to, g_dragoonAim, sizeof to);
    float m[16] = {};
    lookAt(m, from, to, 0.f);
    m[12] = from[0], m[13] = from[1], m[14] = from[2], m[15] = 1.f;
    uint8_t *proj = fireT(tmpl, m, thing, *(unsigned *)(thing + 0x34), 0, 0, nullptr, 1);
    if (proj) start(proj);
    DragoonFired(thing);
    if (getenv("GHOSTPC_LOG_VEHICLE"))
        Log("vehicle: Dragoon fires at %s from %.1f %.1f %.1f to %.1f %.1f %.1f, attack %d ms", target ? (char *)(target + 0xc4) : "nothing",
            from[0], from[1], from[2], to[0], to[1], to[2], ms);
}
typedef void(__thiscall *ChaingunFn)(uint8_t *, unsigned, uint8_t *, float);
typedef void(__thiscall *RocketFn)(uint8_t *, unsigned, float);
static ChaingunFn g_chaingun;
static RocketFn g_rocket;
static void __fastcall GoliathChaingun(uint8_t *ctl, void *, unsigned i, uint8_t *target, float f) {
    uint8_t *thing = *(uint8_t **)(ctl + 0x180);
    if (!IsDragoon(thing)) return g_chaingun(ctl, i, target, f);
    DragoonFire(ctl, thing, g_dragoonTarget);
}
static void __fastcall GoliathRocket(uint8_t *ctl, void *, unsigned i, float f) {
    if (!IsDragoon(*(uint8_t **)(ctl + 0x180))) g_rocket(ctl, i, f);
}
// cGoliathControl::Collide (vtable 22) crushes what the Goliath walks into (200 damage, gibs, breaks containers); a
// Dragoon does not stomp (a hydralisk took 200 twice for touching it): nothing for a Dragoon
typedef void(__thiscall *CollideFn)(uint8_t *, void *, int, uint8_t *);
static CollideFn g_goliathCollide;
static void __fastcall GoliathCollide(uint8_t *ctl, void *, void *ev, int a, uint8_t *other) {
    if (!IsDragoon(*(uint8_t **)(ctl + 0x180))) g_goliathCollide(ctl, ev, a, other);
}
static UpdateFn g_goliathUpdate;
static void __fastcall GoliathUpdateControls(uint8_t *ctl, void *) {
    g_goliathUpdate(ctl);
    uint8_t *thing = *(uint8_t **)(ctl + 0x180);
    if (!IsDragoon(thing)) return;
    static auto getVel = (void(__thiscall *)(uint8_t *, float *))SymAddr("?GetVelocity@tThing@@QAEXPAUXGVECTOR3@@@Z");
    static auto setVel = (void(__thiscall *)(uint8_t *, const float *))SymAddr("?SetVelocity@tThing@@QAEXPBUXGVECTOR3@@@Z");
    float &speed = *(float *)(ctl + 0x268);
    if (speed > DRAGOON_SPEED) speed = DRAGOON_SPEED;
    float v[3];
    if (getVel && setVel) {
        getVel(thing, v);
        float h = sqrtf(v[0] * v[0] + v[1] * v[1]);
        if (h > DRAGOON_SPEED) v[0] *= DRAGOON_SPEED / h, v[1] *= DRAGOON_SPEED / h, setVel(thing, v);
    }
    if (LocalPlayer(0) != thing && LocalPlayer(1) != thing) return;   // (dismounted)
    g_dragoonTarget = DragoonTarget(ctl, thing);
    static auto setTarget = (void(__cdecl *)(uint8_t *, uint8_t *))SymAddr("?vuiTarget_SetTarget@@YAXPAVtPlayer@@PAVtThing@@@Z");
    if (setTarget) setTarget(thing, g_dragoonTarget);
    // the crosshair (user): vuiTarget_Draw draws a vehicle reticle only when the control's +0x70 bit 2 is set
    // (cControl::SetReticle(1)), by control type (+0x10: 4 = cUITarget::DrawGoliath, a turret reticle marking the lock-on
    // target); nothing in the prototype sets it for the Goliath. Set while a Dragoon is driven.
    *(uint32_t *)(ctl + 0x70) |= 2;
    if (getenv("GHOSTPC_LOG_VEHICLE") && g_frame % 90 == 0)
        Log("vehicle: Dragoon control type %d flags %08x (reticle: type 4 = Goliath)", *(int *)(ctl + 0x10), *(uint32_t *)(ctl + 0x70));
    if (getenv("GHOSTPC_LOG_CAM") && g_frame % 15 == 0)
        if (uint8_t *cam = ControlCamera(ctl)) {   // debug: the camera's vectors
            const float *f = (const float *)cam;
            Log("cam %u: pos %.1f %.1f %.1f look %.1f %.1f %.1f | 210: %.2f %.2f %.2f %.2f | 220: %.2f %.2f %.2f %.2f | 230: %.2f %.2f %.2f %.2f | yaw %.2f pitch %.2f | dragoon fwd %.2f %.2f",
                g_frame, f[2], f[3], f[4], f[0x74 / 4], f[0x78 / 4], f[0x7c / 4], f[0x210 / 4], f[0x214 / 4], f[0x218 / 4], f[0x21c / 4],
                f[0x220 / 4], f[0x224 / 4], f[0x228 / 4], f[0x22c / 4], f[0x230 / 4], f[0x234 / 4], f[0x238 / 4], f[0x23c / 4],
                f[0x204 / 4], f[0x20c / 4], ((float *)(thing + 0x90))[0], ((float *)(thing + 0x90))[1]);
        }
    DragoonAimPoint(ctl, thing);   // (where the crosshair goes)
    // footsteps (user: one leg per walk cycle): its walk's keyframes play lwalk and rwalk together, once per 44-frame
    // cycle (customsound.cpp drops them for this vehicle); here four per cycle, one per leg, left / right alternating,
    // timed by its ground speed (the walk plays at speed x 12.5 frames/s)
    static const float *dt = (const float *)(SymAddr("?stdTime@@3VtStdTime@@A") + 0x30);
    static float phase;
    static int leg;
    static auto foley = (int(__thiscall *)(uint8_t *, unsigned, uint8_t *, unsigned *, int, float, void *))SymAddr(
        "?PlayFoleyMode@tObject@@QAEHIPAVtThing@@PAIHM1@Z");
    float h = v[0] * v[0] + v[1] * v[1] > 0.f ? sqrtf(v[0] * v[0] + v[1] * v[1]) : 0.f;
    if (h > DRAGOON_SPEED) h = DRAGOON_SPEED;
    if (h < 0.3f || !dt || *dt <= 0.f || *dt > 0.25f) { phase = 0.f; return; }
    phase += *dt * h * 12.5f / 44.f * 4.f;   // steps
    if (phase >= 1.f && foley) {
        phase -= 1.f;
        g_ghostpcStep = true;
        foley(thing, (leg++ & 1) ? 32 : 23, nullptr, nullptr, 0, 1.f, nullptr);   // rwalk_solid / lwalk_solid
        g_ghostpcStep = false;
    }
}

// Shields (user: none showed; the Reaver has one too): a Protoss unit's shield is a cProtossShield inside its AI (cAIProtossDragoon +0x544;
// InitInstance: Create(actor, its ACTOR_SHIELD +0x374, the same as maximum); +8 amount, +0xC maximum, +0x10 its
// cEffectPsiShield) and its AI's Damaged takes damage off it first (SetAmount), flashing it (Hit). A player Dragoon has
// no AI: each (and each Reaver) gets its own shield (created when ScanReavers finds it, updated every frame), and
// tActor::Damage is detoured: damage to one is taken off its shield first (Hit flashes it), the rest goes on to its health.
// It recharges 2 points a second once 5 s pass without a hit.
// It is off after Create (+0 bit 1; its effect hidden) until On(), which the AI calls when it goes into combat: here
// when the Dragoon fires or is hit (before the hit is taken off), Off(0) after 8 s without either.
// Recharge (user, for the Reaver and for Nova with the Psi Blade): 1 point a second while in combat (hit or hitting in
// the last 8 s), 3 out of it, with no wait after a hit (combatRegen). The Dragoon keeps 2 a second after 5 s unhit.
// Nova's shield (psiblade.cpp, PlayerShieldOn / Off): 40 points, only while she has the Psi Blade; its shell is
// nova_DT43_shield.nod (scripts/make_shield_models.py), the model cProtossShield's effect loads for "<model>_shield".
// Hitting something counts as combat too (the damage's source has a shield).
struct DragoonShield { uint8_t *thing; DWORD lastHit, lastCombat; bool combatRegen, player; uint8_t obj[0x40]; };
bool CustomPlayOnce(const char *key, const float *at);   // customsound.cpp
// Cloaked shields (user: Nova's showed over her cloak, "an astronaut", and cloaked Tanagazj was solid white; switching
// the shield off is not an option): the shield stays on and keeps working, only its shell is not drawn. The shell is
// a model the shield's cEffectPsiShield (+0x10) attaches to the unit's (+0xA4, a cModelInst; its +0x14 bit 3 hides it,
// as TurnOff leaves it and TurnOn clears it); the effect's own flags (+0x50 bit 1) hide its hit flashes.
// Cloaked: a player's tCloakPower (tPlayer +0x4B4) working, or the unit's model drawn with a cloak shader (its
// instance's +0x54 override shader: Tanagazj's Cloak sets "playercloak" there, UnCloak clears it), or the thing hidden
// (+0x48 bit 1, which his UpdateCloak sets while he is unseen).
bool ThingCloaked(uint8_t *thing, bool player) {
    static auto working = (int(__thiscall *)(uint8_t *))SymAddr("?IsWorking@tCloakPower@@UAEHXZ");
    // a player: her cloak power only (user: the blade went invisible and its hum stopped with her uncloaked, her shell
    // logging "cloaked" on and off: a player's model shader override and +0x48 bit 1 change for other reasons)
    // (tCloakPower::IsWorking is not "cloaked": it is true whenever the cloak is not disrupted, so the blade and shell hid
    // most of the time and came back while shooting disrupted it, user. The power's +0x44 is its on flag: Start sets
    // it, End clears it, tPlayer::HasActivePower reads it.)
    if (player) {
        uint8_t *cloak = *(uint8_t **)(thing + 0x4b4);
        return cloak && *(int *)(cloak + 0x44) != 0;
    }
    uint8_t *inst = *(uint8_t **)(thing + 0x2a0);
    return (inst && *(void **)(inst + 0x54)) || (*(uint32_t *)(thing + 0x48) & 2);
}
// Hiding it: the shell's own hide bit (+0x14 bit 3) did not stop it drawing (user: still white), as an attached model
// is drawn with its parent. So while the owner is cloaked the shell is taken out of the parent's attachment slots
// (cModelInst +4 + 4 * slot, 4 slots; cEffectPsiShield::Init attaches it to slot 0) and put back in that slot when it
// uncloaks (before the shield is destroyed, too). The effect's own flags (+0x50 bit 1) hide its hit flashes.
struct HiddenShell { uint8_t *shield, *parent, *shell; int slot; };
static std::vector<HiddenShell> g_hiddenShells;
static HiddenShell *HiddenOf(uint8_t *shield) {
    for (auto &h : g_hiddenShells)
        if (h.shield == shield) return &h;
    return nullptr;
}
void ShellCloak(uint8_t *shield, uint8_t *thing, bool cloaked) {   // (also psiblade.cpp, for Tanagazj's own)
    uint8_t *fx = *(uint8_t **)(shield + 0x10);
    uint8_t *shell = fx ? *(uint8_t **)(fx + 0xa4) : nullptr;
    uint8_t *parent = thing ? *(uint8_t **)(thing + 0x2a0) : nullptr;
    HiddenShell *h = HiddenOf(shield);
    if (cloaked) {
        if (fx) *(uint32_t *)(fx + 0x50) |= 2;
        if (h || !shell || !parent) return;
        for (int i = 0; i < 4; i++)
            if (*(uint8_t **)(parent + 4 + 4 * i) == shell) {
                *(uint8_t **)(parent + 4 + 4 * i) = nullptr;
                g_hiddenShells.push_back({shield, parent, shell, i});
                Log("shield: %p cloaked, shell detached (slot %d)", shield, i);
                return;
            }
    } else if (h) {
        if (fx) *(uint32_t *)(fx + 0x50) &= ~2u;
        if (*(uint8_t **)(h->parent + 4 + 4 * h->slot) == nullptr) *(uint8_t **)(h->parent + 4 + 4 * h->slot) = h->shell;
        Log("shield: %p uncloaked, shell attached again", shield);
        g_hiddenShells.erase(g_hiddenShells.begin() + (h - g_hiddenShells.data()));
    }
}
void ShellForget(uint8_t *shield) {
    if (HiddenShell *h = HiddenOf(shield)) g_hiddenShells.erase(g_hiddenShells.begin() + (h - g_hiddenShells.data()));
}
static bool Cloaked(const DragoonShield *sh) { return ThingCloaked(sh->thing, sh->player); }
static void ShieldCombat(DragoonShield *sh) {
    static auto on = (void(__thiscall *)(uint8_t *))SymAddr("?On@cProtossShield@@QAEXXZ");
    sh->lastCombat = GetTickCount();
    if (on && (*(uint32_t *)sh->obj & 2) == 0 && (*(uint32_t *)sh->obj & 1)) {
        on(sh->obj);
        // Nova's shield coming on: the Zealot's shieldon (user; Sounds\custom\ghostpc\psiblade_shieldon.wav)
        if (sh->player) CustomPlayOnce("ghostpc/psiblade_shieldon", (float *)(sh->thing + 0xb0));
    }
}
static std::vector<DragoonShield *> g_shields;
static DragoonShield *ShieldOf(uint8_t *t) {
    for (DragoonShield *sh : g_shields)
        if (sh->thing == t) return sh;
    return nullptr;
}
static void DragoonShields() {
    static auto create = (void(__thiscall *)(uint8_t *, uint8_t *, float, float))SymAddr("?Create@cProtossShield@@QAEXPAVtActor@@MM@Z");
    static auto update = (void(__thiscall *)(uint8_t *))SymAddr("?Update@cProtossShield@@QAEXXZ");
    static auto setAmount = (void(__thiscall *)(uint8_t *, float, int))SymAddr("?SetAmount@cProtossShield@@QAEXMH@Z");
    static const float *dt = (const float *)(SymAddr("?stdTime@@3VtStdTime@@A") + 0x30);
    static uint8_t *world;
    if (LevelWorld() != world) {   // (the old level's effects went with it)
        world = LevelWorld();
        for (DragoonShield *sh : g_shields) delete sh;
        g_shields.clear();
        g_hiddenShells.clear();
    }
    for (size_t k = 0; k < g_shields.size();)   // (its thing is gone: quitting, or removed; a player's is not tracked
                                                // there, PlayerShieldOff and the level change remove it)
        if (!g_shields[k]->player && !StillThere(g_shields[k]->thing)) ShellForget(g_shields[k]->obj), delete g_shields[k], g_shields.erase(g_shields.begin() + k);
        else k++;
    if (!create || !update || !setAmount) return;
    std::vector<uint8_t *> shielded(g_dragoons);
    shielded.insert(shielded.end(), g_reavers.begin(), g_reavers.end());   // (the Reaver too, user)
    for (uint8_t *t : shielded) {
        if (ShieldOf(t)) continue;
        float amount = *(float *)(t + 0x374);
        if (!(amount > 0.f && amount < 10000.f)) amount = 80.f;
        DragoonShield *sh = new DragoonShield();
        sh->thing = t;
        sh->combatRegen = IsReaver(t);
        create(sh->obj, t, amount, amount);
        // Create caps the amount at the maximum (+0xC) before setting it: a fresh shield started at 0 and took 40 s to
        // charge
        *(float *)(sh->obj + 8) = amount;
        g_shields.push_back(sh);
        if (getenv("GHOSTPC_LOG_VEHICLE")) {
            uint8_t *fx = *(uint8_t **)(sh->obj + 0x10);
            Log("vehicle: %.32s %p shield %.0f, shell model %p", (char *)(t + 0xc4), t, amount, fx ? *(void **)(fx + 0xa4) : nullptr);
            static auto markerPos = (int(__thiscall *)(void *, unsigned char, unsigned short, float *, float *))SymAddr(
                "?GetMarkerPos@cModelInst@@QAEHEGPAUXGVECTOR3@@PAUXGMATRIX@@@Z");
            for (int ty : {2, 27, 0x13, 0x15})
                for (int i = 0; i < 6 && markerPos; i++) {
                    float q[3];
                    if (markerPos(*(void **)(t + 0x2a0), (unsigned char)ty, (unsigned short)i, q, (float *)(t + 0x80)))
                        Log("vehicle:   marker %d #%d at %+.2f %+.2f %+.2f from its origin", ty, i, q[0] - ((float *)(t + 0xb0))[0],
                            q[1] - ((float *)(t + 0xb0))[1], q[2] - ((float *)(t + 0xb0))[2]);
                }
        }
    }
    // a dead Dragoon stops where it fell (user: it slid like on ice): the Goliath's walk sets its friction to 0 and
    // carries it with its velocity (StandControlsUpdate: SetFriction(0) while moving, 4 when stopped), so a Dragoon
    // killed mid-stride kept sliding
    static auto getVel = (void(__thiscall *)(uint8_t *, float *))SymAddr("?GetVelocity@tThing@@QAEXPAUXGVECTOR3@@@Z");
    static auto setVel = (void(__thiscall *)(uint8_t *, const float *))SymAddr("?SetVelocity@tThing@@QAEXPBUXGVECTOR3@@@Z");
    static auto setFriction = (void(__thiscall *)(uint8_t *, float))SymAddr("?SetFriction@tThing@@QAEXM@Z");
    for (uint8_t *t : g_dragoons) {
        if (!(*(uint32_t *)(t + 0x360) & 0x10000000) || !getVel || !setVel || !setFriction) continue;
        float v[3];
        getVel(t, v);
        v[0] = v[1] = 0.f;
        setVel(t, v);
        setFriction(t, 4.f);
    }
    for (DragoonShield *sh : g_shields) {
        if (*(uint32_t *)(sh->thing + 0x360) & 0x10000000) continue;   // dead
        float &amount = *(float *)(sh->obj + 8), max = *(float *)(sh->obj + 0xc);
        float rate = sh->combatRegen ? (GetTickCount() - sh->lastCombat < 8000 ? 1.f : 3.f) : (GetTickCount() - sh->lastHit > 5000 ? 2.f : 0.f);
        if (amount < max && rate > 0.f && dt && *dt > 0.f && *dt < 0.25f)
            setAmount(sh->obj, amount + rate * *dt > max ? max : amount + rate * *dt, 0);
        static auto off = (void(__thiscall *)(uint8_t *, int))SymAddr("?Off@cProtossShield@@QAEXH@Z");
        if (off && !(*(uint32_t *)sh->obj & 1) && GetTickCount() - sh->lastCombat > 8000) off(sh->obj, 0);
        update(sh->obj);
        ShellCloak(sh->obj, sh->thing, Cloaked(sh));
        if (getenv("GHOSTPC_SHIELDS_ON") && g_frame > 200) ShieldCombat(sh);   // debug: every shield on
        if (getenv("GHOSTPC_LOG_SHIELD") && g_frame % 30 == 0) {   // debug: a shield's state, and the model's attachment slot 0
            uint8_t *inst = *(uint8_t **)(sh->thing + 0x2a0), *fx = *(uint8_t **)(sh->obj + 0x10);
            Log("shield: %.24s flags %08x amount %.1f of %.1f, effect %p (shell %p), model slot 0 %p", (char *)(sh->thing + 0xc4),
                *(uint32_t *)sh->obj, amount, max, fx, fx ? *(void **)(fx + 0xa4) : nullptr, inst ? *(void **)(inst + 4) : nullptr);
        }
    }
}
bool PlayerShieldOn(uint8_t *player, float max) {   // psiblade.cpp
    static auto create = (void(__thiscall *)(uint8_t *, uint8_t *, float, float))SymAddr("?Create@cProtossShield@@QAEXPAVtActor@@MM@Z");
    if (!player || !create || ShieldOf(player)) return false;
    DragoonShield *sh = new DragoonShield();
    sh->thing = player;
    sh->combatRegen = sh->player = true;
    create(sh->obj, player, max, max);
    *(float *)(sh->obj + 8) = max;   // (Create starts it empty)
    g_shields.push_back(sh);
    uint8_t *fx = *(uint8_t **)(sh->obj + 0x10);
    Log("vehicles: player %p shield %.0f, shell model %p", player, max, fx ? *(void **)(fx + 0xa4) : nullptr);
    return true;
}
bool PlayerShieldState(uint8_t *player, float *amount, float *max) {   // psiblade.cpp: the shield bar
    DragoonShield *sh = player ? ShieldOf(player) : nullptr;
    if (!sh) return false;
    *amount = *(float *)(sh->obj + 8);
    *max = *(float *)(sh->obj + 0xc);
    return true;
}
void PlayerShieldOff(uint8_t *player) {   // psiblade.cpp: the blade is taken away
    static auto destroy = (void(__thiscall *)(uint8_t *))SymAddr("?Destroy@cProtossShield@@QAEXXZ");
    for (size_t k = 0; k < g_shields.size(); k++)
        if (g_shields[k]->thing == player) {
            ShellCloak(g_shields[k]->obj, player, false);   // (its shell back on her model before it is freed)
            if (destroy) destroy(g_shields[k]->obj);
            delete g_shields[k];
            g_shields.erase(g_shields.begin() + k);
            Log("vehicles: player %p shield removed", player);
            return;
        }
}
static void DragoonFired(uint8_t *thing) {
    if (DragoonShield *sh = ShieldOf(thing)) ShieldCombat(sh);
}
typedef float(__thiscall *DamageFn)(uint8_t *, uint8_t *, int, int, float, float *, float *, unsigned, int, float, int *);
static DamageFn g_actorDamage;
// The Purifier's damage (user: it never died; damage should take its shield first, then its health, as with the
// Zealot). tActor::Damage hands the AI's Damaged (vtable +0x190) the amount after armour (melee: 80 x 0.35 = 28, its
// blocking factor) and applies what it returns; cAIProtossPurifier::Damaged returned 0 for every melee hit with its
// shield (AI +0x554, amount +0x55C) still at 90. Its Damaged is wrapped: the game's still runs (reactions,
// awareness), then the hit is taken off the shield (SetAmount, Hit for the flash) and the rest goes to health.
typedef float(__thiscall *AIDamagedFn)(uint8_t *, uint8_t *, int, int, float, unsigned *, float *, float *, int, float);
static AIDamagedFn g_purifierDamaged;
static float __fastcall PurifierDamagedLog(uint8_t *ai, void *, uint8_t *src, int source, int type, float amount, unsigned *fl,
                                          float *pos, float *dir, int region, float f) {
    static auto setAmount = (void(__thiscall *)(uint8_t *, float, int))SymAddr("?SetAmount@cProtossShield@@QAEXMH@Z");
    static auto hit = (void(__thiscall *)(uint8_t *, float, float *))SymAddr("?Hit@cProtossShield@@QAEXMPAUXGVECTOR3@@@Z");
    float before = *(float *)(ai + 0x55c);
    float r = g_purifierDamaged(ai, src, source, type, amount, fl, pos, dir, region, f);
    if (source != 1) return r;   // (only melee: gunfire works, with its own back / critical hit rules)
    float shield = before, rest = amount;
    if (amount > 0.f && shield > 0.f && setAmount) {
        float take = amount < shield ? amount : shield;
        setAmount(ai + 0x554, shield - take, 0);
        *(float *)(ai + 0x55c) = shield - take;   // (SetAmount left it at 90: the shield's own flags refuse it)
        if (hit) hit(ai + 0x554, take, pos);
        rest = amount - take;
    }
    if (getenv("GHOSTPC_LOG_VEHICLE"))
        Log("vehicle:   cAIProtossPurifier::Damaged(source %d, type %d, %.1f) -> game %.1f; shield %.1f -> %.1f (flags %08x, max %.1f), health takes %.1f",
            source, type, amount, r, before, *(float *)(ai + 0x55c), *(uint32_t *)(ai + 0x554), *(float *)(ai + 0x560), rest);
    return rest;
}
static void HookPurifierDamagedLog() {
    uint32_t *vt = (uint32_t *)SymAddr("??_7cAIProtossPurifier@@6B@");
    if (!vt) return;
    g_purifierDamaged = (AIDamagedFn)vt[0x190 / 4];
    DWORD o;
    VirtualProtect(&vt[0x190 / 4], 4, PAGE_EXECUTE_READWRITE, &o);
    vt[0x190 / 4] = (uint32_t)PurifierDamagedLog;
    VirtualProtect(&vt[0x190 / 4], 4, o, &o);
}

// The Dragoon's shot (user's balance, for the AI's Dragoons and the vehicle alike): proj_dragoon has no PROJECT_DAMAGE
// (the projectile default, 10) and its explosion exp_dragoonProj's damage effect dragoonProjDam (game.nfx) does 10
// within 3 m, sometimes several times at once to one unit. Here a proj_dragoon hit does 40 and its explosion 20, once
// per explosion and unit.
static const float DRAGOON_SHOT_DAMAGE = 40.f, DRAGOON_SPLASH_DAMAGE = 20.f;
static float DragoonShotDamage(uint8_t *a, uint8_t *src, float amount) {
    if (!src) return amount;
    const char *name = (const char *)(src + 0xc4);
    if (!_stricmp(name, "proj_dragoon")) return DRAGOON_SHOT_DAMAGE;
    if (_stricmp(name, "exp_dragoonProj")) return amount;
    struct Hit { uint8_t *src, *victim; DWORD at; };
    static std::vector<Hit> hits;
    DWORD now = GetTickCount();
    hits.erase(std::remove_if(hits.begin(), hits.end(), [now](const Hit &h) { return now - h.at > 1500; }), hits.end());
    for (const Hit &h : hits)
        if (h.src == src && h.victim == a) return 0.f;   // (this explosion already hit it)
    hits.push_back({src, a, now});
    return DRAGOON_SPLASH_DAMAGE;
}
// The Goliath's chaingun (user's balance): 5 per bullet instead of 10 for the base AI Goliath (template "Goliath",
// cAITerranGoliath::FireMachineGun from its attack's keyframes: FireInstantProjectile, 10, source 2) and the player
// vehicle (Goliath_player, cGoliathControl::FireChaingun: gameProjectile_InstantFire, 10, source 2). The bosses keep
// 10: Goliath_232 (cAITerranGoliathBoss::FireGun1 / 2) and the cAIBossGoliath bosses; so do the other AI variants
// (Goliath_elite, _hauler, _rebel), which are other templates. Rockets (hit by the missile) and the crush (source 8)
// are not bullets.
static float GoliathBulletDamage(uint8_t *src, int source, float amount) {
    if (!src || source != 2 || amount != 10.f) return amount;
    const char *name = (const char *)(src + 0xc4);
    return !_stricmp(name, "Goliath") || !_stricmp(name, "Goliath_player") ? 5.f : amount;
}
// Diagnosis (the hellfire's zergling flies off spinning, whatever the seekers' force): the first 4 actors a seeker hits
// in a level are logged for 3 s: each seeker hit (damage type, amount, direction, region, force), then every 3 frames the
// actor's position, facing, dead flag, its walker controller mode (AI +0x374) and AI state (+0x18).
static bool g_inSeekerDamage;   // SeekerDamage is running: ImpulseHook skips the blast on dead things
struct SeekerVictim { unsigned guid; UINT from; };
static SeekerVictim g_seekerVictims[4];
static int g_nSeekerVictims, g_seekerHitLogs;
static void SeekerHit(uint8_t *a, int type, float amount, const float *dir, int region, float force) {
    unsigned guid = *(unsigned *)(a + 0x34);
    int i = 0;
    while (i < g_nSeekerVictims && g_seekerVictims[i].guid != guid) i++;
    if (i == g_nSeekerVictims) {
        if (g_nSeekerVictims == 4) return;
        g_seekerVictims[g_nSeekerVictims++] = {guid, g_frame};
    }
    static bool on = getenv("GHOSTPC_LOG_SEEKER") != nullptr;
    if (on && g_seekerHitLogs++ < 60)
        Log("seeker: frame %u hits %.24s (guid %u): type %d amount %.0f dir %.2f %.2f %.2f region %d force %.1f dead %d", g_frame,
            (char *)(a + 0xc4), guid, type, amount, dir ? dir[0] : 0.f, dir ? dir[1] : 0.f, dir ? dir[2] : 0.f, region, force,
            (*(uint32_t *)(a + 0x360) & 0x10000000) != 0);
}
// ... and what moves it: tThing::SetVelocity / ApplyImpulse / ApplyAngularImpulse are detoured to log calls on a tracked
// victim (or an actor set faster than 12 m/s) with the caller (80 lines a level).
static bool IsSeekerVictim(const uint8_t *t) {
    for (int i = 0; i < g_nSeekerVictims; i++)
        if (g_seekerVictims[i].guid == *(const unsigned *)(t + 0x34) && g_frame - g_seekerVictims[i].from <= 150) return true;
    return false;
}
static int g_moveLogs;
static void LogMove(const char *what, uint8_t *t, const float *v, void *ret, int a, int b) {
    static bool on = getenv("GHOSTPC_LOG_SEEKER") != nullptr;
    if (!on || !t || !v || g_moveLogs >= 80) return;
    float m = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!IsSeekerVictim(t) && !(*(int *)(t + 0x10) == 0 && !strcmp(what, "SetVelocity") && m > 12.f)) return;
    char caller[128];
    SymName((uint32_t)ret, caller, sizeof caller);
    g_moveLogs++;
    Log("seeker-move: frame %u %s %.24s (guid %u dead %d) %.2f %.2f %.2f |%.1f| args %d %d from %s", g_frame, what, (char *)(t + 0xc4),
        *(unsigned *)(t + 0x34), (*(uint32_t *)(t + 0x360) & 0x10000000) != 0, v[0], v[1], v[2], m, a, b, caller);
}
typedef void(__thiscall *SetVelFn)(uint8_t *, const float *);
typedef void(__thiscall *ImpulseFn)(uint8_t *, const float *, int, int, const float *);
typedef void(__thiscall *AngImpulseFn)(uint8_t *, const float *, int);
static SetVelFn g_setVel;
static ImpulseFn g_impulse;
static AngImpulseFn g_angImpulse;
static void __fastcall SetVelHook(uint8_t *t, void *, const float *v) {
    LogMove("SetVelocity", t, v, _ReturnAddress(), 0, 0);
    g_setVel(t, v);
}
// The cause (found with this log): simCollision_DamageThingsInSphere damages the things in the sphere first, then blasts
// them (simCollision_CalcBlastForces with argument 11, the force -> ApplyImpulse + ApplyAngularImpulse). A seeker's
// 70-80 fire kills a zergling in the damage pass, so the blast lands on the fresh corpse, which has no friction: four
// seekers in one frame gave it 4 x 252 (velocity 29, -10, 33 m/s and spinning) and it flew to the skybox. While a
// seeker's damage runs, the blast skips dead things.
// The same for a melee hit (user: a zergling the Psi Blade hit was launched into the distance): cPlayerControl::
// MeleeHitCheck damages, then pushes the target (ApplyImpulse, ApplyAngularImpulse); the blade's 80 kills a zergling,
// so the push landed on the fresh corpse. A push from MeleeHitCheck skips a dead target too.
static bool SkipSeekerBlast(const uint8_t *t, void *caller = nullptr) {
    if (!t || !(*(const uint32_t *)(t + 0x360) & 0x10000000)) return false;
    static uint8_t *melee = (uint8_t *)SymAddr("?MeleeHitCheck@cPlayerControl@@AAEHI@Z");
    return g_inSeekerDamage || (melee && (uint8_t *)caller >= melee && (uint8_t *)caller < melee + 0xa00);
}
static void __fastcall ImpulseHook(uint8_t *t, void *, const float *v, int a, int b, const float *at) {
    LogMove("ApplyImpulse", t, v, _ReturnAddress(), a, b);
    static bool on = getenv("GHOSTPC_LOG_SEEKER") != nullptr;
    if (on && t && IsSeekerVictim(t) && g_moveLogs < 80)
        g_moveLogs++, Log("seeker-move: impulse on %.24s: in seeker damage %d, dead %d -> %s", (char *)(t + 0xc4), g_inSeekerDamage,
                          (*(uint32_t *)(t + 0x360) & 0x10000000) != 0, SkipSeekerBlast(t) ? "skipped" : "applied");
    if (SkipSeekerBlast(t, _ReturnAddress())) return;
    g_impulse(t, v, a, b, at);
}
static void __fastcall AngImpulseHook(uint8_t *t, void *, const float *v, int a) {
    LogMove("ApplyAngularImpulse", t, v, _ReturnAddress(), a, 0);
    if (SkipSeekerBlast(t, _ReturnAddress())) return;
    g_angImpulse(t, v, a);
}

static void SeekerTrack() {
    static unsigned lastNext;
    static unsigned *nextGuid = (unsigned *)SymAddr("?nextGUID@tThing@@2IA");
    if (nextGuid && *nextGuid < lastNext) g_nSeekerVictims = 0, g_seekerHitLogs = 0, g_moveLogs = 0;   // a new level
    if (nextGuid) lastNext = *nextGuid;
    static auto findT = (uint8_t *(__cdecl *)(unsigned))SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    static bool on = getenv("GHOSTPC_LOG_SEEKER") != nullptr;
    for (int i = 0; on && findT && i < g_nSeekerVictims; i++) {
        UINT age = g_frame - g_seekerVictims[i].from;
        if (age > 90 || age % 3) continue;
        uint8_t *t = findT(g_seekerVictims[i].guid);
        if (!t) { Log("seeker: guid %u gone at +%u", g_seekerVictims[i].guid, age); g_seekerVictims[i].from = 0; continue; }
        const float *q = (const float *)(t + 0xb0), *f = (const float *)(t + 0x90);
        uint8_t *ai = *(int *)(t + 0x10) == 0 ? *(uint8_t **)(t + 0x3c4) : nullptr;
        static auto getVel = (void(__thiscall *)(uint8_t *, float *))SymAddr("?GetVelocity@tThing@@QAEXPAUXGVECTOR3@@@Z");
        float v[3] = {0, 0, 0};
        if (getVel && *(void **)(t + 0x158)) getVel(t, v);
        Log("seeker: +%u %.24s at %.1f %.1f %.1f velocity %.1f %.1f %.1f facing %.2f %.2f %.2f dead %d flags %08x mode %d state %d", age,
            (char *)(t + 0xc4), q[0], q[1], q[2], v[0], v[1], v[2], f[0], f[1], f[2], (*(uint32_t *)(t + 0x360) & 0x10000000) != 0,
            *(uint32_t *)(t + 0x360), ai ? *(int *)(ai + 0x374) : -1, ai ? *(int *)(ai + 0x18) : -1);
    }
}

static float __fastcall ActorDamage(uint8_t *a, void *, uint8_t *src, int source, int type, float amount, float *pos, float *dir,
                                    unsigned u, int region, float f, int *out) {
    // debug (GHOSTPC_TEST_GOD): no damage to a player-type thing (type 7: Nova, Echo, and the vehicles they ride: a
    // destroyed turret kills its rider). levelscript.cpp's health restore missed one-hit deaths and riders.
    static bool god = getenv("GHOSTPC_TEST_GOD") != nullptr;
    if (god && a && *(int *)(a + 0x10) == 7) return 0.f;
    amount = GoliathBulletDamage(src, source, amount);
    extern float PsiBladeMeleeDamage(uint8_t *src, float amount, void *caller);   // psiblade.cpp: the blade's hits
    amount = PsiBladeMeleeDamage(src, amount, _ReturnAddress());
    float shot = DragoonShotDamage(a, src, amount);
    if (shot <= 0.f && amount > 0.f) return 0.f;   // (a repeat of a Dragoon explosion's splash)
    amount = shot;
    if (amount > 0.f && src && src != a)
        if (DragoonShield *mine = ShieldOf(src)) ShieldCombat(mine);   // hitting something is combat too
    if (amount > 0.f)
        if (DragoonShield *sh = ShieldOf(a)) {
            static auto setAmount = (void(__thiscall *)(uint8_t *, float, int))SymAddr("?SetAmount@cProtossShield@@QAEXMH@Z");
            static auto hit = (void(__thiscall *)(uint8_t *, float, float *))SymAddr("?Hit@cProtossShield@@QAEXMPAUXGVECTOR3@@@Z");
            ShieldCombat(sh);
            float have = *(float *)(sh->obj + 8);
            sh->lastHit = GetTickCount();
            if (have > 0.f && setAmount) {
                float taken = amount < have ? amount : have;
                setAmount(sh->obj, have - taken, 0);
                if (hit) hit(sh->obj, taken, pos);
                if (getenv("GHOSTPC_LOG_VEHICLE")) Log("vehicle: shield takes %.1f of %.1f (%.1f left)", taken, amount, have - taken);
                amount -= taken;
                if (amount <= 0.f) return 0.f;
            }
        }
    if (getenv("GHOSTPC_LOG_VEHICLE") && src && (*(int *)(a + 0x10) == 0 || !_strnicmp((char *)(src + 0xc4), "proj_dragoon", 12) ||
                                                  !_strnicmp((char *)(src + 0xc4), "exp_dragoon", 11) || !_strnicmp((char *)(src + 0xc4), "Goliath", 7)))   // debug: damage, and from what
        Log("vehicle: %.32s takes %.0f (source %d) from %.32s", (char *)(a + 0xc4), amount, source, (char *)(src + 0xc4));
    if (src && !_stricmp((char *)(src + 0xc4), "exp_Seeker")) {
        SeekerHit(a, type, amount, dir, region, f);
        // the zergling that flew off (user; SeekerTrack's log): two seekers hit it in one frame, the first killed it and
        // the second hit the dead body, which then flew a 20 m high arc. A seeker no longer hits a dead actor.
        if (*(uint32_t *)(a + 0x360) & 0x10000000) return 0.f;
    }
    uint8_t *pai = !_stricmp((char *)(a + 0xc4), "purifier") ? *(uint8_t **)(a + 0x3c4) : nullptr;   // (debug log below)
    float shieldBefore = pai ? *(float *)(pai + 0x55c) : -1.f;
    float hpBefore = *(float *)(a + 0x36c);
    float dealt = g_actorDamage(a, src, source, type, amount, pos, dir, u, region, f, out);
    static bool logDamage = getenv("GHOSTPC_LOG_DAMAGE") != nullptr;   // debug: every hit, and the health it took
    if (logDamage)
        Log("damage: %.24s takes %.1f (source %d type %d) from %.24s: health %.1f -> %.1f (dealt %.1f)", (char *)(a + 0xc4),
            amount, source, type, src ? (char *)(src + 0xc4) : "-", hpBefore, *(float *)(a + 0x36c), dealt);
    if (getenv("GHOSTPC_LOG_VEHICLE") && src && *(int *)(a + 0x10) == 0) {   // (debug: its armour per damage type, +0x3AC)
        static int armorLogs;
        const float *ar = (const float *)(a + 0x3ac);
        if (armorLogs < 8) armorLogs++, Log("vehicle:   %.24s armour by type: %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f", (char *)(a + 0xc4), ar[0], ar[1], ar[2],
                                          ar[3], ar[4], ar[5], ar[6], ar[7]);
    }
    if (getenv("GHOSTPC_LOG_VEHICLE") && src && *(int *)(a + 0x10) == 0)   // (debug: what the hit did)
        Log("vehicle:   -> dealt %.1f, %.32s health %.1f of %.1f%s, shield %.1f -> %.1f (type %d, region %d, flags %08x)", dealt, (char *)(a + 0xc4),
            *(float *)(a + 0x36c), *(float *)(a + 0x370), (*(uint32_t *)(a + 0x360) & 0x10000000) ? " (dead)" : "", shieldBefore,
            pai ? *(float *)(pai + 0x55c) : -1.f, type, region, *(uint32_t *)(a + 0x360));
    return dealt;
}

bool RidingVulture(int local);
// The Vulture's idle rumble (user: a quiet engine rumble, only while mounted): Sounds\custom\ghostpc\vulture_idle.wav
// looped for each local player riding a Vulture, from the frame she is on it until she is not (dismounted, killed) or
// the level changes. The game's own engine loop (lwalk_solid, playerVulture\lwalk_solid.wav) plays over it while it
// moves.
static void VultureIdleCheck() {
    static unsigned loop[2];
    static uint8_t *world;
    if (LevelWorld() != world) world = LevelWorld(), loop[0] = loop[1] = 0;   // (customsound.cpp ended them)
    for (int i = 0; i < 2; i++) {
        bool riding = RidingVulture(i);
        if (riding && !loop[i])
            if (uint8_t *bike = LocalPlayer(i)) loop[i] = CustomLoopStart("ghostpc/vulture_idle", (const float *)(bike + 0xb0), 1.f);
        if (!riding && loop[i]) CustomLoopStop(loop[i]), loop[i] = 0;
    }
}

// AI and vehicles (user: enemies should attack Nova in any vehicle): cAIInstance::IsValidTarget refuses a player thing
// whose actor flags (+0x360) have 0x400, which the player vehicles' ACTOR_FLAGS 1536 (Goliath_player, Vulture_player,
// Arclite_player) set; once a second it is cleared on every vehicle (a player thing driven by a control other than
// Nova's cPlayerControl). GhostPC's Reaver and Dragoon have it off in their templates.
// debug (GHOSTPC_WARP_VEHICLE="<frame> x y z yaw"): moves player 1's current thing (a driven vehicle: the vehicle) there at that
// frame (the level script's warp, tThing::JumpToMatrix; yaw as the level script takes it, 0 = +Y, 90 = -X)
static void DebugWarp() {
    static int at = -2;
    static float p[4];
    if (at == -2) {
        at = -1;
        if (const char *e = getenv("GHOSTPC_WARP_VEHICLE")) sscanf(e, "%d %f %f %f %f", &at, &p[0], &p[1], &p[2], &p[3]);
    }
    if ((int)g_frame != at) return;
    if (uint8_t *t = LocalPlayer(0)) {
        LevelWarpPlayer(t, p);
        Log("vehicles: debug warp %.24s to %.1f %.1f %.1f yaw %.0f", (char *)(t + 0xc4), p[0], p[1], p[2], p[3]);
    }
}
static void VehiclesTargetable() {
    DebugWarp();
    // Stealth (tPlayer +0x554, Nova's stealth meter: 0 = in plain sight, her 0.4 in normal light; cAIInstance::Search
    // raises the alarm by (1 - it)): a driven vehicle is in plain sight, 0, every frame. (Setting it to 1 hid the Reaver
    // completely, user: no reaction at all.)
    static uint32_t novaCtl = SymAddr("??_7cPlayerControl@@6B@");
    for (int i = 0; i < 2; i++) {
        uint8_t *ctl, *t = LocalPlayer(i, &ctl);
        if (!t || !ctl) continue;
        static int logs;
        if (*(uint32_t *)ctl == novaCtl) {
            if (logs < 2 && g_frame % 300 == 0) logs++, Log("vehicles: player %d on foot, visibility %.2f", i + 1, *(float *)(t + 0x554));
            continue;
        }
        if (logs < 4 && *(float *)(t + 0x554) > 0.f) logs++, Log("vehicles: %.24s stealth %.2f -> 0", (char *)(t + 0xc4), *(float *)(t + 0x554));
        *(float *)(t + 0x554) = 0.f;
    }
    static DWORD last;
    if (GetTickCount() - last < 1000) return;
    last = GetTickCount();
    static auto findT = (uint8_t *(__cdecl *)(unsigned))SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    static unsigned *next = (unsigned *)SymAddr("?nextGUID@tThing@@2IA");
    static uint32_t nova = SymAddr("??_7cPlayerControl@@6B@");
    for (unsigned g = 1; findT && next && g < *next && g < 100000; g++)
        if (uint8_t *t = findT(g))
            if (*(int *)(t + 0x10) == 7 && (*(uint32_t *)(t + 0x360) & 0x400)) {
                uint8_t *ctl = *(uint8_t **)(t + 0x3f8);
                if (ctl && *(uint32_t *)ctl != nova) *(uint32_t *)(t + 0x360) &= ~0x400u;
            }
}

// Echo on the Vulture (user: the rider is part of the Vulture's model, novaVulture.nod, Nova in her suit): while
// player 2 (Echo, nova_ice.nod) rides a Vulture its model instance shows novaVulture_echo.nod (the same model with Echo's
// NovaIce_* materials, scripts/make_echo_vulture.py; swapped as psiblade.cpp swaps Nova's: same bone count, no more
// materials, then cModelInst::Finalize for the shader table); put back when she is off it.
static void EchoVultureModel() {
    static auto loadModel = (uint8_t *(__cdecl *)(const char *, unsigned))SymAddr("?renderModel_Load@@YAPAVcModel@@PBDI@Z");
    static auto finalize = (void(__thiscall *)(uint8_t *, void *))SymAddr("?Finalize@cModelInst@@QAEXPAUXGMATRIX@@@Z");
    static uint8_t *bike, *inst, *orig, *echo;
    static uint8_t *world;
    if (LevelWorld() != world) world = LevelWorld(), bike = inst = orig = echo = nullptr;   // (a new level: all gone)
    static int side = getenv("GHOSTPC_ECHO_VULTURE_P1") ? 0 : 1;   // (debug: player 1 gets it, to see it in one-player runs)
    uint8_t *now = RidingVulture(side) ? LocalPlayer(side) : nullptr;
    if (now == bike) return;
    if (bike && inst && orig && !IsBadReadPtr(inst, 4) && *(uint8_t **)inst == echo) {   // off it: Nova's model back
        *(uint8_t **)inst = orig;
        if (finalize) finalize(inst, nullptr);
    }
    bike = inst = orig = echo = nullptr;
    if (!now || !loadModel) return;
    uint8_t *in = *(uint8_t **)(now + 0x2a0), *cur = in ? *(uint8_t **)in : nullptr;
    uint8_t *m = loadModel("novaVulture_echo.nod", 0);
    if (!cur || !m || m[9] != cur[9] || m[4] > cur[4]) {
        Log("vehicles: Echo's Vulture model not swapped (%p, bones %d / %d)", m, m ? m[9] : -1, cur ? cur[9] : -1);
        return;
    }
    bike = now, inst = in, orig = cur, echo = m;
    *(uint8_t **)in = m;
    if (finalize) finalize(in, nullptr);
    Log("vehicles: Echo rides a Vulture: novaVulture_echo.nod");
}

// Vulture self-righting (user: it sometimes flips over and rides upside down): a ridden Vulture whose up vector
// (tThing +0xA0) tilts past 60 degrees is spun back upright with tThing::SetRotVel (Havok's angular velocity, rad/s)
// about the axis from its up to the world's (its forward axis when it is upside down), at 3x the angle, until it is
// within about 25 degrees of level.
static void VultureUpright() {
    static auto setRotVel = (void(__thiscall *)(uint8_t *, const float *))SymAddr("?SetRotVel@tThing@@QAEXPBUXGVECTOR3@@@Z");
    static bool righting[2];
    for (int i = 0; i < 2; i++) {
        uint8_t *bike = RidingVulture(i) ? LocalPlayer(i) : nullptr;
        if (!bike || !setRotVel || !*(void **)(bike + 0x158)) { righting[i] = false; continue; }
        const float *up = (const float *)(bike + 0xa0), *fwd = (const float *)(bike + 0x90);
        if (!righting[i] && up[2] < 0.5f) {
            righting[i] = true;
            Log("vehicles: Vulture %d tipped over (up %.2f %.2f %.2f), righting it", i + 1, up[0], up[1], up[2]);
        }
        if (righting[i] && up[2] > 0.9f) righting[i] = false;
        if (!righting[i]) continue;
        float axis[3] = {up[1], -up[0], 0.f};   // up x (0, 0, 1)
        float len = sqrtf(axis[0] * axis[0] + axis[1] * axis[1]);
        if (len < 0.05f) {   // (upside down: roll about its forward axis)
            float f = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1]);
            if (f < 0.01f) continue;
            axis[0] = fwd[0] / f, axis[1] = fwd[1] / f, len = 1.f;
        } else
            axis[0] /= len, axis[1] /= len;
        float angle = acosf(std::max(-1.f, std::min(1.f, up[2])));
        float rate = std::min(6.f, angle * 3.f);
        float w[3] = {axis[0] * rate, axis[1] * rate, 0.f};
        setRotVel(bike, w);
    }
}

// Reaver crushing (user: a Zergling and a Hydralisk died just standing by it; then: driving into one did not crush it):
// cTankControl::Collide damages or gib-kills any actor or player its hull touches, moving or not (its damage from the
// contact's speed). The Reaver is moved by setting its physics matrix (TankUpdateControls), so its physics velocity is
// about 0 and a contact's speed tiny: the tank's crush never fit it. For a Reaver the contact callback does its own:
// while the stick drives it (g_reaverThrottle) a ground unit (an actor, at most 600 maximum health: no bosses) within
// 40 degrees of the way it drives is killed (collision damage, credited to the Reaver); other contacts do nothing.
typedef void(__thiscall *TankCollideFn)(uint8_t *, void *, int, uint8_t *);
static TankCollideFn g_tankCollide;
static void __fastcall ReaverCollide(uint8_t *ctl, void *, void *event, int a, uint8_t *other) {
    uint8_t *reaver = ctl ? *(uint8_t **)(ctl + 0x180) : nullptr;
    if (!IsReaver(reaver)) return g_tankCollide(ctl, event, a, other);
    // (a stick at least half way: user: turning with it a little back crushed a Zealot at its tail)
    if (!other || fabsf(g_reaverThrottle) < 0.5f || *(int *)(other + 0x10) != 0) return;
    if ((*(uint32_t *)(other + 0x360) & 0x10000000) || *(float *)(other + 0x370) > 600.f) return;   // (dead; big)
    const float *p = (const float *)(reaver + 0xb0), *q = (const float *)(other + 0xb0), *f = (const float *)(reaver + 0x90);
    float dx = q[0] - p[0], dy = q[1] - p[1];
    // (only what is in its path: within 40 degrees of the way it drives, user: a Zealot hitting its flank was crushed)
    float fl = sqrtf(f[0] * f[0] + f[1] * f[1]), dl = sqrtf(dx * dx + dy * dy);
    if (fl < 0.01f || dl < 0.01f || (f[0] * dx + f[1] * dy) / (fl * dl) * (g_reaverThrottle > 0.f ? 1.f : -1.f) < 0.766f) return;
    // (and at the end it drives with: its outer half along its length, from its model's bounds)
    uint8_t *inst = *(uint8_t **)(reaver + 0x2a0), *model = inst ? *(uint8_t **)inst : nullptr;
    if (model) {
        const float *b = (const float *)(model + 0xb0);
        float ly = (f[0] * dx + f[1] * dy) / fl;
        if (g_reaverThrottle > 0.f ? ly < 0.5f * b[4] : ly > 0.5f * b[1]) return;
    }
    typedef int(__thiscall *KillFn)(uint8_t *, uint8_t *, int, int, float, float *, float *, unsigned, int, float);
    static auto kill = (KillFn)SymAddr("?Kill@tActor@@QAEHPAVtThing@@W4eDamageSource@@W4eDamageType@@MPAUXGVECTOR3@@3IW4eHitRegionType@@M@Z");
    if (!kill) return;
    float dir[3] = {dx, dy, 0.f};
    kill(other, reaver, 8, 0, 1000.f, (float *)(other + 0xb0), dir, 0, 0, 0.f);
    Log("vehicles: Reaver crushed %.24s", (char *)(other + 0xc4));
}

// Every AI class (the game's vtables), for patching a cAIInstance virtual in all of them
static const char *const AI_CLASSES[] = {"cAIBaseBossBrain", "cAIBaseBossGoliath", "cAIBaseFloorGun", "cAIBaseFlyer", "cAIBaseVehicle", "cAIBaseWalker", "cAIBaseWanderer", "cAIBossBrain", "cAIBossGoliath", "cAIBossHauler", "cAIBossLurker", "cAIBossSniper", "cAIBossTanagazj", "cAIBossUltralisk", "cAICamera", "cAIChargePlayer", "cAICollideDetector", "cAIDemoAnims", "cAIDetectOverride", "cAIDetector", "cAIDoorScan", "cAIFightPath", "cAIFlightPath", "cAIGhostScan", "cAIHibernateControl", "cAIHibernateDistance", "cAIInstance", "cAIKillMe", "cAIMine", "cAIPause", "cAIProtossCannon", "cAIProtossCrystal", "cAIProtossDragoon", "cAIProtossHighTemplar", "cAIProtossObserver", "cAIProtossProbe2", "cAIProtossProbe", "cAIProtossPurifier", "cAIProtossPylon", "cAIProtossScout", "cAIProtossShieldBattery", "cAIProtossZealot", "cAIPsiDampener", "cAIReactToEvent", "cAISearchlight", "cAISendToPos", "cAISightOverride", "cAITerranFirebat", "cAITerranFlameTurret", "cAITerranGaussTurret", "cAITerranGhost", "cAITerranGhostClose", "cAITerranGoliath", "cAITerranGoliathBoss", "cAITerranLightInfantry", "cAITerranMarine", "cAITerranMarineSidekick", "cAITerranRebel", "cAITerranSCV121", "cAITerranSCV", "cAITerranScienceVessel", "cAITerranScientist", "cAITerranSiegeTank", "cAITerranVulture", "cAITestToggle", "cAITimedPatrol", "cAITurret", "cAITurretScan", "cAITurretSentry", "cAITurretWall", "cAIZergDrone", "cAIZergEgg", "cAIZergHydralisk", "cAIZergInfestedTerran", "cAIZergInfestedTerranEX", "cAIZergInfestedTerranGU", "cAIZergInfestedTerranNE", "cAIZergLarva2", "cAIZergLarva", "cAIZergLurkerTurret", "cAIZergMutalisk", "cAIZergOverlord", "cAIZergPustule", "cAIZergScourge", "cAIZergSpore", "cAIZergSporeColony", "cAIZergSunkenColony", "cAIZergTankCritter", "cAIZergling", "cAIZerglingRushing"};
// Scoring a driven vehicle: cAIInstance::SearchScore (vtable +0x128) is wrapped in every AI class that uses it; a
// driven vehicle's stealth (+0x554) is 0 (in plain sight) when it is scored, and a 0 score is replaced as below.
typedef float(__thiscall *SearchScoreFn)(void *, uint8_t *, unsigned);
static SearchScoreFn g_searchScore;
static float __fastcall SearchScoreHook(void *ai, void *, uint8_t *target, unsigned flags) {
    static uint32_t novaCtl = SymAddr("??_7cPlayerControl@@6B@");
    if (target && *(int *)(target + 0x10) == 7 && (target == LocalPlayer(0) || target == LocalPlayer(1))) {
        uint8_t *ctl = *(uint8_t **)(target + 0x3f8);
        if (ctl && *(uint32_t *)ctl != novaCtl) *(float *)(target + 0x554) = 0.f;   // (in plain sight)
    }
    float r = g_searchScore(ai, target, flags);
    // (still 0 for a driven vehicle: the game's FOVScore samples the target's sight points, which a vehicle without
    // sight markers fails; a vehicle within the AI's sight distance (+10 m for its size) that its line of sight test
    // reaches scores 1)
    if (r <= 0.f && target && *(int *)(target + 0x10) == 7 && (target == LocalPlayer(0) || target == LocalPlayer(1))) {
        uint8_t *ctl = *(uint8_t **)(target + 0x3f8), *me = *(uint8_t **)((uint8_t *)ai + 8);
        if (ctl && *(uint32_t *)ctl != novaCtl && me) {
            typedef float(__thiscall *SightFn)(void *);
            typedef int(__thiscall *LosFn)(void *, uint8_t *, float, int);
            void **vt = *(void ***)ai;
            float sight = ((SightFn)vt[0xe0 / 4])(ai);
            const float *a = (const float *)(me + 0xb0), *b = (const float *)(target + 0xb0);
            float d = sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
            if (d < sight + 10.f && ((LosFn)vt[0x144 / 4])(ai, target, 0.f, 0) == 1) r = 1.f;
        }
    }
    static bool log = getenv("GHOSTPC_LOG_LOS") != nullptr;
    if (log && IsReaver(target)) {
        static int logs;
        static DWORD last;
        if (logs < 120 && GetTickCount() - last > 200) {
            last = GetTickCount(), logs++;
            uint8_t *me = *(uint8_t **)((uint8_t *)ai + 8);
            Log("score: %.20s -> Reaver: %.3f (flags %x, visibility %.2f)", me ? (char *)(me + 0xc4) : "?", r, flags, *(float *)(target + 0x554));
        }
    }
    return r;
}
// Alarm (user: units were only slightly alarmed by a giant Reaver in front of them, "this isn't Nova crouching in a
// corner"): cAIInstance::Search raises an AI's alarm with ALAdd (vtable +0x368) by (1 - stealth) x a distance term
// each time it can target a player, so a vehicle built up alarm as slowly as Nova in the open. For a driven vehicle
// the amount is ten times as much: an AI that sees one goes from noticing to attacking almost at once.
static const float VEHICLE_ALARM = 10.f;
typedef void(__thiscall *ALAddFn)(void *, float, uint8_t *);
static ALAddFn g_alAdd;
static void __fastcall ALAddHook(void *ai, void *, float amount, uint8_t *thing) {
    static uint32_t novaCtl = SymAddr("??_7cPlayerControl@@6B@");
    if (amount > 0.f && thing && *(int *)(thing + 0x10) == 7 && (thing == LocalPlayer(0) || thing == LocalPlayer(1))) {
        uint8_t *ctl = *(uint8_t **)(thing + 0x3f8);
        if (ctl && *(uint32_t *)ctl != novaCtl) amount *= VEHICLE_ALARM;
    }
    g_alAdd(ai, amount, thing);
}
// A driven player vehicle (the target an AI attacks), or null.
static uint8_t *DrivenVehicle(uint8_t *t) {
    static uint32_t novaCtl = SymAddr("??_7cPlayerControl@@6B@");
    if (!t || *(int *)(t + 0x10) != 7 || (t != LocalPlayer(0) && t != LocalPlayer(1))) return nullptr;
    uint8_t *ctl = *(uint8_t **)(t + 0x3f8);
    return ctl && *(uint32_t *)ctl != novaCtl ? t : nullptr;
}
// How far a point is from a vehicle's hull, across the ground: its model's bounds (cModel +0xB0, min xyz / max xyz) as a
// box in its own frame (matrix rows +0x80 side, +0x90 forward, +0xB0 position). The Reaver is about 4.5 m wide and
// twice that long, so one radius around its centre left a unit at its tail 9-10 m from the centre (user: the Zealot ran
// in place there).
static float HullDistance(uint8_t *t, const float *q) {
    const float *p = (const float *)(t + 0xb0), *sx = (const float *)(t + 0x80), *fy = (const float *)(t + 0x90);
    float dx = q[0] - p[0], dy = q[1] - p[1];
    uint8_t *inst = *(uint8_t **)(t + 0x2a0), *model = inst ? *(uint8_t **)inst : nullptr;
    if (!model) return sqrtf(dx * dx + dy * dy) - *(float *)(t + 0x50);
    const float *b = (const float *)(model + 0xb0);
    float lx = dx * sx[0] + dy * sx[1], ly = dx * fy[0] + dy * fy[1];
    float ox = lx < b[0] ? b[0] - lx : lx > b[3] ? lx - b[3] : 0.f;
    float oy = ly < b[1] ? b[1] - ly : ly > b[4] ? ly - b[4] : 0.f;
    return sqrtf(ox * ox + oy * oy);
}
// Melee reach on vehicles (user: the Zealot ran up to the Reaver and ran in place): cAIBaseWalker::CanAttackFrom(target,
// from) wants the distance from there to the target's centre within the AI's AttackRange (vtable +0x9B8: the Zealot's
// 2.4 m) and at most 10 m, then a clear line (vtable +0x150). Against a driven vehicle that distance is to its hull
// (HullDistance) instead. Each class keeps its own CanAttackFrom (by vtable); the game's answer stands when it is yes.
typedef int(__thiscall *CanAttackFn)(void *, uint8_t *, float *);
static std::map<uint32_t, CanAttackFn> g_canAttack;   // vtable -> its CanAttackFrom
static int __fastcall CanAttackHook(uint8_t *ai, void *, uint8_t *target, float *from) {
    void **vt = *(void ***)ai;
    auto it = g_canAttack.find((uint32_t)vt);
    if (it == g_canAttack.end()) return 0;
    int r = it->second(ai, target, from);
    if (r || !from || !DrivenVehicle(target)) return r;
    float range = ((float(__thiscall *)(void *))vt[0x9b8 / 4])(ai);
    float d = HullDistance(target, from);
    if (getenv("GHOSTPC_LOG_MELEE")) {
        static DWORD last;
        if (GetTickCount() - last > 500)
            last = GetTickCount(), Log("melee: %.24s at %.1f m from the vehicle hull (range %.1f)", (char *)(*(uint8_t **)(ai + 8) + 0xc4), d, range);
    }
    if (d > range || d > 10.f) return 0;
    return ((int(__thiscall *)(void *, uint8_t *, float *, int, int))vt[0x150 / 4])(ai, target, from, 0, 1) ? 1 : 0;
}
static void HookCanAttack() {
    uint32_t base = SymAddr("?CanAttackFrom@cAIBaseWalker@@UAEHPAVtActor@@PAUXGVECTOR3@@@Z");
    int n = 0;
    for (const char *c : AI_CLASSES) {
        char sym[96], nm[160];
        _snprintf(sym, sizeof sym, "??_7%s@@6B@", c);
        uint32_t *vt = (uint32_t *)SymAddr(sym);
        if (!vt || IsBadReadPtr(vt + 0x9c0 / 4, 4) || g_canAttack.count((uint32_t)vt)) continue;
        uint32_t f = vt[0x9c0 / 4];
        SymName(f, nm, sizeof nm);
        if (f != base && !strstr(nm, "CanAttackFrom")) continue;   // (a class whose vtable is shorter)
        g_canAttack[(uint32_t)vt] = (CanAttackFn)f;
        DWORD o;
        VirtualProtect(vt + 0x9c0 / 4, 4, PAGE_READWRITE, &o);
        vt[0x9c0 / 4] = (uint32_t)CanAttackHook;
        VirtualProtect(vt + 0x9c0 / 4, 4, o, &o);
        n++;
    }
    Log("vehicles: AI CanAttackFrom wrapped in %d classes (melee reaches vehicle hulls)", n);
}
// The Zealot's swing (user: only its leap hit the Reaver, its normal swings never did; it ran in place by it):
// cAIProtossZealot::ASUpdateCombatRanged swings only within 4.25 m of its target's centre (fld [0x786F50], a shared
// constant), else it ends the attack and chases again. Nothing reaches within 4.25 m of the Reaver's centre. That one
// load reads g_zealotReach instead, which the wrapper sets per call: 4.25 normally; against a driven vehicle, the
// distance to its centre less the distance to its hull, plus 2 m (in reach 2 m from the hull).
static float g_zealotReach = 4.25f;
typedef void(__thiscall *AIStateFn)(uint8_t *);
static AIStateFn g_zealotCombat;
static void __fastcall ZealotCombatHook(uint8_t *ai, void *) {
    static auto findT = (uint8_t *(__cdecl *)(unsigned))SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    g_zealotReach = 4.25f;
    unsigned tg = *(unsigned *)(ai + 0x2d4);
    uint8_t *t = DrivenVehicle(tg && findT ? findT(tg) : nullptr), *me = *(uint8_t **)(ai + 8);
    if (t && me) {
        const float *a = (const float *)(me + 0xb0), *b = (const float *)(t + 0xb0);
        float d = sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
        float reach = d - HullDistance(t, a) + 2.f;
        if (reach > g_zealotReach) g_zealotReach = reach;
        static DWORD last;
        if (getenv("GHOSTPC_LOG_MELEE") && GetTickCount() - last > 400)
            last = GetTickCount(), Log("melee: zealot combat: %.1f m from the centre, %.1f from the hull, reach %.1f, mode %d",
                                       d, HullDistance(t, a), g_zealotReach, *(int *)(ai + 0x374));
    }
    g_zealotCombat(ai);
    g_zealotReach = 4.25f;
}
// Its choice of attack (user: it circles the Reaver, runs in and never swings; by a unit it leaps, circles, swipes):
// cAIProtossZealot::CheckCombatInRanges measures once how far its target's centre is and picks from that: a leap
// (Special2) from 4.25 to 9 m, a dash (Special1) from 2.5 to 4.25, closer a swing. From a Reaver's centre it only ever
// leapt. The measured distance (after its fsqrt, +0x48) is, against a driven vehicle, how far its hull is plus half
// a metre (a unit's body).
static uint32_t s_zealotDistCont;
static float __cdecl ZealotRangeDist(uint8_t *ai, uint8_t *target, float d) {
    uint8_t *me = *(uint8_t **)(ai + 8);
    if (!me || !DrivenVehicle(target)) return d;
    return HullDistance(target, (const float *)(me + 0xb0)) + 0.5f;
}
static __declspec(naked) void ZealotDistHook() {
    __asm {
        fsqrt
        push ecx                 // (the combat group, for the call after)
        sub esp, 4
        fstp dword ptr [esp]     // the distance
        push ebx                 // target
        push esi                 // the AI
        call ZealotRangeDist
        add esp, 12
        pop ecx
        fstp st(3)
        fstp st(0)
        jmp dword ptr [s_zealotDistCont]
    }
}
static void HookZealotRanges() {
    uint8_t *at = (uint8_t *)SymAddr("?CheckCombatInRanges@cAIProtossZealot@@UAEHPAVtActor@@@Z");
    if (!at) return;
    at += 0x48;
    static const uint8_t want[] = {0xd9, 0xfa, 0xdd, 0xdb, 0xdd, 0xd8};   // fsqrt; fstp st(3); fstp st(0)
    if (memcmp(at, want, 6)) { Log("vehicles: unexpected Zealot range code: its attacks on vehicles not fixed"); return; }
    s_zealotDistCont = (uint32_t)at + 6;
    DWORD o;
    VirtualProtect(at, 6, PAGE_EXECUTE_READWRITE, &o);
    at[0] = 0xe9;
    *(int32_t *)(at + 1) = (int32_t)((uint32_t)ZealotDistHook - ((uint32_t)at + 5));
    at[5] = 0x90;
    VirtualProtect(at, 6, o, &o);
    Log("vehicles: Zealot picks its attack by a vehicle's hull");
}
static void HookZealotReach() {
    uint8_t *at = (uint8_t *)SymAddr("?ASUpdateCombatRanged@cAIProtossZealot@@UAEXXZ");
    uint32_t *vt = (uint32_t *)SymAddr("??_7cAIProtossZealot@@6B@");
    if (!at || !vt || vt[0x6f8 / 4] != (uint32_t)at) { Log("vehicles: Zealot combat not where expected: its reach not widened"); return; }
    at += 0x143;   // fld dword ptr [0x786F50]
    if (at[0] != 0xd9 || at[1] != 0x05 || *(uint32_t *)(at + 2) != 0x786f50 - 0x400000 + g_imageBase) {
        Log("vehicles: unexpected Zealot reach code at %p: %02x %02x %08x (image base %08x)", at, at[0], at[1], *(uint32_t *)(at + 2), g_imageBase);
        return;
    }
    DWORD o;
    VirtualProtect(at + 2, 4, PAGE_EXECUTE_READWRITE, &o);
    *(uint32_t *)(at + 2) = (uint32_t)&g_zealotReach;
    VirtualProtect(at + 2, 4, o, &o);
    g_zealotCombat = (AIStateFn)vt[0x6f8 / 4];
    VirtualProtect(vt + 0x6f8 / 4, 4, PAGE_READWRITE, &o);
    vt[0x6f8 / 4] = (uint32_t)ZealotCombatHook;
    VirtualProtect(vt + 0x6f8 / 4, 4, o, &o);
    Log("vehicles: Zealot swings reach vehicle hulls");
}
// Melee swings on vehicles (user: the Zealot's and Zerglings' leaps hit the Reaver, their standard swings did not): a
// swing is cAIInstance::MeleeSphereHitCheck(centre, dir, radius, source, type, amount, flags), a sphere at the blade or
// claw (tCollisionQuery::ThingsInSphere) that damages the closest thing it finds, and it never found the Reaver (its
// collision is the siege tank's, smaller than its body). Its calls are redirected: when a swing finds nothing while the
// AI's target is a driven player vehicle whose hull (HullDistance, up to 6 m up) the sphere touches, the
// vehicle takes the hit (tActor::Damage, as the game's).
typedef uint8_t *(__thiscall *SphereHitFn)(void *, float *, float *, float, int, int, float, unsigned);
static SphereHitFn g_sphereHit;
static uint8_t *__fastcall SphereHitHook(uint8_t *ai, void *, float *c, float *dir, float radius, int source, int type,
                                         float amount, unsigned flags) {
    uint8_t *r = g_sphereHit(ai, c, dir, radius, source, type, amount, flags);
    if (getenv("GHOSTPC_LOG_MELEE"))   // debug: every swing, and what it hit
        Log("melee: %.24s swings (radius %.1f, %.1f) at %.1f %.1f %.1f: %.24s", (char *)(*(uint8_t **)(ai + 8) + 0xc4), radius, amount,
            c ? c[0] : 0.f, c ? c[1] : 0.f, c ? c[2] : 0.f, r ? (char *)(r + 0xc4) : "nothing");
    if (r || !c) return r;
    static auto findT = (uint8_t *(__cdecl *)(unsigned))SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    typedef float(__thiscall *DamageFn)(uint8_t *, uint8_t *, int, int, float, float *, float *, unsigned, int, float, int *);
    static auto damage = (DamageFn)SymAddr("?Damage@tActor@@QAEMPAVtThing@@W4eDamageSource@@W4eDamageType@@MPAUXGVECTOR3@@3IW4eHitRegionType@@MPAH@Z");
    unsigned tg = *(unsigned *)(ai + 0x2d4);
    uint8_t *t = DrivenVehicle(tg && findT ? findT(tg) : nullptr);
    if (!t || !damage) return r;
    float dz = c[2] - *(float *)(t + 0xb8);
    if (HullDistance(t, c) > radius + 0.5f || dz < -radius - 1.f || dz > 6.f + radius) return r;
    float pos[3] = {c[0], c[1], c[2]};
    damage(t, *(uint8_t **)(ai + 8), source, type, amount, pos, dir, flags, 1, 0.f, nullptr);
    if (getenv("GHOSTPC_LOG_DAMAGE")) Log("vehicles: melee swing of %.24s reaches the vehicle hull (%.1f)", (char *)(*(uint8_t **)(ai + 8) + 0xc4), amount);
    return t;
}
static void HookMeleeSphere() {
    static const struct { const char *fn; int off; } SITES[] = {
        {"?CMUpdateSpecial1@cAITerranFirebat@@UAEXXZ", 0x81},
        {"?CMUpdateSpecial1@cAITerranGhostClose@@UAEXXZ", 0x89},
        {"?CMUpdateSpecial2@cAITerranLightInfantry@@UAEXXZ", 0x81},
        {"?CMUpdateSpecial1@cAITerranMarine@@UAEXXZ", 0x89},
        {"?BladeHitCheck@cAIProtossZealot@@QAEHI@Z", 0x196},
        {"?CMUpdateSpecial2@cAIZergInfestedTerran@@UAEXXZ", 0x8a},
        {"?AnimationCallback@cAIZergling@@UAEXPAVcKeyChannel@@W4eRenderKeyTag@@I@Z", 0x1ca},
    };
    uint32_t fn = SymAddr("?MeleeSphereHitCheck@cAIInstance@@QAEPAVtThing@@PAUXGVECTOR3@@0MW4eDamageSource@@W4eDamageType@@MI@Z");
    if (!fn) return;
    g_sphereHit = (SphereHitFn)fn;
    int n = 0;
    for (auto &st : SITES) {
        uint8_t *at = (uint8_t *)SymAddr(st.fn);
        if (!at) continue;
        at += st.off;
        if (at[0] != 0xe8 || (uint32_t)(at + 5) + *(int32_t *)(at + 1) != fn) { Log("vehicles: unexpected melee call at %s+%x", st.fn, st.off); continue; }
        DWORD o;
        VirtualProtect(at + 1, 4, PAGE_EXECUTE_READWRITE, &o);
        *(int32_t *)(at + 1) = (int32_t)((uint32_t)SphereHitHook - (uint32_t)(at + 5));
        VirtualProtect(at + 1, 4, o, &o);
        n++;
    }
    Log("vehicles: %d melee swing calls reach vehicle hulls", n);
}
static void HookSearchScore() {
    uint32_t fn = SymAddr("?SearchScore@cAIInstance@@UAEMPAVtActor@@I@Z");
    if (!fn) return;
    g_searchScore = (SearchScoreFn)fn;
    int n = 0;
    for (const char *c : AI_CLASSES) {
        char sym[96];
        _snprintf(sym, sizeof sym, "??_7%s@@6B@", c);
        uint32_t *vt = (uint32_t *)SymAddr(sym);
        if (!vt || vt[0x128 / 4] != fn) continue;
        DWORD o;
        VirtualProtect(vt + 0x128 / 4, 4, PAGE_READWRITE, &o);
        vt[0x128 / 4] = (uint32_t)SearchScoreHook;
        VirtualProtect(vt + 0x128 / 4, 4, o, &o);
        n++;
    }
    Log("vehicles: AI SearchScore wrapped in %d classes", n);
    uint32_t al = SymAddr("?ALAdd@cAIInstance@@UAEXMPAVtThing@@@Z");
    int m = 0;
    if (al) g_alAdd = (ALAddFn)al;
    for (const char *c : AI_CLASSES) {
        char sym[96];
        _snprintf(sym, sizeof sym, "??_7%s@@6B@", c);
        uint32_t *vt = (uint32_t *)SymAddr(sym);
        if (!al || !vt || vt[0x368 / 4] != al) continue;
        DWORD o;
        VirtualProtect(vt + 0x368 / 4, 4, PAGE_READWRITE, &o);
        vt[0x368 / 4] = (uint32_t)ALAddHook;
        VirtualProtect(vt + 0x368 / 4, 4, o, &o);
        m++;
    }
    Log("vehicles: AI ALAdd wrapped in %d classes (driven vehicles alarm them %gx)", m, VEHICLE_ALARM);
}

// Diagnosis (GHOSTPC_LOG_LOS; user: enemies hardly ever spot the Reaver, a Zealot right in front of it did not):
// cAIInstance::IsTargettable needs HasLOS (vtable +0x140: rays from the AI's eye to the target -- a point from the
// target's vtable +8, then its origin + 1.5 m, ... -- counted as seen when a ray's hit thing GUID is the target's,
// +0x34). Every AI class's slot is wrapped to log the results for Reaver targets.
typedef int(__thiscall *HasLosFn)(void *, float *, uint8_t *, float, int, int);
static HasLosFn g_hasLos;
static int __fastcall HasLosHook(void *ai, void *, float *eye, uint8_t *target, float range, int type, int flag) {
    int r = g_hasLos(ai, eye, target, range, type, flag);
    if (IsReaver(target)) {
        static int logs;
        static DWORD last;
        if (logs < 200 && GetTickCount() - last > 250) {
            last = GetTickCount(), logs++;
            uint8_t *me = *(uint8_t **)((uint8_t *)ai + 8);
            const float *t = (const float *)(target + 0xb0);
            static auto findT = (uint8_t *(__cdecl *)(unsigned))SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
            unsigned tg = *(unsigned *)((uint8_t *)ai + 0x2d4);   // (its current target's GUID)
            uint8_t *cur = tg && findT ? findT(tg) : nullptr;
            Log("los: %.20s at %.1f %.1f %.1f -> Reaver (%.1f %.1f %.1f, guid %u): result %d (range %.1f type %d); its target %.24s",
                me ? (char *)(me + 0xc4) : "?", eye ? eye[0] : 0.f, eye ? eye[1] : 0.f, eye ? eye[2] : 0.f, t[0], t[1], t[2],
                *(unsigned *)(target + 0x34), r, range, type, cur ? (char *)(cur + 0xc4) : "none");
        }
    }
    return r;
}
static void HookHasLos() {
    if (!getenv("GHOSTPC_LOG_LOS")) return;
    uint32_t fn = SymAddr("?HasLOS@cAIInstance@@UAE?AW4eAILOSResult@@PAUXGVECTOR3@@PAVtActor@@MW4eAILOSType@@H@Z");
    if (!fn) return;
    g_hasLos = (HasLosFn)fn;
    static const char *const classes[] = {"cAIInstance", "cAIProtossZealot", "cAIProtossDragoon", "cAITerranMarine", "cAITerranFirebat",
        "cAIZergling", "cAIZergHydralisk", "cAITerranGhost", "cAITerranGoliath", "cAITerranLightInfantry", "cAITerranSiegeTank",
        "cAITerranVulture", "cAIZergMutalisk", "cAIBaseWalker", "cAIProtossScout", "cAITerranRebel", "cAIZergInfestedTerran"};
    int n = 0;
    for (const char *c : classes) {
        char sym[96];
        _snprintf(sym, sizeof sym, "??_7%s@@6B@", c);
        uint32_t *vt = (uint32_t *)SymAddr(sym);
        if (!vt || vt[0x140 / 4] != fn) continue;
        DWORD o;
        VirtualProtect(vt + 0x140 / 4, 4, PAGE_READWRITE, &o);
        vt[0x140 / 4] = (uint32_t)HasLosHook;
        VirtualProtect(vt + 0x140 / 4, 4, o, &o);
        n++;
    }
    Log("vehicles: HasLOS logged for Reaver targets (%d AI classes)", n);
}

// Nova outlives her Reaver (user: she is not in it, she works it from the console): tActor::Kill calls the dying thing's
// control's Killed (cTankControl: its dead state) and then, for a player thing with a player info (+0x520: the Reaver
// while she drives it), the shell's death menu. cTankControl::Killed (vtable slot 16) is wrapped: for a Reaver worked
// by Nova (+0x1A0) she is dismounted as the Spider Mine's death does it (cSpiderMineControl::Killed -> StartDismount(1):
// tPlayer::DismountVehicle(mine, 0, 1, 1), then Remove; TankDismount stands her at the console and
// brings her camera back) and the Reaver's player info cleared, so Kill sees no player die; the Reaver explodes as
// any dead one (ReaverDeathCheck).
typedef void(__thiscall *TankKilledFn)(uint8_t *, uint8_t *, int, int, float, float *, float *, unsigned, float);
static TankKilledFn g_tankKilled;
static void __fastcall ReaverKilled(uint8_t *ctl, void *, uint8_t *by, int source, int type, float amount, float *pos, float *dir,
                                    unsigned u, float f) {
    uint8_t *reaver = ctl ? *(uint8_t **)(ctl + 0x180) : nullptr, *nova = ctl ? *(uint8_t **)(ctl + 0x1a0) : nullptr;
    static auto dismountV = (int(__thiscall *)(uint8_t *, uint8_t *, int, int, int))SymAddr("?DismountVehicle@tPlayer@@QAEHPAV1@HHH@Z");
    if (IsReaver(reaver) && nova && dismountV && (reaver == LocalPlayer(0) || reaver == LocalPlayer(1))) {
        int r = dismountV(nova, reaver, 0, 1, 1);   // (as cSpiderMineControl::StartDismount(1), the mine's death)
        *(void **)(reaver + 0x520) = nullptr;
        Log("vehicles: Reaver destroyed while worked from the console: Nova back at it (dismount %d)", r);
        return;
    }
    g_tankKilled(ctl, by, source, type, amount, pos, dir, u, f);
}

// The Reaver's death (user: it has no death animation, so it should explode): when a Reaver dies, the Pylon's death
// blast (game.nfx psiPylonExp: fireballs, shockwave, splash damage psiPylonExpDam) and the Protoss death smoke
// (protossDeath) go off where it stands, made with cEffectManager::CreateHitEffectFromName as psiblade.cpp makes the
// plunge's; half a second later the hull is removed, as cAIPsiDampener::Killed removes the dampener (+0x364 |= 8,
// tThing::Remove(0)), once nobody rides it (a ridden one is left to the game's death handling).
static void ReaverDeathCheck() {
    struct Dying { DWORD at; bool removed; };
    static std::map<uint8_t *, Dying> dying;   // Reaver -> when it blew up (kept until the level ends: a removed one
                                               // stays in g_reavers until the next scan)
    static uint8_t *world;
    if (LevelWorld() != world) world = LevelWorld(), dying.clear();
    typedef void *(__thiscall *HitFromNameFn)(void *, const char *, void *, float *, float *, int, float, float, void *, void *, float);
    static auto fromName = (HitFromNameFn)SymAddr(
        "?CreateHitEffectFromName@cEffectManager@@QAEPAVcEffect@@PADPAVtThing@@PAUXGVECTOR3@@2HMM0PAVcEffectDef@@M@Z");
    static void *fxManager = (void *)SymAddr("?effectManager_Manager@@3VcEffectManager@@A");
    static auto remove = (void(__thiscall *)(uint8_t *, int))SymAddr("?Remove@tThing@@QAEXH@Z");
    DWORD now = GetTickCount();
    for (uint8_t *t : g_reavers) {
        if (dying.count(t) || !(*(uint32_t *)(t + 0x360) & 0x10000000)) continue;
        dying[t] = {now, false};
        float pos[3] = {((float *)(t + 0xb0))[0], ((float *)(t + 0xb0))[1], ((float *)(t + 0xb0))[2] + 1.5f}, up[3] = {0.f, 0.f, 1.f};
        void *blast = fromName && fxManager ? fromName(fxManager, "psiPylonExp", nullptr, pos, up, 0, 0.f, -99999.f, nullptr, nullptr, 0.f) : nullptr;
        void *smoke = fromName && fxManager ? fromName(fxManager, "protossDeath", nullptr, pos, up, 0, 0.f, -99999.f, nullptr, nullptr, 0.f) : nullptr;
        Log("vehicles: Reaver %p destroyed at %.1f %.1f: explosion %p, smoke %p", t, pos[0], pos[1], blast, smoke);
    }
    for (auto &d : dying) {
        uint8_t *t = d.first;
        if (d.second.removed || now - d.second.at < 500 || t == LocalPlayer(0) || t == LocalPlayer(1) || !remove) continue;
        d.second.removed = true;
        if (std::find(g_reavers.begin(), g_reavers.end(), t) == g_reavers.end()) continue;   // (gone already)
        *(uint32_t *)(t + 0x364) |= 8;
        remove(t, 0);
        Log("vehicles: Reaver %p removed", t);
    }
}

// the Reaver's idle loop ends when nobody drives a Reaver (dismounted, killed) or the level changes
static void ReaverIdleCheck() {
    static uint8_t *world;
    if (LevelWorld() != world) world = LevelWorld(), g_reaverIdle = 0;   // (customsound.cpp ended it)
    if (!g_reaverIdle || IsReaver(LocalPlayer(0)) || IsReaver(LocalPlayer(1))) return;
    CustomLoopStop(g_reaverIdle);
    g_reaverIdle = 0;
}

// The hellfire grenade (user: a zergling it hit flew all over the map, spinning like its particles): its explosion
// (cEffectExplosionHellfire, 20 s) sends seekers at the things around it (SpawnAll: tCollisionQuery::ThingsInSphere);
// each one ends in an exp_Seeker explosion whose damage effect expSeekerDam (game.nfx) does 70-80 fire damage within
// 0.5 m with force 200. Many seekers circling one unit for 20 s kept shoving it from every side. cEffectDamage::
// UpdateDamage's call of simCollision_DamageThingsInSphere (argument 5 = the damage's source thing, 11 = force, the
// damage effect's +0xB4) goes through SeekerDamage: an exp_Seeker's force can be scaled
// (GHOSTPC_SEEKER_FORCE=<scale>; default 1, the original). The force was not the cause of the flying zergling: with it cut
// to 15% and then to 0 the zergling still flew off spinning (user). SeekerTrack logs what happens to a seeker's victim.
typedef int(__cdecl *DamageSphereFn)(void *, float *, float, void *, uint8_t *, int, int, float, float, unsigned, float,
                                    unsigned, unsigned, unsigned);
static DamageSphereFn g_damageSphere;
static int __cdecl SeekerDamage(void *q, float *pos, float radius, void *sector, uint8_t *src, int source, int type, float dmin,
                                float dmax, unsigned a, float force, unsigned b, unsigned c, unsigned d) {
    bool seeker = src && !_stricmp((const char *)(src + 0xc4), "exp_Seeker");
    static int logs;
    static bool on = getenv("GHOSTPC_LOG_SEEKER") != nullptr;
    if (on && logs < 30 && src && !_strnicmp((const char *)(src + 0xc4), "exp_", 4))
        logs++, Log("seeker-dmg: frame %u DamageThingsInSphere from %.24s radius %.2f force %.1f -> seeker %d", g_frame, (const char *)(src + 0xc4),
                    radius, force, seeker);
    if (seeker) {
        static float scale = getenv("GHOSTPC_SEEKER_FORCE") ? (float)atof(getenv("GHOSTPC_SEEKER_FORCE")) : 1.f;
        force *= scale;
    }
    g_inSeekerDamage = seeker;
    int r = g_damageSphere(q, pos, radius, sector, src, source, type, dmin, dmax, a, force, b, c, d);
    g_inSeekerDamage = false;
    return r;
}

// The flamethrower pickup (user): the weapon exists (tPlayer::CreateAllPlayerWeapons, weapon 4) but is disabled
// (tPlayerWeapon +0x5F bit 1: weapon cycling skips it) and nothing enables it; its ammo pickup powFThrower
// (cPowerupFlameAmmo::Activate: fills ammo type 3) is detoured to also make it selectable for the player who picked it
// up (ScriptPlayer::GetPlayerPointer on the handle it gets).
typedef int(__thiscall *ActivateFn)(void *, unsigned);
static ActivateFn g_flameAmmoActivate;
// The original only takes the pickup (ammo +50, the POWFTHROWER pickup message via ScriptWorld::AddPickupFX, pickup
// removed: returns 1) while flame ammo (type 3, tPlayer +0x444[3] maximum) is below its maximum; otherwise it sets the
// minimap hint POWINVFULL ("Inventory Full") and returns 0, and the pickup stays. A mission start fills every ammo type,
// so flame ammo is already full (the Zoo: 200 of 200) while the weapon itself is still disabled (user: the pickup gave
// the flamethrower but stayed, "Inventory Full"). So the pickup that enables the weapon is always taken: through the
// original when there is room, else with the original's message and a 1 without calling it. Later pickups are plain
// ammo. A 0 maximum (a mission without one) becomes FLAME_MAX first.
static const unsigned FLAME_MAX = 1000;
static int __fastcall FlameAmmoActivate(void *self, void *, unsigned who) {
    static auto playerOf = (uint8_t *(__thiscall *)(unsigned *))SymAddr("?GetPlayerPointer@ScriptPlayer@@QAEPAVtPlayer@@XZ");
    static auto getMax = (unsigned(__thiscall *)(uint8_t *, int))SymAddr("?GetMaxAmmo@tPlayer@@QAEIW4ePlayerAmmo@@@Z");
    static auto setMax = (unsigned(__thiscall *)(uint8_t *, int, unsigned))SymAddr("?SetMaxAmmo@tPlayer@@QAEIW4ePlayerAmmo@@I@Z");
    static auto getAmmo = (unsigned(__thiscall *)(uint8_t *, int))SymAddr("?GetAmmo@tPlayer@@QAEIW4ePlayerAmmo@@@Z");
    static auto pickupFX = (void(__cdecl *)(const char *, unsigned char, unsigned, float *))SymAddr(
        "?AddPickupFX@ScriptWorld@@SAXPBDEIPAUXGVECTOR3@@@Z");
    unsigned handle = who;
    uint8_t *player = playerOf ? playerOf(&handle) : nullptr;
    uint8_t *weapon = player ? *(uint8_t **)(player + 0x470) : nullptr;   // tPlayer +0x460[4]
    static auto thingOf = (uint8_t *(__thiscall *)(unsigned *))SymAddr("?GetThingPointer@ScriptThing@@QAEPAVtThing@@XZ");
    unsigned th = *(unsigned *)((uint8_t *)self + 0xc);   // the pickup's thing (as cPowerupCanister::GetItemType reads it)
    uint8_t *thing = thingOf ? thingOf(&th) : nullptr;
    bool weaponPickup = thing && !_stricmp((char *)(thing + 0xc4), "powFlamethrower");
    // Co-op (user: both Nova and Echo get the flamethrower and the Psi Blade): a weapon pickup is used up only once every
    // local player has the weapon; until then it gives it to whoever lacks it and stays (Activate returning 0 leaves a
    // pickup where it is). A player who has it already passes over it (the flamethrower's then counts as its ammo only
    // once nobody lacks the weapon). In single player the one player taking it uses it up, as before.
    extern bool PlayerHasPsiBlade(const uint8_t *player);
    auto hasFlame = [](uint8_t *p) {
        uint8_t *w = p ? *(uint8_t **)(p + 0x470) : nullptr;
        return w && !(w[0x5f] & 2);
    };
    auto everyone = [](bool (*has)(uint8_t *)) {
        for (int i = 0; i < 2; i++)
            if (uint8_t *p = LocalPlayer(i))
                if (!has(p)) return false;
        return true;
    };
    static bool (*hasBlade)(uint8_t *) = [](uint8_t *p) { return PlayerHasPsiBlade(p); };
    static bool (*hasFlameFn)(uint8_t *) = hasFlame;
    if (thing && player && !_stricmp((char *)(thing + 0xc4), "powPsiBlade")) {   // psiblade.cpp: the Psi Blade, not ammo
        extern bool PsiBladeGive(uint8_t *player);
        if (!PlayerHasPsiBlade(player)) {
            PsiBladeGive(player);
            if (pickupFX) pickupFX("POWPSIBLADE", 0xd, 50, (float *)(player + 0xb0));   // "Psi Blade" (add_pickup_strings.py)
        }
        bool done = everyone(hasBlade);
        static int logs;
        if (!done && logs < 4) logs++, Log("vehicles: Psi Blade pickup stays for the other player");
        return done ? 1 : 0;
    }
    if (weaponPickup && player && hasFlame(player) && !everyone(hasFlameFn)) return 0;   // (hers already: left for the other)
    if (!weaponPickup || !player || !weapon || !(weapon[0x5f] & 2) || !getMax || !getAmmo) {
        int r = g_flameAmmoActivate(self, who);
        static int logs;
        if (logs < 4) logs++, Log("vehicles: flame ammo pickup %.24s: weapon flags %02x, ammo %u of %u, taken %d", thing ? (char *)(thing + 0xc4) : "?",
                                  weapon ? weapon[0x5f] : 0, player && getAmmo ? getAmmo(player, 3) : 0, player && getMax ? getMax(player, 3) : 0, r);
        return r;
    }
    if (setMax && getMax(player, 3) == 0) setMax(player, 3, FLAME_MAX);
    weapon[0x5f] &= ~2;   // the flamethrower is selectable from now on
    extern void LogbookUnlock(const char *entry);   // psiblade.cpp: its database entry (user)
    LogbookUnlock("lbentry_weaponflamethrower");
    // its ammo (+50, as the original's) and the message: "Flamethrower" (POWFLAMETHROWER, add_pickup_strings.py; user:
    // it said "Flame Ammo", the ammo pickup's POWFTHROWER). Returning 1 removes the pickup, as the original does.
    static auto setAmmo = (unsigned(__thiscall *)(uint8_t *, int, unsigned))SymAddr("?SetAmmo@tPlayer@@QAEIW4ePlayerAmmo@@I@Z");
    unsigned a = getAmmo(player, 3) + 50, m = getMax(player, 3);
    if (setAmmo) setAmmo(player, 3, a < m ? a : m);
    if (pickupFX) pickupFX("POWFLAMETHROWER", 0xd, 50, (float *)(player + 0xb0));
    int r = 1;
    if (!everyone(hasFlameFn)) {   // (co-op: the other player still lacks it, so it stays)
        r = 0;
        Log("vehicles: flamethrower pickup stays for the other player");
    }
    Log("vehicles: flamethrower pickup: the flamethrower is now selectable (ammo %u of %u)", getAmmo(player, 3), getMax(player, 3));
    return r;
}

// Diagnosis (the game crashed quitting the Zoo: tMotionSet::Free -> tRenderKeyTrack freed twice, from tObject::Free in
// tWorld::Free): tObject::Free is detoured to put the object's name and its motion set's name in the crash breadcrumb.
typedef void(__thiscall *ObjFreeFn)(uint8_t *);
static ObjFreeFn g_objFree;
static void __fastcall ObjectFree(uint8_t *o, void *) {
    static char note[96];
    const char *prev = g_crumb;
    const uint8_t *ms = *(const uint8_t **)(o + 0x2a4);
    _snprintf(note, sizeof note - 1, "tObject::Free %.32s (motion set %.32s, refs %d)", (const char *)(o + 0xc4),
              ms ? (const char *)(ms + 4) : "none", ms ? *(const int *)ms : 0);
    g_crumb = note;
    g_objFree(o);
    g_crumb = prev;
}

// debug (GHOSTPC_HELLFIRE="<frame> <x> <y> <z>"): creates an exp_Hellfire (the hellfire grenade's explosion) there, then
// logs the zerglings within 15 m every 15 frames for 10 s
static void DebugHellfire() {
    static int at = -1;
    static float pos[3];
    static bool init;
    if (!init) {
        init = true;
        if (const char *e = getenv("GHOSTPC_HELLFIRE")) sscanf(e, "%d %f %f %f", &at, &pos[0], &pos[1], &pos[2]);
    }
    if (at < 0 || (int)g_frame < at || (int)g_frame > at + 300) return;
    static auto findT = (uint8_t *(__cdecl *)(unsigned))SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    static unsigned *nextGuid = (unsigned *)SymAddr("?nextGUID@tThing@@2IA");
    if ((int)g_frame == at) {
        static auto find = (uint8_t *(__cdecl *)(const char *))SymAddr("?gameTemplate_Find@@YAPAVtTemplate@@PBD@Z");
        static auto sectorOf = (void *(__thiscall *)(uint8_t *, const float *))SymAddr("?FindPosSector@tWorld@@QAEPAVtSector@@PBUXGVECTOR3@@@Z");
        static auto create = (uint8_t *(__cdecl *)(void *, void *, void *, void *, unsigned))SymAddr(
            "?gameThing_CreateFromTemplateAtPos@@YAPAVtThing@@PAVtTemplate@@PAVtWorld@@PAVtSector@@PBUXGMATRIX@@I@Z");
        uint8_t *w = LevelWorld(), *tmpl = find ? find("exp_Hellfire") : nullptr;
        float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, pos[0], pos[1], pos[2], 1};
        uint8_t *t = w && tmpl && sectorOf && create ? create(tmpl, w, sectorOf(w, pos), m, 0) : nullptr;
        Log("hellfire: exp_Hellfire at %.1f %.1f %.1f -> %p", pos[0], pos[1], pos[2], t);
    }
    if ((g_frame - at) % 15 || !findT || !nextGuid) return;
    for (unsigned g = 1; g < *nextGuid && g < 200000; g++)
        if (uint8_t *t = findT(g))
            if (!_strnicmp((char *)(t + 0xc4), "zergling", 8)) {
                const float *q = (const float *)(t + 0xb0), *f = (const float *)(t + 0x90);
                float dx = q[0] - pos[0], dy = q[1] - pos[1];
                if (dx * dx + dy * dy < 225.f)
                    Log("hellfire: +%u %.24s at %.1f %.1f %.1f heading %.0f flags %08x", g_frame - at, (char *)(t + 0xc4), q[0], q[1], q[2],
                        atan2f(f[1], f[0]) * 57.3f, *(uint32_t *)(t + 0x360));
            }
}

// debug (GHOSTPC_LOG_SORT=<frame>): what goes into the depth-sorted list (tSortList, drawn after the effects) on that
// frame: things (AddSortThing) with their name, effects (AddSortEffect) with their material and flags, and each key
// (tSortList::SortGroupCompare draws the larger key first). For the Psi Blade drawn under a medkit.
typedef void(__thiscall *AddSortFn)(void *, uint8_t *, float, void *);
static AddSortFn g_addSortThing, g_addSortEffect;
static int g_sortLogFrame = -2;
static void __fastcall AddSortThingHook(void *list, void *, uint8_t *t, float key, void *cam) {
    if ((int)g_frame == g_sortLogFrame) Log("sort: frame %u thing %.24s key %.3f", g_frame, t ? (char *)(t + 0xc4) : "?", key);
    g_addSortThing(list, t, key, cam);
}
// The Psi Blade's effect (psiblade.cpp) is drawn last in it: things with flag 0x200000 (the glowing pickups: medkit,
// psi energy) come in at key -1000 (tThing::Draw), after everything else, and the blade (key = Nova's distance, 2 m)
// was drawn before them, so a pickup behind her covered it (user; it writes no depth). It gets -2000; it is still
// depth-tested against what is drawn, so solid things in front of it still hide it.
bool IsPsiBladeFx(const uint8_t *e);   // psiblade.cpp
static void __fastcall AddSortEffectHook(void *list, void *, uint8_t *e, float key, void *cam) {
    if (IsPsiBladeFx(e)) key = -2000.f;
    if ((int)g_frame == g_sortLogFrame)
        Log("sort: frame %u effect %p material %.32s flags %08x key %.3f", g_frame, e, e ? (char *)(e + 0x7c) : "?", e ? *(uint32_t *)(e + 0x50) : 0, key);
    g_addSortEffect(list, e, key, cam);
}

// debug (GHOSTPC_LOG_WEAPONS): player 1's weapons' +0x5F flags (bit 2 = skipped when cycling) and ammo, at a few frames
static void DebugWeapons() {
    static bool on = getenv("GHOSTPC_LOG_WEAPONS") != nullptr;
    if (!on || (g_frame != 5 && g_frame != 20 && g_frame != 60 && g_frame != 140 && g_frame != 160 && g_frame != 300)) return;
    uint32_t nm = SymAddr("?gameMotion_apNames@@3PAPADA");
    uint8_t **info = nm ? *(uint8_t ***)(nm + 0x728) : nullptr;
    if (!info || !info[0]) { Log("weapons: frame %u: no player", g_frame); return; }
    static auto getAmmo = (unsigned(__thiscall *)(uint8_t *, int))SymAddr("?GetAmmo@tPlayer@@QAEIW4ePlayerAmmo@@@Z");
    char line[256] = "";
    for (int w = 0; w < 7; w++) {
        uint8_t *pw = *(uint8_t **)(info[0] + 0x460 + 4 * w);
        size_t n = strlen(line);
        if (pw) _snprintf(line + n, sizeof line - n - 1, " [%d flags %02x ammo type %d: %u]", w, pw[0x5f], *(int *)(pw + 0x44),
                          getAmmo ? getAmmo(info[0], *(int *)(pw + 0x44)) : 0);
    }
    Log("weapons: frame %u current %d%s", g_frame, *(int *)(info[0] + 0x454), line);
}

void VehiclesUpdate() {
    DebugWeapons();
    extern void PsiBladeUpdate();   // psiblade.cpp: Tanagazj's Psi Blade drop
    PsiBladeUpdate();
    extern void SmokeUpdate();   // smoke.cpp: the smoke grenade's aggro drop and stun
    SmokeUpdate();
    SeekerTrack();
    PruneThings();
    DebugHellfire();
    ReaverIdleCheck();
    VultureIdleCheck();
    ReaverDeathCheck();
    VultureUpright();
    VehiclesTargetable();
    EchoVultureModel();
    ShadowStateLog();
    ConsoleUseUpdate();
    DragoonShields();
    DebugMount();
    DebugItem();
    GhostTemplates();
    ScanReavers();
    HoldParkedReavers();
    SteerScarabs();
    DebugScarab();
    FollowScarab();
    GoliathGunSound();
    MinePickup();
}

// local player 0 / 1 (left / right) is riding a Vulture: its cPlayerInfo's player is the vehicle (tPlayer::MountVehicle
// hands it over) and that player's control is a cVultureControl
bool RidingVulture(int local) {
    static uint8_t *names = (uint8_t *)SymAddr("?gameMotion_apNames@@3PAPADA");
    static uint32_t vulture = SymAddr("??_7cVultureControl@@6B@");
    uint8_t **info = names ? *(uint8_t ***)(names + (local ? 0x72c : 0x728)) : nullptr;
    uint8_t *player = info ? info[0] : nullptr;
    uint8_t *ctl = player ? *(uint8_t **)(player + 0x3f8) : nullptr;
    return ctl && vulture && *(uint32_t *)ctl == vulture;
}

// The flame turret's camera (user: it "sucks"): cFlameGunControl only replaces cGunControl's StandControlsUpdate, and
// its copy lacks the block that starts the turret's camera once mounted: when the mount's hold time (+0x16C) has passed
// and +0x1CC is set, cGunControl's calls tSimCamera::BeginChase(avGunCameraPosOffset[camera +0x1D0], 0,
// avGunLookPosOffset[camera], 0) and clears +0x1CC. Without it the camera stayed where Nova's had been when she
// mounted (logged: 1.5 m up, 5 m off, never moving), until the camera button cycled it. The flame turret's
// StandControlsUpdate (its vtable +0x10C) is wrapped to do that block first.
typedef void(__thiscall *ControlUpdateFn)(uint8_t *);
static ControlUpdateFn g_flameStand;
static void __fastcall FlameStandControls(uint8_t *ctl) {
    static const uint32_t *now = (const uint32_t *)(SymAddr("?stdTime@@3VtStdTime@@A") + 0x44);
    static auto getCam = (uint8_t *(__thiscall *)(uint8_t *))SymAddr("?GetCamera@cControl@@QAEPAVtSimCamera@@XZ");
    static auto chase = (void(__thiscall *)(uint8_t *, float *, int, float *, int))SymAddr("?BeginChase@tSimCamera@@QAEXPAUXGVECTOR3@@H0H@Z");
    static float *camOff = (float *)SymAddr("?avGunCameraPosOffset@@3PAUXGVECTOR3@@A");
    static float *lookOff = (float *)SymAddr("?avGunLookPosOffset@@3PAUXGVECTOR3@@A");
    if (now && getCam && chase && camOff && lookOff && *(uint32_t *)(ctl + 0x16c) <= *now && *(int *)(ctl + 0x1cc)) {
        int cam = *(int *)(ctl + 0x1d0);
        if (cam < 0 || cam > 1) cam = 0;
        if (uint8_t *c = getCam(ctl)) chase(c, camOff + 3 * cam, 0, lookOff + 3 * cam, 0);
        *(int *)(ctl + 0x1cc) = 0;
        Log("vehicles: flame turret camera started (camera %d)", cam);
    }
    g_flameStand(ctl);
}
static void FixFlameTurretCamera() {
    uint32_t *vt = (uint32_t *)SymAddr("??_7cFlameGunControl@@6B@");
    uint32_t fn = SymAddr("?StandControlsUpdate@cFlameGunControl@@UAEXXZ");
    if (!vt || !fn || vt[0x10c / 4] != fn) {
        Log("vehicles: unexpected cFlameGunControl vtable, its camera not fixed");
        return;
    }
    g_flameStand = (ControlUpdateFn)fn;
    DWORD o;
    VirtualProtect(&vt[0x10c / 4], 4, PAGE_EXECUTE_READWRITE, &o);
    vt[0x10c / 4] = (uint32_t)FlameStandControls;
    VirtualProtect(&vt[0x10c / 4], 4, o, &o);
}
void VehiclesInit() {
    static bool done;
    if (done) return;
    done = true;
    FixFlameTurretCamera();
    uint8_t *pre = (uint8_t *)SymAddr("?PreFinalize@tThing@@QAEXHH@Z");
    uint8_t *strstrFn = (uint8_t *)SymAddr("_strstr");
    int n = 0;
    for (uint8_t *p = pre; pre && strstrFn && p < pre + 0x400; p++) {
        if (p[0] != 0xE8 || p + 5 + *(int32_t *)(p + 1) != strstrFn) continue;
        DWORD old;
        VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old);
        *(int32_t *)(p + 1) = (int32_t)((uint8_t *)ControlNameMatch - (p + 5));
        VirtualProtect(p + 1, 4, old, &old);
        n++, p += 4;
    }
    if (n != 9) Log("vehicles: tThing::PreFinalize: %d name checks made case-insensitive (expected 9)", n);
    {   // mounting the Reaver (cPlayerControl::UpdateActionButton: push ebp; mov ebp, esp; and esp, -16)
        static const uint8_t start[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
        g_updateAction = (UpdateActionFn)DetourFn((uint8_t *)SymAddr("?UpdateActionButton@cPlayerControl@@QAEXXZ"), start, 6,
                                                  ReaverUpdateAction);
        if (!g_updateAction) Log("vehicles: unexpected cPlayerControl::UpdateActionButton, the Reaver cannot be mounted");
    }
    {   // damage burn points (cModelInst::GetNumMarkers: mov ecx, [ecx]; xor eax, eax; test ecx, ecx;
        // cModelInst::GetMarkerPos: push ebp; mov ebp, esp; and esp, -16)
        static const uint8_t num[] = {0x8B, 0x09, 0x33, 0xC0, 0x85, 0xC9};
        static const uint8_t pos[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
        g_numMarkers = (NumMarkersFn)DetourFn((uint8_t *)SymAddr("?GetNumMarkers@cModelInst@@QAEEE@Z"), num, 6, BurnNumMarkers);
        g_fullShadow = (FullShadowFn)DetourFn((uint8_t *)SymAddr("?DrawFullShadow@tThing@@QAEHPAVtSimCamera@@PAUtRenderLight@@M@Z"),
                                              pos, 6, FullShadowHook);
        if (!g_fullShadow) Log("vehicles: unexpected tThing::DrawFullShadow, vehicle shadows not adjusted");
        // push esi; push edi; mov edi, ecx; mov eax, [edi + 0x10]   /   push edi; mov edi, ecx; mov eax, [edi + 0x10]
        static const uint8_t ideal[] = {0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x47, 0x10}, draw[] = {0x57, 0x8B, 0xF9, 0x8B, 0x47, 0x10};
        g_idealLight = (IdealLightFn)DetourFn((uint8_t *)SymAddr("?FindIdealShadowLight@tThing@@QAEHPAUtGatherLight@@I@Z"), ideal, 7, IdealLightHook);
        g_drawShadow = (DrawShadowFn)DetourFn((uint8_t *)SymAddr("?DrawShadow@tThing@@QAEXPAVtSimCamera@@@Z"), draw, 6, DrawShadowHook);
        if (!g_idealLight || !g_drawShadow) Log("vehicles: unexpected tThing::FindIdealShadowLight / DrawShadow, vehicle shadows end at 10 m");
        // levels of detail capped for the models whose lower ones drop parts (LodCapLoad / LodCapDetail)
        static const uint8_t load[] = {0x81, 0xEC, 0xE0, 0x00, 0x00, 0x00}, detail[] = {0xD9, 0x44, 0x24, 0x04, 0x53};
        g_modelLoadFn = (ModelLoadAnyFn)DetourFn((uint8_t *)SymAddr("?renderModel_Load@@YAPAVcModel@@PBDI@Z"), load, 6, LodCapLoad);
        g_setDetail = (SetDetailFn)DetourFn((uint8_t *)SymAddr("?SetDetail@cModelInst@@QAEXM@Z"), detail, 5, LodCapDetail);
        if (!g_modelLoadFn || !g_setDetail) Log("vehicles: unexpected renderModel_Load / cModelInst::SetDetail, levels of detail not capped");
        g_markerPosAll = (MarkerPosFn)DetourFn((uint8_t *)SymAddr("?GetMarkerPos@cModelInst@@QAEHEGPAUXGVECTOR3@@PAUXGMATRIX@@@Z"),
                                               pos, 6, BurnMarkerPos);
        if (!g_numMarkers || !g_markerPosAll) Log("vehicles: unexpected cModelInst marker functions, no damage burn points");
    }
    {   // the seeker victim's movers (diagnosis): push ebp; mov ebp, esp; and esp, -16; sub esp, <n>
        static const uint8_t vel[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x1C};
        static const uint8_t imp[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x24};
        static const uint8_t ang[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x2C};
        g_setVel = (SetVelFn)DetourFn((uint8_t *)SymAddr("?SetVelocity@tThing@@QAEXPBUXGVECTOR3@@@Z"), vel, 9, SetVelHook);
        g_impulse = (ImpulseFn)DetourFn((uint8_t *)SymAddr("?ApplyImpulse@tThing@@QAEXPBUXGVECTOR3@@HH0@Z"), imp, 9, ImpulseHook);
        g_angImpulse = (AngImpulseFn)DetourFn((uint8_t *)SymAddr("?ApplyAngularImpulse@tThing@@QAEXPBUXGVECTOR3@@H@Z"), ang, 9, AngImpulseHook);
        if (!g_setVel || !g_impulse || !g_angImpulse) Log("vehicles: unexpected tThing velocity / impulse prologue, not all logged");
    }
    {   // the sorted list: the Psi Blade drawn last (always); its entries logged on one frame (GHOSTPC_LOG_SORT)
        if (const char *e = getenv("GHOSTPC_LOG_SORT")) g_sortLogFrame = atoi(e);
        static const uint8_t start[] = {0x8B, 0x81, 0x00, 0x30, 0x00, 0x00};   // mov eax, [ecx + 0x3000]
        g_addSortEffect = (AddSortFn)DetourFn((uint8_t *)SymAddr("?AddSortEffect@tSortList@@QAEXPAVcEffect@@MPAVtSimCamera@@@Z"), start, 6, AddSortEffectHook);
        if (g_sortLogFrame >= 0)
            g_addSortThing = (AddSortFn)DetourFn((uint8_t *)SymAddr("?AddSortThing@tSortList@@QAEXPAVtThing@@MPAVtSimCamera@@@Z"), start, 6, AddSortThingHook);
        if (!g_addSortEffect) Log("vehicles: unexpected tSortList::AddSortEffect, the Psi Blade is drawn under glowing pickups");
    }
    HookPurifierDamagedLog();
    {   // the quit-crash breadcrumb: tObject::Free (push esi; mov esi, ecx; mov eax, [esi + 0x28c])
        static const uint8_t start[] = {0x56, 0x8B, 0xF1, 0x8B, 0x86, 0x8C, 0x02, 0x00, 0x00};
        g_objFree = (ObjFreeFn)DetourFn((uint8_t *)SymAddr("?Free@tObject@@UAEXXZ"), start, 9, ObjectFree);
        if (!g_objFree) Log("vehicles: unexpected tObject::Free, no object names in crash reports");
    }
    {   // the flamethrower pickup: cPowerupFlameAmmo::Activate (mov eax, [esp+4]; push esi; lea ecx, [esp+8])
        static const uint8_t start[] = {0x8B, 0x44, 0x24, 0x04, 0x56, 0x8D, 0x4C, 0x24, 0x08};
        g_flameAmmoActivate = (ActivateFn)DetourFn((uint8_t *)SymAddr("?Activate@cPowerupFlameAmmo@@UAEHI@Z"), start, 9, FlameAmmoActivate);
        Log(g_flameAmmoActivate ? "vehicles: flamethrower pickup hooked" : "vehicles: unexpected cPowerupFlameAmmo::Activate, the flamethrower pickup gives ammo only");
    }
    {   // hellfire seekers' force: cEffectDamage::UpdateDamage's call of simCollision_DamageThingsInSphere
        uint8_t *upd = (uint8_t *)SymAddr("?UpdateDamage@cEffectDamage@@QAEXPAUXGVECTOR3@@PAVtSector@@PAVtThing@@@Z");
        uint8_t *dts = (uint8_t *)SymAddr("?simCollision_DamageThingsInSphere@@YAHPAVtCollisionQuery@@PAUXGVECTOR3@@MPAVtSector@@PAVtThing@@W4eDamageSource@@W4eDamageType@@MMIMIII@Z");
        int n = 0;
        for (uint8_t *q = upd; upd && dts && q < upd + 0x600; q++) {
            if (q[0] != 0xE8 || q + 5 + *(int32_t *)(q + 1) != dts) continue;
            g_damageSphere = (DamageSphereFn)dts;
            DWORD o;
            VirtualProtect(q + 1, 4, PAGE_EXECUTE_READWRITE, &o);
            *(int32_t *)(q + 1) = (int32_t)((uint8_t *)SeekerDamage - (q + 5));
            VirtualProtect(q + 1, 4, o, &o);
            n++, q += 4;
        }
        if (n != 1) Log("vehicles: cEffectDamage::UpdateDamage: %d DamageThingsInSphere calls wrapped (expected 1)", n);
    }
    {   // the Reaver's crosshair: vui_Update +0xBB (call vuiTarget_Draw)
        uint8_t *ui = (uint8_t *)SymAddr("?vui_Update@@YAXXZ");
        uint8_t *draw = (uint8_t *)SymAddr("?vuiTarget_Draw@@YAXXZ");
        uint8_t *call = ui ? ui + 0xbb : nullptr;
        // (shell.cpp's SplitTargetDraw, per-player reticle state, may hold this call already: chained)
        if (call && draw && call[0] == 0xE8) {
            g_vuiTargetDraw = (VoidFn)(call + 5 + *(int32_t *)(call + 1));
            DWORD o;
            VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &o);
            *(int32_t *)(call + 1) = (int32_t)((uint8_t *)VuiTargetDraw - (call + 5));
            VirtualProtect(call + 1, 4, o, &o);
        } else
            Log("vehicles: unexpected vui_Update, the Reaver has no crosshair");
    }
    {   // the Reaver's entry markers (cModelInst::GetMarkerMatrix: mov eax, [esp+8]; push ebx)
        static const uint8_t start[] = {0x8B, 0x44, 0x24, 0x08, 0x53};
        g_markerMatrix = (MarkerMatrixFn)DetourFn((uint8_t *)SymAddr("?GetMarkerMatrix@cModelInst@@QAEHEGPAUXGMATRIX@@0@Z"),
                                                  start, 5, ReaverMarkerMatrix);
        if (!g_markerMatrix) Log("vehicles: unexpected cModelInst::GetMarkerMatrix, the Reaver cannot be entered");
    }
    {   // the tank hull's scale (simPhysics_SetupTankRB +0xD7: call simPhysics_CreateConvexHull)
        uint8_t *rb = (uint8_t *)SymAddr("?simPhysics_SetupTankRB@@YAXPAVtActor@@H@Z");
        uint8_t *hull = (uint8_t *)SymAddr("?simPhysics_CreateConvexHull@@YAPAVhkShape@@PAVcModel@@PAUXGVECTOR3@@@Z");
        uint8_t *call = rb ? rb + 0xd7 : nullptr;
        if (call && hull && call[0] == 0xE8 && call + 5 + *(int32_t *)(call + 1) == hull) {
            g_createHull = (HullFn)hull;
            DWORD o;
            VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &o);
            *(int32_t *)(call + 1) = (int32_t)((uint8_t *)TankHull - (call + 5));
            VirtualProtect(call + 1, 4, o, &o);
        } else
            Log("vehicles: unexpected simPhysics_SetupTankRB hull call");
    }
    {   // the tank's collision model load (simPhysics_SetupTankRB +0xCA: call renderModel_Load)
        uint8_t *rb = (uint8_t *)SymAddr("?simPhysics_SetupTankRB@@YAXPAVtActor@@H@Z");
        uint8_t *load = (uint8_t *)SymAddr("?renderModel_Load@@YAPAVcModel@@PBDI@Z");
        uint8_t *call = rb ? rb + 0xca : nullptr;
        if (call && load && call[0] == 0xE8 && call + 5 + *(int32_t *)(call + 1) == load) {
            g_modelLoad = (ModelLoadFn)load;
            int32_t rel = (int32_t)((uint8_t *)TankHullLoad - (call + 5));
            DWORD o;
            VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &o);
            *(int32_t *)(call + 1) = rel;
            VirtualProtect(call + 1, 4, o, &o);
        } else
            Log("vehicles: unexpected simPhysics_SetupTankRB, no collision-model fallback");
    }
    {   // the tank suspension's wheel markers (SetupSuspension: four calls of cModelInst::GetMarkerPos)
        // two functions share the name (tank, Vulture): the tank's is InitTank - 0x17B0 (0x47A130 vs 0x47B8E0)
        uint8_t *init = (uint8_t *)SymAddr("?InitTank@cNSITank@@QAEXXZ");
        uint8_t *fn = init ? init - 0x17b0 : nullptr;
        uint8_t *get = (uint8_t *)SymAddr("?GetMarkerPos@cModelInst@@QAEHEGPAUXGVECTOR3@@PAUXGMATRIX@@@Z");
        int n = 0;
        for (uint8_t *q = fn; fn && get && q < fn + 0x240; q++) {
            if (q[0] != 0xE8 || q + 5 + *(int32_t *)(q + 1) != get) continue;
            g_markerPos = (MarkerPosFn)get;
            DWORD o;
            VirtualProtect(q + 1, 4, PAGE_EXECUTE_READWRITE, &o);
            *(int32_t *)(q + 1) = (int32_t)((uint8_t *)TankWheelMarker - (q + 5));
            VirtualProtect(q + 1, 4, o, &o);
            n++, q += 4;
        }
        if (n != 4) Log("vehicles: tank SetupSuspension: %d marker reads wrapped (expected 4)", n);
    }
    {   // the tank code's PlayMotion calls (offsets from InitTank, 0x47B8E0 in Ghost.map)
        static const uint32_t sites[] = {0x47798d, 0x4779cb, 0x4779f0, 0x4785f0, 0x479abc, 0x479afa, 0x479b1f, 0x479ca7,
                                         0x479d09, 0x479d2b, 0x47ac03, 0x47aefa, 0x47af1c, 0x47b568, 0x47b85d};
        uint8_t *init = (uint8_t *)SymAddr("?InitTank@cNSITank@@QAEXXZ");
        uint8_t *play = (uint8_t *)SymAddr("?PlayMotion@tObject@@QAEHW4eMotionMode@@IMPAVtMotionSet@@MM@Z");
        int n = 0;
        for (uint32_t va : sites) {
            uint8_t *q = init ? init + (int32_t)(va - 0x47b8e0) : nullptr;
            if (!q || !play || q[0] != 0xE8 || q + 5 + *(int32_t *)(q + 1) != play) continue;
            g_playMotion = (PlayMotionFn)play;
            DWORD o;
            VirtualProtect(q + 1, 4, PAGE_EXECUTE_READWRITE, &o);
            *(int32_t *)(q + 1) = (int32_t)((uint8_t *)TankPlayMotion - (q + 5));
            VirtualProtect(q + 1, 4, o, &o);
            n++;
        }
        if (n != 15) Log("vehicles: tank PlayMotion calls: %d of 15 wrapped", n);
    }
    {   // Reaver: no trajectory arc (cTankControl vtable slot 26 = UIRenderCallback)
        uint32_t *vt = (uint32_t *)SymAddr("??_7cTankControl@@6B@");
        uint32_t fn = SymAddr("?UIRenderCallback@cTankControl@@UAEXXZ");
        if (vt && fn && vt[26] == fn) {
            g_tankUI = (UpdateFn)fn;
            DWORD o;
            VirtualProtect(vt + 26, 4, PAGE_READWRITE, &o);
            vt[26] = (uint32_t)TankUIRender;
            VirtualProtect(vt + 26, 4, o, &o);
        } else
            Log("vehicles: unexpected cTankControl vtable slot 26");
    }
    {   // Reaver from a console: cTankControl vtable slots 53 / 54 = Mount / Dismount
        uint32_t *vt = (uint32_t *)SymAddr("??_7cTankControl@@6B@");
        uint32_t mount = SymAddr("?Mount@cTankControl@@UAEXXZ"), dismount = SymAddr("?Dismount@cTankControl@@UAEXXZ");
        if (vt && mount && dismount && vt[53] == mount && vt[54] == dismount) {
            g_tankMount = (UpdateFn)mount, g_tankDismount = (UpdateFn)dismount;
            DWORD o;
            VirtualProtect(vt + 53, 8, PAGE_READWRITE, &o);
            vt[53] = (uint32_t)TankMount, vt[54] = (uint32_t)TankDismount;
            VirtualProtect(vt + 53, 8, o, &o);
        } else
            Log("vehicles: unexpected cTankControl vtable slots 53 / 54, the Reaver's console ride is incomplete");
    }
    HookHasLos();
    HookSearchScore();
    HookCanAttack();
    HookZealotReach();
    HookZealotRanges();
    HookMeleeSphere();
    {   // Nova outlives her Reaver: cTankControl::Killed, vtable slot 16
        uint32_t *vt = (uint32_t *)SymAddr("??_7cTankControl@@6B@");
        uint32_t fn = SymAddr("?Killed@cTankControl@@UAEXPAVtThing@@W4eDamageSource@@W4eDamageType@@MPAUXGVECTOR3@@3IM@Z");
        if (vt && fn && vt[16] == fn) {
            g_tankKilled = (TankKilledFn)fn;
            DWORD o;
            VirtualProtect(vt + 16, 4, PAGE_READWRITE, &o);
            vt[16] = (uint32_t)ReaverKilled;
            VirtualProtect(vt + 16, 4, o, &o);
        } else
            Log("vehicles: unexpected cTankControl vtable slot 16, Nova dies with her Reaver");
    }
    {   // Reaver crushing: cTankControl::Collide (found in the vtable)
        uint32_t *vt = (uint32_t *)SymAddr("??_7cTankControl@@6B@");
        uint32_t fn = SymAddr("?Collide@cTankControl@@UAEXAAUhkContactPointAddedEvent@@HPAVtThing@@@Z");
        int slot = -1;
        for (int i = 0; vt && fn && i < 0x100 && slot < 0; i++)
            if (vt[i] == fn) slot = i;
        if (slot >= 0) {
            g_tankCollide = (TankCollideFn)fn;
            DWORD o;
            VirtualProtect(vt + slot, 4, PAGE_READWRITE, &o);
            vt[slot] = (uint32_t)ReaverCollide;
            VirtualProtect(vt + slot, 4, o, &o);
        } else
            Log("vehicles: cTankControl::Collide not in its vtable, the Reaver crushes whatever touches it");
    }
    {   // Reaver: cTankControl vtable slot 10 = UpdateControls
        uint32_t *vt = (uint32_t *)SymAddr("??_7cTankControl@@6B@");
        uint32_t fn = SymAddr("?UpdateControls@cTankControl@@UAEXXZ");
        if (vt && fn && vt[10] == fn) {
            g_tankUpdate = (UpdateFn)fn;
            DWORD o;
            VirtualProtect(vt + 10, 4, PAGE_READWRITE, &o);
            vt[10] = (uint32_t)TankUpdateControls;
            VirtualProtect(vt + 10, 4, o, &o);
        } else
            Log("vehicles: unexpected cTankControl vtable, the Reaver is a plain tank");
    }
    {   // the Dragoon on cGoliathControl: its PlayMotion / FireChaingun / FireRocket calls (simGoliathControl.obj, from
        // Killed to the end of UpdateControls) and UpdateControls (vtable slot 10)
        uint8_t *from = (uint8_t *)SymAddr("?Killed@cGoliathControl@@UAEXPAVtThing@@W4eDamageSource@@W4eDamageType@@MPAUXGVECTOR3@@3IM@Z");
        uint8_t *to = (uint8_t *)SymAddr("?UpdateControls@cGoliathControl@@UAEXXZ");
        uint8_t *play = (uint8_t *)SymAddr("?PlayMotion@tObject@@QAEHW4eMotionMode@@IMPAVtMotionSet@@MM@Z");
        uint8_t *cg = (uint8_t *)SymAddr("?FireChaingun@cGoliathControl@@QAEXIPAVtThing@@M@Z");
        uint8_t *rk = (uint8_t *)SymAddr("?FireRocket@cGoliathControl@@QAEXIM@Z");
        int nPlay = 0, nGun = 0;
        for (uint8_t *q = from; from && to && play && cg && rk && q < to + 0x40; q++) {
            if (q[0] != 0xE8) continue;
            uint8_t *t = q + 5 + *(int32_t *)(q + 1), *w = nullptr;
            if (t == play) w = (uint8_t *)GoliathPlayMotion, g_goliathPlayMotion = (PlayMotionFn)play, nPlay++;
            else if (t == cg) w = (uint8_t *)GoliathChaingun, g_chaingun = (ChaingunFn)cg, nGun++;
            else if (t == rk) w = (uint8_t *)GoliathRocket, g_rocket = (RocketFn)rk, nGun++;
            if (!w) continue;
            DWORD o;
            VirtualProtect(q + 1, 4, PAGE_EXECUTE_READWRITE, &o);
            *(int32_t *)(q + 1) = (int32_t)(w - (q + 5));
            VirtualProtect(q + 1, 4, o, &o);
            q += 4;
        }
        if (!g_goliathPlayMotion) g_goliathPlayMotion = (PlayMotionFn)play;
        uint32_t *vt = (uint32_t *)SymAddr("??_7cGoliathControl@@6B@");
        uint32_t fn = SymAddr("?UpdateControls@cGoliathControl@@UAEXXZ");
        if (vt && fn && vt[10] == fn) {
            g_goliathUpdate = (UpdateFn)fn;
            DWORD o;
            VirtualProtect(vt + 10, 4, PAGE_READWRITE, &o);
            vt[10] = (uint32_t)GoliathUpdateControls;
            VirtualProtect(vt + 10, 4, o, &o);
        }
        uint32_t col = SymAddr("?Collide@cGoliathControl@@UAEXAAUhkContactPointAddedEvent@@HPAVtThing@@@Z");
        if (vt && col && vt[22] == col) {
            g_goliathCollide = (CollideFn)col;
            DWORD o;
            VirtualProtect(vt + 22, 4, PAGE_READWRITE, &o);
            vt[22] = (uint32_t)GoliathCollide;
            VirtualProtect(vt + 22, 4, o, &o);
        }
        Log("vehicles: Dragoon on cGoliathControl: %d motion calls, %d gun calls wrapped%s", nPlay, nGun,
            g_goliathUpdate ? "" : ", UpdateControls NOT wrapped");
    }
    {   // Dragoon shields: tActor::Damage (sub esp, 8; push ebx; mov ebx, [esp + 0x20])
        static const uint8_t start[] = {0x83, 0xEC, 0x08, 0x53, 0x8B, 0x5C, 0x24, 0x20};
        g_actorDamage = (DamageFn)DetourFn((uint8_t *)SymAddr(
            "?Damage@tActor@@QAEMPAVtThing@@W4eDamageSource@@W4eDamageType@@MPAUXGVECTOR3@@3IW4eHitRegionType@@MPAH@Z"), start, 8, ActorDamage);
        if (!g_actorDamage) Log("vehicles: unexpected tActor::Damage, the Dragoon has no shield");
    }
    {   // Firefly: RT = the cut DoSilentKill (cFireflyControl vtable slot 68 = UpdateInput)
        uint32_t *vt = (uint32_t *)SymAddr("??_7cFireflyControl@@6B@");
        uint32_t fn = SymAddr("?UpdateInput@cFireflyControl@@MAEHH@Z");
        if (vt && fn && vt[68] == fn) {
            g_fireflyInput = (UpdateInputFn)fn;
            DWORD o;
            VirtualProtect(vt + 68, 4, PAGE_READWRITE, &o);
            vt[68] = (uint32_t)FireflyUpdateInput;
            VirtualProtect(vt + 68, 4, o, &o);
        } else
            Log("vehicles: unexpected cFireflyControl vtable, the Firefly's silent kill is not restored");
    }
    if (getenv("GHOSTPC_LOG_VEHICLE")) {
        uint32_t *vt = (uint32_t *)SymAddr("??_7cVultureControl@@6B@");
        uint32_t fn = SymAddr("?UpdateControls@cVultureControl@@UAEXXZ");
        for (int i = 0; vt && fn && i < 120; i++)
            if (vt[i] == fn) {
                g_vultureUpdate = (UpdateFn)fn;
                DWORD o;
                VirtualProtect(vt + i, 4, PAGE_READWRITE, &o);
                vt[i] = (uint32_t)VultureUpdateLog;
                VirtualProtect(vt + i, 4, o, &o);
                Log("vehicles: logging cVultureControl::UpdateControls (vtable slot %d)", i);
                break;
            }
    }
}
