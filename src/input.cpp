// Xbox controller input (XInputOpen/GetState) from PC XInput pads, keyboard and mouse.
// Keyboard/mouse bindings use Cxbx-Reloaded's profile syntax ([input] in ghostpc.ini); the default
// is the "Cute1" profile. Mouse motion is read with raw input while the cursor is captured.
#include "common.h"
#include "settings.h"
#include <xinput.h>
#include <string>
#include <vector>
#include <math.h>
#pragma comment(lib, "xinput9_1_0.lib")

#pragma pack(push, 1)
struct XB_GAMEPAD {
    WORD wButtons;
    BYTE bAnalogButtons[8];   // A B X Y BLACK WHITE LTRIGGER RTRIGGER
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct XB_INPUT_STATE { DWORD dwPacketNumber; XB_GAMEPAD Gamepad; };
#pragma pack(pop)

enum { XB_DPAD_UP = 1, XB_DPAD_DOWN = 2, XB_DPAD_LEFT = 4, XB_DPAD_RIGHT = 8, XB_START = 0x10, XB_BACK = 0x20,
       XB_LTHUMB = 0x40, XB_RTHUMB = 0x80 };
enum { AB_A, AB_B, AB_X, AB_Y, AB_BLACK, AB_WHITE, AB_LT, AB_RT };

extern HWND g_hwnd;   // game window (created in d3d_core.cpp)
HWND g_hwnd;

// ---------------------------------------------------------------------------
// Bindings
enum Ctl {
    C_DUP, C_DDOWN, C_DLEFT, C_DRIGHT, C_START, C_BACK, C_LTHUMB, C_RTHUMB, C_A, C_B, C_X, C_Y, C_BLACK, C_WHITE,
    C_LT, C_RT, C_LXP, C_LXM, C_LYP, C_LYM, C_RXP, C_RXM, C_RYP, C_RYM, C_COUNT
};
static const char *kCtlNames[C_COUNT] = {
    "D Pad Up", "D Pad Down", "D Pad Left", "D Pad Right", "Start", "Back", "L Thumb", "R Thumb", "A", "B", "X", "Y",
    "Black", "White", "L Trigger", "R Trigger", "Left Axis X+", "Left Axis X-", "Left Axis Y+", "Left Axis Y-",
    "Right Axis X+", "Right Axis X-", "Right Axis Y+", "Right Axis Y-"};
// default = Cxbx profile "Cute1"
static const char *kDefault[C_COUNT] = {
    "NUMPAD8 | UP", "NUMPAD5 | DOWN | F", "NUMPAD4 | LEFT", "NUMPAD6 | RIGHT", "RETURN", "BACK", "B | Wheel+ | Wheel-", "M | Click 2", "SPACE", "E", "Q", "Click 1",
    "C", "X", "LCONTROL", "Click 0", "D", "A", "W", "S", "Axis X+", "Axis X-", "Axis Y-", "Axis Y+"};

enum BindKind { BK_NONE, BK_KEY, BK_MBTN, BK_MAXIS, BK_WHEEL };
struct Bind { int kind = BK_NONE; int code = 0; int sign = 0; };
static std::vector<Bind> g_bind[C_COUNT];   // several bindings per control: "A | B"
static float g_mouseRange = 10.f;   // mouse counts per frame for full stick deflection (Cxbx MouseAxisRange)
static float g_wheelRange = 80.f;
static bool g_captureMouse = true;

struct KeyName { const char *n; int vk; };
static const KeyName kKeys[] = {
    {"ESCAPE", VK_ESCAPE}, {"RETURN", VK_RETURN}, {"ENTER", VK_RETURN}, {"BACK", VK_BACK}, {"TAB", VK_TAB},
    {"SPACE", VK_SPACE}, {"LCONTROL", VK_LCONTROL}, {"RCONTROL", VK_RCONTROL}, {"LSHIFT", VK_LSHIFT},
    {"RSHIFT", VK_RSHIFT}, {"LMENU", VK_LMENU}, {"RMENU", VK_RMENU}, {"LALT", VK_LMENU}, {"RALT", VK_RMENU},
    {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"HOME", VK_HOME}, {"END", VK_END},
    {"PRIOR", VK_PRIOR}, {"NEXT", VK_NEXT}, {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE}, {"CAPITAL", VK_CAPITAL},
    {"NUMPAD0", VK_NUMPAD0}, {"NUMPAD1", VK_NUMPAD1}, {"NUMPAD2", VK_NUMPAD2}, {"NUMPAD3", VK_NUMPAD3},
    {"NUMPAD4", VK_NUMPAD4}, {"NUMPAD5", VK_NUMPAD5}, {"NUMPAD6", VK_NUMPAD6}, {"NUMPAD7", VK_NUMPAD7},
    {"NUMPAD8", VK_NUMPAD8}, {"NUMPAD9", VK_NUMPAD9}, {"NUMPADENTER", VK_RETURN}, {"ADD", VK_ADD},
    {"SUBTRACT", VK_SUBTRACT}, {"MULTIPLY", VK_MULTIPLY}, {"DIVIDE", VK_DIVIDE}, {"DECIMAL", VK_DECIMAL},
    {"MINUS", VK_OEM_MINUS}, {"EQUALS", VK_OEM_PLUS}, {"LBRACKET", VK_OEM_4}, {"RBRACKET", VK_OEM_6},
    {"SEMICOLON", VK_OEM_1}, {"APOSTROPHE", VK_OEM_7}, {"GRAVE", VK_OEM_3}, {"BACKSLASH", VK_OEM_5},
    {"COMMA", VK_OEM_COMMA}, {"PERIOD", VK_OEM_PERIOD}, {"SLASH", VK_OEM_2},
};
static Bind ParseBind(std::string s) {
    Bind b;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '"')) s.pop_back();
    while (!s.empty() && (s[0] == ' ' || s[0] == '"')) s.erase(0, 1);
    if (s.empty()) return b;
    int n;
    if (sscanf(s.c_str(), "Click %d", &n) == 1) { b.kind = BK_MBTN; b.code = n; return b; }
    if (s.size() == 7 && s.compare(0, 5, "Axis ") == 0) {
        b.kind = BK_MAXIS; b.code = s[5] == 'X' ? 0 : 1; b.sign = s[6] == '+' ? 1 : -1; return b;
    }
    if (s == "Wheel+" || s == "Wheel-") { b.kind = BK_WHEEL; b.sign = s[5] == '+' ? 1 : -1; return b; }
    for (auto &k : kKeys) if (!_stricmp(s.c_str(), k.n)) { b.kind = BK_KEY; b.code = k.vk; return b; }
    if (s.size() == 1 && isalnum((unsigned char)s[0])) { b.kind = BK_KEY; b.code = toupper((unsigned char)s[0]); return b; }
    if (s.size() >= 2 && (s[0] == 'F' || s[0] == 'f') && atoi(s.c_str() + 1) >= 1 && atoi(s.c_str() + 1) <= 12) {
        b.kind = BK_KEY; b.code = VK_F1 + atoi(s.c_str() + 1) - 1; return b;
    }
    Log("input: unknown binding '%s'", s.c_str());
    return b;
}

static std::vector<Bind> ParseBinds(const std::string &v) {
    std::vector<Bind> r;
    size_t p = 0;
    while (true) {
        size_t bar = v.find('|', p);
        Bind b = ParseBind(v.substr(p, bar == std::string::npos ? std::string::npos : bar - p));
        if (b.kind != BK_NONE) r.push_back(b);
        if (bar == std::string::npos) break;
        p = bar + 1;
    }
    return r;
}
// writes the commented default [input] section when ghostpc.ini doesn't exist yet
void InputEnsureIni() {
    char path[MAX_PATH];
    _snprintf(path, MAX_PATH, "%s\\ghostpc.ini", g_gameDir);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
    if (FILE *f = fopen(path, "w")) {
        fprintf(f, "; GhostPC settings. Key names follow Cxbx-Reloaded profiles (DirectInput names,\n"
                   "; Click 0/1/2 = mouse buttons, Axis X+/X-/Y+/Y- = mouse motion, Wheel+/-).\n"
                   "; Default bindings = Cxbx profile \"Cute1\" (+ arrow keys on the D-pad). Several keys: \"NUMPAD8 | UP\".\n"
                   "; F10 toggles mouse capture. [video], [launcher], InputDevice and MouseSensitivity are set by the\n"
                   "; settings window (hold Shift while starting GhostPC to open it).\n\n[input]\n");
        for (int i = 0; i < C_COUNT; i++) fprintf(f, "%s = %s\n", kCtlNames[i], kDefault[i]);
        fprintf(f, "MouseAxisRange = 10\nMouseWheelRange = 80\nCaptureMouse = 1\n");
        fclose(f);
    }
}
static void LoadBindings() {
    for (int i = 0; i < C_COUNT; i++) g_bind[i] = ParseBinds(kDefault[i]);
    InputEnsureIni();
    char path[MAX_PATH];
    _snprintf(path, MAX_PATH, "%s\\ghostpc.ini", g_gameDir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    bool inInput = false;
    while (fgets(line, sizeof line, f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (l.empty() || l[0] == ';') continue;
        if (l[0] == '[') { inInput = l.find("[input") == 0; continue; }
        if (!inInput) continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        if (k == "MouseAxisRange") { g_mouseRange = std::max(1.f, (float)atof(v.c_str())); continue; }
        if (k == "MouseWheelRange") { g_wheelRange = std::max(1.f, (float)atof(v.c_str())); continue; }
        if (k == "CaptureMouse") { g_captureMouse = atoi(v.c_str()) != 0; continue; }
        for (int i = 0; i < C_COUNT; i++) if (k == kCtlNames[i]) g_bind[i] = ParseBinds(v);
    }
    fclose(f);
    // settings window: device choice and sensitivity (100% = the profile's MouseAxisRange)
    g_mouseRange = g_mouseRange * 100.f / std::max(10, g_settings.mouseSensitivity);
    if (g_settings.input == IN_CONTROLLER) g_captureMouse = false;
}

// ---------------------------------------------------------------------------
// Mouse: raw input deltas, accumulated between XInputGetState polls
static volatile LONG g_mdx, g_mdy, g_mwheel;
static volatile LONG g_lookDX, g_lookDY;   // mouse motion for direct camera control (camera.cpp)
float g_frameLookDX, g_frameLookDY;        // per-frame snapshot (all camera calls of a frame see the same motion)
void SnapshotMouseLook() {
    g_frameLookDX = (float)InterlockedExchange(&g_lookDX, 0);
    g_frameLookDY = (float)InterlockedExchange(&g_lookDY, 0);
}
static bool g_captured;
bool MouseLookActive() { return g_settings.input == IN_KBM && g_captured; }
static bool g_mbtn[5];
// a press stays visible until the next XInputGetState poll has seen it: a quick click (down and up between two game
// frames) would otherwise never reach the game; the wheel click (middle button) is often that quick
static bool g_mlatch[5];
static void MouseButton(int i, bool down, const char *src = "raw") {
    if (down) g_mlatch[i] = true;
    g_mbtn[i] = down;
    static bool log = getenv("GHOSTPC_LOGINPUT") != nullptr;
    if (log) Log("input: mouse button %d %s (%s)", i, down ? "down" : "up", src);
}
static bool g_rawRegistered;

static bool Focused() { return g_hwnd && GetForegroundWindow() == g_hwnd; }

static void SetMouseCapture(bool on) {
    if (on == g_captured) return;
    g_captured = on;
    if (on) {
        RECT r; GetClientRect(g_hwnd, &r);
        POINT a = {r.left, r.top}, b = {r.right, r.bottom};
        ClientToScreen(g_hwnd, &a); ClientToScreen(g_hwnd, &b);
        RECT c = {a.x, a.y, b.x, b.y};
        ClipCursor(&c);
        while (ShowCursor(FALSE) >= 0) {}
    } else {
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {}
    }
}

// called from the game window's WndProc; returns true when the message was handled
bool InputWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (!g_rawRegistered) {
        RAWINPUTDEVICE rid = {0x01, 0x02, 0, h};   // generic desktop / mouse
        BOOL ok = RegisterRawInputDevices(&rid, 1, sizeof rid);
        g_rawRegistered = true;
        if (getenv("GHOSTPC_LOGINPUT")) Log("input: logging mouse buttons; raw input %s for window %p", ok ? "registered" : "FAILED", h);
    }
    switch (m) {
    case WM_INPUT: {
        RAWINPUT ri;
        UINT sz = sizeof ri;
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 && ri.header.dwType == RIM_TYPEMOUSE) {
            auto &mo = ri.data.mouse;
            if (g_captured && !(mo.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                InterlockedExchangeAdd(&g_mdx, mo.lLastX);
                InterlockedExchangeAdd(&g_mdy, mo.lLastY);
                InterlockedExchangeAdd(&g_lookDX, mo.lLastX);
                InterlockedExchangeAdd(&g_lookDY, mo.lLastY);
            }
            USHORT f = mo.usButtonFlags;
            if (f & RI_MOUSE_BUTTON_1_DOWN) MouseButton(0, true);
            if (f & RI_MOUSE_BUTTON_1_UP) MouseButton(0, false);
            if (f & RI_MOUSE_BUTTON_2_DOWN) MouseButton(1, true);
            if (f & RI_MOUSE_BUTTON_2_UP) MouseButton(1, false);
            if (f & RI_MOUSE_BUTTON_3_DOWN) MouseButton(2, true);
            if (f & RI_MOUSE_BUTTON_3_UP) MouseButton(2, false);
            if (f & RI_MOUSE_BUTTON_4_DOWN) MouseButton(3, true);
            if (f & RI_MOUSE_BUTTON_4_UP) MouseButton(3, false);
            if (f & RI_MOUSE_BUTTON_5_DOWN) MouseButton(4, true);
            if (f & RI_MOUSE_BUTTON_5_UP) MouseButton(4, false);
            if (f & RI_MOUSE_WHEEL) InterlockedExchangeAdd(&g_mwheel, (SHORT)mo.usButtonData);
        }
        return false;   // DefWindowProc must still run for WM_INPUT
    }
    case WM_LBUTTONDOWN:
        if (g_captureMouse && !g_captured) { SetMouseCapture(true); g_mbtn[0] = false; g_mlatch[0] = false; return true; }
        if (!g_mbtn[0]) MouseButton(0, true, "msg");
        break;
    // every button also through the legacy messages, for mice whose clicks raw input misses (seen with the wheel
    // click, and with the right button in the front-end menus while the cursor is free)
    case WM_LBUTTONUP: if (g_mbtn[0]) MouseButton(0, false, "msg"); break;
    case WM_RBUTTONDOWN: if (!g_mbtn[1]) MouseButton(1, true, "msg"); break;
    case WM_RBUTTONUP: if (g_mbtn[1]) MouseButton(1, false, "msg"); break;
    case WM_MBUTTONDOWN: if (!g_mbtn[2]) MouseButton(2, true, "msg"); break;
    case WM_MBUTTONUP: if (g_mbtn[2]) MouseButton(2, false, "msg"); break;
    case WM_XBUTTONDOWN: { int i = HIWORD(w) == XBUTTON1 ? 3 : 4; if (!g_mbtn[i]) MouseButton(i, true, "msg"); break; }
    case WM_XBUTTONUP: { int i = HIWORD(w) == XBUTTON1 ? 3 : 4; if (g_mbtn[i]) MouseButton(i, false, "msg"); break; }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE && !(l & (1 << 30))) { extern volatile LONG g_escPressed; InterlockedExchange(&g_escPressed, 1); return true; }
        if (w == VK_F10) { g_captureMouse = !g_captureMouse; SetMouseCapture(g_captureMouse && Focused()); return true; }
        break;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) { SetMouseCapture(false); memset(g_mbtn, 0, sizeof g_mbtn); }
        else if (g_captureMouse) SetMouseCapture(true);
        break;
    case WM_SIZE: case WM_MOVE:
        if (g_captured) { g_captured = false; SetMouseCapture(true); }
        break;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Scripted input for automated tests: GHOSTPC_INPUT="900:Start,1000:A,1100:Left Axis Y+,1200:Esc" holds each
// control for 8 frames starting at that frame.
extern UINT g_frame;
struct ScriptEv { UINT frame; int ctl; };
static std::vector<ScriptEv> g_script;
static void LoadScript() {
    static bool done;
    if (done) return;
    done = true;
    const char *e = getenv("GHOSTPC_INPUT");
    if (!e) return;
    std::string s = e;
    size_t p = 0;
    while (p < s.size()) {
        size_t c = s.find(',', p);
        std::string it = s.substr(p, c == std::string::npos ? std::string::npos : c - p);
        size_t colon = it.find(':');
        if (colon != std::string::npos && it.substr(colon + 1) == "Esc") g_script.push_back({(UINT)atoi(it.c_str()), -1});
        else if (colon != std::string::npos)
            for (int i = 0; i < C_COUNT; i++)
                if (it.substr(colon + 1) == kCtlNames[i]) g_script.push_back({(UINT)atoi(it.c_str()), i});
        if (c == std::string::npos) break;
        p = c + 1;
    }
}
// a short synthetic press of the Start button (Esc in missions acts like Enter/Start)
void PressStart() { g_script.push_back({g_frame + 1, C_START}); }
void PressBack() { g_script.push_back({g_frame + 1, C_B}); }   // B = "Back" in the front-end menus
// scripted Esc presses (ctl -1) go through the same path as the key
void ScriptEsc() {
    extern volatile LONG g_escPressed;
    for (auto &e : g_script) if (e.ctl == -1 && g_frame == e.frame) InterlockedExchange(&g_escPressed, 1);
}
static float ScriptValue(int ctl) {
    for (auto &e : g_script) if (ctl == e.ctl && g_frame >= e.frame && g_frame < e.frame + 2) return 1.f;
    return 0.f;
}

// ---------------------------------------------------------------------------
// Evaluate bindings
struct MouseFrame { float dx, dy, wheel; };
static float BindValue(const Bind &b, const MouseFrame &mf) {
    switch (b.kind) {
    case BK_KEY: return (GetAsyncKeyState(b.code) & 0x8000) ? 1.f : 0.f;
    case BK_MBTN: return (b.code >= 0 && b.code < 5 && (g_mbtn[b.code] || g_mlatch[b.code])) ? 1.f : 0.f;
    case BK_MAXIS: {
        if (MouseLookActive()) return 0.f;   // the camera reads mouse motion directly (camera.cpp)
        float d = (b.code == 0 ? mf.dx : mf.dy) * b.sign / g_mouseRange;
        return std::min(std::max(d, 0.f), 1.f);
    }
    case BK_WHEEL: return std::min(std::max(mf.wheel * b.sign / g_wheelRange, 0.f), 1.f);
    }
    return 0.f;
}
static SHORT Axis(float pos, float neg) {
    float v = std::min(std::max(pos - neg, -1.f), 1.f);
    return (SHORT)(v < 0 ? v * 32768.f : v * 32767.f);
}

static uint32_t g_gamepadTable;
static DWORD g_reported;   // ports reported as inserted

static DWORD PortsFor(void *type) {
    if (!g_gamepadTable) g_gamepadTable = SymAddr("_XDEVICE_TYPE_GAMEPAD_TABLE");
    if ((uint32_t)type == g_gamepadTable) return 1;   // one pad on port 0 (keyboard/mouse and XInput pad 0)
    return 0;
}
XSTD(void, XInitDevices, 8, (DWORD, void *)) { LoadBindings(); }
XSTD(DWORD, XGetDevices, 4, (void *type)) { return PortsFor(type); }
XSTD(BOOL, XGetDeviceChanges, 12, (void *type, DWORD *ins, DWORD *rem)) {
    DWORD now = PortsFor(type);
    bool pad = (uint32_t)type == g_gamepadTable;
    DWORD prev = pad ? g_reported : 0;
    *ins = now & ~prev;
    *rem = prev & ~now;
    if (pad) g_reported = now;
    return (*ins | *rem) != 0;
}
XSTD(HANDLE, XInputOpen, 16, (void *type, DWORD port, DWORD slot, void *params)) {
    Log("XInputOpen port %u", port);
    return (HANDLE)(uintptr_t)(port + 1);
}
XSTD(void, XInputClose, 4, (HANDLE)) {}
XSTD(DWORD, XInputSetState, 8, (HANDLE h, void *fb)) { return 0; }

XSTD(DWORD, XInputGetState, 8, (HANDLE h, XB_INPUT_STATE *st)) {
    static DWORD packet;
    XB_GAMEPAD g = {};
    if ((uintptr_t)h == 1) {
        XINPUT_STATE x;
        if (XInputGetState(0, &x) == ERROR_SUCCESS) {
            auto &p = x.Gamepad;
            if (p.wButtons & XINPUT_GAMEPAD_DPAD_UP) g.wButtons |= XB_DPAD_UP;
            if (p.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) g.wButtons |= XB_DPAD_DOWN;
            if (p.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) g.wButtons |= XB_DPAD_LEFT;
            if (p.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) g.wButtons |= XB_DPAD_RIGHT;
            if (p.wButtons & XINPUT_GAMEPAD_START) g.wButtons |= XB_START;
            if (p.wButtons & XINPUT_GAMEPAD_BACK) g.wButtons |= XB_BACK;
            if (p.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) g.wButtons |= XB_LTHUMB;
            if (p.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) g.wButtons |= XB_RTHUMB;
            g.bAnalogButtons[AB_A] = (p.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
            g.bAnalogButtons[AB_B] = (p.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
            g.bAnalogButtons[AB_X] = (p.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
            g.bAnalogButtons[AB_Y] = (p.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
            g.bAnalogButtons[AB_BLACK] = (p.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
            g.bAnalogButtons[AB_WHITE] = (p.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
            g.bAnalogButtons[AB_LT] = p.bLeftTrigger;
            g.bAnalogButtons[AB_RT] = p.bRightTrigger;
            g.sThumbLX = p.sThumbLX; g.sThumbLY = p.sThumbLY;
            g.sThumbRX = p.sThumbRX; g.sThumbRY = p.sThumbRY;
        }
        // keyboard + mouse (only while the game window has focus)
        MouseFrame mf = {(float)InterlockedExchange(&g_mdx, 0), (float)InterlockedExchange(&g_mdy, 0),
                         (float)InterlockedExchange(&g_mwheel, 0)};
        LoadScript();
        bool focus = Focused();
        if (focus || !g_script.empty()) {   // (g_script also holds synthetic presses)
            float v[C_COUNT];
            bool kbm = focus && g_settings.input == IN_KBM;
            for (int i = 0; i < C_COUNT; i++) {
                float bv = 0;
                if (kbm) for (auto &b : g_bind[i]) bv = std::max(bv, BindValue(b, mf));
                v[i] = std::max(bv, ScriptValue(i));
            }
            if (kbm) memset(g_mlatch, 0, sizeof g_mlatch);   // the game has seen this poll's presses
            static const int btn[8][2] = {{C_DUP, XB_DPAD_UP}, {C_DDOWN, XB_DPAD_DOWN}, {C_DLEFT, XB_DPAD_LEFT},
                                          {C_DRIGHT, XB_DPAD_RIGHT}, {C_START, XB_START}, {C_BACK, XB_BACK},
                                          {C_LTHUMB, XB_LTHUMB}, {C_RTHUMB, XB_RTHUMB}};
            for (auto &b : btn) if (v[b[0]] > 0.5f) g.wButtons |= b[1];
            static const int ab[8] = {C_A, C_B, C_X, C_Y, C_BLACK, C_WHITE, C_LT, C_RT};
            for (int i = 0; i < 8; i++) g.bAnalogButtons[i] = std::max(g.bAnalogButtons[i], (BYTE)(v[ab[i]] * 255.f + 0.5f));
            auto merge = [](SHORT pad, SHORT kb) { return kb ? kb : pad; };
            g.sThumbLX = merge(g.sThumbLX, Axis(v[C_LXP], v[C_LXM]));
            g.sThumbLY = merge(g.sThumbLY, Axis(v[C_LYP], v[C_LYM]));
            g.sThumbRX = merge(g.sThumbRX, Axis(v[C_RXP], v[C_RXM]));
            g.sThumbRY = merge(g.sThumbRY, Axis(v[C_RYP], v[C_RYM]));
        }
    }
    st->dwPacketNumber = ++packet;
    st->Gamepad = g;
    return 0;
}
