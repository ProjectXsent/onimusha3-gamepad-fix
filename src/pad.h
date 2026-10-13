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
    std::string Axis[8];                 // index = X,Y,Z,Rx,Ry,Rz,Slider0,Slider1; "" or "None" = held at center
    std::string Button[32];              // "" or "None" = unmapped; otherwise a name from kButtonNames
    std::string Action[kMaxActions];     // "" = ACT_DEFAULT; otherwise None / Hat / ButtonN / AxisN
    Config() {
        // Axes
        Axis[0] = "LeftX";    // X
        Axis[1] = "LeftY";    // Y
        Axis[2] = "None";     // Z
        Axis[3] = "RightX";   // Rx
        Axis[4] = "RightY";   // Ry
        Axis[5] = "None";     // Rz
        Axis[6] = "None";     // Slider0
        Axis[7] = "None";     // Slider1

        // Buttons
        Button[0]  = "A";
        Button[1]  = "B";
        Button[2]  = "X";
        Button[3]  = "Y";
        Button[4]  = "LB";
        Button[5]  = "RB";
        Button[6]  = "LT";
        Button[7]  = "RT";
        Button[8]  = "Back";
        Button[9]  = "Start";
        Button[10] = "LeftStick";
        Button[11] = "RightStick";
        Button[12] = "Guide";

        // Actions
        Action[0]  = "Axis1";    // Forward / Backward Movement
        Action[1]  = "Axis0";    // Left / Right Movement
        Action[2]  = "Axis3";    // Draw weapon X
        Action[3]  = "Axis4";    // Draw weapon Y
        Action[4]  = "Button0";  // Examine / Confirm
        Action[5]  = "Button1";  // Absorb Souls / Cancel
        Action[6]  = "Button2";  // Attack
        Action[7]  = "Button3";  // Special Attack
        Action[8]  = "Button4";  // Block
        Action[9]  = "Button5";  // Ready Stance
        Action[10] = "Button6";  // Show current location (hold)
        Action[11] = "Button7";  // 180 degree quick turn
        Action[12] = "Button8";  // Pause
        Action[13] = "Button9";  // Inventory Screen
        Action[14] = "None";     // Run
        Action[15] = "None";     // Back step
        Action[16] = "Button11"; // Onimusha Mode
        Action[17] = "None";     // Left Rotation
        Action[18] = "None";     // Right Rotation
        Action[69] = "Hat";      // Direction Keys
    }
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
