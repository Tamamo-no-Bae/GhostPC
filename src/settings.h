// GhostPC settings (ghostpc.ini) and the startup settings window
#pragma once
#include "common.h"

enum DisplayMode { DM_WINDOWED = 0, DM_FULLSCREEN = 1, DM_BORDERLESS = 2 };
enum InputDevice { IN_KBM = 0, IN_CONTROLLER = 1 };

struct Settings {
    int width = 1280, height = 960;   // output resolution (windowed / fullscreen)
    int mode = DM_WINDOWED;
    int input = IN_KBM;
    int mouseSensitivity = 100;       // percent
    bool vsync = true;
    bool showAtStartup = true;
};
extern Settings g_settings;

void SettingsLoad();
void SettingsSave();
bool SettingsDialog();               // false = the player chose Quit
const char *IniPath();
void InputEnsureIni();               // input.cpp: writes the default [input] section if the ini is new
int RenderScaleFor(int height);      // internal resolution multiple of 640x480 for an output height
