// proxy.cpp - dinput8.dll proxy.
//
// Exports DirectInput8Create, forwards everything to the real system dinput8.dll,
// and additionally presents ONE virtual joystick (backed by an SDL game controller)
// through IDirectInput8::EnumDevices / CreateDevice.
//
// Build as 32-bit (the game is a 32-bit process).

#define DIRECTINPUT_VERSION 0x0800
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dinput.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

#include <type_traits>
#include "ff.h"
#include "pad.h"

using namespace oni3;

// --- Macros that not every dinput.h defines. Only used if the SDK header doesn't have them.
#ifndef DIA_FORCEFEEDBACK
#define DIA_FORCEFEEDBACK 0x00000001
#endif
#ifndef DIDBAM_PRESERVE
#define DIDBAM_PRESERVE 0x00000001
#endif
#ifndef DIEDBSFL_FORCEFEEDBACK
#define DIEDBSFL_FORCEFEEDBACK 0x00000100
#endif
#ifndef DIEFT_GETTYPE
#define DIEFT_GETTYPE(n) ((BYTE)(n))
#endif
#ifndef DIDFT_GETTYPE
#define DIDFT_GETTYPE(n) ((BYTE)(n))
#endif
#ifndef DIDFT_GETINSTANCE
#define DIDFT_GETINSTANCE(n) ((WORD)((n) >> 8))
#endif
#ifndef DIDFT_MAKEINSTANCE
#define DIDFT_MAKEINSTANCE(n) ((WORD)(n) << 8)
#endif
#ifndef DIDFT_FFEFFECTTRIGGER
#define DIDFT_FFEFFECTTRIGGER 0x02000000
#endif

// --- Action semantics. Standard semantic codes (DIAXIS_*, DIBUTTON_*, DIHATSWITCH_*) carry
// the control type in bits 9-10: 0x200 = axis, 0x400 = button, 0x600 = hat switch, and bit 15
// marks a relative axis (e.g. DIAXIS_ARCADEP_LATERAL = 0x22200200, DIBUTTON_ARCADEP_FIRE =
// 0x22000401, DIHATSWITCH_ARCADEP_VIEW = 0x22004601). These are defined here instead of using
// SDK macros because the SDK's naming for them varies. The dump below prints the decoded type
// next to the raw semantic so a wrong guess is easy to spot in the log.
enum SemType { SEM_UNKNOWN = 0, SEM_AXIS = 0x200, SEM_BUTTON = 0x400, SEM_POV = 0x600 };
static DWORD SemTypeOf(DWORD semantic) { return semantic & 0x600; }
static bool  SemIsRelative(DWORD semantic) { return (semantic & 0x8000) != 0; }
static const char* SemTypeName(DWORD semantic) {
    switch (SemTypeOf(semantic)) {
        case SEM_AXIS:   return SemIsRelative(semantic) ? "axis(rel)" : "axis";
        case SEM_BUTTON: return "button";
        case SEM_POV:    return "hat";
        default:         return "?";
    }
}

// Identity of the virtual device (arbitrary, just needs to be unique and stable).
// {6F1D2B40-5A37-4C0E-9B53-0E1A4F7C2D91}
static const GUID kGuidInstance = {0x6f1d2b40, 0x5a37, 0x4c0e, {0x9b, 0x53, 0x0e, 0x1a, 0x4f, 0x7c, 0x2d, 0x91}};
// {6F1D2B41-5A37-4C0E-9B53-0E1A4F7C2D91}
static const GUID kGuidProduct  = {0x6f1d2b41, 0x5a37, 0x4c0e, {0x9b, 0x53, 0x0e, 0x1a, 0x4f, 0x7c, 0x2d, 0x91}};

// {6F1D2B42-5A37-4C0E-9B53-0E1A4F7C2D91}
static const GUID kGuidFFDriver = {0x6f1d2b42, 0x5a37, 0x4c0e, {0x9b, 0x53, 0x0e, 0x1a, 0x4f, 0x7c, 0x2d, 0x91}};

static const DWORD kDevType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8) | DIDEVTYPE_HID;
static const char* kDevName = "SDL Gamepad";

// ------------------------------------------------------------ A/W traits ---

template <bool W> struct Tr;

template <> struct Tr<false> {
    using DI         = IDirectInput8A;
    using Dev        = IDirectInputDevice8A;
    using DevInst    = DIDEVICEINSTANCEA;
    using ObjInst    = DIDEVICEOBJECTINSTANCEA;
    using EnumDevCb  = LPDIENUMDEVICESCALLBACKA;
    using EnumObjCb  = LPDIENUMDEVICEOBJECTSCALLBACKA;
    using EnumEffCb  = LPDIENUMEFFECTSCALLBACKA;
    using EffInfoPtr = LPDIEFFECTINFOA;
    using Str        = LPCSTR;
    using ActFmt     = LPDIACTIONFORMATA;
    using SemCb      = LPDIENUMDEVICESBYSEMANTICSCBA;
    using CfgParams  = LPDICONFIGUREDEVICESPARAMSA;
    using ImgHdr     = LPDIDEVICEIMAGEINFOHEADERA;
    static const IID& IidDI()  { return IID_IDirectInput8A; }
    static const IID& IidDev() { return IID_IDirectInputDevice8A; }
};
template <> struct Tr<true> {
    using DI         = IDirectInput8W;
    using Dev        = IDirectInputDevice8W;
    using DevInst    = DIDEVICEINSTANCEW;
    using ObjInst    = DIDEVICEOBJECTINSTANCEW;
    using EnumDevCb  = LPDIENUMDEVICESCALLBACKW;
    using EnumObjCb  = LPDIENUMDEVICEOBJECTSCALLBACKW;
    using EnumEffCb  = LPDIENUMEFFECTSCALLBACKW;
    using EffInfoPtr = LPDIEFFECTINFOW;
    using Str        = LPCWSTR;
    using ActFmt     = LPDIACTIONFORMATW;
    using SemCb      = LPDIENUMDEVICESBYSEMANTICSCBW;
    using CfgParams  = LPDICONFIGUREDEVICESPARAMSW;
    using ImgHdr     = LPDIDEVICEIMAGEINFOHEADERW;
    static const IID& IidDI()  { return IID_IDirectInput8W; }
    static const IID& IidDev() { return IID_IDirectInputDevice8W; }
};

static void CopyStr(char* dst, size_t n, const char* s) { strncpy_s(dst, n, s, _TRUNCATE); }
static void CopyStr(wchar_t* dst, size_t n, const char* s) {
    size_t i = 0;
    for (; s[i] && i + 1 < n; ++i) dst[i] = (unsigned char)s[i];
    dst[i] = 0;
}

static bool IsGameCtrlType(DWORD devType) {
    switch (GET_DIDEVICE_TYPE(devType)) {
        case DI8DEVTYPE_JOYSTICK: case DI8DEVTYPE_GAMEPAD: case DI8DEVTYPE_DRIVING:
        case DI8DEVTYPE_FLIGHT:   case DI8DEVTYPE_1STPERSON: case DI8DEVTYPE_SUPPLEMENTAL:
            return true;
    }
    return false;
}

// Should the real gamepads be hidden / replaced by the virtual one right now?
static bool TakeOver() {
    const Config& c = GetConfig();
    return c.hideReal && (PadConnected() || c.alwaysExpose);
}

template <class Inst>
static void FillInstance(Inst& i) {
    memset(&i, 0, sizeof(i));
    i.dwSize       = sizeof(i);
    i.guidInstance = kGuidInstance;
    i.guidProduct  = kGuidProduct;
    if (GetConfig().rumble) i.guidFFDriver = kGuidFFDriver;
    i.dwDevType    = kDevType;
    i.wUsagePage   = 0x01;  // generic desktop
    i.wUsage       = 0x05;  // game pad
    CopyStr(i.tszInstanceName, MAX_PATH, kDevName);
    CopyStr(i.tszProductName,  MAX_PATH, kDevName);
}

static void NameOut(char* o, size_t n, const char* s) { strncpy_s(o, n, s, _TRUNCATE); }
static void NameOut(char* o, size_t n, const wchar_t* s) {
    WideCharToMultiByte(CP_ACP, 0, s, -1, o, (int)n, nullptr, nullptr);
    o[n - 1] = 0;
}

// Logs the game's DIACTIONFORMAT. This is what tells us which semantics the game wants.
template <class AF>
static void DumpActions(const char* tag, AF af, DWORD flags) {
    if (!GetConfig().log || !af) return;
    Log("%s flags=0x%lx genre=0x%lx numActions=%lu axisRange=[%ld,%ld] bufSize=%lu",
        tag, flags, af->dwGenre, af->dwNumActions, af->lAxisMin, af->lAxisMax, af->dwBufferSize);
    if (!af->rgoAction) return;
    for (DWORD i = 0; i < af->dwNumActions; ++i) {
        const auto& a = af->rgoAction[i];
        char name[128] = "-";
        const uintptr_t v = (uintptr_t)a.lptszActionName;
        if (v >= 0x10000) NameOut(name, sizeof(name), a.lptszActionName);
        else              sprintf_s(name, "res#%u", (unsigned)v);
        Log("  [%lu] appData=%llu semantic=0x%08lx (%s) flags=0x%lx how=0x%lx obj=0x%08lx name=\"%s\"",
            i, (unsigned long long)a.uAppData, a.dwSemantic, SemTypeName(a.dwSemantic),
            a.dwFlags, a.dwHow, a.dwObjID, name);
    }
}

static const DWORD kFfParams = DIEP_DURATION | DIEP_GAIN | DIEP_ENVELOPE | DIEP_TYPESPECIFICPARAMS |
                               DIEP_STARTDELAY | DIEP_DIRECTION | DIEP_AXES | DIEP_TRIGGERBUTTON |
                               DIEP_TRIGGERREPEATINTERVAL | DIEP_SAMPLEPERIOD;

template <class EI>
static void FillEff(EI& e, const GUID& g) {
    memset(&e, 0, sizeof(e));
    e.dwSize = sizeof(e);
    e.guid = g;
    e.dwEffType = FfEffectType(g);
    e.dwStaticParams = e.dwDynamicParams = kFfParams;
    CopyStr(e.tszName, MAX_PATH, FfEffectName(g));
}

// ----------------------------------------------------- the virtual device ---

template <bool W>
class VirtualDevice final : public Tr<W>::Dev {
    using T = Tr<W>;

    enum Kind { K_NONE, K_AXIS, K_POV, K_BUTTON };
    struct Slot { DWORD ofs; Kind kind; int idx; };   // where to write what in the game's buffer
    struct Obj  { const GUID* guid; DWORD type; DWORD flags; Kind kind; int idx; char name[24]; };

    LONG              ref_ = 1;
    std::vector<Slot> slots_;
    LONG              range_[8][2];   // per-axis [min,max], DirectInput default 0..65535
    bool              loggedState_ = false;

    // --- action-mapping / buffered-data state
    struct Act { UINT_PTR appData; Kind kind; int idx; };
    std::vector<Act>                acts_;       // actions bound to this device by SetActionMap
    std::deque<DIDEVICEOBJECTDATA>  events_;
    DWORD                           bufSize_ = 64;
    DWORD                           seq_ = 0;
    bool                            havePrev_ = false;
    LONG                            prevAxis_[8] = {};
    DWORD                           prevPov_ = 0xFFFFFFFFu;
    uint8_t                         prevBtn_[32] = {};

    static DWORD ObjId(Kind k, int idx) {
        switch (k) {
            case K_AXIS:   return DIDFT_ABSAXIS   | DIDFT_MAKEINSTANCE(idx);
            case K_POV:    return DIDFT_POV       | DIDFT_MAKEINSTANCE(idx);
            case K_BUTTON: return DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(idx);
            default:       return 0;
        }
    }
    static bool DecodeObj(DWORD id, Kind& k, int& idx) {
        const DWORD t = DIDFT_GETTYPE(id), n = DIDFT_GETINSTANCE(id);
        if      (t & DIDFT_AXIS)   { k = K_AXIS;   idx = (int)n; return n < 6; }
        else if (t & DIDFT_POV)    { k = K_POV;    idx = (int)n; return n < 1; }
        else if (t & DIDFT_BUTTON) { k = K_BUTTON; idx = (int)n; return n < 32; }
        return false;
    }

    // Poll the pad and queue an event for every bound action whose value changed.
    void Capture() {
        PadState s;
        PadPoll(s);
        LONG ax[8];
        for (int i = 0; i < 8; ++i) ax[i] = AxisValue(i, s);
        if (!havePrev_) {   // baseline = neutral, so anything already held fires an event
            for (int i = 0; i < 8; ++i) prevAxis_[i] = (range_[i][0] + range_[i][1]) / 2;
            memset(prevBtn_, 0, sizeof(prevBtn_));
            prevPov_ = 0xFFFFFFFFu;
            havePrev_ = true;
        }
        const DWORD now = GetTickCount();
        for (const Act& a : acts_) {
            DWORD cur = 0, old = 0;
            switch (a.kind) {
                case K_BUTTON: cur = s.btn[a.idx] ? 0x80 : 0; old = prevBtn_[a.idx] ? 0x80 : 0; break;
                case K_AXIS:   cur = (DWORD)ax[a.idx];        old = (DWORD)prevAxis_[a.idx];    break;
                case K_POV:    cur = (DWORD)s.pov;            old = prevPov_;                   break;
                default: continue;
            }
            if (cur == old) continue;
            DIDEVICEOBJECTDATA e;
            memset(&e, 0, sizeof(e));
            e.dwOfs       = (DWORD)a.appData;   // see README: unclear which field the game reads
            e.dwData      = cur;
            e.dwTimeStamp = now;
            e.dwSequence  = seq_++;
            e.uAppData    = a.appData;
            if (events_.size() >= bufSize_) events_.pop_front();
            events_.push_back(e);
        }
        for (int i = 0; i < 8; ++i) prevAxis_[i] = ax[i];
        memcpy(prevBtn_, s.btn, sizeof(prevBtn_));
        prevPov_ = (DWORD)s.pov;
    }

    static bool Eq(const GUID* a, const GUID& b) { return a && IsEqualGUID(*a, b); }

    void DefaultSlots() {
        slots_.clear();
        const DWORD ax[8] = {
            offsetof(DIJOYSTATE, lX),  offsetof(DIJOYSTATE, lY),  offsetof(DIJOYSTATE, lZ),
            offsetof(DIJOYSTATE, lRx), offsetof(DIJOYSTATE, lRy), offsetof(DIJOYSTATE, lRz),
            offsetof(DIJOYSTATE, rglSlider), offsetof(DIJOYSTATE, rglSlider) + 4};
        for (int i = 0; i < 8; ++i) slots_.push_back({ax[i], K_AXIS, i});
        for (int i = 0; i < 4; ++i)  slots_.push_back({(DWORD)offsetof(DIJOYSTATE, rgdwPOV) + 4 * i, K_POV, i});
        for (int i = 0; i < 32; ++i) slots_.push_back({(DWORD)offsetof(DIJOYSTATE, rgbButtons) + i, K_BUTTON, i});
    }

    DWORD OfsOf(Kind k, int idx) const {
        for (const Slot& s : slots_) if (s.kind == k && s.idx == idx) return s.ofs;
        return 0;
    }

    std::vector<Obj> Objects() const {
        static const GUID* axGuid[6] = {&GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis, &GUID_RyAxis, &GUID_RzAxis};
        static const char* axName[6] = {"X Axis", "Y Axis", "Z Axis", "X Rotation", "Y Rotation", "Z Rotation"};
        std::vector<Obj> v;
        for (int i = 0; i < 6; ++i) {
            const bool ff = GetConfig().rumble && i < 2;   // X/Y act as the FF actuators
            Obj o{axGuid[i], DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(i) | (ff ? DIDFT_FFACTUATOR : 0u),
                  DIDOI_ASPECTPOSITION | (ff ? DIDOI_FFACTUATOR : 0u), K_AXIS, i, {}};
            strcpy_s(o.name, axName[i]);
            v.push_back(o);
        }
        Obj p{&GUID_POV, DIDFT_POV | DIDFT_MAKEINSTANCE(0), 0, K_POV, 0, {}};
        strcpy_s(p.name, "Hat Switch");
        v.push_back(p);
        const int nb = GetConfig().numButtons;
        for (int i = 0; i < nb; ++i) {
            Obj b{&GUID_Button, DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(i), 0, K_BUTTON, i, {}};
            sprintf_s(b.name, "Button %d", i);
            v.push_back(b);
        }
        return v;
    }

    void FillObj(typename T::ObjInst& d, const Obj& o) const {
        memset(&d, 0, sizeof(d));
        d.dwSize   = sizeof(d);
        d.guidType = *o.guid;
        d.dwOfs    = OfsOf(o.kind, o.idx);
        d.dwType   = o.type;
        d.dwFlags  = o.flags;
        if (o.flags & DIDOI_FFACTUATOR) { d.dwFFMaxForce = 10000; d.dwFFForceResolution = 1; }
        CopyStr(d.tszName, MAX_PATH, o.name);
    }

    // Which of our 8 axes does a DIPROPHEADER address? (bitmask)
    unsigned AxesFor(const DIPROPHEADER* h) const {
        if (h->dwHow == DIPH_DEVICE) return 0xFFu;
        if (h->dwHow == DIPH_BYOFFSET) {
            for (const Slot& s : slots_)
                if (s.kind == K_AXIS && s.ofs == h->dwObj) return 1u << s.idx;
            return 0;
        }
        if (h->dwHow == DIPH_BYID) {
            if ((DIDFT_GETTYPE(h->dwObj) & DIDFT_AXIS) && DIDFT_GETINSTANCE(h->dwObj) < 8)
                return 1u << DIDFT_GETINSTANCE(h->dwObj);
        }
        return 0;
    }

    LONG AxisValue(int axis, const PadState& s) const {
        float v = s.src[GetConfig().axisMap[axis]];
        v = v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
        const float lo = (float)range_[axis][0], hi = (float)range_[axis][1];
        return (LONG)std::lround(lo + (v + 1.f) * 0.5f * (hi - lo));
    }

public:
    VirtualDevice() {
        for (auto& r : range_) { r[0] = 0; r[1] = 65535; }
        DefaultSlots();
        Log("VirtualDevice created (%s)", W ? "W" : "A");
    }

    // --- IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, T::IidDev())) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return (ULONG)r;
    }

    // --- the parts that matter
    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS caps) override {
        if (!caps || caps->dwSize < sizeof(DIDEVCAPS_DX3) || caps->dwSize > sizeof(DIDEVCAPS))
            return DIERR_INVALIDPARAM;
        const DWORD sz = caps->dwSize;
        memset(caps, 0, sz);
        caps->dwSize    = sz;
        caps->dwFlags   = DIDC_ATTACHED;
        caps->dwDevType = kDevType;
        caps->dwAxes    = 6;
        caps->dwButtons = (DWORD)GetConfig().numButtons;
        caps->dwPOVs    = 1;
        if (GetConfig().rumble) {
            caps->dwFlags |= DIDC_FORCEFEEDBACK | DIDC_FFATTACK | DIDC_FFFADE | DIDC_STARTDELAY;
            if (sz >= sizeof(DIDEVCAPS)) { caps->dwFFSamplePeriod = 10000; caps->dwFFMinTimeResolution = 10000; }
        }
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumObjects(typename T::EnumObjCb cb, LPVOID ref, DWORD flags) override {
        if (!cb) return DIERR_INVALIDPARAM;
        Log("EnumObjects flags=0x%lx", flags);
        for (const Obj& o : Objects()) {
            if (flags != DIDFT_ALL &&
                !((flags & o.type & 0xFF) || (flags & o.type & (DIDFT_FFACTUATOR | DIDFT_FFEFFECTTRIGGER)))) continue;
            typename T::ObjInst d;
            FillObj(d, o);
            if (cb(&d, ref) == DIENUM_STOP) break;
        }
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetObjectInfo(typename T::ObjInst* p, DWORD obj, DWORD how) override {
        if (!p || p->dwSize < sizeof(DIDEVICEOBJECTINSTANCE_DX3A)) return DIERR_INVALIDPARAM;
        for (const Obj& o : Objects()) {
            const bool hit = (how == DIPH_BYOFFSET) ? (OfsOf(o.kind, o.idx) == obj)
                           : (how == DIPH_BYID)     ? ((o.type & 0x00FFFFFF) == (obj & 0x00FFFFFF))
                           : false;
            if (!hit) continue;
            typename T::ObjInst d;
            FillObj(d, o);
            const DWORD n = p->dwSize < sizeof(d) ? p->dwSize : (DWORD)sizeof(d);
            const DWORD keep = p->dwSize;
            memcpy(p, &d, n);
            p->dwSize = keep;
            return DI_OK;
        }
        return DIERR_OBJECTNOTFOUND;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceInfo(typename T::DevInst* p) override {
        if (!p || p->dwSize < sizeof(DIDEVICEINSTANCE_DX3A)) return DIERR_INVALIDPARAM;
        typename T::DevInst d;
        FillInstance(d);
        const DWORD keep = p->dwSize;
        memcpy(p, &d, keep < sizeof(d) ? keep : sizeof(d));
        p->dwSize = keep;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT df) override {
        if (!df || !df->rgodf) return DIERR_INVALIDPARAM;
        Log("SetDataFormat dataSize=%lu numObjs=%lu flags=0x%lx", df->dwDataSize, df->dwNumObjs, df->dwFlags);
        slots_.clear();
        int nBtn = 0, nPov = 0, nSlider = 0;
        for (DWORD i = 0; i < df->dwNumObjs; ++i) {
            const DIOBJECTDATAFORMAT& o = df->rgodf[i];
            Slot s{o.dwOfs, K_NONE, 0};
            const GUID* g = o.pguid;
            if      (Eq(g, GUID_XAxis))  { s.kind = K_AXIS; s.idx = 0; }
            else if (Eq(g, GUID_YAxis))  { s.kind = K_AXIS; s.idx = 1; }
            else if (Eq(g, GUID_ZAxis))  { s.kind = K_AXIS; s.idx = 2; }
            else if (Eq(g, GUID_RxAxis)) { s.kind = K_AXIS; s.idx = 3; }
            else if (Eq(g, GUID_RyAxis)) { s.kind = K_AXIS; s.idx = 4; }
            else if (Eq(g, GUID_RzAxis)) { s.kind = K_AXIS; s.idx = 5; }
            else if (Eq(g, GUID_Slider)) { if (nSlider < 2) { s.kind = K_AXIS; s.idx = 6 + nSlider; } ++nSlider; }
            else if (Eq(g, GUID_POV))    { s.kind = K_POV; s.idx = nPov++; }
            else if (Eq(g, GUID_Button) || (!g && (o.dwType & DIDFT_BUTTON))) {
                const DWORD inst = DIDFT_GETINSTANCE(o.dwType);
                s.kind = K_BUTTON;
                s.idx  = (inst != 0xFFFF && !g) ? (int)inst : nBtn;
                ++nBtn;
            }
            slots_.push_back(s);
        }
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD cb, LPVOID data) override {
        if (!data) return E_POINTER;
        PadState s;
        PadPoll(s);
        if (!loggedState_) { loggedState_ = true; Log("First GetDeviceState cb=%lu", cb); }

        memset(data, 0, cb);
        uint8_t* base = static_cast<uint8_t*>(data);
        for (const Slot& sl : slots_) {
            switch (sl.kind) {
            case K_AXIS:
                if (sl.ofs + 4 <= cb) *reinterpret_cast<LONG*>(base + sl.ofs) = AxisValue(sl.idx, s);
                break;
            case K_POV:
                if (sl.ofs + 4 <= cb)
                    *reinterpret_cast<DWORD*>(base + sl.ofs) = (sl.idx == 0) ? (DWORD)s.pov : 0xFFFFFFFFu;
                break;
            case K_BUTTON:
                if (sl.ofs + 1 <= cb) base[sl.ofs] = (sl.idx < 32 && s.btn[sl.idx]) ? 0x80 : 0x00;
                break;
            default: break;
            }
        }
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD cbObj, LPDIDEVICEOBJECTDATA out, LPDWORD n, DWORD flags) override {
        if (!n) return DIERR_INVALIDPARAM;
        if (cbObj < sizeof(DIDEVICEOBJECTDATA_DX3) || cbObj > sizeof(DIDEVICEOBJECTDATA)) return DIERR_INVALIDPARAM;
        Capture();
        if (!out) { events_.clear(); return DI_OK; }   // NULL buffer == flush
        const DWORD cap = *n;
        DWORD cnt = 0;
        uint8_t* dst = reinterpret_cast<uint8_t*>(out);
        while (cnt < cap && cnt < events_.size()) {
            memcpy(dst + (size_t)cnt * cbObj, &events_[cnt], cbObj);
            ++cnt;
        }
        if (!(flags & DIGDD_PEEK)) events_.erase(events_.begin(), events_.begin() + cnt);
        *n = cnt;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID g, LPDIPROPHEADER h) override {
        if (!h || h->dwSize < sizeof(DIPROPHEADER)) return DIERR_INVALIDPARAM;
        if (&g == &DIPROP_RANGE) {
            if (h->dwSize < sizeof(DIPROPRANGE)) return DIERR_INVALIDPARAM;
            const unsigned m = AxesFor(h);
            if (!m) return DIERR_OBJECTNOTFOUND;
            int a = 0; while (!(m & (1u << a))) ++a;
            auto* r = reinterpret_cast<DIPROPRANGE*>(h);
            r->lMin = range_[a][0];
            r->lMax = range_[a][1];
            return DI_OK;
        }
        if (h->dwSize < sizeof(DIPROPDWORD)) return DIERR_INVALIDPARAM;
        auto* d = reinterpret_cast<DIPROPDWORD*>(h);
        if (&g == &DIPROP_FFGAIN)     { d->dwData = FfGetGain(); return DI_OK; }
        if (&g == &DIPROP_AUTOCENTER) { d->dwData = DIPROPAUTOCENTER_OFF; return DI_OK; }
        if (&g == &DIPROP_DEADZONE || &g == &DIPROP_AXISMODE || &g == &DIPROP_BUFFERSIZE ||
            &g == &DIPROP_JOYSTICKID) { d->dwData = 0; return DI_OK; }   // 0 == DIPROPAXISMODE_ABS
        if (&g == &DIPROP_SATURATION)  { d->dwData = 10000; return DI_OK; }
        if (&g == &DIPROP_GRANULARITY) { d->dwData = 1; return DI_OK; }
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID g, LPCDIPROPHEADER h) override {
        if (!h || h->dwSize < sizeof(DIPROPHEADER)) return DIERR_INVALIDPARAM;
        if (&g == &DIPROP_RANGE) {
            if (h->dwSize < sizeof(DIPROPRANGE)) return DIERR_INVALIDPARAM;
            auto* r = reinterpret_cast<const DIPROPRANGE*>(h);
            const unsigned m = AxesFor(h);
            Log("SetProperty RANGE [%ld,%ld] mask=0x%x", r->lMin, r->lMax, m);
            for (int a = 0; a < 8; ++a)
                if (m & (1u << a)) { range_[a][0] = r->lMin; range_[a][1] = r->lMax; }
            return DI_OK;
        }
        if (&g == &DIPROP_FFGAIN && h->dwSize >= sizeof(DIPROPDWORD)) {
            FfSetGain(reinterpret_cast<const DIPROPDWORD*>(h)->dwData);
            return DI_OK;
        }
        if (&g == &DIPROP_BUFFERSIZE && h->dwSize >= sizeof(DIPROPDWORD)) {
            const DWORD n = reinterpret_cast<const DIPROPDWORD*>(h)->dwData;
            bufSize_ = n < 1 ? 1 : (n > 1024 ? 1024 : n);
            Log("SetProperty BUFFERSIZE=%lu", n);
            return DI_OK;
        }
        // Accepted and ignored: we apply our own deadzone, and delivery is always absolute/unbuffered.
        if (&g == &DIPROP_DEADZONE || &g == &DIPROP_SATURATION || &g == &DIPROP_AXISMODE ||
            &g == &DIPROP_BUFFERSIZE || &g == &DIPROP_FFGAIN || &g == &DIPROP_AUTOCENTER)
            return DI_OK;
        Log("SetProperty unsupported prop");
        return DIERR_UNSUPPORTED;
    }

    // --- trivial
    HRESULT STDMETHODCALLTYPE Acquire() override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE Unacquire() override {
        if (GetConfig().rumble) FfCommand(DISFFC_STOPALL);   // effects stop when the device is released
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE Poll() override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE) override { return DI_POLLEDDEVICE; }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND, DWORD) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }

    // --- force feedback / effects / action maps: not supported
    // --- force feedback (rumble)
    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID g, LPCDIEFFECT p, LPDIRECTINPUTEFFECT* out, LPUNKNOWN) override {
        if (!GetConfig().rumble) return DIERR_UNSUPPORTED;
        if (!out) return E_POINTER;
        return FfCreateEffect(g, p, out);
    }
    HRESULT STDMETHODCALLTYPE EnumEffects(typename T::EnumEffCb cb, LPVOID ref, DWORD type) override {
        if (!GetConfig().rumble) return DI_OK;
        if (!cb) return DIERR_INVALIDPARAM;
        Log("EnumEffects type=0x%lx", type);
        for (int i = 0; i < FfEffectCount(); ++i) {
            const GUID& g = FfEffectGuid(i);
            if (type != DIEFT_ALL && DIEFT_GETTYPE(FfEffectType(g)) != DIEFT_GETTYPE(type)) continue;
            std::remove_pointer_t<typename T::EffInfoPtr> e;
            FillEff(e, g);
            if (cb(&e, ref) == DIENUM_STOP) break;
        }
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEffectInfo(typename T::EffInfoPtr info, REFGUID g) override {
        if (!GetConfig().rumble || !FfKnownEffect(g)) return DIERR_DEVICENOTREG;
        if (!info || info->dwSize < sizeof(*info)) return DIERR_INVALIDPARAM;
        FillEff(*info, g);
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD state) override {
        if (!GetConfig().rumble) return DIERR_UNSUPPORTED;
        if (!state) return E_POINTER;
        *state = FfStateFlags();
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD cmd) override {
        if (!GetConfig().rumble) return DIERR_UNSUPPORTED;
        Log("SendForceFeedbackCommand 0x%lx", cmd);
        FfCommand(cmd);
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD, DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(typename T::Str, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(typename T::Str, DWORD, LPDIFILEEFFECT, DWORD) override { return DIERR_UNSUPPORTED; }
    // Assign each of the game's actions to one of our objects. Order of precedence:
    //   1. [Actions] ActionN=... override in the INI (N = index in the game's action array)
    //   2. a mapping the game already holds (DIDBAM_PRESERVE)
    //   3. a plain sequential default by semantic type (axis / button / POV)
    // The sequential default is only a starting point; use the logged semantics to refine it.
    HRESULT STDMETHODCALLTYPE BuildActionMap(typename T::ActFmt af, typename T::Str, DWORD flags) override {
        if (!af || !af->rgoAction) return DIERR_INVALIDPARAM;
        DumpActions("BuildActionMap", af, flags);
        const Config& c = GetConfig();
        static const int axisOrder[6] = {0, 1, 3, 4, 2, 5};   // LX, LY, RX, RY, Z, Rz
        int usedAxis = 0, usedBtn = 0, usedFf = 0;

        for (DWORD i = 0; i < af->dwNumActions; ++i) {
            auto& a = af->rgoAction[i];
            if ((flags & DIDBAM_PRESERVE) && IsEqualGUID(a.guidInstance, kGuidInstance) &&
                (a.dwHow & (DIAH_USERCONFIG | DIAH_APPREQUESTED | DIAH_HWAPP)))
                continue;

            Kind kind = K_NONE; int idx = 0; bool ffObj = false;
            const int ok = i < (DWORD)kMaxActions ? c.actionKind[i] : ACT_DEFAULT;
            if (ok == ACT_BUTTON)      { kind = K_BUTTON; idx = c.actionIdx[i] & 31; }
            else if (ok == ACT_AXIS)   { kind = K_AXIS;   idx = c.actionIdx[i] % 6; }
            else if (ok == ACT_HAT)    { kind = K_POV;    idx = 0; }
            else if (ok == ACT_DEFAULT && (a.dwFlags & DIA_FORCEFEEDBACK)) {
                // force-feedback action: bind to an actuator axis (only if rumble is enabled)
                if (c.rumble) { kind = K_AXIS; idx = usedFf++ % 2; ffObj = true; }
            }
            else if (ok == ACT_DEFAULT && ((a.dwSemantic >> 24) == 0x81 || (a.dwSemantic >> 24) == 0x82)) {
                // 0x81xxxxxx = keyboard-only key semantics, 0x82xxxxxx = mouse-only. A gamepad can't
                // have these; leave them unmapped (otherwise they would steal pad buttons).
            }
            else if (ok == ACT_DEFAULT) {
                switch (SemTypeOf(a.dwSemantic)) {
                    case SEM_AXIS:   if (usedAxis < 6) { kind = K_AXIS; idx = axisOrder[usedAxis++]; } break;
                    case SEM_BUTTON: if (usedBtn < c.numButtons) { kind = K_BUTTON; idx = usedBtn++; } break;
                    case SEM_POV:    kind = K_POV; idx = 0; break;
                    default: break;   // unknown semantic type: leave unmapped
                }
            }
            if (kind == K_NONE) {
                a.guidInstance = GUID_NULL; a.dwObjID = 0; a.dwHow = DIAH_UNMAPPED;
            } else {
                a.guidInstance = kGuidInstance; a.dwObjID = ObjId(kind, idx) | (ffObj ? DIDFT_FFACTUATOR : 0u);
                a.dwHow = DIAH_DEFAULT;
            }
            Log("  action[%lu] -> kind=%d idx=%d obj=0x%08lx", i, (int)kind, idx, a.dwObjID);
        }
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE SetActionMap(typename T::ActFmt af, typename T::Str, DWORD flags) override {
        if (!af || !af->rgoAction) return DIERR_INVALIDPARAM;
        DumpActions("SetActionMap", af, flags);
        acts_.clear();
        events_.clear();
        for (DWORD i = 0; i < af->dwNumActions; ++i) {
            const auto& a = af->rgoAction[i];
            if (a.dwFlags & DIA_FORCEFEEDBACK) continue;   // FF actuators produce no input events
            if (a.dwHow == DIAH_UNMAPPED || !IsEqualGUID(a.guidInstance, kGuidInstance)) continue;
            Kind k; int idx;
            if (!DecodeObj(a.dwObjID, k, idx)) continue;
            acts_.push_back({a.uAppData, k, idx});
        }
        if (af->lAxisMin != af->lAxisMax)
            for (auto& r : range_) { r[0] = af->lAxisMin; r[1] = af->lAxisMax; }
        if (af->dwBufferSize) bufSize_ = af->dwBufferSize > 1024 ? 1024 : af->dwBufferSize;
        havePrev_ = false;
        Log("SetActionMap: %zu actions bound, axisRange=[%ld,%ld], buffer=%lu",
            acts_.size(), range_[0][0], range_[0][1], bufSize_);
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE GetImageInfo(typename T::ImgHdr) override { return DIERR_UNSUPPORTED; }
};

// ------------------------------------------------- IDirectInput8 wrapper ---

template <bool W>
class Proxy final : public Tr<W>::DI {
    using T = Tr<W>;
    typename T::DI* real_;
    LONG            ref_ = 1;

    struct EnumCtx { typename T::EnumDevCb cb; LPVOID ref; bool hide; };

    struct EnumState { EnumCtx ctx; bool stopped; };
    static BOOL FAR PASCAL EnumThunkTrack(const typename T::DevInst* inst, LPVOID p) {
        auto* st = static_cast<EnumState*>(p);
        if (st->ctx.hide && IsGameCtrlType(inst->dwDevType)) return DIENUM_CONTINUE;  // hide real pads
        const BOOL r = st->ctx.cb(inst, st->ctx.ref);
        if (r == DIENUM_STOP) st->stopped = true;
        return r;
    }

public:
    struct SemState { typename T::SemCb cb; LPVOID ref; bool hide; bool stopped; };
    static BOOL FAR PASCAL SemThunk(const typename T::DevInst* inst, typename T::Dev* dev,
                                    DWORD f, DWORD remaining, LPVOID p) {
        auto* st = static_cast<SemState*>(p);
        if (st->hide && IsGameCtrlType(inst->dwDevType)) return DIENUM_CONTINUE;  // hide real pads
        const BOOL r = st->cb(inst, dev, f, remaining, st->ref);
        if (r == DIENUM_STOP) st->stopped = true;
        return r;
    }

    explicit Proxy(typename T::DI* real) : real_(real) { Log("Proxy created (%s)", W ? "W" : "A"); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, T::IidDI())) {
            *ppv = this; AddRef(); return S_OK;
        }
        return real_->QueryInterface(riid, ppv);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) { real_->Release(); delete this; }
        return (ULONG)r;
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID g, typename T::Dev** out, LPUNKNOWN outer) override {
        if (!out) return E_POINTER;
        if (IsEqualGUID(g, kGuidInstance)) {
            *out = new VirtualDevice<W>();
            return DI_OK;
        }
        typename T::Dev* dev = nullptr;
        HRESULT hr = real_->CreateDevice(g, &dev, outer);
        if (SUCCEEDED(hr) && dev && TakeOver()) {
            // The game may remember a real pad's GUID from an old config; redirect it to ours.
            typename T::DevInst info;
            memset(&info, 0, sizeof(info));
            info.dwSize = sizeof(info);
            if (SUCCEEDED(dev->GetDeviceInfo(&info)) && IsGameCtrlType(info.dwDevType)) {
                Log("Redirecting real gamepad CreateDevice to virtual device");
                dev->Release();
                *out = new VirtualDevice<W>();
                return DI_OK;
            }
        }
        *out = dev;
        return hr;
    }

    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD type, typename T::EnumDevCb cb, LPVOID ref, DWORD flags) override {
        if (!cb) return real_->EnumDevices(type, cb, ref, flags);
        const bool wantPad = (type == DI8DEVCLASS_ALL || type == DI8DEVCLASS_GAMECTRL) &&
                             (GetConfig().rumble || !(flags & DIEDFL_FORCEFEEDBACK));
        const bool expose  = wantPad && (PadConnected() || GetConfig().alwaysExpose);

        EnumState st{{cb, ref, expose && GetConfig().hideReal}, false};
        HRESULT hr = real_->EnumDevices(type, &EnumThunkTrack, &st, flags);
        if (SUCCEEDED(hr) && expose && !st.stopped) {
            typename T::DevInst inst;
            FillInstance(inst);
            Log("EnumDevices: reporting virtual gamepad");
            cb(&inst, ref);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID g) override {
        if (IsEqualGUID(g, kGuidInstance)) return PadConnected() || GetConfig().alwaysExpose ? DI_OK : DI_NOTATTACHED;
        return real_->GetDeviceStatus(g);
    }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND h, DWORD f) override { return real_->RunControlPanel(h, f); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE h, DWORD v) override { return real_->Initialize(h, v); }
    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID c, typename T::Str n, LPGUID g) override { return real_->FindDevice(c, n, g); }
    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(typename T::Str user, typename T::ActFmt af,
                                                     typename T::SemCb cb, LPVOID ref, DWORD flags) override {
        DumpActions("EnumDevicesBySemantics", af, flags);
        if (!cb) return real_->EnumDevicesBySemantics(user, af, cb, ref, flags);

        const bool expose = (GetConfig().rumble || !(flags & DIEDBSFL_FORCEFEEDBACK)) &&
                            (PadConnected() || GetConfig().alwaysExpose);
        SemState st{cb, ref, expose && GetConfig().hideReal, false};
        HRESULT hr = real_->EnumDevicesBySemantics(user, af, &SemThunk, &st, flags);
        if (expose && !st.stopped) {
            typename T::DevInst inst;
            FillInstance(inst);
            auto* dev = new VirtualDevice<W>();
            Log("EnumDevicesBySemantics: reporting virtual gamepad (real hr=0x%08lx)", (unsigned long)hr);
            cb(&inst, dev, 0, 0, ref);
            dev->Release();   // the game AddRefs it if it wants to keep it
            hr = DI_OK;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK cb, typename T::CfgParams p,
                                               DWORD f, LPVOID r) override {
        Log("ConfigureDevices called (virtual device will not appear in that dialog)");
        return real_->ConfigureDevices(cb, p, f, r);
    }
};

// ------ exports -----

using PFN_Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

static PFN_Create LoadReal() {
    static PFN_Create fn = []() -> PFN_Create {
        wchar_t p[MAX_PATH] = {};
        GetSystemDirectoryW(p, MAX_PATH);   // SysWOW64 for a 32-bit process on 64-bit Windows
        wcscat_s(p, L"\\dinput8.dll");
        HMODULE m = LoadLibraryW(p);
        return m ? reinterpret_cast<PFN_Create>(GetProcAddress(m, "DirectInput8Create")) : nullptr;
    }();
    return fn;
}

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    PFN_Create real = LoadReal();
    if (!real) return DIERR_GENERIC;
    Log("DirectInput8Create ver=0x%lx", ver);
    HRESULT hr = real(hinst, ver, riid, out, outer);
    if (FAILED(hr) || !out || !*out) return hr;

    if (IsEqualIID(riid, IID_IDirectInput8A))
        *out = new Proxy<false>(static_cast<IDirectInput8A*>(*out));
    else if (IsEqualIID(riid, IID_IDirectInput8W))
        *out = new Proxy<true>(static_cast<IDirectInput8W*>(*out));
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        oni3::SetModule(h);
    }
    return TRUE;
}
