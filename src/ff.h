// ff.h - DirectInput force-feedback emulation on top of SDL rumble.
#pragma once
#define DIRECTINPUT_VERSION 0x0800
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dinput.h>

namespace oni3 {

// Effect catalogue the virtual device reports.
int         FfEffectCount();
const GUID& FfEffectGuid(int i);
bool        FfKnownEffect(const GUID& g);
DWORD       FfEffectType(const GUID& g);   // DIEFT_* incl. capability flags
const char* FfEffectName(const GUID& g);

// IDirectInputDevice8::CreateEffect
HRESULT     FfCreateEffect(const GUID& g, LPCDIEFFECT params, LPDIRECTINPUTEFFECT* out);

// Device-wide controls (DIPROP_FFGAIN, SendForceFeedbackCommand, GetForceFeedbackState)
void        FfSetGain(DWORD gain);         // 0..10000
DWORD       FfGetGain();
void        FfCommand(DWORD cmd);          // DISFFC_*
DWORD       FfStateFlags();                // DIGFFS_*

} // namespace oni3
