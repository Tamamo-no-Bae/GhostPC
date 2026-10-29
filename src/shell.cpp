// Front-end helpers on top of the game's shell (cShell at shell_Shell): state tracking and Esc handling.
#include "common.h"
#include <stdlib.h>
#include <math.h>
#include <string>
#include <vector>
#include <array>
#include <intrin.h>

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

// Typing a profile name. The shell's on-screen keyboard (cShell +0x6E40 cShellKeyboard: +0xAC m_aOutText[8], +0xB4
// m_nOutTextIndex (7 characters at most), +0xA0 m_bFirstKey (the first key replaces the text), +0xA4 m_nState (0 =
// typing, 1/2 = the invalid-name dialogs)) only took the controller: keyboard keys moved its cursor, Esc was B (its
// backspace) and there was no way out. While it is typing, characters typed on the PC keyboard go straight into the
// name (those its keys have, and space), Backspace and Enter work like its Back and Done (IsValidName first), and Esc
// leaves it (Cancel); input.cpp turns the keyboard's controller bindings off meanwhile.
static bool TypeIntoKeyboard(uint8_t *s, bool esc) {
    extern void SetTextEntry(bool);
    extern int PopTypedChar();
    uint8_t *kb = *(uint8_t **)(s + 0x6e40);
    bool typing = kb && kb[0xa4] == 0;
    SetTextEntry(typing);
    if (!typing) return false;
    typedef void(__thiscall *KbFn)(void *);
    typedef uint8_t(__thiscall *ValidFn)(void *);
    static auto backspace = (KbFn)SymAddr("?BackSpace@cShellKeyboard@@IAEXXZ");
    static auto done = (KbFn)SymAddr("?Done@cShellKeyboard@@IAEXXZ");
    static auto cancel = (KbFn)SymAddr("?Cancel@cShellKeyboard@@IAEXXZ");
    static auto valid = (ValidFn)SymAddr("?IsValidName@cShellKeyboard@@IAEEXZ");
    if (esc) { Log("Esc on the name keyboard: cancel"); cancel(kb); return true; }
    for (int c; (c = PopTypedChar()) >= 0;) {
        if (c == 8) backspace(kb);
        else if (c == 13) {
            uint8_t r = valid(kb);
            if (r == 0) { done(kb); return true; }
            kb[0xa4] = r;   // the "invalid name" / "name in use" dialog
            return true;
        } else if (c == ' ' || (c > ' ' && c < 127 && memchr(kb + 0x10, c, 0x80))) {   // m_aKeyText: the keys' letters
            if (*(int *)(kb + 0xa0)) kb[0xb4] = 0, *(int *)(kb + 0xa0) = 0;
            uint8_t i = kb[0xb4];
            if (i >= 7) continue;
            kb[0xac + i] = (char)c, kb[0xac + i + 1] = 0, kb[0xb4] = i + 1;
        }
    }
    return false;
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
    if (chronicle == 2) return 1;   // competitive (ghostmpcomp.nsc: the arena only): open to every profile
    if (((UnlockedFn)g_unlockedTramp)(profile, mission, chronicle)) return 1;
    // co-op (ghostmpcoop.nsc's save slot 1): a mission either player has unlocked (or every mission with a NOVA / NSI
    // profile in either seat); the game asked player 1's profile only
    static void **players = (void **)SymAddr("?mainProfile_pPlayer0@@3PAVcProfile@@A");   // pPlayer0, pPlayer1
    if (chronicle == 1 && players)
        for (int i = 0; i < 2; i++)
            if (players[i] && players[i] != profile && ((UnlockedFn)g_unlockedTramp)(players[i], mission, chronicle))
                return 1;
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
// The co-op profile menu (cShell::Menu_SelProfileMultiCoop, menu 20, +0x6ED8 = 0x28) is shown twice: player 1's pick
// (mainProfile_pPlayer0 not set yet; State_LoadProfile sets player 0, then player 1, and returns to
// State_SelectProfileCoop, which opens it again until both are set), then player 2's (player 1's profile greyed out).
// Both said "Select Profile" (the menu name, MNU_SEL_PROF, is its title) and "{y} Create New" in both prompt boxes.
// Now the title is MNU_SEL_PROF_P1 / _P2 (in game.nls, unused) and box 3 is "{x} Delete" on player 1's pick (X
// deletes, ProfileSelectInit) and empty on player 2's, where deleting player 1's profile would leave it dangling.
static void __fastcall SelProfileCoopCutscene(uint8_t *shell, void *, char *cutscene) {
    static auto setTextBox = (TextBoxFn)SymAddr("?SetTextBox@cShellMenu@@QAEXEPADMMMMMMPBDH@Z");
    static auto findString = (const char *(__cdecl *)(const char *))SymAddr("?gameString_Find@@YAPADPBD@Z");
    static void **player0 = (void **)SymAddr("?mainProfile_pPlayer0@@3PAVcProfile@@A");
    char *menu = *(char **)(shell + 0x6e3c);
    if (menu && setTextBox && findString && player0) {
        bool first = !*player0;
        strcpy(menu, first ? "MNU_SEL_PROF_P1" : "MNU_SEL_PROF_P2");   // cShellMenu +0: name (title), 0x2C bytes
        setTextBox(menu, 3, nullptr, 0, 0, 0, 0, 0, 0, first ? findString("MNU_X_DELETE") : "", 0);
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
    static void **player0 = (void **)SymAddr("?mainProfile_pPlayer0@@3PAVcProfile@@A");
    if (!s || !buttons) return;
    uint32_t menu = *(uint32_t *)(s + 0x6ed8);
    if (menu != 0x27 && !(menu == 0x28 && player0 && !*player0)) return;   // single player, co-op player 1's pick
    for (unsigned i = 0; i < *(unsigned *)(list + 0x24); i++) {
        uint8_t *b = buttons + i * 0x74;
        b[0x6b] = 1;                           // X
        *(uint32_t *)(b + 0x70) = 0x45b + i;   // delete profile i
    }
}
static bool RedirectCall(uint8_t *call, void *expect, void *to);
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
    uint8_t *coop = (uint8_t *)SymAddr("?Menu_SelProfileMultiCoop@cShell@@QAEXXZ");
    if (!coop || !RedirectCall(coop + 0x13f, cutscene, SelProfileCoopCutscene))
        Log("shell: unexpected cShell::Menu_SelProfileMultiCoop, co-op profile menus stay as they were");
}
// a call instruction (E8 rel32) to `expect` redirected to `to`; false (and nothing changed) if it is something else
static bool RedirectCall(uint8_t *call, void *expect, void *to) {
    if (!call || call[0] != 0xE8 || call + 5 + *(int32_t *)(call + 1) != expect) return false;
    DWORD old;
    VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old);
    *(int32_t *)(call + 1) = (int32_t)((uint8_t *)to - (call + 5));
    VirtualProtect(call + 1, 4, old, &old);
    return true;
}
// Player 2's start. cPlayerInfo::BeginLevel places the right-hand player at the start with order 1
// (gameMultiplayer_PlacePlayer -> tWorld::FindPlayerStart(number, order, 0), which falls back to a start with the
// same number, then PlacePlayer to number 0, then to the origin), and no level has order-1 starts: player 2 landed
// inside player 1, at every start and every level change. PlacePlayer's lookups go through P2FindStart, which gives
// order 1, when the level has none of its own, a copy of the order-0 start (tPlayerStart 0x60 bytes: sector +0,
// matrix +0x10 with the position in its last row +0x40, number +0x50, order +0x54) moved 1 m along the start's side
// axis (the other way, or not at all, when that point is in none of the level's sectors). A level without starts
// (the zoo) places player 1 at the origin: player 2 gets the origin moved the same way. The copies are kept (per
// world and start number), as the player may hold on to its start.
typedef uint8_t *(__thiscall *FindStartFn)(void *world, unsigned number, unsigned order, int conversation);
static FindStartFn g_findStart;
static uint8_t *__fastcall P2FindStart(void *world, void *, unsigned number, unsigned order, int conversation) {
    uint8_t *r = g_findStart(world, number, order, conversation);
    if (order != 1 || conversation || (r && *(unsigned *)(r + 0x54) == 1)) return r;
    static struct { void *world; unsigned number; uint8_t rec[0x60]; } copies[32];
    static unsigned next;
    for (auto &c : copies)
        if (c.world == world && c.number == number) return c.rec;
    typedef void *(__thiscall *SectorFn)(void *, const float *);
    static auto sectorOf = (SectorFn)SymAddr("?FindPosSector@tWorld@@QAEPAVtSector@@PBUXGVECTOR3@@@Z");
    uint8_t *base = g_findStart(world, number, 0, 0);
    if (!base) base = r;
    if (!base && *(unsigned *)((uint8_t *)world + 0xf0)) return nullptr;   // PlacePlayer tries start number 0 next
    auto &c = copies[next++ % 32];
    c.world = world, c.number = number;
    memset(c.rec, 0, sizeof c.rec);
    if (base) memcpy(c.rec, base, sizeof c.rec);
    else for (int i = 0; i < 4; i++) ((float *)(c.rec + 0x10))[5 * i] = 1.f;   // identity
    *(unsigned *)(c.rec + 0x50) = number;
    *(unsigned *)(c.rec + 0x54) = 1;
    float *m = (float *)(c.rec + 0x10), *pos = m + 12;
    float sx = m[0], sy = m[1], len = sqrtf(sx * sx + sy * sy);
    if (len < 0.01f) sx = 1, sy = 0, len = 1;
    const float home[3] = {pos[0], pos[1], pos[2]};
    static const float sides[] = {1.f, -1.f, 0.f};
    for (float d : sides) {
        float p[3] = {home[0] + d * sx / len, home[1] + d * sy / len, home[2] + 0.1f};
        void *sector = sectorOf ? sectorOf(world, p) : nullptr;
        if (!sector && d) continue;
        pos[0] = p[0], pos[1] = p[1], pos[2] = home[2];
        if (sector) *(void **)c.rec = sector;
        break;
    }
    Log("shell: player 2 start %u: %.2f %.2f %.2f (%s)", number, pos[0], pos[1], pos[2],
        base ? "beside player 1's" : "the origin");
    return c.rec;
}
// Co-op from the main menu. The shell has the whole co-op path (Menu_Multiplayer, menu 28: Cooperative ->
// State_SelectProfileCoop, which loads Misc\ghostmpcoop.nsc, asks for player 2's profile (menu 20) and opens the
// co-op mission select (menu 31); cShell +0x6E34 makes gameMultiplayer_BeginGame create player 2), but nothing raised
// its state 0x22: Menu_Main has Resume Campaign, Options and (mainOptions_bDebugMenu) Debug. Menu_Main now makes one
// button more (its SetNumberOfButtons immediates 2 / 3): button 1, set where Menu_Main goes on to its text boxes, is
// "Multiplayer" (no such string in game.nls) under button 0, now "Singleplayer" (was MNU_RESUME_CAMPAIGN, "Select
// Profile"); Options moves to button 2 and
// a row down (its SetButton immediates), Debug to button 3. Up/down goes in button order.
typedef void(__thiscall *SetButtonFn)(void *, uint8_t, char *, char *, float, float, float, float, const char *,
                                      unsigned long, uint8_t, uint8_t, unsigned);
typedef void(__thiscall *SetTextBoxesFn)(void *, uint8_t);
static void __fastcall MainMenuTextBoxes(uint8_t *menu, void *, uint8_t n) {
    static auto setButton = (SetButtonFn)SymAddr("?SetButton@cShellMenu@@QAEXEPAD0MMMMPBDKEEI@Z");
    static auto setTextBoxes = (SetTextBoxesFn)SymAddr("?SetNumberOfTextBoxes@cShellMenu@@QAEXE@Z");
    if (setButton && menu[0x44] > 2) {   // cShellMenu: +0x40 buttons, +0x44 count
        setButton(menu, 0, nullptr, nullptr, 320.f, 350.f, 0, 0, "Singleplayer", 0x27, 4, 0xb, 0);   // was "Select Profile"
        setButton(menu, 1, nullptr, nullptr, 320.f, 372.f, 0, 0, "Multiplayer", 0x22, 4, 0xb, 0);
    }
    setTextBoxes(menu, n);
}
// The co-op mission select (cShell::Menu_MultiCoop, menu 31) builds the same list as single player's
// (cShellMenu::CreateMissionSelect) but without what Menu_MissionSelect sets first: the menu's +0x2C flag and its
// name (title) MNU_SEL_MISSION. Its list did not scroll past the screen's bottom and it kept the previous menu's name.
static void __fastcall CoopMissionSelect(uint8_t *menu, void *) {
    static auto create = (void(__thiscall *)(void *))SymAddr("?CreateMissionSelect@cShellMenu@@QAEXXZ");
    menu[0x2c] = 1;
    strcpy((char *)menu, "MNU_SEL_MISSION");
    create(menu);
}
// Competitive asks for the number of players first. Menu_Multiplayer (menu 28, shell state 0x22) is built again as
// that menu: "2 Players" goes on to State_SelectProfileComp (state 0x2B, which loads ghostmpcomp.nsc when the
// previous state is 0x22, as it still is), "4 Players" is greyed out (cShellMenuItem +0x28 nonzero: Render greys
// it, SelectNext/PreviousButton skip it; 4 players is for another patch), B goes back to the Multiplayer menu.
// Competitive's button raises 0x2B as before; its call in cShell::UpdateState goes through SelectProfileComp, which
// shows the players menu unless the player came from "2 Players" (or is between the two profile picks).
static bool g_playersMenuNext, g_inPlayersMenu, g_compPlayersChosen;
static void __fastcall MultiplayerCutscene(uint8_t *shell, void *, char *cutscene) {
    static auto setButton = (SetButtonFn)SymAddr("?SetButton@cShellMenu@@QAEXEPAD0MMMMPBDKEEI@Z");
    static auto setEvent = (void(__thiscall *)(void *, uint8_t, unsigned long, uint8_t))
        SymAddr("?SetEventButton@cShellMenu@@QAEXEKE@Z");
    static auto requestCutscene = (CutsceneFn)SymAddr("?RequestMenuCutscene@cShell@@IAEXPAD@Z");
    uint8_t *menu = *(uint8_t **)(shell + 0x6e3c);
    g_inPlayersMenu = g_playersMenuNext && menu && setButton && setEvent;
    g_playersMenuNext = g_compPlayersChosen = false;
    if (g_inPlayersMenu) {
        strcpy((char *)menu, "MNU_SEL_PLAYERS");
        // where Menu_Multiplayer puts Cooperative / Competitive (position, text offset 85, 6)
        static char laser[] = "DebugTextbox";
        setButton(menu, 0, nullptr, laser, 150.f, 160.f, 85.f, 6.f, "2 Players", 0x2b, 4, 0xb, 0);
        setButton(menu, 1, nullptr, laser, 330.f, 220.f, 85.f, 6.f, "4 Players", 0x22, 4, 0xb, 0);
        *(uint32_t *)(*(uint8_t **)(menu + 0x40) + 0x74 + 0x28) = 4;   // greyed, not selectable
        setEvent(menu, 0, 0x22, 5);   // B: back to the Multiplayer menu
    }
    requestCutscene(shell, cutscene);
}
static void __fastcall SelectProfileComp(uint8_t *shell, void *) {
    static auto selectComp = (void(__thiscall *)(void *))SymAddr("?State_SelectProfileComp@cShell@@QAEXXZ");
    static auto changeState = (int(__thiscall *)(void *, unsigned long, int))SymAddr("?ChangeState@cShell@@QAEHKH@Z");
    if (g_inPlayersMenu) g_inPlayersMenu = false, g_compPlayersChosen = true;   // "2 Players"
    if (g_compPlayersChosen) {
        selectComp(shell);
        return;
    }
    g_playersMenuNext = true;   // Competitive: the players menu first
    changeState(shell, 0x22, 0);
}
static void HookCoopMenu() {
    static bool done;
    if (done) return;
    done = true;
    uint8_t *mp = (uint8_t *)SymAddr("?Menu_Multiplayer@cShell@@QAEXXZ");
    uint8_t *us = (uint8_t *)SymAddr("?UpdateState@cShell@@IAEXXZ");
    if (!mp || !us || !RedirectCall(mp + 0x150, (void *)SymAddr("?RequestMenuCutscene@cShell@@IAEXPAD@Z"),
                                    MultiplayerCutscene) ||
        !RedirectCall(us + 0x371, (void *)SymAddr("?State_SelectProfileComp@cShell@@QAEXXZ"), SelectProfileComp))
        Log("shell: unexpected cShell::Menu_Multiplayer / UpdateState, Competitive has no players menu");
    // Competitive's profile menu (Menu_SelProfileMultiComp, menu 21) is an older layout of the co-op one (three text
    // boxes, "Select Profile" as a text box, no title, red/white button tints): it jumps to Menu_SelProfileMultiCoop
    // (same events, same +0x6ED8 = 0x28), which has the P1/P2 titles, Delete and Create New. Its mission select
    // (Menu_MultiComp, menu 32) gets the co-op list's +0x2C flag and title.
    uint8_t *compProf = (uint8_t *)SymAddr("?Menu_SelProfileMultiComp@cShell@@QAEXXZ");
    uint8_t *coopProf = (uint8_t *)SymAddr("?Menu_SelProfileMultiCoop@cShell@@QAEXXZ");
    if (compProf && coopProf && compProf[0] == 0x56 && compProf[1] == 0x8b) {   // push esi; mov esi, ecx
        DWORD old;
        VirtualProtect(compProf, 5, PAGE_EXECUTE_READWRITE, &old);
        compProf[0] = 0xE9;
        *(int32_t *)(compProf + 1) = (int32_t)(coopProf - (compProf + 5));
        VirtualProtect(compProf, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), compProf, 5);
    } else
        Log("shell: unexpected cShell::Menu_SelProfileMultiComp, the competitive profile menu stays as it was");
    uint8_t *comp = (uint8_t *)SymAddr("?Menu_MultiComp@cShell@@QAEXXZ");
    if (!comp || !RedirectCall(comp + 0x30, (void *)SymAddr("?CreateMissionSelect@cShellMenu@@QAEXXZ"), CoopMissionSelect))
        Log("shell: unexpected cShell::Menu_MultiComp, the competitive mission list does not scroll");
    uint8_t *coop = (uint8_t *)SymAddr("?Menu_MultiCoop@cShell@@QAEXXZ");
    if (!coop || !RedirectCall(coop + 0x30, (void *)SymAddr("?CreateMissionSelect@cShellMenu@@QAEXXZ"), CoopMissionSelect))
        Log("shell: unexpected cShell::Menu_MultiCoop, the co-op mission list does not scroll");
    uint8_t *f = (uint8_t *)SymAddr("?Menu_Main@cShell@@QAEXXZ");
    void *textBoxes = (void *)SymAddr("?SetNumberOfTextBoxes@cShellMenu@@QAEXE@Z");
    static const float optionsY = 394.f;
    if (!f || memcmp(f + 0x2d, "\x6a\x03\xeb\x02\x6a\x02", 6) || memcmp(f + 0x8d, "\x68\x00\x00\xba\x43", 5) ||
        memcmp(f + 0x9b, "\x6a\x01", 2) || memcmp(f + 0xd9, "\x6a\x02", 2) ||
        !RedirectCall(f + 0x106, textBoxes, MainMenuTextBoxes)) {
        Log("shell: unexpected cShell::Menu_Main, no Multiplayer button");
        return;
    }
    DWORD old;
    VirtualProtect(f + 0x2d, 0xb0, PAGE_EXECUTE_READWRITE, &old);
    f[0x2e] = 4, f[0x32] = 3;           // SetNumberOfButtons(debug ? 4 : 3)
    memcpy(f + 0x8e, &optionsY, 4);     // Options: y 372 -> 394
    f[0x9c] = 2, f[0xda] = 3;           // Options button 2, Debug button 3
    VirtualProtect(f + 0x2d, 0xb0, old, &old);
}
// Player 2's look. Both players were made from the same template (mainOptions_aPlayerTemplate, "nova") by
// gameMultiplayer_CreatePlayer, so player 2 was a second, identical Nova. The build's colour variants (Nova_red,
// Nova_bl, Nova_br, Nova_ice) define nothing of their own. Player 2 is now made from the Nova_ice template
// (GHOSTPC_P2_TEMPLATE changes it), and that template gets the model nova_ice.nod (scripts\make_skin.py: Nova with
// the Ice materials of Materials\ghostpc.nsa): its model modifier (tTemplate +0x28 count, +0x2C 36-byte tModifier
// array, id 4 = model, as in level placements) is set, or added when the template has none of its own. The calls
// that create player 2 (gameMultiplayer_BeginGame's second, CmdNewPlayer's) go through P2CreatePlayer.
typedef uint8_t *(__cdecl *CreatePlayerFn)(void *, int);
static CreatePlayerFn g_createPlayer;
static const char *P2Template() {
    static const char *t = getenv("GHOSTPC_P2_TEMPLATE") ? getenv("GHOSTPC_P2_TEMPLATE") : "Nova_ice";
    return t;
}
static uint8_t *__cdecl P2CreatePlayer(void *tmpl, int flag) {
    typedef void *(__cdecl *FindFn)(const char *);
    static auto find = (FindFn)SymAddr("?gameTemplate_Find@@YAPAVtTemplate@@PBD@Z");
    if (void *p2 = find ? find(P2Template()) : nullptr) tmpl = p2;
    Log("shell: player 2 from template %s", tmpl ? (const char *)tmpl : "(none)");
    uint8_t *p = g_createPlayer(tmpl, flag);
    uint32_t names = SymAddr("?gameMotion_apNames@@3PAPADA");
    Log("shell: player 2 %p info %p (left %p right %p)", p, p ? *(void **)(p + 0x520) : nullptr,
        *(void **)(names + 0x728), *(void **)(names + 0x72c));
    return p;
}
static void SetSkinModel(const char *templ, const char *model) {
    typedef uint8_t *(__cdecl *FindFn)(const char *);
    typedef void *(__cdecl *MallocFn)(unsigned, const char *, unsigned);
    typedef int(__cdecl *OpenFn)();
    static auto find = (FindFn)SymAddr("?gameTemplate_Find@@YAPAVtTemplate@@PBD@Z");
    static auto alloc = (MallocFn)SymAddr("?stdMemory_Malloc@@YAPAXIPADI@Z");
    static auto open = (OpenFn)SymAddr("?stdMemory_Open@@YAHXZ");
    uint8_t *t = find ? find(templ) : nullptr;
    if (!t || !alloc || !open) return;
    unsigned n = *(unsigned *)(t + 0x28);
    uint8_t *mods = *(uint8_t **)(t + 0x2c);
    for (unsigned i = 0; i < n; i++) {
        if (*(unsigned *)(mods + 36 * i) != 4) continue;
        Log("shell: template %s: model %s -> %s", templ, (char *)(mods + 36 * i + 4), model);
        strncpy((char *)(mods + 36 * i + 4), model, 31);
        return;
    }
    open();
    uint8_t *m = (uint8_t *)alloc(36 * (n + 1), "new", 0);
    if (!m) return;
    memcpy(m, mods, 36 * n);
    memset(m + 36 * n, 0, 36);
    *(unsigned *)(m + 36 * n) = 4;
    strncpy((char *)(m + 36 * n + 4), model, 31);
    *(uint8_t **)(t + 0x2c) = m;
    *(unsigned *)(t + 0x28) = n + 1;
    Log("shell: template %s: %u modifier(s), model %s added", templ, n, model);
}
// The two players' pads. gameMultiplayer_UpdateControls / UpdateCameras / UpdateCamerasBasic / UpdateRender / UpdateUI run
// each local player in turn (esi = pLeftLocalPlayer, then pRightLocalPlayer) after an inlined "make it current":
// stdInput_SetCurrentGamepad(current->pad), then current (0x828520) = the player. That selects the previous player's
// pad (cPlayerInfo +0x10): with two players the pads swapped (player 1's keyboard/pad moved player 2). The pad is now
// read from the player being made current: movzx eax, word [eax+0x10] -> [esi+0x10], mov cx, [eax+0x10] -> [esi+0x10].
static void FixSplitPads() {
    uint8_t *from = (uint8_t *)SymAddr("?gameMultiplayer_UpdateControls@@YAXXZ");
    uint8_t *to = (uint8_t *)SymAddr("?gameMultiplayer_UpdateUI@@YAXXZ");
    if (!from || !to || to < from) return;
    to += 0x8c;   // UpdateUI's body
    int n = 0;
    for (uint8_t *p = from; p + 6 < to; p++) {
        // [eax+0x10] / [ecx+0x10] (UpdateRender: [edx+0x10]) -> [esi+0x10]
        bool a = !memcmp(p, "\x0f\xb7\x40\x10\x50\xe8", 6) || !memcmp(p, "\x66\x8b\x42\x10\x50\xe8", 6);
        bool b = !memcmp(p, "\x66\x8b\x48\x10\x51\xe8", 6) || !memcmp(p, "\x66\x8b\x4a\x10\x51\xe8", 6);
        if (!a && !b) continue;
        DWORD old;
        VirtualProtect(p + 2, 1, PAGE_EXECUTE_READWRITE, &old);
        p[2] = a ? 0x46 : 0x4e;
        VirtualProtect(p + 2, 1, old, &old);
        n++;
    }
    Log("shell: split-screen pads: %d gamepad selections fixed", n);
}
// debug (GHOSTPC_LOG_TERRAIN): terrain areas drawn per render camera, logged every 120 frames
typedef unsigned(__cdecl *DrawAreaFn)(void *, void *, unsigned);
static DrawAreaFn g_drawArea;
static unsigned __cdecl LogDrawArea(void *terrain, void *area, unsigned flags) {
    static void **cur = (void **)SymAddr("?renderCamera_pCurCamera@@3PAVtRenderCamera@@A");
    static void *cams[4];
    static unsigned n[4], drawn[4], last;
    extern UINT g_frame;
    void *c = cur ? *cur : nullptr;
    int k = 0;
    while (k < 3 && cams[k] && cams[k] != c) k++;
    cams[k] = c;
    unsigned r = g_drawArea(terrain, area, flags);
    n[k]++, drawn[k] += r;
    if (g_frame - last >= 120) {
        last = g_frame;
        Log("terrain: frame %u: cam %p %u areas (%u), cam %p %u areas (%u)", g_frame, cams[0], n[0], drawn[0], cams[1], n[1], drawn[1]);
        memset(n, 0, sizeof n), memset(drawn, 0, sizeof drawn);
    }
    return r;
}
// Split-screen HUD: GhostPC's D3D layer maps each player's HUD from the full screen into the player's half (see
// d3d_core.cpp, SetHudMapping). cPlayerInfo::GetScreenPos (both overloads), which did that for a few HUD parts, is an
// identity now (it would map them twice; with one player the canvas is the whole screen anyway). What both players
// share stays full-screen, as in single player: the briefing and dialogue windows (vuiBriefing_Update ->
// cUIBriefing::Update, from vui_Update), the objective box (cMiniMap::DrawHint, from vuiMiniMap_Render; drawn in both
// players' views at the same place), and the shell's menus and load screen.
void SetHudMapping(bool on);
typedef void(__cdecl *VoidFn)();
static VoidFn g_briefUpdate;
static void __cdecl BriefUpdate() {
    SetHudMapping(false);
    g_briefUpdate();
    SetHudMapping(true);
}
typedef void(__thiscall *DrawHintFn)(void *, int);
static DrawHintFn g_drawHint;
static void __fastcall DrawHint(void *map, void *, int a) {
    SetHudMapping(false);
    g_drawHint(map, a);
    SetHudMapping(true);
}
bool ShellMenuShown() {   // a shell menu (or the load screen) is up: cShell +0x6E3C
    uint8_t *s = Shell();
    return s && *(void **)(s + 0x6e3c);
}
// Whose HUD: HUD code asks gameMultiplayer_GetFirstLocalPlayer (always player 1) where it means the player whose HUD
// it draws: both halves showed player 1's health bars, timer bar (searching a body), crosshair state, radar and calldown
// UI. In these functions the calls go to HudPlayer: the current local player (gameMultiplayer_GetActivePlayer's: the
// one being drawn or updated; with one player the same), player 1 while none is current (GetActivePlayer itself
// would read through null then).
static void *__cdecl HudPlayer() {
    uint32_t names = SymAddr("?gameMotion_apNames@@3PAPADA");
    void **info = *(void ***)(names + 0x720);
    if (!info) info = *(void ***)(names + 0x728);
    return info ? info[0] : nullptr;
}
static void HudPlayerCalls() {
    static const char *fns[] = {
        "?vuiPlayer_DrawTimerBar@@YAXXZ", "?DrawPlayerBars@cMiniMap@@AAEXXZ", "?DrawVehicleHealth@cMiniMap@@AAEXXZ",
        "?DrawCrossHair@cUITarget@@AAEXPAVtPlayer@@PAVtObject@@@Z", "?RenderBlips@cRadar@@IAEXMM@Z",
        "?RenderBackground@cRadar@@IAEXMM@Z", "?Setup@cUICalldown@@UAEHXZ", "?Reset@cUICalldown@@QAEXXZ",
        "?IsValidTargetUI@@YAHPAVtThing@@@Z", "?IsFriendUI@@YAHPAVtThing@@@Z"};
    void *first = (void *)SymAddr("?gameMultiplayer_GetFirstLocalPlayer@@YAPAVtPlayer@@XZ");
    void *active = (void *)HudPlayer;
    int n = 0;
    for (const char *name : fns) {
        uint8_t *f = (uint8_t *)SymAddr(name);
        if (!f || !first || !active) continue;
        for (uint8_t *p = f; p < f + 0x2000; p++) {
            const GhostSym *at = SymAt((uint32_t)p);
            if (!at || at->addr != (uint32_t)f) break;   // the next function
            if (RedirectCall(p, first, active)) n++, p += 4;
        }
    }
    Log("shell: split-screen HUD: %d player lookups now per player", n);
}
// Deaths in co-op. tActor::Kill opens the death menu (cShell::RequestDeathMenu) for a player only in a single-player
// game; with two local players cPlayerControl::UpdateDead instead revives the dead one in place on a button press
// (what competitive wants), so in co-op player 2 died without consequence. In co-op (the chronicle's game type 1,
// ghostmpcoop.nsc; gameChronicle +4) both checks now answer "single player": either player's death ends the mission
// as Nova's does. Falling to death fails the mission with HUD_DEATH_FALL ("Nova fell to her death."); for player 2
// (Echo) the reason reads "Echo fell to her death." (a string gameString_Find does not know is shown as given).
static int __cdecl CoopIsSinglePlayer() {
    static auto single = (int(__cdecl *)())SymAddr("?gameMultiplayer_IsSinglePlayerGame@@YAHXZ");
    static uint8_t *chron = (uint8_t *)SymAddr("?gameChronicle@@3VcChronicle@@A");
    return single() || (chron && *(int *)(chron + 4) == 1);
}
typedef int(__thiscall *FailFn)(void *, char *);
static FailFn g_failMission;
static int __fastcall FellToDeath(void *seq, void *, char *reason) {
    uint32_t names = SymAddr("?gameMotion_apNames@@3PAPADA");
    void *right = *(void **)(names + 0x72c);
    static char echo[] = "Echo fell to her death.";
    return g_failMission(seq, right && *(void **)(names + 0x720) == right ? echo : reason);
}
// Competitive respawn: a dead player is revived in place on a button press (cPlayerControl::UpdateDead ->
// tActor::Revive); in a competitive game (the chronicle's game type 2, ghostmpcomp.nsc) she then goes to a random one
// of the level's respawn points (the level script's "respawn" lines), not one within 8 m of the other player, nor
// the one the other player took in the last 5 s (both dead at once: she may not have left it, or not have been moved
// there yet).
const std::vector<std::array<float, 4>> &LevelRespawns();   // levelscript.cpp
void LevelWarpPlayer(uint8_t *player, const float *p);
typedef int(__thiscall *ReviveFn)(void *, float, float, int);
static ReviveFn g_revive;
static int __fastcall CompetitiveRevive(uint8_t *actor, void *, float health, float shield, int flag) {
    int r = g_revive(actor, health, shield, flag);
    static uint8_t *chron = (uint8_t *)SymAddr("?gameChronicle@@3VcChronicle@@A");
    const auto &spots = LevelRespawns();
    if (!chron || *(int *)(chron + 4) != 2 || spots.empty()) return r;
    uint32_t names = SymAddr("?gameMotion_apNames@@3PAPADA");
    void **l = *(void ***)(names + 0x728), **rt = *(void ***)(names + 0x72c);
    uint8_t *other = l && l[0] == actor ? (rt ? (uint8_t *)rt[0] : nullptr) : (l ? (uint8_t *)l[0] : nullptr);
    static struct { void *who; int spot; DWORD at; } taken[2];
    int self = l && l[0] == actor ? 0 : 1;
    DWORD now = GetTickCount();
    auto takenByOther = [&](int i) { return taken[!self].spot == i && taken[!self].who && now - taken[!self].at < 5000; };
    std::vector<int> ok;
    for (int i = 0; i < (int)spots.size(); i++) {
        const float *o = other ? (const float *)(other + 0xb0) : nullptr, *p = spots[i].data();
        if (takenByOther(i)) continue;
        if (!o || (o[0] - p[0]) * (o[0] - p[0]) + (o[1] - p[1]) * (o[1] - p[1]) > 64.f) ok.push_back(i);
    }
    if (ok.empty()) for (int i = 0; i < (int)spots.size(); i++) if (!takenByOther(i)) ok.push_back(i);
    if (ok.empty()) ok.push_back(0);
    static unsigned seed = GetTickCount();
    seed = seed * 1103515245u + 12345u + (unsigned)(uintptr_t)actor;
    int spot = ok[(seed >> 8) % ok.size()];
    taken[self] = {actor, spot, now};
    const float *p = spots[spot].data();
    LevelWarpPlayer(actor, p);
    Log("shell: competitive respawn: player %d at point %d (%.1f %.1f, %u points free)", self + 1, spot, p[0], p[1],
        (unsigned)ok.size());
    return r;
}
static void HookCoopDeath() {
    {
        uint8_t *dead = (uint8_t *)SymAddr("?UpdateDead@cPlayerControl@@AAEXXZ");
        void *revive = (void *)SymAddr("?Revive@tActor@@QAEHMMH@Z");
        g_revive = (ReviveFn)revive;
        if (!dead || !RedirectCall(dead + 0x4c, revive, CompetitiveRevive)) Log("shell: unexpected UpdateDead, competitive revives in place");
    }
    void *single = (void *)SymAddr("?gameMultiplayer_IsSinglePlayerGame@@YAHXZ");
    uint8_t *kill = (uint8_t *)SymAddr("?Kill@tActor@@QAEHPAVtThing@@W4eDamageSource@@W4eDamageType@@MPAUXGVECTOR3@@3IW4eHitRegionType@@M@Z");
    uint8_t *dead = (uint8_t *)SymAddr("?UpdateDead@cPlayerControl@@AAEXXZ");
    if (!kill || !dead || !RedirectCall(kill + 0x319, single, CoopIsSinglePlayer) || !RedirectCall(dead + 3, single, CoopIsSinglePlayer))
        Log("shell: unexpected tActor::Kill / cPlayerControl::UpdateDead, co-op deaths do not end the mission");
    void *fail = (void *)SymAddr("?FailMission@tSequence@@QAEHPAD@Z");
    g_failMission = (FailFn)fail;
    static const char *falls[][2] = {{"?FallUpdate@cPlayerControl@@AAEXXZ", (const char *)0x103},
                                     {"?StrafeFallUpdate@cPlayerControl@@AAEXXZ", (const char *)0xe4}};
    for (auto &f : falls) {
        uint8_t *fn = (uint8_t *)SymAddr(f[0]);
        if (!fn || !RedirectCall(fn + (uintptr_t)f[1], fail, FellToDeath)) Log("shell: unexpected %s, Echo's fall reads as Nova's", f[0]);
    }
}
// The reticle. vuiTarget_Draw keeps the aiming state in two globals: a "keep drawing" counter (0x7D69D0, the map's
// vuiSound_pFoleySet+0x4C; 1 while the player's control has its aim flag, +0x70 bit 1, then counted down) and the
// aiming control's type (0x7C3AD8, vuiBriefing_briefing+0xB88). Run once per player's HUD, player 1's aim left them set
// for player 2's pass: either player aiming put the reticle on both screens. Each local player now has its own copy.
static void(__cdecl *g_targetDraw)();
static void __cdecl SplitTargetDraw() {
    static uint32_t *linger = (uint32_t *)(SymAddr("?vuiSound_pFoleySet@@3PAVtFoleySet@@A") + 0x4c);
    static uint32_t *type = (uint32_t *)(SymAddr("?vuiBriefing_briefing@@3VcUIBriefing@@A") + 0xb88);
    static uint32_t saved[2][2];
    uint32_t names = SymAddr("?gameMotion_apNames@@3PAPADA");
    void *right = *(void **)(names + 0x72c);
    if (!right) { g_targetDraw(); return; }
    int i = *(void **)(names + 0x720) == right;
    *linger = saved[i][0], *type = saved[i][1];
    g_targetDraw();
    saved[i][0] = *linger, saved[i][1] = *type;
}
// The aim target. One global (0x87B0C8, the map's effectManager_Manager+0x1F80: the GUID of what the aiming player
// points at) is written by every player's laser sight (cEffectLaserSight::Render, owner actor +0x5C) and calldown
// (tPlayerCalldown::GetTarget, a ray from its player's camera; player +0x10), and read by the HUD (reticle, weapon
// callouts, the calldown UI's target: cUICalldown::DrawTarget) and cPlayerControl::UpdateAssistTarget. With two players
// the other one's write came between a player's aim and its HUD: player 1's irradiate target was cleared before her
// calldown UI drew. Each local player now has its own copy: the writers work on their owner's, and the multiplayer
// loops' per-player sections (their stdInput_SetCurrentGamepad calls, gameMultiplayer_Update*) switch the global to
// the player whose turn it is.
static uint32_t g_aimTarget[2];
static void *g_lastCalldownTarget[2];   // debug (GHOSTPC_LOG_CALLDOWN)
static int g_aimSlot;
static uint32_t *AimGlobal() {
    static uint32_t *g = (uint32_t *)(SymAddr("?effectManager_Manager@@3VcEffectManager@@A") + 0x1f80);
    return g;
}
static int PlayerSlot(void *player) {   // 0/1: pLeftLocalPlayer's / pRightLocalPlayer's tPlayer, -1 otherwise
    uint32_t names = SymAddr("?gameMotion_apNames@@3PAPADA");
    void **l = *(void ***)(names + 0x728), **r = *(void ***)(names + 0x72c);
    return !r || !player ? -1 : l && l[0] == player ? 0 : r[0] == player ? 1 : -1;
}
// the calldown target next to it (+0x1F84: what the HUD's callout offers a calldown on, cleared by
// cUIWeapons::DrawCallouts and set by tThing::DrawCallout in a player's HUD pass, read by
// cPlayerControl::ActivateCalldown in her control update) is swapped along with it: player 2's HUD pass had left it
// empty for player 1's B, so her irradiate never started
static uint32_t g_calldownTarget[2];
static void SwitchAimSlot(int s) {
    if (s < 0 || s == g_aimSlot) return;
    g_aimTarget[g_aimSlot] = AimGlobal()[0], g_calldownTarget[g_aimSlot] = AimGlobal()[1];
    AimGlobal()[0] = g_aimTarget[s], AimGlobal()[1] = g_calldownTarget[s];
    g_aimSlot = s;
}
static void __cdecl SectionGamepad(unsigned short pad) {   // the MP loops' "make this player current"
    static auto set = (void(__cdecl *)(unsigned short))SymAddr("?stdInput_SetCurrentGamepad@@YAXG@Z");
    set(pad);
    uint32_t names = SymAddr("?gameMotion_apNames@@3PAPADA");
    uint8_t *l = *(uint8_t **)(names + 0x728), *r = *(uint8_t **)(names + 0x72c);
    if (l && r) SwitchAimSlot(*(unsigned short *)(r + 0x10) == pad ? 1 : 0);
}
// a writer on its owner's copy, then back to the section's
template <class F> static void OnOwnersAim(void *player, F call) {
    int s = PlayerSlot(player), cur = g_aimSlot;
    if (s < 0) { call(); return; }
    SwitchAimSlot(s);
    call();
    SwitchAimSlot(cur);
}
typedef void(__thiscall *LaserRenderFn)(void *);
static LaserRenderFn g_laserRender;
static void __fastcall LaserRender(uint8_t *laser, void *) {
    uint8_t *owner = *(uint8_t **)(laser + 0x5c);
    static bool logOn = getenv("GHOSTPC_LOG_CALLDOWN") != nullptr;
    static UINT lastLog;
    extern UINT g_frame;
    if (logOn && g_frame - lastLog >= 30) {
        lastLog = g_frame;
        Log("calldown: laser owner %p (type %d) slot %d, aim copies %08x %08x, global %08x (section %d)", owner,
            owner ? *(int *)(owner + 0x10) : -1, PlayerSlot(owner), g_aimTarget[0], g_aimTarget[1], *AimGlobal(),
            g_aimSlot + 1);
    }
    // each laser is drawn in both players' views; drawn in the other one's (its setup fails there and it writes 0)
    // it must not change its owner's target: that cleared player 1's every other frame (no HOLD B, the calldown broke off)
    int s = PlayerSlot(owner);
    if (s >= 0 && s != g_aimSlot) {
        uint32_t keep = s ? g_aimTarget[1] : g_aimTarget[0];
        OnOwnersAim(owner, [&] { g_laserRender(laser); });
        g_aimTarget[s] = keep;
        return;
    }
    OnOwnersAim(owner, [&] { g_laserRender(laser); });
}
typedef void *(__thiscall *GetTargetFn)(void *, float *);
static GetTargetFn g_getTarget;
static void *__fastcall CalldownTarget(uint8_t *calldown, void *, float *dist) {
    void *r = nullptr;
    OnOwnersAim(*(void **)(calldown + 0x10), [&] { r = g_getTarget(calldown, dist); });
    int s = PlayerSlot(*(void **)(calldown + 0x10));
    if (s >= 0) g_lastCalldownTarget[s] = r;
    static UINT calls[2], lastLog;
    extern UINT g_frame;
    if (s >= 0) calls[s]++;
    if (getenv("GHOSTPC_LOG_CALLDOWN") && g_frame - lastLog >= 30) {
        lastLog = g_frame;
        Log("calldown: GetTarget calls player 1 %u, player 2 %u; this one player %d -> %p", calls[0], calls[1], s + 1, r);
    }
    return r;
}
// A running calldown (tPlayerCalldown::Update, vtable slot 4 of every calldown class) checks every frame that its
// player still holds B (stdInput_pCurrentGamepad, the control's button +0x1B0) and stops otherwise. It runs outside
// the players' sections, with the gamepad of whoever went last (player 2): player 1's irradiate started and stopped
// at once (no minigame, B fell through to shooting), and player 2's B kept or ended both. It now runs with its
// owner's pad (player +0x10 -> +0x520 cPlayerInfo, pad +0x10) and aim target.
typedef void(__thiscall *CalldownTickFn)(void *);
static CalldownTickFn g_calldownTick;
// The strikes (tComsatCalldown / tArcliteCalldown::DoFire, tYamatoCalldown::Explode) create their effect thing in
// gameMultiplayer_GetFirstLocalPlayer's world and sector (player +0x14 / +0x24): player 2's strike went into player
// 1's sector. During a calldown's update those calls return the calldown's own player (else player 1, as before).
static uint8_t *g_calldownOwner;
static void *__cdecl CalldownPlayer() {
    static auto first = (void *(__cdecl *)())SymAddr("?gameMultiplayer_GetFirstLocalPlayer@@YAPAVtPlayer@@XZ");
    return g_calldownOwner ? g_calldownOwner : first();
}
static void __fastcall CalldownTick(uint8_t *cd, void *) {
    uint8_t *player = *(uint8_t **)(cd + 0x10);
    int s = PlayerSlot(player);
    uint8_t *info = player ? *(uint8_t **)(player + 0x520) : nullptr;
    if (s < 0 || !info) { g_calldownTick(cd); return; }
    static void **pad = (void **)SymAddr("?stdInput_pCurrentGamepad@@3PAUtGamepad@@A");
    static auto setPad = (void(__cdecl *)(unsigned short))SymAddr("?stdInput_SetCurrentGamepad@@YAXG@Z");
    void *oldPad = *pad;
    int oldSlot = g_aimSlot;
    setPad(*(unsigned short *)(info + 0x10));
    SwitchAimSlot(s);
    uint8_t *oldOwner = g_calldownOwner;
    g_calldownOwner = player;
    g_calldownTick(cd);
    g_calldownOwner = oldOwner;
    SwitchAimSlot(oldSlot);
    *pad = oldPad;
}
static void HookCalldownTicks() {
    static const char *calldowns[] = {"tPlayerCalldown", "tComsatCalldown", "tIrradiateCalldown", "tArcliteCalldown",
                                      "tYamatoCalldown", "tNukeCalldown", "tEMPCalldown", "tIdentifyCalldown"};
    uint32_t tick = SymAddr("?Update@tPlayerCalldown@@UAEXXZ");
    g_calldownTick = (CalldownTickFn)tick;
    int n = 0;
    for (const char *c : calldowns) {
        uint32_t *vt = (uint32_t *)SymAddr(("??_7" + std::string(c) + "@@6B@").c_str());
        if (!vt || !tick || vt[4] != tick) continue;
        DWORD old;
        VirtualProtect(vt + 4, 4, PAGE_READWRITE, &old);
        vt[4] = (uint32_t)CalldownTick;
        VirtualProtect(vt + 4, 4, old, &old);
        n++;
    }
    Log("shell: calldowns run with their player's pad: %d of 8", n);
    void *first = (void *)SymAddr("?gameMultiplayer_GetFirstLocalPlayer@@YAPAVtPlayer@@XZ");
    static const char *strikes[] = {"?DoFire@tComsatCalldown@@UAEHXZ", "?DoFire@tArcliteCalldown@@UAEHXZ",
                                    "?Explode@tYamatoCalldown@@AAEXXZ"};
    int m = 0;
    for (const char *name : strikes) {
        uint8_t *f = (uint8_t *)SymAddr(name);
        for (uint8_t *p = f; f && first && p < f + 0x400; p++) {
            const GhostSym *at = SymAt((uint32_t)p);
            if (!at || at->addr != (uint32_t)f) break;
            if (RedirectCall(p, first, CalldownPlayer)) m++, p += 4;
        }
    }
    Log("shell: calldown strikes in their player's sector: %d call sites", m);
}
static void HookAimTarget() {
    HookCalldownTicks();
    auto slot = [](uint32_t *at, void *to) {
        DWORD old;
        VirtualProtect(at, 4, PAGE_READWRITE, &old);
        *at = (uint32_t)to;
        VirtualProtect(at, 4, old, &old);
    };
    uint32_t get = SymAddr("?GetTarget@tPlayerCalldown@@UAEPAVtThing@@AAM@Z");
    g_getTarget = (GetTargetFn)get;
    static const char *calldowns[] = {"tPlayerCalldown", "tComsatCalldown", "tIrradiateCalldown", "tArcliteCalldown",
                                      "tYamatoCalldown", "tNukeCalldown", "tEMPCalldown", "tIdentifyCalldown"};
    int n = 0;
    for (const char *c : calldowns) {
        uint32_t *vt = (uint32_t *)SymAddr(("??_7" + std::string(c) + "@@6B@").c_str());
        if (vt && get && vt[1] == get) slot(vt + 1, CalldownTarget), n++;
    }
    uint32_t *lvt = (uint32_t *)SymAddr("??_7cEffectLaserSight@@6B@");
    uint32_t render = SymAddr("?Render@cEffectLaserSight@@UAEXXZ");
    if (lvt && render && lvt[0x44 / 4] == render) g_laserRender = (LaserRenderFn)render, slot(lvt + 0x44 / 4, LaserRender), n++;
    // the per-player sections of gameMultiplayer_UpdateControls .. UpdateUI
    uint8_t *from = (uint8_t *)SymAddr("?gameMultiplayer_UpdateControls@@YAXXZ");
    uint8_t *to = (uint8_t *)SymAddr("?gameMultiplayer_UpdateUI@@YAXXZ");
    void *pad = (void *)SymAddr("?stdInput_SetCurrentGamepad@@YAXG@Z");
    int calls = 0;
    for (uint8_t *p = from; from && to && p < to + 0x8c; p++)
        if (RedirectCall(p, pad, SectionGamepad)) calls++, p += 4;
    Log("shell: aim target per player: %d writers, %d sections", n, calls);
}
// Script grants go to both players. Mission scripts enable and disable calldowns (ScriptPlayer::SetCalldownEnabled:
// player +0x4F0 calldowns, +0xB8 bit 3 = disabled; e.g. irradiate at the 1_2_1 gate, cCalldownToggle) and weapons
// (EnablePlayerWeapon / DisablePlayerWeapon: player +0x460 weapons, +0x5F bit 1 = disabled) on one player, the one
// the trigger or script names: the other one went without (only Echo could irradiate). Done on that player and, in
// co-op, on the other local player as well. (ScriptThing +0: the GUID; replacements of the three functions.)
static uint8_t *ScriptThingPlayer(void *script) {
    static auto find = (uint8_t *(__cdecl *)(unsigned))SymAddr("?gameThing_FindGUID@@YAPAVtThing@@I@Z");
    uint8_t *t = find ? find(*(unsigned *)script) : nullptr;
    return t && *(int *)(t + 0x10) == 7 ? t : nullptr;
}
template <class F> static int ForScriptPlayers(void *script, F apply) {
    uint8_t *t = ScriptThingPlayer(script);
    if (!t) return 0;
    int ok = apply(t);
    int s = PlayerSlot(t);
    if (s >= 0) {
        uint32_t names = SymAddr("?gameMotion_apNames@@3PAPADA");
        uint8_t **other = *(uint8_t ***)(names + (s ? 0x728 : 0x72c));
        if (other && other[0]) apply(other[0]);
    }
    return ok;
}
static int __fastcall ScriptSetCalldownEnabled(void *script, void *, unsigned i, int enable) {
    return ForScriptPlayers(script, [&](uint8_t *p) {
        uint8_t *c = *(uint8_t **)(p + 0x4f0 + 4 * i);
        if (!c) return 0;
        uint32_t &f = *(uint32_t *)(c + 0xb8);
        f = enable ? f & ~8u : f | 8u;
        return 1;
    });
}
static int SetWeaponDisabled(void *script, unsigned w, bool off) {
    return ForScriptPlayers(script, [&](uint8_t *p) {
        uint8_t *wp = *(uint8_t **)(p + 0x460 + 4 * w);
        if (!wp) return 0;
        wp[0x5f] = off ? wp[0x5f] | 2 : wp[0x5f] & ~2;
        return 1;
    });
}
static int __fastcall ScriptEnableWeapon(void *script, void *, unsigned w) { return SetWeaponDisabled(script, w, false); }
static int __fastcall ScriptDisableWeapon(void *script, void *, unsigned w) { return SetWeaponDisabled(script, w, true); }
static void HookScriptGrants() {
    static const struct { const char *name; void *to; uint8_t first[5]; } fns[] = {
        {"?SetCalldownEnabled@ScriptPlayer@@QAEHIH@Z", (void *)ScriptSetCalldownEnabled, {0xe8}},
        {"?EnablePlayerWeapon@ScriptPlayer@@QAEHW4ePlayerWeapons@@@Z", (void *)ScriptEnableWeapon, {0xe8}},
        {"?DisablePlayerWeapon@ScriptPlayer@@QAEHW4ePlayerWeapons@@@Z", (void *)ScriptDisableWeapon, {0xe8}}};
    for (auto &f : fns) {
        uint8_t *p = (uint8_t *)SymAddr(f.name);
        if (!p || p[0] != 0xe8) { Log("shell: unexpected %s, scripts grant it to one player", f.name); continue; }
        DWORD old;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
        p[0] = 0xe9;
        *(int32_t *)(p + 1) = (int32_t)((uint8_t *)f.to - (p + 5));
        VirtualProtect(p, 5, old, &old);
    }
}
// Either player clears triggers. Mission scripts name "the player" with ScriptPlayer::GetCurPlayer (player 1's GUID,
// tThing +0x34) and many triggers check the actor that set them off against it (objective updates, level changes,
// weapon/calldown toggles, damage regions, cM131_* / cM432_* scenes): player 2 alone could not clear them. While a
// trigger event (scriptMessage_Triggered / Entered / Exited / Touched / Activated, every call of them) names a local
// player, GetCurPlayer answers that player; otherwise player 1, as before.
static uint8_t *g_scriptActor;
static unsigned __cdecl ScriptCurPlayer() {
    static auto first = (uint8_t *(__cdecl *)())SymAddr("?gameMultiplayer_GetFirstLocalPlayer@@YAPAVtPlayer@@XZ");
    uint8_t *p = g_scriptActor ? g_scriptActor : first();
    return p ? *(unsigned *)(p + 0x34) : 0;
}
template <class F> static auto AsActor(void *a, void *b, F call) -> decltype(call()) {
    uint8_t *old = g_scriptActor;
    if (PlayerSlot(a) >= 0) g_scriptActor = (uint8_t *)a;
    else if (PlayerSlot(b) >= 0) g_scriptActor = (uint8_t *)b;
    struct Restore { uint8_t *o; ~Restore() { g_scriptActor = o; } } restore{old};
    return call();
}
typedef void(__cdecl *TriggeredFn)(void *, int, unsigned, int, float, float, float, float);
typedef void(__cdecl *PairFn)(void *, void *);
typedef int(__cdecl *ActivatedFn)(void *, void *);
static TriggeredFn g_msgTriggered;
static PairFn g_msgEntered, g_msgExited, g_msgTouched;
static ActivatedFn g_msgActivated;
static void __cdecl MsgTriggered(void *t, int a, unsigned b, int c, float x, float y, float z, float w) {
    AsActor(t, nullptr, [&] { g_msgTriggered(t, a, b, c, x, y, z, w); });
}
static void __cdecl MsgEntered(void *a, void *b) { AsActor(a, b, [&] { g_msgEntered(a, b); }); }
static void __cdecl MsgExited(void *a, void *b) { AsActor(a, b, [&] { g_msgExited(a, b); }); }
static void __cdecl MsgTouched(void *a, void *b) { AsActor(a, b, [&] { g_msgTouched(a, b); }); }
static int __cdecl MsgActivated(void *a, void *b) { return AsActor(a, b, [&] { return g_msgActivated(a, b); }); }
static void HookScriptActor() {
    uint8_t *cur = (uint8_t *)SymAddr("?GetCurPlayer@ScriptPlayer@@SAIXZ");
    if (cur && cur[0] == 0xe8) {
        DWORD old;
        VirtualProtect(cur, 5, PAGE_EXECUTE_READWRITE, &old);
        cur[0] = 0xe9;
        *(int32_t *)(cur + 1) = (int32_t)((uint8_t *)ScriptCurPlayer - (cur + 5));
        VirtualProtect(cur, 5, old, &old);
    } else
        Log("shell: unexpected ScriptPlayer::GetCurPlayer, triggers answer player 1 only");
    struct { const char *name; void *to; void **orig; } msgs[] = {
        {"?scriptMessage_Triggered@@YAXPAVtThing@@HIHMMMM@Z", (void *)MsgTriggered, (void **)&g_msgTriggered},
        {"?scriptMessage_Entered@@YAXPAVtThing@@0@Z", (void *)MsgEntered, (void **)&g_msgEntered},
        {"?scriptMessage_Exited@@YAXPAVtThing@@0@Z", (void *)MsgExited, (void **)&g_msgExited},
        {"?scriptMessage_Touched@@YAXPAVtThing@@0@Z", (void *)MsgTouched, (void **)&g_msgTouched},
        {"?scriptMessage_Activated@@YAHPAVtThing@@0@Z", (void *)MsgActivated, (void **)&g_msgActivated}};
    uint8_t *lo = nullptr, *hi = nullptr;   // the code section (.text) of the game image
    {
        uint8_t *base = (uint8_t *)g_imageBase;
        auto *nt = (IMAGE_NT_HEADERS32 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        auto *sec = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
            if (!memcmp(sec->Name, ".text", 6)) lo = base + sec->VirtualAddress, hi = lo + sec->Misc.VirtualSize - 5;
    }
    int n = 0;
    for (auto &m : msgs) {
        void *f = (void *)SymAddr(m.name);
        if (!f) continue;
        *m.orig = f;
        for (uint8_t *p = lo; p < hi; p++)
            if (p[0] == 0xe8 && p + 5 + *(int32_t *)(p + 1) == f && RedirectCall(p, f, m.to)) n++, p += 4;
    }
    Log("shell: triggers accept either player: %d event call sites", n);
}
// Player 2 follows player 1's script and cutscene moves. Cutscenes star player 1 (Nova): cCueRow::WarpPlayerIntoPlace
// and DoPlayerMove put her in place, and scripts move "the player" (ScriptThing::SetPosition / JumpToThingMatrix),
// e.g. into the gated area before its gate closes, leaving player 2 (Echo) outside. Their tThing::JumpToMatrix calls
// go through FollowJump: when it moves player 1, player 2 goes 1 m to her side (the other side, or onto her spot, when
// that point is in none of the level's sectors). XGMATRIX rows: right, forward, up, position.
typedef void(__thiscall *JumpFn)(void *, void *, void *, float *, int);
static JumpFn g_jump;
static void __fastcall FollowJump(uint8_t *thing, void *, void *world, void *sector, float *m, int flag) {
    g_jump(thing, world, sector, m, flag);
    uint32_t names = SymAddr("?gameMotion_apNames@@3PAPADA");
    void **l = *(void ***)(names + 0x728), **r = *(void ***)(names + 0x72c);
    if (!l || !r || l[0] != thing || !r[0] || !world) return;
    typedef void *(__thiscall *SectorFn)(void *, const float *);
    static auto sectorOf = (SectorFn)SymAddr("?FindPosSector@tWorld@@QAEPAVtSector@@PBUXGVECTOR3@@@Z");
    float m2[16];
    memcpy(m2, m, sizeof m2);
    float sx = m[0], sy = m[1], len = sqrtf(sx * sx + sy * sy);
    if (len < 0.01f) sx = 1, sy = 0, len = 1;
    void *sec = sector;
    static const float sides[] = {1.f, -1.f, 0.f};
    for (float d : sides) {
        float p[3] = {m[12] + d * sx / len, m[13] + d * sy / len, m[14] + 0.1f};
        void *s = sectorOf ? sectorOf(world, p) : nullptr;
        if (!s && d) continue;
        m2[12] = p[0], m2[13] = p[1];
        if (s) sec = s;
        break;
    }
    g_jump(r[0], world, sec, m2, flag);
    Log("shell: player 2 follows player 1 to %.1f %.1f %.1f", m2[12], m2[13], m2[14]);
}
static void HookFollowJumps() {
    void *jump = (void *)SymAddr("?JumpToMatrix@tThing@@QAEXPAVtWorld@@PAVtSector@@PAUXGMATRIX@@H@Z");
    g_jump = (JumpFn)jump;
    static const char *fns[] = {"?WarpPlayerIntoPlace@cCueRow@@QAEXXZ", "?DoPlayerMove@cCueRow@@AAEXHM@Z",
                                "?SetPosition@ScriptThing@@QAEHPAUXGVECTOR3@@@Z", "?JumpToThingMatrix@ScriptThing@@QAEHI@Z"};
    int n = 0;
    for (const char *name : fns) {
        uint8_t *f = (uint8_t *)SymAddr(name);
        for (uint8_t *p = f; f && jump && p < f + 0x800; p++) {
            const GhostSym *at = SymAt((uint32_t)p);
            if (!at || at->addr != (uint32_t)f) break;
            if (RedirectCall(p, jump, FollowJump)) n++, p += 4;
        }
    }
    Log("shell: player 2 follows player 1's script moves: %d call sites", n);
}
// The calldown minigame (irradiate etc.: cUICalldown, one per player's cUISelect; owner = cSelectUI +0x18 -> +4
// cPlayerInfo) updates outside the players' sections, with the gamepad (stdInput_pCurrentGamepad) and aim target of
// whichever player went last - player 2: her B hold drove both players' minigames, player 1's never started. Each
// now updates with its owner's pad (cPlayerInfo +0x10) and aim target. Debug (GHOSTPC_LOG_CALLDOWN): every 30 frames
// per player its state (the owner's control +0x70 aim flags, +0x7C..0x84 the three zones, the last target ray).
static void(__thiscall *g_calldownUpdate)(void *);
static void __fastcall CalldownUpdate(uint8_t *ui, void *) {
    extern UINT g_frame;
    uint8_t *info = *(uint8_t **)(*(uint8_t **)(ui + 0x18) + 4);
    uint8_t *player = info ? *(uint8_t **)info : nullptr;
    int s = PlayerSlot(player);
    static UINT last[2];
    static bool logOn = getenv("GHOSTPC_LOG_CALLDOWN") != nullptr;
    if (logOn && s >= 0 && g_frame - last[s] >= 30) {
        last[s] = g_frame;
        uint8_t *ctl = player ? *(uint8_t **)(player + 0x3f8) : nullptr;
        Log("calldown: frame %u player %d ui %p aim %02x zones %u %u %u weapon %u target %p section %d pad %p", g_frame,
            s + 1, ui, ctl ? ctl[0x70] : 0, *(uint32_t *)(ui + 0x7c), *(uint32_t *)(ui + 0x80), *(uint32_t *)(ui + 0x84),
            player ? *(uint32_t *)(player + 0x454) : 0, g_lastCalldownTarget[s], g_aimSlot + 1,
            *(void **)SymAddr("?stdInput_pCurrentGamepad@@3PAUtGamepad@@A"));
    }
    if (s < 0) { g_calldownUpdate(ui); return; }
    static void **pad = (void **)SymAddr("?stdInput_pCurrentGamepad@@3PAUtGamepad@@A");
    static auto setPad = (void(__cdecl *)(unsigned short))SymAddr("?stdInput_SetCurrentGamepad@@YAXG@Z");
    void *oldPad = *pad;
    int oldSlot = g_aimSlot;
    setPad(*(unsigned short *)(info + 0x10));
    SwitchAimSlot(s);
    g_calldownUpdate(ui);
    SwitchAimSlot(oldSlot);
    *pad = oldPad;
}
// debug (GHOSTPC_LOG_CALLDOWN): the callout pass (cUIWeapons::DrawCallouts, which offers the HOLD B calldown) and
// cPlayerControl::ActivateCalldown (B), with the current local player, the aim slot and both target globals
static void(__thiscall *g_drawCallouts)(void *, int);
static void(__thiscall *g_activateCalldown)(void *);
static void __fastcall LogDrawCallouts(void *w, void *, int a) {
    extern UINT g_frame;
    static UINT last;
    uint32_t before = AimGlobal()[1];
    g_drawCallouts(w, a);
    if (g_frame - last >= 15) {
        last = g_frame;
        uint32_t names = SymAddr("?gameMotion_apNames@@3PAPADA");
        void **cur = *(void ***)(names + 0x720);
        Log("calldown: DrawCallouts(%d) frame %u current player %d slot %d aim %08x calldown %08x -> %08x", a, g_frame,
            PlayerSlot(cur ? cur[0] : nullptr) + 1, g_aimSlot + 1, AimGlobal()[0], before, AimGlobal()[1]);
    }
}
static void __fastcall LogActivateCalldown(uint8_t *ctl, void *) {
    Log("calldown: ActivateCalldown player %d slot %d aim %08x calldown %08x", PlayerSlot(*(void **)(ctl + 0x180)) + 1,
        g_aimSlot + 1, AimGlobal()[0], AimGlobal()[1]);
    g_activateCalldown(ctl);
}
typedef void(__thiscall *CutEvFn)(void *, unsigned, int, int, float, float, float);
static CutEvFn g_interruptCut;
static void __fastcall LogInterruptCut(uint8_t *script, void *, unsigned id, int type, int a, float x, float y, float z) {
    Log("calldown: interrupt script cutscene event %u type %d (player guid %08x)", id, type, *(uint32_t *)(script + 0x2c));
    g_interruptCut(script, id, type, a, x, y, z);
}
typedef int(__thiscall *StartFireFn)(void *, int);
static StartFireFn g_irradiateStart;
static int __fastcall LogIrradiateStart(uint8_t *cd, void *, int skipScript) {
    int r = g_irradiateStart(cd, skipScript);
    Log("calldown: irradiate StartFire(%d) player %d target %08x -> %d", skipScript, PlayerSlot(*(void **)(cd + 0x10)) + 1,
        *(uint32_t *)(cd + 0x90), r);
    return r;
}
static void HookCalldownLog() {
    if (!getenv("GHOSTPC_LOG_CALLDOWN")) return;
    auto slot = [](const char *vtName, int i, void *to, void **orig) {
        uint32_t *vt = (uint32_t *)SymAddr(vtName);
        if (!vt) return;
        *orig = (void *)vt[i];
        DWORD old;
        VirtualProtect(vt + i, 4, PAGE_READWRITE, &old);
        vt[i] = (uint32_t)to;
        VirtualProtect(vt + i, 4, old, &old);
    };
    slot("??_7cCalldownInterrupt@@6B@", 48, (void *)LogInterruptCut, (void **)&g_interruptCut);
    slot("??_7tIrradiateCalldown@@6B@", 2, (void *)LogIrradiateStart, (void **)&g_irradiateStart);
    void *dc = (void *)SymAddr("?DrawCallouts@cUIWeapons@@QAEXH@Z"), *ac = (void *)SymAddr("?ActivateCalldown@cPlayerControl@@QAEXXZ");
    g_drawCallouts = (void(__thiscall *)(void *, int))dc, g_activateCalldown = (void(__thiscall *)(void *))ac;
    static const char *fns[] = {"?RenderInGame@cUISelect@@QAEXXZ", "?Render@cUISelect@@QAEXPAVtPlayer@@@Z",
                                "?GunCrouchUpdate@cPlayerControl@@AAEXXZ", "?GaussRifleUpdate@cPlayerControl@@AAEXXZ",
                                "?SniperUpdate@cPlayerControl@@AAEXXZ", "?SniperHangUpdate@cPlayerControl@@AAEXXZ"};
    int n = 0;
    for (const char *name : fns) {
        uint8_t *f = (uint8_t *)SymAddr(name);
        for (uint8_t *p = f; f && p < f + 0x800; p++) {
            const GhostSym *at = SymAt((uint32_t)p);
            if (!at || at->addr != (uint32_t)f) break;
            if (RedirectCall(p, dc, LogDrawCallouts) || RedirectCall(p, ac, LogActivateCalldown)) n++, p += 4;
        }
    }
    Log("calldown: logging %d call sites", n);
}
static void HookSplitHud() {
    HudPlayerCalls();
    HookFollowJumps();
    HookScriptGrants();
    HookScriptActor();
    HookCalldownLog();
    {
        uint32_t *vt = (uint32_t *)SymAddr("??_7cUICalldown@@6B@");
        uint32_t upd = SymAddr("?Update@cUICalldown@@UAEXXZ");
        if (vt && vt[3] == upd) {
            g_calldownUpdate = (void(__thiscall *)(void *))upd;
            DWORD old;
            VirtualProtect(vt + 3, 4, PAGE_READWRITE, &old);
            vt[3] = (uint32_t)CalldownUpdate;
            VirtualProtect(vt + 3, 4, old, &old);
        }
    }
    HookAimTarget();
    {
        uint8_t *vui = (uint8_t *)SymAddr("?vui_Update@@YAXXZ");
        void *draw = (void *)SymAddr("?vuiTarget_Draw@@YAXXZ");
        g_targetDraw = (void(__cdecl *)())draw;
        if (!vui || !RedirectCall(vui + 0xbb, draw, SplitTargetDraw)) Log("shell: unexpected vui_Update, the reticle shows on both screens");
    }
    HookCoopDeath();
    static const char *pos[] = {"?GetScreenPos@cPlayerInfo@@QAEXPAUXGVECTOR2@@@Z",
                                "?GetScreenPos@cPlayerInfo@@QAEXPAUXGVECTOR3@@@Z"};
    for (const char *n : pos) {
        uint8_t *f = (uint8_t *)SymAddr(n);
        if (!f || f[0] != 0x66 || f[1] != 0x83 || f[2] != 0x3d) { Log("shell: unexpected %s", n); continue; }
        DWORD old;
        VirtualProtect(f, 3, PAGE_EXECUTE_READWRITE, &old);
        f[0] = 0xC2, f[1] = 4, f[2] = 0;   // ret 4
        VirtualProtect(f, 3, old, &old);
    }
    uint8_t *vui = (uint8_t *)SymAddr("?vui_Update@@YAXXZ");
    void *brief = (void *)SymAddr("?vuiBriefing_Update@@YAXXZ");
    g_briefUpdate = (VoidFn)brief;
    if (!vui || !RedirectCall(vui + 0xa3, brief, BriefUpdate) || !RedirectCall(vui + 0x10f, brief, BriefUpdate))
        Log("shell: unexpected vui_Update, the co-op briefing is split in two");
    uint8_t *mini = (uint8_t *)SymAddr("?vuiMiniMap_Render@@YAXXZ");
    void *hint = (void *)SymAddr("?DrawHint@cMiniMap@@QAEXH@Z");
    g_drawHint = (DrawHintFn)hint;
    if (!mini || !RedirectCall(mini + 0x65, hint, DrawHint))
        Log("shell: unexpected vuiMiniMap_Render, the co-op objective box is split in two");
}
static void HookPlayer2() {
    static bool hooked, skinned;
    if (!hooked) {
        hooked = true;
        FixSplitPads();
        HookSplitHud();
        if (getenv("GHOSTPC_LOG_TERRAIN")) {
            uint8_t *rt = (uint8_t *)SymAddr("?RenderTerrain@@YAIPAVcRenderTerrain@@PAVtRenderCamera@@I@Z");
            void *draw = (void *)SymAddr("?rlTerrain_DrawTerrainArea@@YAIPAVcRenderTerrain@@PAVcRenderTerrainArea@@I@Z");
            g_drawArea = (DrawAreaFn)draw;
            Log("shell: GHOSTPC_LOG_TERRAIN: %d", rt && RedirectCall(rt + 0xab, draw, LogDrawArea));
        }
        if (getenv("GHOSTPC_TEST_TERRAIN")) {   // debug: RenderTerrain draws every area (no frustum test)
            uint8_t *rt = (uint8_t *)SymAddr("?RenderTerrain@@YAIPAVcRenderTerrain@@PAVtRenderCamera@@I@Z");
            if (rt && rt[0x7e] == 0x74) {
                DWORD old;
                VirtualProtect(rt + 0x7e, 2, PAGE_EXECUTE_READWRITE, &old);
                rt[0x7e] = 0x90, rt[0x7f] = 0x90;
                VirtualProtect(rt + 0x7e, 2, old, &old);
                Log("shell: GHOSTPC_TEST_TERRAIN: terrain frustum test off");
            }
        }
        uint8_t *create = (uint8_t *)SymAddr("?gameMultiplayer_CreatePlayer@@YAPAVtPlayer@@PAVtTemplate@@H@Z");
        g_createPlayer = (CreatePlayerFn)create;
        static const char *sites[][2] = {{"?gameMultiplayer_BeginGame@@YAXXZ", (const char *)0x31},
                                         {"?CmdNewPlayer@@YAHPAD@Z", (const char *)0x1b}};
        for (auto &s : sites) {
            uint8_t *call = (uint8_t *)SymAddr(s[0]);
            if (!RedirectCall(call ? call + (uintptr_t)s[1] : nullptr, create, P2CreatePlayer))
                Log("shell: unexpected %s, player 2 keeps player 1's template", s[0]);
        }
        // player 2's start: both of gameMultiplayer_PlacePlayer's tWorld::FindPlayerStart calls
        uint8_t *place = (uint8_t *)SymAddr("?gameMultiplayer_PlacePlayer@@YAXPAVtPlayer@@PAVtWorld@@HII@Z");
        void *find = (void *)SymAddr("?FindPlayerStart@tWorld@@QAEPAUtPlayerStart@@IIH@Z");
        g_findStart = (FindStartFn)find;
        if (!place || !RedirectCall(place + 0x89, find, P2FindStart) || !RedirectCall(place + 0x97, find, P2FindStart))
            Log("shell: unexpected gameMultiplayer_PlacePlayer, player 2 starts on player 1");
    }
    if (!skinned) {   // once the templates are loaded (gameTemplate_Find reads gameTemplate_pHashtable, made at startup)
        typedef void *(__cdecl *FindFn)(const char *);
        static auto find = (FindFn)SymAddr("?gameTemplate_Find@@YAPAVtTemplate@@PBD@Z");
        static void **table = (void **)SymAddr("?gameTemplate_pHashtable@@3PAVtHashtable@@A");
        if (find && table && *table && find("nova")) {
            skinned = true;
            SetSkinModel("Nova_ice", "nova_ice.nod");
        }
    }
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
    {   // -coop / GHOSTPC_COOP=1: the game starts in co-op (with -level <name>: straight into that level). Until player 2
        // exists: the shell's second-player flag (cShell +0x6E34, read by gameMultiplayer_BeginGame, which the co-op
        // menu's Menu_Multiplayer sets) and the co-op game type (gameChronicle +4: co-op deaths, CoopIsSinglePlayer)
        extern bool g_coopStart;
        static bool on = g_coopStart || getenv("GHOSTPC_COOP");
        uint32_t names = SymAddr("?gameMotion_apNames@@3PAPADA");
        if (on && names && *(void **)(names + 0x72c)) on = false, Log("shell: co-op start: player 2 is in");
        static uint8_t *shell = (uint8_t *)SymAddr("?shell_Shell@@3VcShell@@A");
        static uint8_t *chron = (uint8_t *)SymAddr("?gameChronicle@@3VcChronicle@@A");
        if (on && shell && chron) *(int *)(shell + 0x6e34) = 1, *(int *)(chron + 4) = 1;
    }
    {   // debug (GHOSTPC_PLAYER_TEMPLATE=<template>): the template players are created from (default "nova")
        static bool done;
        const char *t = done ? nullptr : getenv("GHOSTPC_PLAYER_TEMPLATE");
        done = true;
        if (char *dst = t ? (char *)SymAddr("?mainOptions_aPlayerTemplate@@3PADA") : nullptr) {
            strncpy(dst, t, 31);
            Log("shell: player template %s", dst);
        }
    }
    {   // GhostPC's own materials (Materials\ghostpc.nsa: e.g. Nova_DT's, which the build never defined), opened like
        // the archives in shaderlist.txt (cShader::LoadArchives: OpenArchive(name, 0x32)) once cShader::Startup has
        // made the shader table (cShader::pHashtable; OpenArchive looks every name up in it)
        static bool done;
        static void **table = (void **)SymAddr("?pHashtable@cShader@@2PAVtHashtable@@A");
        if (!done && table && *table) {
            done = true;
            typedef int(__cdecl *OpenFn)(const char *, unsigned short);
            if (auto open = (OpenFn)SymAddr("?OpenArchive@cShader@@SAHPBDG@Z")) Log("shell: materials ghostpc.nsa -> %d", open("ghostpc", 0x32));
        }
    }
    HookMissionUnlock();
    HookProfileCreateNew();
    HookPlayer2();
    HookCoopMenu();
    extern void CompetitiveInit();
    CompetitiveInit();
    DebugTestUnlock();
    extern void DebugDeathInstall();
    DebugDeathInstall();
    extern void CustomSoundInstall();
    CustomSoundInstall();
    DebugEarlyPlayerPos();
    extern void LevelScriptUpdate();
    LevelScriptUpdate();
    g_crumb = "competitive: match";
    extern void CompetitiveUpdate();
    CompetitiveUpdate();
    g_crumb = "shell: debug";
    {   // debug (GHOSTPC_TEST_P2=<frame>): the console's "new player" (CmdNewPlayer: a second local player and the
        // left/right split screen), to look at the cut split-screen mode
        extern UINT g_frame;
        static int p2At = getenv("GHOSTPC_TEST_P2") ? atoi(getenv("GHOSTPC_TEST_P2")) : -1;
        if (p2At >= 0 && (int)g_frame == p2At)
            if (auto cmd = (int(__cdecl *)(char *))SymAddr("?CmdNewPlayer@@YAHPAD@Z")) {
                char none[1] = "";
                Log("shell: GHOSTPC_TEST_P2: CmdNewPlayer -> %d", cmd(none));
                // the new player's camera (pRightLocalPlayer -> cPlayerInfo +4: tSimCamera) has no camera region yet
                // (+0x188 left at the allocator's fill; a level load sets it): none
                uint8_t *info = *(uint8_t **)(SymAddr("?gameMotion_apNames@@3PAPADA") + 0x72c);
                uint8_t *cam = info ? *(uint8_t **)(info + 4) : nullptr;
                if (cam && *(uint32_t *)(cam + 0x188) == 0xcccccccc) *(uint32_t *)(cam + 0x188) = 0;
                Log("shell: GHOSTPC_TEST_P2: right player info %p camera %p", info, cam);
            }
    }
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
    bool esc = InterlockedExchange(&g_escPressed, 0) != 0;
    if (!TypeIntoKeyboard(s, esc) && esc) HandleEsc(s);
    static DWORD last[4] = {~0u, ~0u, ~0u, ~0u};
    DWORD cur[4] = {*(DWORD *)(s + 0x3c), *(DWORD *)(s + 0x6ee0), *(DWORD *)(s + 0x6e40) != 0, *(DWORD *)(s + 0x6e3c) != 0};
    if (memcmp(cur, last, sizeof cur)) {
        extern UINT g_frame;
        Log("shell: frame %u state %u menu %u keyboard %u menuobj %u", g_frame, cur[0], cur[1], cur[2], cur[3]);
        memcpy(last, cur, sizeof cur);
    }
    {   // debug (GHOSTPC_SKIP_BRIEFING): A once a second while the briefing is up (vuiBriefing_briefing +0x58, which
        // vuiTarget_Draw checks too), whenever it comes
        extern UINT g_frame;
        extern void PressA();
        static bool on = getenv("GHOSTPC_SKIP_BRIEFING") != nullptr;
        static int *showing = (int *)(SymAddr("?vuiBriefing_briefing@@3VcUIBriefing@@A") + 0x58);
        if (on && showing && *showing && !ShellMenuShown() && g_frame % 30 == 0) PressA();   // not the pause menu
                                                                                           // (it pauses the briefing)
    }
    {   // debug (GHOSTPC_AUTOCOOP): into a co-op game whatever the menus' timing: Start through the intro and title
        // (menu 0 / 15), Down and A on the main menu (14: Multiplayer), then A on Multiplayer (28: Cooperative),
        // player 2's profile (20), the co-op missions (31) and the difficulty (18); once player 2 exists, A (Space) on the
        // finished load screen (state 24) and twice more in the level (the briefing)
        extern UINT g_frame;
        extern void PressStart();
        extern void PressA();
        extern void PressDown();
        // GHOSTPC_AUTOCOMP: the same into a competitive game (Down and A on Multiplayer: Competitive, A on the players
        // menu: 2 Players, the profiles (21), the competitive missions (32))
        static bool comp = getenv("GHOSTPC_AUTOCOMP") != nullptr;
        static bool on = comp || getenv("GHOSTPC_AUTOCOOP") != nullptr;
        static bool holdMissions = comp && !_stricmp(getenv("GHOSTPC_AUTOCOMP"), "hold");   // =hold: stop at the missions
        static DWORD downs = getenv("GHOSTPC_AUTOCOOP") ? atoi(getenv("GHOSTPC_AUTOCOOP")) - 1 : 0;   // =n: mission n
        static DWORD menu = ~0u, since, pressed, presses, joined, starts;
        uint32_t names = SymAddr("?gameMotion_apNames@@3PAPADA");
        if (on && !joined && names && *(void **)(names + 0x72c)) joined = g_frame;
        if (on && joined) {
            static DWORD loaded, inLevel;
            if (cur[0] == 24 && !loaded) loaded = g_frame;
            if (loaded && cur[0] != 24 && !inLevel) inLevel = g_frame;
            bool wait = cur[0] == 24 ? g_frame - loaded < 60 : !inLevel || g_frame - inLevel < 60;
            if (!wait && g_frame - pressed >= 60) {
                pressed = g_frame;
                PressA();
                if (cur[0] != 24 && ++starts == 2) on = false;
            }
        } else if (on) {
            DWORD shown = cur[1] | (g_inPlayersMenu ? 0x100 : 0);
            if (shown != menu) menu = shown, since = g_frame, pressed = 0, presses = 0;
            if (g_frame - since >= 40 && g_frame - pressed >= 60) {
                pressed = g_frame;
                if (menu == 0 || menu == 15) PressStart();
                else if (menu == 14 || (comp && menu == 28)) presses++ ? PressA() : PressDown();
                else if (menu == (28 | 0x100) || menu == 21 || (menu == 32 && !holdMissions)) PressA();
                else if (menu == 31 && presses < downs) presses++, PressDown();
                else if (menu == 28 || menu == 20 || menu == 31 || menu == 18) PressA();
            }
        }
    }
    g_crumb = "game (after GhostPC's frame step)";
}
