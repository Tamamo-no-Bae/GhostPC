// Settings window shown before the game starts (resolution, display mode, input device, mouse sensitivity).
// Values live in ghostpc.ini next to the key bindings: [video], [input], [multiplayer] and [launcher].
#include "settings.h"
#include <commctrl.h>
#include <algorithm>
#include <string>
#include <vector>
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")

Settings g_settings;

const char *IniPath() {
    static char p[MAX_PATH];
    if (!p[0]) _snprintf(p, MAX_PATH, "%s\\ghostpc.ini", g_gameDir);
    return p;
}
static int IniInt(const char *sec, const char *key, int def) { return GetPrivateProfileIntA(sec, key, def, IniPath()); }
static void IniPut(const char *sec, const char *key, const char *v) { WritePrivateProfileStringA(sec, key, v, IniPath()); }
static void IniPut(const char *sec, const char *key, int v) { char b[32]; sprintf(b, "%d", v); IniPut(sec, key, b); }

int RenderScaleFor(int height) { return std::max(1, std::min(6, (height + 479) / 480)); }

void SettingsLoad() {
    InputEnsureIni();
    char res[64];
    GetPrivateProfileStringA("video", "Resolution", "1280x960", res, sizeof res, IniPath());
    int w, h;
    if (sscanf(res, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240) { g_settings.width = w; g_settings.height = h; }
    char mode[32];
    GetPrivateProfileStringA("video", "DisplayMode", "windowed", mode, sizeof mode, IniPath());
    g_settings.mode = !_stricmp(mode, "fullscreen") ? DM_FULLSCREEN : !_stricmp(mode, "borderless") ? DM_BORDERLESS : DM_WINDOWED;
    g_settings.vsync = IniInt("video", "VSync", 1) != 0;
    char in[32];
    GetPrivateProfileStringA("input", "InputDevice", "keyboardmouse", in, sizeof in, IniPath());
    g_settings.input = !_stricmp(in, "controller") ? IN_CONTROLLER : IN_KBM;
    g_settings.mouseSensitivity = std::max(10, std::min(500, IniInt("input", "MouseSensitivity", 100)));
    g_settings.showAtStartup = IniInt("launcher", "ShowSettings", 1) != 0;
    g_settings.matchMinutes = std::max(1, std::min(99, IniInt("multiplayer", "MatchMinutes", 10)));
}
void SettingsSave() {
    char res[64];
    sprintf(res, "%dx%d", g_settings.width, g_settings.height);
    IniPut("video", "Resolution", res);
    IniPut("video", "DisplayMode", g_settings.mode == DM_FULLSCREEN ? "fullscreen" : g_settings.mode == DM_BORDERLESS ? "borderless" : "windowed");
    IniPut("video", "VSync", g_settings.vsync ? 1 : 0);
    IniPut("input", "InputDevice", g_settings.input == IN_CONTROLLER ? "controller" : "keyboardmouse");
    IniPut("input", "MouseSensitivity", g_settings.mouseSensitivity);
    IniPut("multiplayer", "MatchMinutes", g_settings.matchMinutes);
    IniPut("launcher", "ShowSettings", g_settings.showAtStartup ? 1 : 0);
}

// ---------------------------------------------------------------------------
// Dialog (plain Win32 controls, built in code)
struct Res { int w, h; const char *tag; };
static const Res kRes[] = {
    {640, 480, "4:3, original"}, {800, 600, "4:3"}, {1024, 768, "4:3"}, {1280, 960, "4:3"}, {1440, 1080, "4:3"},
    {1600, 1200, "4:3"}, {1920, 1440, "4:3"}, {2048, 1536, "4:3"}, {2880, 2160, "4:3"},
    {1280, 720, "16:9"}, {1366, 768, "16:9"}, {1600, 900, "16:9"}, {1920, 1080, "16:9"}, {2560, 1440, "16:9"},
    {3840, 2160, "16:9"}, {1920, 1200, "16:10"}, {2560, 1600, "16:10"},
};
enum { ID_RES = 100, ID_WIN, ID_FULL, ID_BORDER, ID_KBM, ID_PAD, ID_SENS, ID_SENSVAL, ID_VSYNC, ID_SHOW, ID_PLAY, ID_QUIT, ID_INFO, ID_MATCH };
static const int kMatchMinutes[] = {3, 5, 10, 15, 20, 30};
static std::vector<int> g_matchList;
static HFONT g_font, g_bold;
static int g_result = -1;
static std::vector<Res> g_resList;

static HWND Ctl(HWND p, const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id, HFONT f = nullptr) {
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, p, (HMENU)(INT_PTR)id, GetModuleHandleA(nullptr), nullptr);
    SendMessageA(c, WM_SETFONT, (WPARAM)(f ? f : g_font), TRUE);
    return c;
}
static void UpdateInfo(HWND h) {
    int sel = (int)SendDlgItemMessageA(h, ID_RES, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)g_resList.size()) return;
    bool border = IsDlgButtonChecked(h, ID_BORDER) == BST_CHECKED;
    int oh = g_resList[sel].h;
    if (border) oh = GetSystemMetrics(SM_CYSCREEN);
    int s = RenderScaleFor(oh);
    char b[200];
    _snprintf(b, sizeof b, "Renders at %dx%d (%dx the Xbox resolution)%s.", 640 * s, 480 * s, s,
              border ? ", shown on the whole desktop" : "");
    SetDlgItemTextA(h, ID_INFO, b);
    bool pad = IsDlgButtonChecked(h, ID_PAD) == BST_CHECKED;
    EnableWindow(GetDlgItem(h, ID_SENS), !pad);
    sprintf(b, "%d%%", (int)SendDlgItemMessageA(h, ID_SENS, TBM_GETPOS, 0, 0));
    SetDlgItemTextA(h, ID_SENSVAL, b);
}
static LRESULT CALLBACK DlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_PLAY: {
            int sel = (int)SendDlgItemMessageA(h, ID_RES, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)g_resList.size()) { g_settings.width = g_resList[sel].w; g_settings.height = g_resList[sel].h; }
            g_settings.mode = IsDlgButtonChecked(h, ID_FULL) == BST_CHECKED ? DM_FULLSCREEN
                            : IsDlgButtonChecked(h, ID_BORDER) == BST_CHECKED ? DM_BORDERLESS : DM_WINDOWED;
            g_settings.input = IsDlgButtonChecked(h, ID_PAD) == BST_CHECKED ? IN_CONTROLLER : IN_KBM;
            g_settings.mouseSensitivity = (int)SendDlgItemMessageA(h, ID_SENS, TBM_GETPOS, 0, 0);
            g_settings.vsync = IsDlgButtonChecked(h, ID_VSYNC) == BST_CHECKED;
            g_settings.showAtStartup = IsDlgButtonChecked(h, ID_SHOW) == BST_CHECKED;
            int m = (int)SendDlgItemMessageA(h, ID_MATCH, CB_GETCURSEL, 0, 0);
            if (m >= 0 && m < (int)g_matchList.size()) g_settings.matchMinutes = g_matchList[m];
            g_result = 1;
            DestroyWindow(h);
            return 0;
        }
        case ID_QUIT: g_result = 0; DestroyWindow(h); return 0;
        case ID_RES: if (HIWORD(w) == CBN_SELCHANGE) UpdateInfo(h); return 0;
        case ID_WIN: case ID_FULL: case ID_BORDER: case ID_KBM: case ID_PAD: UpdateInfo(h); return 0;
        }
        break;
    case WM_HSCROLL: UpdateInfo(h); return 0;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: {
        SetBkMode((HDC)w, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_KEYDOWN: if (w == VK_ESCAPE) { g_result = 0; DestroyWindow(h); } return 0;
    case WM_CLOSE: g_result = 0; DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

bool SettingsDialog() {
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    NONCLIENTMETRICSA ncm = {sizeof ncm};
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectA(&ncm.lfMessageFont);
    LOGFONTA lb = ncm.lfMessageFont;
    lb.lfWeight = FW_BOLD;
    g_bold = CreateFontIndirectA(&lb);

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DlgProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = "GhostPCSettings";
    RegisterClassA(&wc);
    const int W = 420, H = 490;
    RECT r = {0, 0, W, H};
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    int sx = (GetSystemMetrics(SM_CXSCREEN) - (r.right - r.left)) / 2, sy = (GetSystemMetrics(SM_CYSCREEN) - (r.bottom - r.top)) / 2;
    HWND h = CreateWindowExA(WS_EX_DLGMODALFRAME, "GhostPCSettings", "StarCraft: Ghost - GhostPC settings", style, sx, sy,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);

    int y = 14;
    Ctl(h, "STATIC", "Display", 0, 16, y, 200, 20, -1, g_bold); y += 26;
    Ctl(h, "STATIC", "Resolution:", 0, 28, y + 3, 90, 20, -1);
    HWND cb = Ctl(h, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 120, y, 270, 300, ID_RES);
    g_resList.clear();
    int sel = -1;
    for (auto &rs : kRes) {
        g_resList.push_back(rs);
        char b[64];
        sprintf(b, "%d x %d  (%s)", rs.w, rs.h, rs.tag);
        SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)b);
        if (rs.w == g_settings.width && rs.h == g_settings.height) sel = (int)g_resList.size() - 1;
    }
    if (sel < 0) {   // custom value from the ini
        g_resList.push_back({g_settings.width, g_settings.height, "custom"});
        char b[64];
        sprintf(b, "%d x %d  (custom)", g_settings.width, g_settings.height);
        SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)b);
        sel = (int)g_resList.size() - 1;
    }
    SendMessageA(cb, CB_SETCURSEL, sel, 0);
    y += 34;
    Ctl(h, "STATIC", "Mode:", 0, 28, y + 2, 90, 20, -1);
    Ctl(h, "BUTTON", "Windowed", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 120, y, 100, 22, ID_WIN);
    Ctl(h, "BUTTON", "Fullscreen", BS_AUTORADIOBUTTON, 222, y, 90, 22, ID_FULL);
    Ctl(h, "BUTTON", "Borderless", BS_AUTORADIOBUTTON, 314, y, 100, 22, ID_BORDER);
    CheckDlgButton(h, g_settings.mode == DM_FULLSCREEN ? ID_FULL : g_settings.mode == DM_BORDERLESS ? ID_BORDER : ID_WIN, BST_CHECKED);
    y += 30;
    Ctl(h, "BUTTON", "VSync", BS_AUTOCHECKBOX | WS_TABSTOP, 120, y, 120, 22, ID_VSYNC);
    CheckDlgButton(h, ID_VSYNC, g_settings.vsync ? BST_CHECKED : BST_UNCHECKED);
    y += 28;
    Ctl(h, "STATIC", "", 0, 28, y, 370, 36, ID_INFO);
    y += 46;

    Ctl(h, "STATIC", "Controls", 0, 16, y, 200, 20, -1, g_bold); y += 26;
    Ctl(h, "BUTTON", "Keyboard + mouse", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 28, y, 160, 22, ID_KBM);
    Ctl(h, "BUTTON", "Controller", BS_AUTORADIOBUTTON, 200, y, 160, 22, ID_PAD);
    CheckDlgButton(h, g_settings.input == IN_CONTROLLER ? ID_PAD : ID_KBM, BST_CHECKED);
    y += 32;
    Ctl(h, "STATIC", "Mouse sensitivity:", 0, 28, y + 4, 110, 20, -1);
    HWND tb = Ctl(h, TRACKBAR_CLASSA, "", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 140, y, 200, 28, ID_SENS);
    SendMessageA(tb, TBM_SETRANGE, TRUE, MAKELPARAM(10, 500));
    SendMessageA(tb, TBM_SETPAGESIZE, 0, 10);
    SendMessageA(tb, TBM_SETPOS, TRUE, g_settings.mouseSensitivity);
    Ctl(h, "STATIC", "", 0, 346, y + 4, 50, 20, ID_SENSVAL);
    y += 34;
    Ctl(h, "STATIC", "Key bindings: [input] in ghostpc.ini (Cxbx-Reloaded profile syntax).", 0, 28, y, 370, 20, -1);
    y += 30;

    Ctl(h, "STATIC", "Multiplayer", 0, 16, y, 200, 20, -1, g_bold); y += 26;
    Ctl(h, "STATIC", "Competitive time limit:", 0, 28, y + 3, 140, 20, -1);
    HWND mc = Ctl(h, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 170, y, 120, 200, ID_MATCH);
    g_matchList.assign(std::begin(kMatchMinutes), std::end(kMatchMinutes));
    if (std::find(g_matchList.begin(), g_matchList.end(), g_settings.matchMinutes) == g_matchList.end())
        g_matchList.push_back(g_settings.matchMinutes);   // custom value from the ini
    for (int i = 0; i < (int)g_matchList.size(); i++) {
        char b[32];
        sprintf(b, "%d minutes", g_matchList[i]);
        SendMessageA(mc, CB_ADDSTRING, 0, (LPARAM)b);
        if (g_matchList[i] == g_settings.matchMinutes) SendMessageA(mc, CB_SETCURSEL, i, 0);
    }
    y += 40;

    Ctl(h, "BUTTON", "Show this window at startup (hold Shift while launching to get it back)", BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP, 16, y, 388, 36, ID_SHOW);
    CheckDlgButton(h, ID_SHOW, g_settings.showAtStartup ? BST_CHECKED : BST_UNCHECKED);
    Ctl(h, "BUTTON", "Play", BS_DEFPUSHBUTTON | WS_TABSTOP, W - 196, H - 44, 86, 30, ID_PLAY);
    Ctl(h, "BUTTON", "Quit", BS_PUSHBUTTON | WS_TABSTOP, W - 102, H - 44, 86, 30, ID_QUIT);
    UpdateInfo(h);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    SetFocus(GetDlgItem(h, ID_PLAY));

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) { SendMessageA(h, WM_COMMAND, ID_PLAY, 0); continue; }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { SendMessageA(h, WM_COMMAND, ID_QUIT, 0); continue; }
        if (!IsDialogMessageA(h, &msg)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    }
    if (g_result == 1) SettingsSave();
    return g_result == 1;
}
