// Front-end helpers on top of the game's shell (cShell at shell_Shell): state tracking and Esc handling.
#include "common.h"
#include <stdlib.h>

static uint8_t *Shell() {
    static uint8_t *s = (uint8_t *)SymAddr("?shell_Shell@@3VcShell@@A");
    return s;
}
volatile LONG g_escPressed;   // set by the window procedure (input.cpp)

// Esc in the front end: "Select Profile / Options" (Menu_Main) -> back to Press Start; Press Start -> quit;
// the screens behind it (profile/mission/difficulty select, options, ...) -> B, their "Back" button.
enum { MENU_MAKEPROFILE = 13, MENU_MAIN = 14, MENU_START = 15, MENU_MISSION_SELECT = 0x11, MENU_CREDITS = 0x22 };
// In a mission (no menu, or an in-game menu: ids 1-12) Esc presses Start, like Enter.
static void HandleEsc(uint8_t *s) {
    DWORD state = *(DWORD *)(s + 0x3c), menu = *(DWORD *)(s + 0x6ee0);
    bool menuOn = *(DWORD *)(s + 0x6efc) != 0;
    if (!menuOn || (menu >= 1 && menu <= 12)) {
        extern void PressStart();
        PressStart();
        return;
    }
    if (state != 0) return;   // a transition or video is running
    if (menu == MENU_START) {
        Log("Esc on the Press Start screen: quitting");
        ExitProcess(0);
    }
    if (menu == MENU_MAIN) {
        Log("Esc on the main menu: back to Press Start");
        typedef void(__thiscall * RequestMenu_t)(void *, DWORD, BOOL);
        static auto requestMenu = (RequestMenu_t)SymAddr("?RequestMenu@cShell@@QAEXIH@Z");
        requestMenu(s, MENU_START, FALSE);
        return;
    }
    // eShellMenu 0x11 MISSION_SELECT .. 0x22 CREDITS: mission/difficulty/profile select, delete prompts,
    // options and settings, load game, multiplayer
    if (menu >= MENU_MISSION_SELECT && menu <= MENU_CREDITS) {
        Log("Esc in menu %u: Back", menu);
        extern void PressBack();
        PressBack();
    }
}

// debug: GHOSTPC_SIGHT=<frame> forces the thermal sight view on from that frame (tPlayer +0x51C bit 0, read by
// tPlayer::GetSight)
static void DebugForceSight() {
    static int from = getenv("GHOSTPC_SIGHT") ? atoi(getenv("GHOSTPC_SIGHT")) : -1;
    extern UINT g_frame;
    if (from < 0 || (int)g_frame < from) return;
    static auto activePlayer = (uint8_t *(__cdecl *)())SymAddr("?gameMultiplayer_GetActivePlayer@@YAPAVtPlayer@@XZ");
    if (uint8_t *p = activePlayer()) *(DWORD *)(p + 0x51c) |= 1;
}

// the active player, or null while the game has none (the player list is not set up during startup)
typedef uint8_t *(__cdecl *ActivePlayerFn)();
static uint8_t *CallGuarded(ActivePlayerFn f) {
    __try { return f(); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static uint8_t *SafeActivePlayer() {
    static auto activePlayer = (ActivePlayerFn)SymAddr("?gameMultiplayer_GetActivePlayer@@YAPAVtPlayer@@XZ");
    return CallGuarded(activePlayer);
}
// debug (GHOSTPC_FRAMESTATS): log the player's shadow light (tThing +0x2DC, set by FindIdealShadowLight) when it changes
static void DebugShadowLight() {
    static bool on = getenv("GHOSTPC_FRAMESTATS") != nullptr;
    if (!on) return;
    uint8_t *p = SafeActivePlayer();
    if (!p) return;
    extern UINT g_frame;
    if (g_frame % 300 == 0) { float *pos = (float *)(p + 0xb0); Log("player pos frame %u: %.1f %.1f %.1f", g_frame, pos[0], pos[1], pos[2]); }
    static uint8_t *last;
    uint8_t *l = *(uint8_t **)(p + 0x2dc);
    if (l == last) return;
    last = l;
    extern UINT g_frame;
    if (!l) { Log("shadowlight frame %u: none", g_frame); return; }
    float *pos = (float *)(l + 0x20);
    Log("shadowlight frame %u: %p flags %x type %u pos %.1f %.1f %.1f", g_frame, l, *(DWORD *)l, *(DWORD *)(l + 4), pos[0], pos[1], pos[2]);
}

// debug (GHOSTPC_FRAMESTATS): camera effect slots (tSimCamera +0x26C, 8 x 0x2C; type 3 = shake) and the shake
// offset at +0x3CC, logged while non-zero
static void DebugCameraShake() {
    static bool on = getenv("GHOSTPC_FRAMESTATS") != nullptr;
    if (!on) return;
    static auto firstInfo = (ActivePlayerFn)SymAddr("?gameMultiplayer_GetFirstLocalPlayerInfo@@YAPAVcPlayerInfo@@XZ");
    uint8_t *info = CallGuarded(firstInfo);
    uint8_t *cam = info ? *(uint8_t **)(info + 4) : nullptr;
    if (!cam) return;
    extern UINT g_frame;
    static float lastPos[3];
    float *cp = (float *)(cam + 0x240);
    if (memcmp(cp, lastPos, 12)) {
        Log("campos frame %u: %.5f %.5f %.5f (d %.5f %.5f %.5f)", g_frame, cp[0], cp[1], cp[2], cp[0] - lastPos[0], cp[1] - lastPos[1], cp[2] - lastPos[2]);
        memcpy(lastPos, cp, 12);
    }
    float *off = (float *)(cam + 0x3cc);
    for (int s = 0; s < 8; s++) {
        uint8_t *e = cam + 0x26c + s * 0x2c;
        if (*(DWORD *)e == 3)
            Log("camfx frame %u: slot %d shake start %u end %u amp %g flags %x  offset %.4f %.4f %.4f", g_frame, s,
                *(DWORD *)(e + 0xc), *(DWORD *)(e + 0x10), *(float *)(e + 0x14), *(WORD *)(e + 0x18), off[0], off[1], off[2]);
    }
}

// debug (GHOSTPC_LOGDEATH): the player's position on the first frames of a level
static void DebugEarlyPlayerPos() {
    static bool on = getenv("GHOSTPC_LOGDEATH") != nullptr;
    extern UINT g_frame;
    if (!on || g_frame > 12) return;
    static float *wb = (float *)SymAddr("?simPhysics_worldBounds@@3UtBound3D@@A");
    if (wb) Log("early frame %u: simPhysics_worldBounds %g %g %g .. %g %g %g", g_frame, wb[0], wb[1], wb[2], wb[3], wb[4], wb[5]);
    uint8_t *p = SafeActivePlayer();
    if (!p) { Log("early frame %u: no player", g_frame); return; }
    float *pos = (float *)(p + 0xb0);
    Log("early frame %u: player %p pos %.3f %.3f %.3f", g_frame, p, pos[0], pos[1], pos[2]);
}

// called once per frame (from Swap, on the game thread)
// The Enemy Zoo is open to every profile. cProfile::IsMissionUnlocked(mission, chronicle) unlocks every mission for
// profiles named "NSI" or "NOVA" (case-insensitive) or while mainProfile_bUnlockCheat is set (a command-line switch /
// the unlock-all console command); other profiles see only what they reached in play. The detour adds the zoo
// (mission ID 9_1_ZOO: gameChronicle +0x8 count, +0xC mission pointers, ID at +0x00; cChronicle::FindMission matches
// the level name at +0x20) to what every profile sees; the rest stays as it was.
typedef int(__thiscall *UnlockedFn)(void *, unsigned, unsigned);
static uint8_t *g_unlockedTramp;
static int __fastcall IsMissionUnlocked(void *profile, void *, unsigned mission, unsigned chronicle) {
    if (((UnlockedFn)g_unlockedTramp)(profile, mission, chronicle)) return 1;
    static uint8_t *chron = (uint8_t *)SymAddr("?gameChronicle@@3VcChronicle@@A");
    if (!chron || mission >= *(unsigned *)(chron + 8)) return 0;
    const char *id = *(const char **)(chron + 0xc + 4 * mission);
    return id && !_stricmp(id, "9_1_ZOO");
}
static void HookMissionUnlock() {
    static bool done;
    if (done) return;
    done = true;
    uint8_t *fn = (uint8_t *)SymAddr("?IsMissionUnlocked@cProfile@@QAEHII@Z");
    uint32_t flag = SymAddr("?mainProfile_bUnlockCheat@@3HA");
    if (!fn || fn[0] != 0xA1 || *(uint32_t *)(fn + 1) != flag) {   // mov eax, [mainProfile_bUnlockCheat]
        Log("shell: unexpected cProfile::IsMissionUnlocked, the Enemy Zoo stays locked for new profiles");
        return;
    }
    uint8_t *t = (uint8_t *)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(t, fn, 5);   // the absolute load moves as is
    t[5] = 0xE9;
    *(int32_t *)(t + 6) = (int32_t)((fn + 5) - (t + 10));
    g_unlockedTramp = t;
    uint8_t jmp[5] = {0xE9};
    *(int32_t *)(jmp + 1) = (int32_t)((uint8_t *)IsMissionUnlocked - (fn + 5));
    DWORD old;
    VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy(fn, jmp, 5);
    VirtualProtect(fn, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
}
// Single player can make more than one profile. cShell::Menu_SelProfileSingle (the profile list shown once a profile
// exists) maps the Y button to state 0 and leaves its prompt box empty, so after the first profile there was no way
// to make another; the co-op version of the same menu maps Y to state 0x25 (make profile, also what single player
// goes to while no profile exists; +0x6edc = 0xd brings it back here) with the "{y} Create New" prompt in box 2.
// Patched to match: the Y event's state byte, and the RequestMenuCutscene call at the end now fills the prompt first.
typedef void(__thiscall *CutsceneFn)(void *, char *);
typedef void(__thiscall *TextBoxFn)(void *, unsigned char, char *, float, float, float, float, float, float,
                                    const char *, int);
static CutsceneFn g_menuCutscene;
static void __fastcall SelProfileSingleCutscene(uint8_t *shell, void *, char *cutscene) {
    static auto setTextBox = (TextBoxFn)SymAddr("?SetTextBox@cShellMenu@@QAEXEPADMMMMMMPBDH@Z");
    static auto findString = (const char *(__cdecl *)(const char *))SymAddr("?gameString_Find@@YAPADPBD@Z");
    void *menu = *(void **)(shell + 0x6e3c);
    if (menu && setTextBox && findString) {
        setTextBox(menu, 2, nullptr, 0, 0, 0, 0, 0, 0, findString("MNU_Y_CREATENEW"), 0);
        setTextBox(menu, 3, nullptr, 0, 0, 0, 0, 0, 0, findString("MNU_X_DELETE"), 0);
    }
    g_menuCutscene(shell, cutscene);
}
// Deleting a profile was cut from the menus but not from the shell: events 0x45B + index go to
// cShell::State_DeleteProfile_Start (cShell::UpdateState), which asks "Are you sure want to delete this profile?"
// (Menu_RUSureDelete) and deletes it (State_DeleteProfile). cShell::UpdateInput passes A (stdInput button 4), X (1)
// and start (11) to the profile list (cShellButtonList::ControllerEvent): a button (cShellButton, 0x74 bytes, in the
// list's array at +4, count +0x24) fires its first event (+0x6C) for the buttons in +0x69/+0x6A and its second
// (+0x70) for the one in +0x6B. cShellProfileSelect::Init (vtable slot 0) sets A/start/start and gives both events
// 0x35A + index (load), so X did nothing. In the single-player list (cShell +0x6ED8 = 0x27, set by
// Menu_SelProfileSingle) each button now has X -> 0x45B + index, with the "{x} Delete" prompt (MNU_X_DELETE, a
// string no code used) in the menu's text box 3.
typedef void(__thiscall *ProfileInitFn)(void *);
static ProfileInitFn g_profileInit;
static void __fastcall ProfileSelectInit(uint8_t *list, void *) {
    g_profileInit(list);
    uint8_t *s = Shell();
    uint8_t *buttons = *(uint8_t **)(list + 4);
    if (!s || !buttons || *(uint32_t *)(s + 0x6ed8) != 0x27) return;
    for (unsigned i = 0; i < *(unsigned *)(list + 0x24); i++) {
        uint8_t *b = buttons + i * 0x74;
        b[0x6b] = 1;                           // X
        *(uint32_t *)(b + 0x70) = 0x45b + i;   // delete profile i
    }
}
static void HookProfileCreateNew() {
    static bool done;
    if (done) return;
    done = true;
    uint8_t *fn = (uint8_t *)SymAddr("?Menu_SelProfileSingle@cShell@@QAEXXZ");
    uint8_t *cutscene = (uint8_t *)SymAddr("?RequestMenuCutscene@cShell@@IAEXPAD@Z");
    static const uint8_t yEvent[] = {0x6A, 0x02, 0x6A, 0x00, 0x6A, 0x02};   // SetEventButton(2, state 0, Y)
    if (!fn || !cutscene || memcmp(fn + 0xf9, yEvent, sizeof yEvent) || fn[0x108] != 0xE8 ||
        fn + 0x10d + *(int32_t *)(fn + 0x109) != cutscene) {
        Log("shell: unexpected cShell::Menu_SelProfileSingle, single player stays at one profile");
        return;
    }
    g_menuCutscene = (CutsceneFn)cutscene;
    uint32_t *vt = (uint32_t *)SymAddr("??_7cShellProfileSelect@@6B@");
    if (vt && vt[0] == SymAddr("?Init@cShellProfileSelect@@UAEXXZ")) {
        g_profileInit = (ProfileInitFn)vt[0];
        DWORD o;
        VirtualProtect(vt, 4, PAGE_EXECUTE_READWRITE, &o);
        vt[0] = (uint32_t)ProfileSelectInit;
        VirtualProtect(vt, 4, o, &o);
    } else
        Log("shell: unexpected cShellProfileSelect vtable, profiles cannot be deleted");
    DWORD old;
    VirtualProtect(fn + 0xfc, 0x11, PAGE_EXECUTE_READWRITE, &old);
    fn[0xfc] = 0x25;
    *(int32_t *)(fn + 0x109) = (int32_t)((uint8_t *)SelProfileSingleCutscene - (fn + 0x10d));
    VirtualProtect(fn + 0xfc, 0x11, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn + 0xfc, 0x11);
}
// debug (GHOSTPC_TEST_UNLOCK): at frame 200, which missions a blank profile (no progress, name "test") may select
static void DebugTestUnlock() {
    extern UINT g_frame;
    static bool on = getenv("GHOSTPC_TEST_UNLOCK") != nullptr;
    if (!on || g_frame != 200 || !g_unlockedTramp) return;
    static uint8_t profile[0x20000];
    strcpy((char *)profile + 8, "test");
    uint8_t *chron = (uint8_t *)SymAddr("?gameChronicle@@3VcChronicle@@A");
    unsigned n = chron ? *(unsigned *)(chron + 8) : 0;
    char line[512] = "";
    for (unsigned i = 0; i < n && i < 64; i++) {
        int r = IsMissionUnlocked(profile, nullptr, i, *(unsigned *)chron);
        const char *name = *(const char **)(chron + 0xc + 4 * i) + 0x20;
        if (r) _snprintf(line + strlen(line), sizeof line - strlen(line), " %s", name);
    }
    Log("unlock test: %u missions, unlocked for a blank profile:%s", n, line);
}
void ShellWatch() {
    g_crumb = "shell: hooks";
    HookMissionUnlock();
    HookProfileCreateNew();
    DebugTestUnlock();
    extern void DebugDeathInstall();
    DebugDeathInstall();
    extern void CustomSoundInstall();
    CustomSoundInstall();
    DebugEarlyPlayerPos();
    extern void LevelScriptUpdate();
    LevelScriptUpdate();
    g_crumb = "shell: debug";
    {   // debug (GHOSTPC_TEST_CRASH=<frame>): a deliberate fault, to check the crash report and minidump
        extern UINT g_frame;
        static int crashAt = getenv("GHOSTPC_TEST_CRASH") ? atoi(getenv("GHOSTPC_TEST_CRASH")) : -1;
        if (crashAt >= 0 && (int)g_frame == crashAt) g_crumb = "shell: GHOSTPC_TEST_CRASH", *(volatile int *)8 = 1;
    }
    DebugCameraShake();
    DebugForceSight();
    DebugShadowLight();
    g_crumb = "shell: menus";
    uint8_t *s = Shell();
    if (!s) { g_crumb = "game (after GhostPC's frame step)"; return; }
    extern void ScriptEsc();
    extern void SnapshotMouseLook();
    ScriptEsc();
    SnapshotMouseLook();
    if (InterlockedExchange(&g_escPressed, 0)) HandleEsc(s);
    static DWORD last[4] = {~0u, ~0u, ~0u, ~0u};
    DWORD cur[4] = {*(DWORD *)(s + 0x3c), *(DWORD *)(s + 0x6ee0), *(DWORD *)(s + 0x6e40) != 0, *(DWORD *)(s + 0x6e3c) != 0};
    if (memcmp(cur, last, sizeof cur)) {
        extern UINT g_frame;
        Log("shell: frame %u state %u menu %u keyboard %u menuobj %u", g_frame, cur[0], cur[1], cur[2], cur[3]);
        memcpy(last, cur, sizeof cur);
    }
    g_crumb = "game (after GhostPC's frame step)";
}
