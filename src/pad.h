// pad.h - SDL gamepad backend for the OniInput dinput8 proxy.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <string>

namespace oni3 {

// Where a virtual DirectInput axis gets its value from.
enum AxisSrc {
    AX_NONE = 0,   // held at centre
    AX_LX, AX_LY,  // left stick
    AX_RX, AX_RY,  // right stick
    AX_TRIG,       // RT - LT combined (what XInput-over-DirectInput does)
    AX_LT, AX_RT,  // individual triggers
    AX_COUNT
};

// Button source values: 0..20 are SDL_GameControllerButton values, plus:
constexpr int SRC_NONE = -1;
constexpr int SRC_LT   = 100;  // left trigger as a digital button
constexpr int SRC_RT   = 101;  // right trigger as a digital button

// Per-action overrides for DirectInput action mapping ([Actions] in the INI).
enum ActKind { ACT_DEFAULT = 0, ACT_BUTTON, ACT_AXIS, ACT_HAT, ACT_NONE };
constexpr int kMaxActions = 128;

struct Config {
    bool  log           = false;
    bool  hideReal      = true;   // hide real gamepads from the game while we are active
    bool  alwaysExpose  = true;   // expose the virtual pad even with no controller attached
    bool  rumble        = true;   // emulate DirectInput force feedback via SDL rumble
    float rumbleStrength = 1.0f;  // overall rumble scale, 0..2
    int   padIndex      = 0;      // which connected SDL controller to use
    float deadzone      = 0.15f;  // radial stick deadzone, 0..1
    float triggerThreshold = 0.5f;// trigger-as-button threshold, 0..1
    int   numButtons    = 16;     // 1..32
    int   buttonMap[32];          // virtual button -> SDL button / SRC_*
    int   axisMap[8];             // X,Y,Z,Rx,Ry,Rz,Slider0,Slider1 -> AxisSrc
    int   actionKind[kMaxActions] = {};  // ActKind, indexed by position in the game's action array
    int   actionIdx[kMaxActions]  = {};  // button number / axis number for ACT_BUTTON / ACT_AXIS
    // Built-in defaults. Precedence: these strings -> Preset (if set) -> Oni3Input.ini overrides.
    // Preset: "" or "none" = use the strings below as-is; "xbox" / "ps2" = overwrite them with
    // that built-in layout. (Leave empty if you want the strings below to be the defaults.)
    std::string Preset = "";
    // Axes
    std::string X = "LeftX";
    std::string Y = "LeftY";
    std::string Z = "None";
    std::string Rx = "RightX";
    std::string Ry = "RightY";
    std::string Rz = "None";
    std::string Slider0 = "None";
    std::string Slider1 = "None";
    // Buttons
    std::string Button0 = "A";
    std::string Button1 = "B";
    std::string Button2 = "X";
    std::string Button3 = "Y";
    std::string Button4 = "LB";
    std::string Button5 = "RB";
    std::string Button6 = "LT";
    std::string Button7 = "RT";
    std::string Button8 = "Back";
    std::string Button9 = "Start";
    std::string Button10 = "LeftStick";
    std::string Button11 = "RightStick";
    std::string Button12 = "Guide";
    // Actions
    std::string Action0 = "Axis1";
    std::string Action1 = "Axis0";
    std::string Action2 = "Axis3";
    std::string Action3 = "Axis4";
    std::string Action4 = "Button0";
    std::string Action5 = "Button1";
    std::string Action6 = "Button2";
    std::string Action7 = "Button3";
    std::string Action8 = "Button4";
    std::string Action9 = "Button5";
    std::string Action10 = "Button6";
    std::string Action11 = "Button7";
    std::string Action12 = "Button8";
    std::string Action13 = "Button9";
    std::string Action14 = "None";
    std::string Action15 = "None";
    std::string Action16 = "Button11";
    std::string Action17 = "None";
    std::string Action18 = "None";
    std::string Action69 = "Hat";
};

struct PadState {
    float   src[AX_COUNT];  // normalised -1..1, indexed by AxisSrc
    int32_t pov;            // centidegrees (0 = up, clockwise), -1 = centred
    uint8_t btn[32];        // 0 / 1
};

void SetModule(HMODULE m);     // called from DllMain
const Config& GetConfig();     // loads Oni3Input.ini next to the DLL on first use
bool PadConnected();           // true if an SDL gamepad is currently open
void PadPoll(PadState& out);   // neutral state if nothing is connected
void PadRumble(float strong, float weak);  // 0..1 each; (0,0) stops. No-op without a pad.
void Log(const char* fmt, ...);

} // namespace oni3
