// The competitive match (Multiplayer -> Competitive: ghostmpcomp.nsc, the chronicle's game type 2, gameChronicle +4):
// kills per player, a time limit (Settings: [multiplayer] MatchMinutes, 10), the scoreboard on Tab / Back, and at the
// end the victory screen (one minute, Space or A skips it), then back to the main menu.
//
// Kills: tActor::Kill already counts them in a multiplayer game. When the victim is a player (tThing +0x10 = 7) and
// the killer is one too (or a projectile, type 4, whose owner GUID at +0x378 is), the killer's cPlayerInfo +0x78 goes
// up (+0x7A too for a kill the victim's AI calls special); cPlayerInfo::BeginLevel clears it. The build also counted
// a player's own death by her own grenade or rocket as a kill: KillCredit (at Kill+0x402 / +0x40E) skips the credit
// when killer and victim are the same.
// Spider mines did not count: cSpiderMineControl::ExitSpiderMine (the mine going off) creates a new "spidermine"
// thing at the mine (gameThing_CreateFromTemplateAtPos) and kills it, and that thing's death explosion is the killer,
// with no link to the player who drove the mine (the control's +0x1A0, the rider it dismounts). The creation is
// recorded (MineCreated, thing -> rider) and Kill's killer check (+0x3C8, after a projectile's owner GUID is looked
// up) goes through ResolveKiller, which maps such a mine to its rider. A player's death that still credits nobody is
// logged with the killer's type.
//
// Drawing: vui_Update (once a frame, after the per-player HUDs) ends with a tail jump to cUiText::RenderAll, the
// game's floating texts; that jump goes to MatchRender, which also draws the match's own cUiText objects (0x40 bytes:
// +0 x, +4 y, +8 colour, +0xC text, +0x10 flags: 1 shown, 4 left out of RenderAll) full-screen in 640x480 (whatever
// half-screen viewport the HUD left set, without the split-screen HUD mapping: d3d_core.cpp, SetFullScreen2D).
//
// Victory: gameMultiplayer_UpdateControls (tSequence::UpdateSim's only call) is skipped, so neither player moves,
// shoots or revives; the end takes the pause menu's Quit (QuitToMenu: tSequence::TerminateGame, then the main menu).
#include "common.h"
#include "settings.h"
#include <algorithm>

bool ShellMenuShown();          // shell.cpp
void SetFullScreen2D(bool on);  // d3d_core.cpp
bool ScoreboardHeld();          // input.cpp
bool SkipPressed();
uint8_t *LevelWorld();          // levelscript.cpp: the loaded tWorld
extern UINT g_frame;

namespace {

uint8_t *Names() { static uint8_t *n = (uint8_t *)SymAddr("?gameMotion_apNames@@3PAPADA"); return n; }
uint8_t *Info(int i) { return Names() ? *(uint8_t **)(Names() + (i ? 0x72c : 0x728)) : nullptr; }   // left, right
int Kills(int i) { uint8_t *p = Info(i); return p ? *(uint16_t *)(p + 0x78) : 0; }
bool Competitive() {
    static uint8_t *chron = (uint8_t *)SymAddr("?gameChronicle@@3VcChronicle@@A");
    return chron && *(int *)(chron + 4) == 2 && Info(0) && Info(1);
}
// the two players' names: their profiles' (mainProfile_pPlayer0 / 1, cProfile +8: the name the profile lists show),
// else Nova / Echo
const char *Name(int i) {
    static uint8_t **players = (uint8_t **)SymAddr("?mainProfile_pPlayer0@@3PAVcProfile@@A");
    const char *n = players && players[i] ? (const char *)(players[i] + 8) : nullptr;
    return n && n[0] ? n : i ? "Echo" : "Nova";
}

// ---------------------------------------------------------------------------
// kill credit
uint32_t g_killDone;   // Kill+0x422, where both credit paths continue
void __cdecl LogKill(uint8_t *killer, uint8_t *victim) {
    if (!Competitive()) return;
    int k = Info(1) && *(uint8_t **)Info(1) == killer, v = Info(1) && *(uint8_t **)Info(1) == victim;
    Log("competitive: %s killed %s%s (kills %d - %d)", Name(k), Name(v), killer == victim ? " (self, not counted)" : "",
        Kills(0), Kills(1));
}
__declspec(naked) void KillCredit() {   // edi killer (tPlayer), esi victim
    __asm {
        cmp edi, esi
        je self
        mov eax, [edi + 0x520]
        inc word ptr [eax + 0x78]
    self:
        pushad
        push esi
        push edi
        call LogKill
        add esp, 8
        popad
        jmp dword ptr [g_killDone]
    }
}
__declspec(naked) void KillCreditSpecial() {
    __asm {
        cmp edi, esi
        je self
        mov eax, [edi + 0x520]
        inc word ptr [eax + 0x7a]
        inc word ptr [eax + 0x78]
    self:
        pushad
        push esi
        push edi
        call LogKill
        add esp, 8
        popad
        jmp dword ptr [g_killDone]
    }
}

// spider mines: the things ExitSpiderMine creates, and whose they are
struct MineOwner { uint8_t *thing, *owner; DWORD at; };
MineOwner g_mines[16];
unsigned g_nextMine;
uint8_t *g_mineCtl;   // the cSpiderMineControl ExitSpiderMine runs for (its ebx, saved by MineCreateStub)
typedef uint8_t *(__cdecl *CreateAtPosFn)(void *, void *, void *, void *, unsigned);
CreateAtPosFn g_createAtPos;
uint8_t *__cdecl MineCreated(void *tmpl, void *world, void *sector, void *matrix, unsigned flags) {
    uint8_t *t = g_createAtPos(tmpl, world, sector, matrix, flags);
    uint8_t *owner = g_mineCtl ? *(uint8_t **)(g_mineCtl + 0x1a0) : nullptr;
    uint8_t *mine = g_mineCtl ? *(uint8_t **)(g_mineCtl + 0x180) : nullptr;
    if (t && owner) {
        g_mines[g_nextMine++ % 16] = {t, owner, GetTickCount()};
        if (mine) g_mines[g_nextMine++ % 16] = {mine, owner, GetTickCount()};
    }
    if (Competitive()) Log("competitive: spider mine %p (mine %p) goes off for player %p", t, mine, owner);
    return t;
}
__declspec(naked) void MineCreateStub() {
    __asm {
        mov g_mineCtl, ebx
        jmp MineCreated
    }
}
// Kill+0x3C8: edi = the killer (or a projectile's owner), esi = the victim; returns who gets the credit (a player
// with a cPlayerInfo), or null
uint8_t *__cdecl ResolveKiller(uint8_t *killer, uint8_t *victim) {
    if (killer && *(int *)(killer + 0x10) == 7 && *(void **)(killer + 0x520)) return killer;
    DWORD now = GetTickCount();
    if (killer)
        for (auto &m : g_mines)
            if (m.thing == killer && now - m.at < 30000) return m.owner;
    if (victim && *(int *)(victim + 0x10) == 7 && *(void **)(victim + 0x520) && Competitive())
        Log("competitive: a player died, no kill credited (killer %p, type %d)", killer, killer ? *(int *)(killer + 0x10) : -1);
    return nullptr;
}
uint32_t g_killCredit;   // Kill+0x3D2
__declspec(naked) void KillerCheck() {
    __asm {
        push ecx
        push edx
        push esi
        push edi
        call ResolveKiller
        add esp, 8
        pop edx
        pop ecx
        test eax, eax
        je none
        mov edi, eax
        jmp dword ptr [g_killCredit]
    none:
        jmp dword ptr [g_killDone]
    }
}

// ---------------------------------------------------------------------------
// texts
struct Text {
    alignas(16) uint8_t obj[0x40];
    bool made;
};
typedef void *(__thiscall *TextCtorFn)(void *, const char *, float, float, unsigned long, unsigned);
typedef void(__thiscall *TextSetFn)(void *, const char *);
typedef void(__thiscall *TextIntFn)(void *, int);
typedef unsigned(__thiscall *TextRenderFn)(void *, int);
void Draw(Text &t, const char *s, float x, float y, unsigned long color) {
    static auto ctor = (TextCtorFn)SymAddr("??0cUiText@@QAE@PBDMMKI@Z");
    static auto setText = (TextSetFn)SymAddr("?SetText@cUiText@@QAEXPBD@Z");
    static auto setCentered = (TextIntFn)SymAddr("?SetCentered@cUiText@@QAEXH@Z");
    static auto render = (TextRenderFn)SymAddr("?Render@cUiText@@QAEIH@Z");
    static auto memOpen = (int(__cdecl *)())SymAddr("?stdMemory_Open@@YAHXZ");
    if (!ctor || !setText || !render) return;
    if (!t.made) {
        if (memOpen) memOpen();
        ctor(t.obj, "", x, y, color, 0x80);
        if (setCentered) setCentered(t.obj, 1);
        t.made = true;
    }
    float *pos = (float *)t.obj;
    pos[0] = x, pos[1] = y;
    *(unsigned long *)(t.obj + 8) = color;
    *(uint32_t *)(t.obj + 0x10) |= 5;   // shown; not RenderAll's
    setText(t.obj, s);
    render(t.obj, 0);
}

// ---------------------------------------------------------------------------
// the match
enum { M_NONE, M_PLAY, M_VICTORY, M_DONE };
int g_state;
double g_elapsed, g_victoryElapsed;
uint8_t *g_world;
int g_final[2];
bool g_skipArmed;
LARGE_INTEGER g_last;

double Limit() {
    static double s = getenv("GHOSTPC_MATCH_SECONDS") ? atof(getenv("GHOSTPC_MATCH_SECONDS")) : 0;
    return s > 0 ? s : g_settings.matchMinutes * 60.0;
}
double FrameSeconds() {
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    double dt = g_last.QuadPart ? (double)(now.QuadPart - g_last.QuadPart) / f.QuadPart : 0;
    g_last = now;
    return std::min(std::max(dt, 0.0), 0.1);   // a hitch or a load does not eat the clock
}
void Clock(char *b, size_t n, double secs) {
    int s = (int)std::max(0.0, secs + 0.999);
    _snprintf(b, n, "%d:%02d", s / 60, s % 60);
}
uint8_t *ShellObj() { static uint8_t *s = (uint8_t *)SymAddr("?shell_Shell@@3VcShell@@A"); return s; }

void EndMatch() {
    g_final[0] = Kills(0), g_final[1] = Kills(1);
    g_state = M_VICTORY;
    g_victoryElapsed = 0;
    g_skipArmed = false;
    Log("competitive: time up, %s %d - %d %s: %s", Name(0), g_final[0], g_final[1], Name(1),
        g_final[0] == g_final[1] ? "draw" : g_final[0] > g_final[1] ? "player 1 wins" : "player 2 wins");
}
// the pause menu's Quit: its "Are you sure?" menu (Menu_IG_RUSureQuit) answers Yes with state 0x4E (State_Quit:
// frees the menu, tSequence::TerminateGame, the main menu). cShell::UpdateState runs only while the shell is on and
// State_Quit needs its menu object (cShell::ResetMenu), so the shell is switched on first (cShell::MenuOn, as
// RequestDeathMenu does before its state 0x45)
void QuitToMenu() {
    static auto menuOn = (void(__thiscall *)(void *))SymAddr("?MenuOn@cShell@@QAEXXZ");
    static auto changeState = (int(__thiscall *)(void *, unsigned long, int))SymAddr("?ChangeState@cShell@@QAEHKH@Z");
    g_state = M_DONE;
    Log("competitive: back to the main menu");
    if (!menuOn || !changeState || !ShellObj()) return;
    menuOn(ShellObj());
    changeState(ShellObj(), 0x4e, 0);
}

// called once a frame from the shell's frame step
void Update() {
    double dt = FrameSeconds();
    uint8_t *world = LevelWorld();
    if (world != g_world) {   // a new level (or none)
        g_world = world;
        g_state = M_NONE;
    }
    if (!world || !Competitive()) {
        if (g_state != M_NONE) g_state = M_NONE;
        return;
    }
    if (g_state == M_NONE) {
        g_state = M_PLAY, g_elapsed = 0;
        Log("competitive: match started, %.0f s: %s vs %s", Limit(), Name(0), Name(1));
    }
    if (ShellMenuShown()) return;   // paused, loading: the clock stops
    if (g_state == M_PLAY) {
        g_elapsed += dt;
        if (g_elapsed >= Limit()) EndMatch();
    } else if (g_state == M_VICTORY) {
        g_victoryElapsed += dt;
        bool skip = SkipPressed();
        if (!skip && g_victoryElapsed > 1.0) g_skipArmed = true;   // a press held from the match does not skip it
        if (g_victoryElapsed >= 60.0 || (g_skipArmed && skip)) QuitToMenu();
    }
}

Text g_tTimer, g_tBoard[5], g_tWin[5];
void DrawMatch() {
    if (g_state != M_PLAY && g_state != M_VICTORY) return;
    if (ShellMenuShown()) return;
    char b[96], c[16];
    const unsigned long white = 0xffffffff, gold = 0xffffd040, blue = 0xff80d0ff, ice = 0xffa0ffff, grey = 0xffc0c0c0;
    if (g_state == M_PLAY) {
        Clock(c, sizeof c, Limit() - g_elapsed);
        Draw(g_tTimer, c, 320, 14, Limit() - g_elapsed < 30 ? 0xffff6060 : white);
        if (ScoreboardHeld()) {
            int k0 = Kills(0), k1 = Kills(1);
            Draw(g_tBoard[0], "SCORE", 320, 170, gold);
            _snprintf(b, sizeof b, "%s   %d kill%s", Name(0), k0, k0 == 1 ? "" : "s");
            Draw(g_tBoard[1], b, 320, 205, blue);
            _snprintf(b, sizeof b, "%s   %d kill%s", Name(1), k1, k1 == 1 ? "" : "s");
            Draw(g_tBoard[2], b, 320, 230, ice);
            _snprintf(b, sizeof b, "Time left  %s", c);
            Draw(g_tBoard[3], b, 320, 265, grey);
        }
        return;
    }
    int w = g_final[0] == g_final[1] ? -1 : g_final[0] > g_final[1] ? 0 : 1;
    Draw(g_tWin[0], w < 0 ? "MATCH OVER" : "VICTORY", 320, 150, gold);
    _snprintf(b, sizeof b, w < 0 ? "Draw" : "%s wins", w < 0 ? "" : Name(w));
    Draw(g_tWin[1], b, 320, 185, w == 1 ? ice : w == 0 ? blue : white);
    _snprintf(b, sizeof b, "%s  %d  -  %d  %s", Name(0), g_final[0], g_final[1], Name(1));
    Draw(g_tWin[2], b, 320, 225, white);
    _snprintf(b, sizeof b, "Press Space to continue (%d)", (int)std::max(0.0, 60.0 - g_victoryElapsed + 0.999));
    Draw(g_tWin[3], b, 320, 290, grey);
}
typedef unsigned(__cdecl *RenderAllFn)();
RenderAllFn g_renderAll;
unsigned __cdecl MatchRender() {
    unsigned r = g_renderAll();
    if (g_state == M_PLAY || g_state == M_VICTORY) {
        SetFullScreen2D(true);
        DrawMatch();
        SetFullScreen2D(false);
    }
    return r;
}
void(__cdecl *g_updateControls)();
void __cdecl MatchControls() {
    if (g_state == M_VICTORY && Competitive()) return;   // frozen on the victory screen
    g_updateControls();
}

bool PatchJmp(uint8_t *at, uint8_t op, void *expect, void *to) {   // E8 call / E9 jmp rel32
    if (!at || at[0] != op || (expect && at + 5 + *(int32_t *)(at + 1) != expect)) return false;
    DWORD old;
    VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old);
    *(int32_t *)(at + 1) = (int32_t)((uint8_t *)to - (at + 5));
    VirtualProtect(at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    return true;
}
bool PlaceJmp(uint8_t *at, const uint8_t *expect, size_t n, void *to) {   // overwrite n bytes with jmp + nops
    if (!at || memcmp(at, expect, n)) return false;
    DWORD old;
    VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old);
    memset(at, 0x90, n);
    at[0] = 0xE9;
    *(int32_t *)(at + 1) = (int32_t)((uint8_t *)to - (at + 5));
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

}   // namespace

// The mission select's info panel (image, progress, objective): cShellMenuItem::RenderText's "!" command calls
// RenderMissionDesc(mission, x, y), which draws only the name unless the item is the selected one (+0x30). With a
// single mission (the competitive chronicle) the list's select-next wraps onto the same item and clears it again
// after setting it, so the panel stayed empty: with one mission, its item counts as selected.
// GHOSTPC_LOG_MISSIONDESC logs the calls when their arguments change.
typedef unsigned(__thiscall *MissionDescFn)(void *, unsigned, float, float);
MissionDescFn g_missionDesc;
unsigned __fastcall MissionDesc(uint8_t *item, void *, unsigned mission, float x, float y) {
    static uint8_t *chron = (uint8_t *)SymAddr("?gameChronicle@@3VcChronicle@@A");
    if (chron && *(unsigned *)(chron + 8) == 1 && mission == 0) *(int *)(item + 0x30) = 1;
    unsigned r = g_missionDesc(item, mission, x, y);
    static bool log = getenv("GHOSTPC_LOG_MISSIONDESC") != nullptr;
    static unsigned last = ~0u, lastR = ~0u;
    if (log && (mission != last || r != lastR))
        Log("missiondesc: mission %u of %u (game type %d) at %.0f %.0f -> %u", mission, chron ? *(unsigned *)(chron + 8) : 0,
            chron ? *(int *)(chron + 4) : -1, x, y, r);
    last = mission, lastR = r;
    return r;
}

void CompetitiveInit() {
    static bool done;
    if (done) return;
    done = true;
    uint8_t *rt = (uint8_t *)SymAddr("?RenderText@cShellMenuItem@@IAEIIPADMM@Z");
    g_missionDesc = (MissionDescFn)SymAddr("?RenderMissionDesc@cShellMenuItem@@IAEIIMM@Z");
    if (!PatchJmp(rt ? rt + 0x41 : nullptr, 0xE8, (void *)g_missionDesc, MissionDesc))
        Log("competitive: unexpected cShellMenuItem::RenderText, a one-mission list shows no mission info");
    uint8_t *kill = (uint8_t *)SymAddr("?Kill@tActor@@QAEHPAVtThing@@W4eDamageSource@@W4eDamageType@@MPAUXGVECTOR3@@3IW4eHitRegionType@@M@Z");
    // +0x402: mov edi, [edi+0x520]; inc word [edi+0x78]   +0x40E: mov edi, [edi+0x520]; inc word [edi+0x7a]; inc word [edi+0x78]
    static const uint8_t credit[] = {0x8B, 0xBF, 0x20, 0x05, 0x00, 0x00, 0x66, 0xFF, 0x47, 0x78};
    static const uint8_t special[] = {0x8B, 0xBF, 0x20, 0x05, 0x00, 0x00, 0x66, 0xFF, 0x47, 0x7A, 0x66, 0xFF, 0x47, 0x78};
    if (kill) g_killDone = (uint32_t)(kill + 0x422), g_killCredit = (uint32_t)(kill + 0x3d2);
    // +0x3C8: test edi, edi; je +0x422; cmp dword [edi+0x10], 7; jne +0x422
    static const uint8_t check[] = {0x85, 0xFF, 0x74, 0x56, 0x83, 0x7F, 0x10, 0x07, 0x75, 0x50};
    if (!kill || !PlaceJmp(kill + 0x3c8, check, sizeof check, KillerCheck))
        Log("competitive: unexpected tActor::Kill, spider mine kills do not count");
    uint8_t *exitMine = (uint8_t *)SymAddr("?ExitSpiderMine@cSpiderMineControl@@AAEXHHH@Z");
    g_createAtPos = (CreateAtPosFn)SymAddr("?gameThing_CreateFromTemplateAtPos@@YAPAVtThing@@PAVtTemplate@@PAVtWorld@@PAVtSector@@PBUXGMATRIX@@I@Z");
    if (!PatchJmp(exitMine ? exitMine + 0x92 : nullptr, 0xE8, (void *)g_createAtPos, MineCreateStub))
        Log("competitive: unexpected cSpiderMineControl::ExitSpiderMine, spider mine kills do not count");
    if (!kill || !PlaceJmp(kill + 0x402, credit, sizeof credit, KillCredit) ||
        !PlaceJmp(kill + 0x40e, special, sizeof special, KillCreditSpecial))
        Log("competitive: unexpected tActor::Kill, kills by one's own weapons count");
    uint8_t *vui = (uint8_t *)SymAddr("?vui_Update@@YAXXZ");
    g_renderAll = (RenderAllFn)SymAddr("?RenderAll@cUiText@@SAIXZ");
    if (!PatchJmp(vui ? vui + 0x12a : nullptr, 0xE9, (void *)g_renderAll, MatchRender))
        Log("competitive: unexpected vui_Update, no timer, scoreboard or victory screen");
    uint8_t *sim = (uint8_t *)SymAddr("?UpdateSim@tSequence@@AAEXXZ");
    g_updateControls = (void(__cdecl *)())SymAddr("?gameMultiplayer_UpdateControls@@YAXXZ");
    if (!PatchJmp(sim ? sim + 0x113 : nullptr, 0xE8, (void *)g_updateControls, MatchControls))
        Log("competitive: unexpected tSequence::UpdateSim, the victory screen does not freeze the players");
}
void CompetitiveUpdate() { Update(); }
bool CompetitiveVictory() { return g_state == M_VICTORY; }
bool CompetitiveMatch() { return g_state == M_PLAY || g_state == M_VICTORY; }
