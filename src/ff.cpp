// ff.cpp - DirectInput effects -> SDL rumble.
//
// Each IDirectInputEffect is a small object holding its parameters. A background thread
// (started on first Start()) evaluates every playing effect ~40x/second, sums their
// magnitudes, and drives the controller's two rumble motors:
//   strong (low-frequency) motor: sum of all effect magnitudes
//   weak   (high-frequency) motor: fast periodic effects at full level, everything else at half
// Direction and axes are ignored (a pad has no directional actuators).

#include "ff.h"
#include "pad.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace oni3 {
namespace {

enum Kind { K_CONST, K_RAMP, K_SQUARE, K_SINE, K_TRI, K_SAWUP, K_SAWDOWN, K_INERT };

struct EffDef { const GUID* guid; Kind kind; DWORD type; const char* name; };

const EffDef kDefs[] = {
    {&GUID_ConstantForce, K_CONST,   DIEFT_CONSTANTFORCE, "Constant Force"},
    {&GUID_RampForce,     K_RAMP,    DIEFT_RAMPFORCE,     "Ramp Force"},
    {&GUID_Square,        K_SQUARE,  DIEFT_PERIODIC,      "Square"},
    {&GUID_Sine,          K_SINE,    DIEFT_PERIODIC,      "Sine"},
    {&GUID_Triangle,      K_TRI,     DIEFT_PERIODIC,      "Triangle"},
    {&GUID_SawtoothUp,    K_SAWUP,   DIEFT_PERIODIC,      "Sawtooth Up"},
    {&GUID_SawtoothDown,  K_SAWDOWN, DIEFT_PERIODIC,      "Sawtooth Down"},
    // Condition effects are accepted (so a game that creates a centring spring doesn't
    // conclude FF is broken) but produce no output on a rumble pad.
    {&GUID_Spring,        K_INERT,   DIEFT_CONDITION,     "Spring"},
    {&GUID_Damper,        K_INERT,   DIEFT_CONDITION,     "Damper"},
    {&GUID_Inertia,       K_INERT,   DIEFT_CONDITION,     "Inertia"},
    {&GUID_Friction,      K_INERT,   DIEFT_CONDITION,     "Friction"},
};
constexpr int kDefCount = (int)(sizeof(kDefs) / sizeof(kDefs[0]));

const EffDef* FindDef(const GUID& g) {
    for (const EffDef& d : kDefs)
        if (IsEqualGUID(*d.guid, g)) return &d;
    return nullptr;
}

double NowUs() {
    static const LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)freq.QuadPart;
}

class Effect;

struct Engine {
    std::mutex              m;
    std::condition_variable cv;
    std::vector<Effect*>    live;
    DWORD                   gain = 10000;
    bool                    paused = false;
    bool                    actuators = true;
    double                  pausedAt = 0;
    bool                    threadStarted = false;
};

// Leaked on purpose: the worker thread must never see a destroyed engine at process exit.
Engine& E() { static Engine* e = new Engine; return *e; }

DWORD WINAPI Loop(LPVOID);

class Effect final : public IDirectInputEffect {
public:
    explicit Effect(const EffDef* d) : def(d) {}

    const EffDef*        def;
    LONG                 ref = 1;
    // parameters (guarded by Engine::m)
    DWORD                duration   = INFINITE;   // microseconds
    DWORD                startDelay = 0;          // microseconds
    DWORD                gain       = 10000;
    bool                 hasEnv     = false;
    DIENVELOPE           env        = {};
    std::vector<uint8_t> tsp;                     // type-specific parameters
    // playback (guarded by Engine::m)
    bool                 playing    = false;
    double               start      = 0;
    DWORD                iterations = 1;

    // ---- must be called with Engine::m held
    void Apply(const DIEFFECT* p, DWORD flags) {
        if (flags & DIEP_DURATION)  duration = p->dwDuration;
        if (flags & DIEP_GAIN)      gain = p->dwGain > 10000 ? 10000 : p->dwGain;
        if ((flags & DIEP_STARTDELAY) && p->dwSize >= sizeof(DIEFFECT)) startDelay = p->dwStartDelay;
        if (flags & DIEP_ENVELOPE) {
            if (p->lpEnvelope && p->lpEnvelope->dwSize >= sizeof(DIENVELOPE)) { hasEnv = true; env = *p->lpEnvelope; }
            else hasEnv = false;
        }
        if (flags & DIEP_TYPESPECIFICPARAMS) {
            if (p->lpvTypeSpecificParams && p->cbTypeSpecificParams) {
                const uint8_t* b = static_cast<const uint8_t*>(p->lpvTypeSpecificParams);
                tsp.assign(b, b + p->cbTypeSpecificParams);
            } else {
                tsp.clear();
            }
        }
    }

    // Current output 0..1 (before device gain); returns false once the effect has finished.
    bool Eval(double now, float& mag, bool& high) const {
        mag = 0.f; high = false;
        const double t = now - start - (double)startDelay;
        if (t < 0) return true;                           // still in start delay

        const bool infDur = (duration == INFINITE);
        const bool forever = infDur || iterations == INFINITE;
        const double dur = infDur ? 0.0 : (double)(duration ? duration : 1);
        if (!forever && t >= dur * (double)iterations) return false;
        const double tt = infDur ? t : std::fmod(t, dur);  // time inside the current iteration

        double v = 0.0;                                    // -10000..10000
        switch (def->kind) {
        case K_CONST: {
            DICONSTANTFORCE cf;
            if (tsp.size() >= sizeof(cf)) { memcpy(&cf, tsp.data(), sizeof(cf)); v = cf.lMagnitude; }
            break;
        }
        case K_RAMP: {
            DIRAMPFORCE r;
            if (tsp.size() >= sizeof(r)) {
                memcpy(&r, tsp.data(), sizeof(r));
                const double f = infDur ? 0.0 : tt / dur;
                v = r.lStart + (r.lEnd - r.lStart) * f;
            }
            break;
        }
        case K_SQUARE: case K_SINE: case K_TRI: case K_SAWUP: case K_SAWDOWN: {
            DIPERIODIC p;
            if (tsp.size() < sizeof(p)) break;
            memcpy(&p, tsp.data(), sizeof(p));
            const double period = p.dwPeriod ? (double)p.dwPeriod : 1.0;   // microseconds
            double ph = std::fmod(tt / period + (double)p.dwPhase / 36000.0, 1.0);
            double w = 0.0;
            switch (def->kind) {
                case K_SQUARE:  w = ph < 0.5 ? 1.0 : -1.0; break;
                case K_SINE:    w = std::sin(2.0 * 3.14159265358979323846 * ph); break;
                case K_TRI:     w = ph < 0.25 ? 4 * ph : (ph < 0.75 ? 2 - 4 * ph : 4 * ph - 4); break;
                case K_SAWUP:   w = 2 * ph - 1; break;
                default:        w = 1 - 2 * ph; break;  // K_SAWDOWN
            }
            v = p.lOffset + (double)p.dwMagnitude * w;
            high = period < 40000.0;                        // faster than ~25 Hz feels "buzzy"
            break;
        }
        default:
            return true;                                    // inert (condition) effects
        }

        double m = std::fabs(v);
        if (hasEnv) {
            if (env.dwAttackTime && tt < (double)env.dwAttackTime)
                m = env.dwAttackLevel + (m - env.dwAttackLevel) * (tt / (double)env.dwAttackTime);
            if (!infDur && env.dwFadeTime && tt > dur - (double)env.dwFadeTime)
                m = m + (env.dwFadeLevel - m) * ((tt - (dur - (double)env.dwFadeTime)) / (double)env.dwFadeTime);
        }
        m = std::min(10000.0, std::max(0.0, m));
        mag = (float)(m / 10000.0 * (double)gain / 10000.0);
        return true;
    }

    // ---- IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, IID_IDirectInputEffect)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&ref);
        if (r == 0) {
            {
                Engine& e = E();
                std::lock_guard<std::mutex> lk(e.m);
                e.live.erase(std::remove(e.live.begin(), e.live.end(), this), e.live.end());
                e.cv.notify_all();
            }
            delete this;
        }
        return (ULONG)r;
    }

    // ---- IDirectInputEffect
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }

    HRESULT STDMETHODCALLTYPE GetEffectGuid(LPGUID g) override {
        if (!g) return E_POINTER;
        *g = *def->guid;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetParameters(LPDIEFFECT p, DWORD flags) override {
        if (!p || p->dwSize < sizeof(DIEFFECT_DX5)) return DIERR_INVALIDPARAM;
        Engine& e = E();
        std::lock_guard<std::mutex> lk(e.m);
        if (flags & DIEP_DURATION)  p->dwDuration = duration;
        if (flags & DIEP_GAIN)      p->dwGain = gain;
        if ((flags & DIEP_STARTDELAY) && p->dwSize >= sizeof(DIEFFECT)) p->dwStartDelay = startDelay;
        if (flags & DIEP_TRIGGERBUTTON)         p->dwTriggerButton = DIEB_NOTRIGGER;
        if (flags & DIEP_TRIGGERREPEATINTERVAL) p->dwTriggerRepeatInterval = 0;
        if (flags & DIEP_SAMPLEPERIOD)          p->dwSamplePeriod = 0;
        if ((flags & DIEP_ENVELOPE) && p->lpEnvelope && hasEnv) {
            const DWORD keep = p->lpEnvelope->dwSize;
            *p->lpEnvelope = env;
            p->lpEnvelope->dwSize = keep;
        }
        if (flags & DIEP_TYPESPECIFICPARAMS) {
            const DWORD need = (DWORD)tsp.size();
            const DWORD have = p->cbTypeSpecificParams;
            p->cbTypeSpecificParams = need;
            if (p->lpvTypeSpecificParams && have >= need) memcpy(p->lpvTypeSpecificParams, tsp.data(), need);
            else if (need) return DIERR_MOREDATA;
        }
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE SetParameters(LPCDIEFFECT p, DWORD flags) override {
        if (!p || p->dwSize < sizeof(DIEFFECT_DX5)) return DIERR_INVALIDPARAM;
        Engine& e = E();
        std::lock_guard<std::mutex> lk(e.m);
        Apply(p, flags);
        if (playing && !(flags & DIEP_NORESTART)) start = NowUs();     // restart with new params
        if (flags & DIEP_START) StartLocked(1);
        Log("Effect %s SetParameters flags=0x%lx duration=%lu gain=%lu tsp=%zu",
            def->name, flags, duration, gain, tsp.size());
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE Start(DWORD iters, DWORD flags) override {
        Engine& e = E();
        std::lock_guard<std::mutex> lk(e.m);
        if (flags & DIES_SOLO)
            for (Effect* f : e.live) if (f != this) f->playing = false;
        StartLocked(iters);
        Log("Effect %s Start iterations=%lu flags=0x%lx", def->name, iters, flags);
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE Stop() override {
        Engine& e = E();
        std::lock_guard<std::mutex> lk(e.m);
        playing = false;
        e.cv.notify_all();
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetEffectStatus(LPDWORD s) override {
        if (!s) return E_POINTER;
        Engine& e = E();
        std::lock_guard<std::mutex> lk(e.m);
        *s = playing ? DIEGES_PLAYING : 0;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE Download() override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE Unload() override   { return DI_OK; }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }

    void StartLocked(DWORD iters) {   // Engine::m held
        Engine& e = E();
        playing = true;
        start = NowUs();
        iterations = iters ? iters : 1;
        if (!e.threadStarted) {
            e.threadStarted = true;
            if (HANDLE h = CreateThread(nullptr, 0, Loop, nullptr, 0, nullptr)) CloseHandle(h);
        }
        e.cv.notify_all();
    }
};

DWORD WINAPI Loop(LPVOID) {
    Engine& e = E();
    bool rumbling = false;
    for (;;) {
        float strong = 0.f, weak = 0.f;
        {
            std::unique_lock<std::mutex> lk(e.m);
            bool any = false;
            const double now = NowUs();
            for (Effect* f : e.live) {
                if (!f->playing) continue;
                if (e.paused) { any = true; continue; }   // frozen while paused
                float m; bool high;
                if (!f->Eval(now, m, high)) { f->playing = false; continue; }
                any = true;
                strong += m;
                weak   += high ? m : m * 0.5f;
            }
            if (!any && !rumbling) { e.cv.wait(lk); continue; }   // idle until something starts

            const float k = (e.actuators && !e.paused)
                          ? (float)e.gain / 10000.f * GetConfig().rumbleStrength : 0.f;
            strong = std::min(1.f, strong * k);
            weak   = std::min(1.f, weak * k);
        }
        PadRumble(strong, weak);
        rumbling = (strong > 0.f || weak > 0.f);
        Sleep(25);
    }
}

} // namespace

// ------------------------------------------------------------ public API ---

int FfEffectCount() { return kDefCount; }
const GUID& FfEffectGuid(int i) { return *kDefs[i < 0 || i >= kDefCount ? 0 : i].guid; }
bool FfKnownEffect(const GUID& g) { return FindDef(g) != nullptr; }

DWORD FfEffectType(const GUID& g) {
    const EffDef* d = FindDef(g);
    if (!d) return 0;
    if (d->type == DIEFT_CONDITION) return d->type;
    return d->type | DIEFT_FFATTACK | DIEFT_FFFADE | DIEFT_STARTDELAY;
}

const char* FfEffectName(const GUID& g) {
    const EffDef* d = FindDef(g);
    return d ? d->name : "Unknown";
}

HRESULT FfCreateEffect(const GUID& g, LPCDIEFFECT p, LPDIRECTINPUTEFFECT* out) {
    const EffDef* d = FindDef(g);
    if (!d) { Log("CreateEffect: unsupported effect GUID"); return DIERR_DEVICENOTREG; }
    if (p && p->dwSize < sizeof(DIEFFECT_DX5)) return DIERR_INVALIDPARAM;

    Effect* eff = new Effect(d);
    Engine& e = E();
    {
        std::lock_guard<std::mutex> lk(e.m);
        if (p) eff->Apply(p, DIEP_DURATION | DIEP_GAIN | DIEP_STARTDELAY | DIEP_ENVELOPE | DIEP_TYPESPECIFICPARAMS);
        e.live.push_back(eff);
    }
    Log("CreateEffect %s duration=%lu gain=%lu tsp=%zu axes=%lu trigger=0x%lx",
        d->name, eff->duration, eff->gain, eff->tsp.size(),
        p ? p->cAxes : 0, p ? p->dwTriggerButton : 0);
    *out = eff;
    return DI_OK;
}

void FfSetGain(DWORD gain) {
    Engine& e = E();
    std::lock_guard<std::mutex> lk(e.m);
    e.gain = gain > 10000 ? 10000 : gain;
}

DWORD FfGetGain() {
    Engine& e = E();
    std::lock_guard<std::mutex> lk(e.m);
    return e.gain;
}

void FfCommand(DWORD cmd) {
    Engine& e = E();
    std::lock_guard<std::mutex> lk(e.m);
    if (cmd & (DISFFC_RESET | DISFFC_STOPALL))
        for (Effect* f : e.live) f->playing = false;
    if ((cmd & DISFFC_PAUSE) && !e.paused) { e.paused = true; e.pausedAt = NowUs(); }
    if ((cmd & DISFFC_CONTINUE) && e.paused) {
        const double d = NowUs() - e.pausedAt;
        for (Effect* f : e.live) f->start += d;          // don't let effects expire while paused
        e.paused = false;
    }
    if (cmd & DISFFC_SETACTUATORSON)  e.actuators = true;
    if (cmd & DISFFC_SETACTUATORSOFF) e.actuators = false;
    e.cv.notify_all();
}

DWORD FfStateFlags() {
    Engine& e = E();
    std::lock_guard<std::mutex> lk(e.m);
    bool anyPlaying = false;
    for (Effect* f : e.live) anyPlaying |= f->playing;
    DWORD s = DIGFFS_POWERON;
    s |= e.actuators ? DIGFFS_ACTUATORSON : DIGFFS_ACTUATORSOFF;
    if (e.paused)        s |= DIGFFS_PAUSED;
    if (e.live.empty())  s |= DIGFFS_EMPTY;
    if (!anyPlaying)     s |= DIGFFS_STOPPED;
    return s;
}

} // namespace oni3
