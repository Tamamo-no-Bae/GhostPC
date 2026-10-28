// Direct mouse look. The game turns the camera by cControl::GetCameraYaw()/GetCameraPitch() (shaped right
// stick: 10% deadzone, squared, clamped) * frame time * 160 deg/s (yaw) / 80 deg/s (pitch). Through the stick a
// mouse fell into the deadzone and the curve, so these are reimplemented: the controller part is unchanged and
// mouse motion is added as an exact angle (degrees per count), independent of frame rate.
#include "common.h"
#include "settings.h"
#include <math.h>
#include <stdlib.h>

extern float g_frameLookDX, g_frameLookDY;
bool MouseLookActive();

static const float kDegPerCount = 0.15f;   // at 100% sensitivity
static const float kYawRate = 160.f, kPitchRate = 80.f;   // UpdateCameraThirdPerson's deg/s per unit

static float FrameTime() {
    static float *dt = (float *)(SymAddr("?stdTime@@3VtStdTime@@A") + 0x30);
    return dt ? *dt : 0.f;
}
static uint8_t *CurrentPad() {
    static uint8_t **pad = (uint8_t **)SymAddr("?stdInput_pCurrentGamepad@@3PAUtGamepad@@A");
    return pad ? *pad : nullptr;
}
// cControl -> +0x180 -> +0x520 -> +0x70: camera options; +0x248 bit 4 = invert X, bit 2 = invert Y
static bool Inverted(void *ctl, DWORD bit) {
    __try {
        uint8_t *a = *(uint8_t **)((uint8_t *)ctl + 0x180);
        uint8_t *b = a ? *(uint8_t **)(a + 0x520) : nullptr;
        uint8_t *c = b ? *(uint8_t **)(b + 0x70) : nullptr;
        return c && (*(DWORD *)(c + 0x248) & bit);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static float Shape(float v) {
    float a = fabsf(v);
    if (a <= 0.1f) return 0.f;
    float t = (a - 0.1f) * (1.f / 0.9f);
    t *= t;
    if (t > 1.f) t = 1.f;
    return v < 0 ? -t : t;
}
static float MouseTerm(float counts, float rate) {
    if (!MouseLookActive() || counts == 0.f) return 0.f;
    float dt = FrameTime();
    if (dt <= 0.f) return 0.f;
    float deg = counts * kDegPerCount * g_settings.mouseSensitivity / 100.f;
    return deg / (dt * rate);
}
static float __fastcall GetCameraYaw(void *ctl, void *) {
    uint8_t *pad = CurrentPad();
    float v = -(pad ? *(float *)(pad + 0x5c) : 0.f);
    v = Shape(v) - MouseTerm(g_frameLookDX, kYawRate);   // mouse right = turn right
    if (Inverted(ctl, 4)) v = -v;
    return v;
}
static float __fastcall GetCameraPitch(void *ctl, void *) {
    uint8_t *pad = CurrentPad();
    float v = -(pad ? *(float *)(pad + 0x60) : 0.f);
    v = Shape(v) - MouseTerm(g_frameLookDY, kPitchRate);  // mouse up = look up (the game's invert option applies)
    if (Inverted(ctl, 2)) v = -v;
    return v;
}

// Aim (Z-target: GunCrouch/GaussRifle states) and the sniper scope don't use the two functions above: they turn
// the camera angles (tSimCamera +0x20C yaw, +0x204 pitch) by a velocity that accelerates toward the stick target
// (ZTargetMovement: 200/100 deg/s, cut to 30%/50% below 80% deflection; sniper: 90/50 deg/s scaled by zoom). A
// mouse through that would lag, so mouse motion is added straight to the angles instead.
typedef float(__cdecl *AngleFn)(float);
static void *GameCamera(void *ctl) {
    static auto get = (void *(__fastcall *)(void *, void *))SymAddr("?GetCamera@cControl@@QAEPAVtSimCamera@@XZ");
    return get(ctl, nullptr);
}
// Mouse turn in degrees for this frame (stick-equivalent signs: mouse right = stick right, mouse up = stick down,
// as in GetCameraYaw/Pitch above), then the camera angles.
static void MouseTurnCamera(void *ctl, float scale, float pitchMin, float pitchMax) {
    if (!MouseLookActive() || (g_frameLookDX == 0.f && g_frameLookDY == 0.f)) return;
    static auto norm = (AngleFn)SymAddr("?mathTrig_NormalizeAngle@@YAMM@Z");
    static auto acute = (AngleFn)SymAddr("?mathTrig_NormalizeAngleAcute@@YAMM@Z");
    float k = kDegPerCount * g_settings.mouseSensitivity / 100.f * scale;
    float dyaw = -g_frameLookDX * k, dpitch = -g_frameLookDY * k;
    if (Inverted(ctl, 4)) dyaw = -dyaw;
    if (Inverted(ctl, 2)) dpitch = -dpitch;
    uint8_t *cam = (uint8_t *)GameCamera(ctl);
    if (!cam) return;
    float &yaw = *(float *)(cam + 0x20c), &pitch = *(float *)(cam + 0x204);
    yaw = norm(yaw + dyaw);
    float p = acute(pitch + dpitch);
    pitch = p < pitchMin ? pitchMin : p > pitchMax ? pitchMax : p;
}

static void __fastcall GetCameraAxes(void *ctl, void *, float *x, float *y) {
    static auto project = (void(__cdecl *)(float *, float *))SymAddr("?mathVector_ProjectCube@@YAXAAM0@Z");
    uint8_t *pad = CurrentPad();
    *x = -(pad ? *(float *)(pad + 0x5c) : 0.f);
    *y = -(pad ? *(float *)(pad + 0x60) : 0.f);
    project(x, y);
    if (Inverted(ctl, 4)) *x = -*x;
    *x = Shape(*x);
    if (Inverted(ctl, 2)) *y = -*y;
    *y = Shape(*y);
    MouseTurnCamera(ctl, 1.f, -47.f, 50.f);   // ZTargetMovement's pitch limits
}

// UpdateCameraSniper starts with "mov eax, [stdTime+0x5C]" (5 bytes; > 0 skips the update). The hook adds the
// mouse turn, replays that instruction and continues at +5.
static uint32_t s_sniperCont, s_sniperGate;
static void __cdecl SniperMouse(void *ctl) {
    if (*(int *)s_sniperGate > 0) return;
    uint8_t *cam = (uint8_t *)GameCamera(ctl);
    float fov = cam ? *(float *)(cam + 0x68) : 75.f;   // 5 (full zoom) .. 75
    if (fov < 1.f) fov = 1.f;
    // scale with the zoomed view so a count covers the same part of the screen at every zoom level
    float scale = tanf(fov * 0.5f * 0.0174533f) / tanf(37.5f * 0.0174533f);
    MouseTurnCamera(ctl, scale, -90.f, 90.f);
}
static __declspec(naked) void UpdateCameraSniperHook() {
    __asm {
        push ecx
        push ecx
        call SniperMouse
        add esp, 4
        pop ecx
        mov eax, s_sniperGate
        mov eax, [eax]
        jmp dword ptr [s_sniperCont]
    }
}
void CameraInit() {
    s_sniperGate = SymAddr("?stdTime@@3VtStdTime@@A") + 0x5c;
    s_sniperCont = SymAddr("?UpdateCameraSniper@cPlayerControl@@IAEXXZ") + 5;
    const uint8_t *p = (const uint8_t *)(s_sniperCont - 5);
    if (p[0] != 0xA1 || *(uint32_t *)(p + 1) != s_sniperGate) Log("camera: unexpected UpdateCameraSniper prologue");
}

XRAW("?GetCameraYaw@cControl@@QAEMXZ", GetCameraYaw);
XRAW("?GetCameraPitch@cControl@@QAEMXZ", GetCameraPitch);
XRAW("?GetCameraAxes@cControl@@QAEXAAM0@Z", GetCameraAxes);
XRAW("?UpdateCameraSniper@cPlayerControl@@IAEXXZ", UpdateCameraSniperHook);

// ScriptWorld::ScreenShake(duration ms, amplitude, flags): same as the game's (adds a shake effect to the first local
// player's camera), logged for GHOSTPC_FRAMESTATS runs
static void __cdecl ScreenShake(DWORD ms, float amp, DWORD flags) {
    static auto firstInfo = (uint8_t *(__cdecl *)())SymAddr("?gameMultiplayer_GetFirstLocalPlayerInfo@@YAPAVcPlayerInfo@@XZ");
    static auto addShake = (DWORD(__fastcall *)(void *, void *, DWORD, float, DWORD))SymAddr("?AddShakeEffect@tSimCamera@@QAEIIMI@Z");
    static bool log = getenv("GHOSTPC_FRAMESTATS") != nullptr;
    extern UINT g_frame;
    uint8_t *info = firstInfo();
    void *cam = info ? *(void **)(info + 4) : nullptr;
    if (log) Log("ScreenShake frame %u ms %u amp %g flags %x cam %p", g_frame, ms, amp, flags, cam);
    if (cam) addShake(cam, nullptr, ms, amp, flags);
}
XRAW("?ScreenShake@ScriptWorld@@SAXIMI@Z", ScreenShake);
