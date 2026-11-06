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
#include <vector>

extern UINT g_frame;
void LevelWarpPlayer(uint8_t *player, const float *p);   // levelscript.cpp
uint8_t *LevelWorld();                                    // levelscript.cpp: the loaded tWorld

namespace {

char *__cdecl StrStrI(const char *hay, const char *needle) {
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) h++, n++;
        if (!*n) return (char *)hay;
    }
    return *needle ? nullptr : (char *)hay;
}
// PreFinalize's name checks: case-insensitive, and GhostPC's reaver_vehicle_player (ghostpc.not) counts as "arclite"
// (the Siege Tank's cTankControl, which vehicles.cpp turns into the Reaver's controls)
char *__cdecl ControlNameMatch(const char *hay, const char *needle) {
    if (!_stricmp(needle, "arclite") && StrStrI(hay, "reaver_vehicle_player")) return (char *)hay;
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
        if (!ctl || *(uint32_t *)ctl != goliath) { seen[local] = 0; continue; }
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
static const float REAVER_SPEED = 0.9f;   // its walk at 0.75 scale: segments slide ~1.5 model units per 1.3 s cycle
static const DWORD REAVER_RELOAD = 3000;
static const float REAVER_TURN = 0.6f;   // rad/s (about 35 deg/s) at full stick
static UpdateFn g_tankUpdate;
static const float REAVER_SCALE = 0.75f;   // its MODELSCALE (ghostpc.not): hull, mount zone, entry points and camera
static bool IsReaver(uint8_t *thing) { return thing && !_stricmp((char *)(thing + 0xc4), "reaver_vehicle_player"); }
static void PlayMotionMode(uint8_t *thing, int mode, bool restart = false) {
    static auto getMode = (int(__thiscall *)(uint8_t *))SymAddr("?GetMoveMode@tObject@@QAE?AW4eMotionMode@@XZ");
    static auto play = (int(__thiscall *)(uint8_t *, int, unsigned, float, void *, float, float))SymAddr(
        "?PlayMotion@tObject@@QAEHW4eMotionMode@@IMPAVtMotionSet@@MM@Z");
    if (!getMode || !play) return;
    int cur = getMode(thing);
    if (!restart && cur == mode) return;
    int r = play(thing, mode, 1, 30.f, nullptr, 7.5f, -1.f);
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
static void ReaverCamera(uint8_t *ctl) {
    static auto getCam = (uint8_t *(__thiscall *)(uint8_t *))SymAddr("?GetCamera@cControl@@QAEPAVtSimCamera@@XZ");
    static auto setPos = (void(__thiscall *)(uint8_t *, const float *, int))SymAddr("?SetChaseCameraPosOffset@tSimCamera@@QAEXPAUXGVECTOR3@@H@Z");
    static auto setLook = (void(__thiscall *)(uint8_t *, const float *, int))SymAddr("?SetChaseLookPosOffset@tSimCamera@@QAEXPAUXGVECTOR3@@H@Z");
    static const float *tankPos = (const float *)SymAddr("?avTankCameraPosOffset@@3PAUXGVECTOR3@@A");
    // at native size: chase, wider chase, at its head; kept low so the camera stays under the field's 12 m walls
    static const float nPos[3][3] = {{0, -18, 9}, {0, -24, 11}, {0, 7, 7.5f}};
    static const float nLook[3][3] = {{0, 6, 4}, {0, 6, 4.5f}, {0, 18, 6.5f}};
    float rPos[3][3], rLook[3][3];
    for (int i = 0; i < 9; i++) (&rPos[0][0])[i] = (&nPos[0][0])[i] * REAVER_SCALE, (&rLook[0][0])[i] = (&nLook[0][0])[i] * REAVER_SCALE;
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
// cTankControl::UIRenderCallback (vtable slot 26) draws the shell's trajectory arc (vuiPlayer_pGrenadeArc) from the
// model's type-2 marker; the Reaver's points down under the map, and a ballistic arc does not describe a homing Scarab,
// so it is not drawn for the Reaver (its aim is the target marker on the unit the next Scarab goes for).
static UpdateFn g_tankUI;
static void __fastcall TankUIRender(uint8_t *ctl, void *) {
    if (!IsReaver(*(uint8_t **)(ctl + 0x180))) g_tankUI(ctl);
}
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
    uint8_t saveX[24], saveRT[24];
    float stickX = 0.f, stickY = 0.f;
    bool firePressed = false;
    if (pad) {   // stdInput slots: 24 bytes from tGamepad +0x64; 1 = X, 7 = RT (+4: the frame it was pressed)
        memcpy(saveX, pad + 0x64 + 24 * 1, 24), memcpy(saveRT, pad + 0x64 + 24 * 7, 24);
        firePressed = frame && *(unsigned *)(saveRT + 4) == *frame;
        memset(pad + 0x64 + 24 * 1, 0, 24), memset(pad + 0x64 + 24 * 7, 0, 24);
        // the left stick (+0x54 X, +0x58 Y): the tank drives toward the stick's direction relative to the camera, so
        // the mouse steered it and it slid sideways; the Reaver drives itself below (tank controls)
        stickX = *(float *)(pad + 0x54), stickY = *(float *)(pad + 0x58);
        *(float *)(pad + 0x54) = 0.f, *(float *)(pad + 0x58) = 0.f;
    }
    g_tankUpdate(ctl);
    if (pad) {
        memcpy(pad + 0x64 + 24 * 1, saveX, 24), memcpy(pad + 0x64 + 24 * 7, saveRT, 24);
        *(float *)(pad + 0x54) = stickX, *(float *)(pad + 0x58) = stickY;
    }
    // the tank code may have dismounted (B): then the control is no longer driven and its camera is gone
    if (LocalPlayer(0) != thing && LocalPlayer(1) != thing) return;
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
        if (fl > 0.01f) {
            v[0] = hf[0] / fl * throttle * REAVER_SPEED, v[1] = hf[1] / fl * throttle * REAVER_SPEED;
            setVel(thing, v);
        }
        // turning: the raycast vehicle's wheels and angular damper resist a spin (tThing::SetRotVel did nothing), so
        // its matrix is turned by turn * REAVER_TURN * frame time and handed to the physics (tThing::SetPhysMatrix)
        static auto setPhysMatrix = (void(__thiscall *)(uint8_t *, const float *))SymAddr("?SetPhysMatrix@tThing@@QAEXPBUXGMATRIX@@@Z");
        static const float *dt = (const float *)(SymAddr("?stdTime@@3VtStdTime@@A") + 0x30);
        if (turn != 0.f && setPhysMatrix && dt && *dt > 0.f && *dt < 0.25f) {
            float a = -turn * REAVER_TURN * *dt, c = cosf(a), sn = sinf(a);
            float m[16];
            memcpy(m, thing + 0x80, sizeof m);   // rows: right, forward, up, position
            for (int r = 0; r < 3; r++) {
                float x = m[4 * r], y = m[4 * r + 1];
                m[4 * r] = x * c - y * sn, m[4 * r + 1] = x * sn + y * c;
            }
            setPhysMatrix(thing, m);
        }
        float h = sqrtf(v[0] * v[0] + v[1] * v[1]);
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
        if (firePressed && now >= nextFire) {
            nextFire = now + REAVER_RELOAD, recoilUntil = now + 1300;
            PlayMotionMode(thing, 48, true);   // recoilstand
            FireScarab(thing);
        } else if (now >= recoilUntil) {
            if (h > 0.2f) PlayMotionMode(thing, 1);                       // walk
            else if (fabsf(dyaw) > 0.3f) PlayMotionMode(thing, dyaw > 0 ? 132 : 133);   // turn left / right
            else PlayMotionMode(thing, 3);                                // stand
        }
        // engine: the Dragoon's idle hum (its foley set's special1, mode 152), which its idle animation's keyframes
        // play in pairs, 0.45 s then 0.8 s apart (logged in the Zoo); here on that rhythm the whole time the Reaver is
        // driven. customsound.cpp drops the tank's own engine sounds for the Reaver.
        static DWORD nextHum;
        static bool second;
        static auto foley = (int(__thiscall *)(uint8_t *, unsigned, uint8_t *, unsigned *, int, float, void *))SymAddr(
            "?PlayFoleyMode@tObject@@QAEHIPAVtThing@@PAIHM1@Z");
        if (foley && now >= nextHum) {
            nextHum = now + (second ? 800 : 450);
            second = !second;
            foley(thing, 152, nullptr, nullptr, 0, 1.f, nullptr);
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
static void *__cdecl TankHull(uint8_t *model, float *scale) {
    if (model && model == g_hullFallback) {
        static float sz = getenv("GHOSTPC_REAVER_HULLZ") ? (float)atof(getenv("GHOSTPC_REAVER_HULLZ")) : 1.f;
        float s[3] = {scale[0] * 2.f * REAVER_SCALE, scale[1] * 2.f * REAVER_SCALE, scale[2] * sz};
        void *h = g_createHull(model, s);
        return h;
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
    // (wider points made the full-size Reaver bounce and stall; these keep it level at about 0.3 m)
    static float z = getenv("GHOSTPC_REAVER_Z") ? (float)atof(getenv("GHOSTPC_REAVER_Z")) : 0.6f;
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

// Entering: cPlayerControl::UpdateActionButton offers "Mount" for a vehicle when Nova stands within 1.5 m (and 1 m in
// height) of one of its entry markers (type 0x0F, numbers 0 / 1; cModelInst::GetMarkerMatrix: +0x10 the direction she
// must face, +0x30 the position) and faces it. The Reaver's model has none: GetMarkerMatrix is detoured and a Reaver's
// missing entry markers are its sides, 7 m left (0) and right (1) of its centre, facing it.
static std::vector<uint8_t *> g_reavers;   // Reaver things in the world (ScanReavers, once a second)
static void ScanReavers() {
    static auto findT = (uint8_t *(__cdecl *)(unsigned))SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    static unsigned *nextGuid = (unsigned *)SymAddr("?nextGUID@tThing@@2IA");
    static uint8_t *world;
    if (LevelWorld() != world) world = LevelWorld(), g_reavers.clear();
    if (!findT || !nextGuid || g_frame % 30) return;
    std::vector<uint8_t *> found;
    for (unsigned g = 1; g < *nextGuid && g < 200000; g++)
        if (uint8_t *t = findT(g))
            if (IsReaver(t)) found.push_back(t);
    g_reavers.swap(found);
    static bool log = getenv("GHOSTPC_LOG_VEHICLE") != nullptr;
    for (uint8_t *t : g_reavers)
        if (log) Log("vehicle: Reaver at %.2f %.2f %.2f heading %.0f", ((float *)(t + 0xb0))[0], ((float *)(t + 0xb0))[1],
                     ((float *)(t + 0xb0))[2], atan2f(((float *)(t + 0x90))[1], ((float *)(t + 0x90))[0]) * 57.29578f);
}
typedef int(__thiscall *MarkerMatrixFn)(void *, unsigned char, unsigned short, float *, float *);
static MarkerMatrixFn g_markerMatrix;
static int __fastcall ReaverMarkerMatrix(void *inst, void *, unsigned char type, unsigned short idx, float *out, float *m) {
    int r = g_markerMatrix(inst, type, idx, out, m);
    if (r || type != 0x0f || idx > 1 || !out) return r;
    for (uint8_t *t : g_reavers) {
        if (*(void **)(t + 0x2a0) != inst) continue;
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
    return r;
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

// The game finds the vehicle to mount with a 1.5 m sphere around Nova's feet (UpdateActionButton: ThingsInSphere),
// which never reaches the Reaver's hull (its underside is about 1.6 m up, over its wide footprint), so its entry
// markers were never looked at. UpdateActionButton is wrapped: when it found nothing to do and Nova stands within
// 1.5 m of a Reaver's hull (any side), facing it, the Reaver becomes her activate target
// with the "Mount" prompt (action 6), as the game does for a vehicle; B then mounts it (cPlayerControl::Activate).
typedef void(__thiscall *UpdateActionFn)(uint8_t *);
static UpdateActionFn g_updateAction;
static void __fastcall ReaverUpdateAction(uint8_t *ctl, void *) {
    g_updateAction(ctl);
    static auto setTarget = (void(__thiscall *)(uint8_t *, uint8_t *))SymAddr("?SetActivateTarget@cPlayerControl@@QAEXPAVtThing@@@Z");
    uint8_t *nova = *(uint8_t **)(ctl + 0x180);
    if (!setTarget || !nova || g_reavers.empty() || *(unsigned *)(ctl + 0x238)) return;
    const float *np = (const float *)(nova + 0xb0), *nf = (const float *)(nova + 0x90);
    for (uint8_t *t : g_reavers) {
        if (*(uint32_t *)(t + 0x360) & 0x10000000) continue;   // dead
        const float *right = (const float *)(t + 0x80), *fwd = (const float *)(t + 0x90), *pos = (const float *)(t + 0xb0);
        float dx = np[0] - pos[0], dy = np[1] - pos[1];
        float lx = dx * right[0] + dy * right[1], ly = dx * fwd[0] + dy * fwd[1];   // Nova in the Reaver's frame
        // close to its hull on any side: within 1.5 m of it (the borrowed tank hull scaled: x +-3.05, y -3.4 .. +5.0,
        // times 2 * REAVER_SCALE)
        const float k = 2.f * REAVER_SCALE;
        if (fabsf(lx) > 3.05f * k + 1.5f || ly < -3.4f * k - 1.5f || ly > 5.f * k + 1.5f || fabsf(pos[2] - np[2]) > 2.f)
            continue;
        float d = sqrtf(dx * dx + dy * dy);
        if (d < 0.1f || -(dx * nf[0] + dy * nf[1]) / d < 0.3f) continue;   // facing it
        setTarget(ctl, t);
        ctl[0x1a] = 6, ctl[0x1b] = 0, ctl[0x5b] = 0;   // "Mount"
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

void VehiclesUpdate() {
    DebugMount();
    DebugItem();
    GhostTemplates();
    ScanReavers();
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

void VehiclesInit() {
    static bool done;
    if (done) return;
    done = true;
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
