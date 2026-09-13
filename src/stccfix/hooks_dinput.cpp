// DirectInput (DX5, DIRECTINPUT_VERSION 0x0500) の呼び出しログ。Phase 4 の調査用。
//
// ゲームの流れ（jp-1.02）: EnumDevices(JOYSTICK) → CreateDevice → QI(IDirectInputDevice2A)
//   → SetDataFormat(DIJOYSTATE) → SetCooperativeLevel(EXCLUSIVE|FOREGROUND) → GetCapabilities
//   → SetProperty(DIPROP_RANGE: X 48..208, Y -160..160, Z 48..208) → FFB エフェクト作成 → Acquire
//   毎フレーム: Poll → GetDeviceState
// GetDeviceState は値が変化したときだけ、間引いて記録する。
#include "common.h"
#include "vtable_hook.h"

#define DIRECTINPUT_VERSION 0x0500
#include <dinput.h>

#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace stcc {
namespace {

// ---------------------------------------------------------------- 表示用ヘルパ
std::string GuidStr(const GUID* g) {
    if (!g) {
        return "null";
    }
    char b[48];
    std::snprintf(b, sizeof(b), "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g->Data1, g->Data2, g->Data3,
                  g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    return b;
}

const char* PropName(const GUID* prop) {
    // 定義済みプロパティは GUID ポインタに小さな整数が入る（MAKEDIPROP）
    switch (reinterpret_cast<std::uintptr_t>(prop)) {
        case 1: return "BUFFERSIZE";
        case 2: return "AXISMODE";
        case 3: return "GRANULARITY";
        case 4: return "RANGE";
        case 5: return "DEADZONE";
        case 6: return "SATURATION";
        case 7: return "FFGAIN";
        case 8: return "FFLOAD";
        case 9: return "AUTOCENTER";
        case 10: return "CALIBRATIONMODE";
        default: return "?";
    }
}

const char* HowName(DWORD how) {
    switch (how) {
        case DIPH_DEVICE: return "DEVICE";
        case DIPH_BYOFFSET: return "BYOFFSET";
        case DIPH_BYID: return "BYID";
        default: return "?";
    }
}

// DIJOYSTATE のオフセットを軸名に（DIJOFS_* マクロは定数式で C4644 になるので標準の offsetof を使う）
const char* JoyOffsetName(DWORD ofs) {
    struct Entry {
        std::size_t ofs;
        const char* name;
    };
    static constexpr Entry kNames[] = {
        {offsetof(DIJOYSTATE, lX), "X"},
        {offsetof(DIJOYSTATE, lY), "Y"},
        {offsetof(DIJOYSTATE, lZ), "Z"},
        {offsetof(DIJOYSTATE, lRx), "Rx"},
        {offsetof(DIJOYSTATE, lRy), "Ry"},
        {offsetof(DIJOYSTATE, lRz), "Rz"},
        {offsetof(DIJOYSTATE, rglSlider), "Slider0"},
        {offsetof(DIJOYSTATE, rglSlider) + sizeof(LONG), "Slider1"},
        {offsetof(DIJOYSTATE, rgdwPOV), "POV0"},
    };
    for (const auto& e : kNames) {
        if (e.ofs == ofs) {
            return e.name;
        }
    }
    return "";
}

// ---------------------------------------------------------------- IDirectInputDevice2A
constexpr int kAxisCount = 8;  // X Y Z Rx Ry Rz Slider0 Slider1（DIJOYSTATE 先頭から 4 バイトずつ）

struct DeviceState {
    IDirectInputDevice2A* dev;
    // ゲームが DIPROP_RANGE で設定した軸ごとのレンジ（未設定なら DirectInput 既定の 0..65535）
    LONG rangeMin[kAxisCount];
    LONG rangeMax[kAxisCount];
    DIJOYSTATE last;
    bool hasLast;
    bool isWheel;  // GetCapabilities の本来のサブタイプが WHEEL（ペダルは軸が別で、Y を素通しすると離したペダルの生値になる）
    DWORD lastLogTick;
    LONG polls;
};
DeviceState g_devices[4];
SRWLOCK g_devLock = SRWLOCK_INIT;

DeviceState* DeviceSlot(IDirectInputDevice2A* dev) {
    AcquireSRWLockExclusive(&g_devLock);
    DeviceState* found = nullptr;
    for (auto& d : g_devices) {
        if (d.dev == dev) {
            found = &d;
            break;
        }
    }
    if (!found) {
        for (auto& d : g_devices) {
            if (!d.dev) {
                d = {};
                d.dev = dev;
                for (int i = 0; i < kAxisCount; ++i) {
                    d.rangeMin[i] = 0;
                    d.rangeMax[i] = 65535;
                }
                found = &d;
                break;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_devLock);
    return found;
}

LONG& AxisRef(DIJOYSTATE& js, int axis) {
    return axis < 6 ? *(&js.lX + axis) : js.rglSlider[axis - 6];
}

// 軸値を 0..1 に正規化（invert で反転）
double Normalize01(const DeviceState& s, int axis, LONG v, bool invert) {
    const double span = static_cast<double>(s.rangeMax[axis]) - s.rangeMin[axis];
    double t = span > 0 ? (static_cast<double>(v) - s.rangeMin[axis]) / span : 0.0;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return invert ? 1.0 - t : t;
}

// ゲームに渡す直前の DIJOYSTATE を設定に従って書き換える
void RemapState(const DeviceState& s, DIJOYSTATE& js) {
    const Config& cfg = GetConfig();

    if (cfg.triggerPedals) {
        const double dz = cfg.pedalDeadzone / 100.0;
        auto pedal = [&](int axis, bool invert) {
            double t = Normalize01(s, axis, AxisRef(js, axis), invert);
            return t < dz ? 0.0 : (t - dz) / (1.0 - dz);
        };
        const double accel = pedal(cfg.accelAxis, cfg.accelInvert);
        const double brake = pedal(cfg.brakeAxis, cfg.brakeInvert);
        // 両方離していればスティックの Y を素通しする。ただしホイールや Y 自体がペダルの設定では、Y は
        // ペダル（Fanatec はクラッチ）なので素通しすると離したペダルの生値が渡る → 常に合成値で上書きする
        const bool noStickY = s.isWheel || cfg.accelAxis == 1 || cfg.brakeAxis == 1;
        if (noStickY || accel > 0 || brake > 0) {
            // Y は「最小値側 = スティック上 = アクセル」
            const double center = (static_cast<double>(s.rangeMin[1]) + s.rangeMax[1]) / 2;
            const double half = (static_cast<double>(s.rangeMax[1]) - s.rangeMin[1]) / 2;
            js.lY = static_cast<LONG>(std::lround(center + (brake - accel) * half));
        }
    }

    if (cfg.steerDeadzone > 0 || cfg.steerLinearity != 100) {
        const double center = (static_cast<double>(s.rangeMin[0]) + s.rangeMax[0]) / 2;
        const double half = (static_cast<double>(s.rangeMax[0]) - s.rangeMin[0]) / 2;
        if (half > 0) {
            double x = (js.lX - center) / half;
            const double sign = x < 0 ? -1.0 : 1.0;
            double mag = std::fabs(x);
            mag = mag > 1 ? 1 : mag;
            const double dz = cfg.steerDeadzone / 100.0;
            mag = mag < dz ? 0.0 : (mag - dz) / (1.0 - dz);
            mag = std::pow(mag, cfg.steerLinearity / 100.0);
            js.lX = static_cast<LONG>(std::lround(center + sign * mag * half));
        }
    }
}

BOOL CALLBACK EnumObjectsLog(LPCDIDEVICEOBJECTINSTANCEA o, LPVOID) {
    Log("    object ofs=0x%02lX(%s) type=0x%08lX flags=0x%lX name=\"%s\" guid=%s", o->dwOfs, JoyOffsetName(o->dwOfs),
        o->dwType, o->dwFlags, o->tszName, GuidStr(&o->guidType).c_str());
    return DIENUM_CONTINUE;
}

HRESULT STDMETHODCALLTYPE Dev_GetCapabilities(IDirectInputDevice2A* self, LPDIDEVCAPS caps) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice2A*, LPDIDEVCAPS)>(self, 3);
    HRESULT hr = fn(self, caps);
    if (SUCCEEDED(hr) && caps) {
        if (DeviceState* s = DeviceSlot(self)) {
            s->isWheel = GET_DIDEVICE_TYPE(caps->dwDevType) == DIDEVTYPE_JOYSTICK &&
                         GET_DIDEVICE_SUBTYPE(caps->dwDevType) == DIDEVTYPEJOYSTICK_WHEEL;
        }
    }
    // ゲーム (Input_SetupDevice 0x4688DE) はこのフラグを見て AUTOCENTER 設定と ConstantForce 作成を行う
    if (SUCCEEDED(hr) && caps && GetConfig().ffbMode == FfbMode::Off && (caps->dwFlags & DIDC_FORCEFEEDBACK)) {
        caps->dwFlags &= ~DIDC_FORCEFEEDBACK;
        Log("%p->GetCapabilities: DIDC_FORCEFEEDBACK hidden ([ForceFeedback] Mode=off)", static_cast<void*>(self));
    }
    if (caps) {
        Log("%p->GetCapabilities -> 0x%08lX flags=0x%lX devType=0x%lX axes=%lu buttons=%lu povs=%lu ffPeriod=%lu",
            static_cast<void*>(self), static_cast<unsigned long>(hr), caps->dwFlags, caps->dwDevType, caps->dwAxes,
            caps->dwButtons, caps->dwPOVs, caps->dwFFSamplePeriod);
    }
    // 調査用に、ゲームは呼ばない EnumObjects で全オブジェクトを列挙しておく（初回のみ）
    static LONG once = 0;
    if (InterlockedExchange(&once, 1) == 0) {
        self->EnumObjects(EnumObjectsLog, nullptr, DIDFT_ALL);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Dev_SetProperty(IDirectInputDevice2A* self, const GUID* prop, LPCDIPROPHEADER ph) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice2A*, const GUID*, LPCDIPROPHEADER)>(self, 6);
    const auto id = reinterpret_cast<std::uintptr_t>(prop);
    // [ForceFeedback] Mode=game: ゲームは FFB 機種に AUTOCENTER=1 を設定する。DD ベースの内蔵センタリングは強すぎるので切る
    DIPROPDWORD autoCenterOff;
    const Config& cfg = GetConfig();
    if (id == 9 && ph && ph->dwSize >= sizeof(DIPROPDWORD) && cfg.ffbMode == FfbMode::Game && !cfg.ffbAutoCenter) {
        std::memcpy(&autoCenterOff, ph, sizeof(autoCenterOff));
        autoCenterOff.dwData = DIPROPAUTOCENTER_OFF;
        ph = &autoCenterOff.diph;
    }
    HRESULT hr = fn(self, prop, ph);
    if (ph && id == 4 && ph->dwSize >= sizeof(DIPROPRANGE)) {
        auto r = reinterpret_cast<const DIPROPRANGE*>(ph);
        // 軸加工で使うため、ゲームが設定したレンジを覚えておく
        if (SUCCEEDED(hr)) {
            if (DeviceState* s = DeviceSlot(self)) {
                if (ph->dwHow == DIPH_DEVICE) {
                    for (int i = 0; i < kAxisCount; ++i) {
                        s->rangeMin[i] = r->lMin;
                        s->rangeMax[i] = r->lMax;
                    }
                } else if (ph->dwHow == DIPH_BYOFFSET && ph->dwObj % 4 == 0 && ph->dwObj / 4 < kAxisCount) {
                    s->rangeMin[ph->dwObj / 4] = r->lMin;
                    s->rangeMax[ph->dwObj / 4] = r->lMax;
                }
            }
        }
        Log("%p->SetProperty(RANGE %s obj=0x%02lX(%s) min=%ld max=%ld) -> 0x%08lX", static_cast<void*>(self),
            HowName(ph->dwHow), ph->dwObj, JoyOffsetName(ph->dwObj), r->lMin, r->lMax, static_cast<unsigned long>(hr));
    } else if (ph && id < 0x10000 && ph->dwSize >= sizeof(DIPROPDWORD)) {
        auto d = reinterpret_cast<const DIPROPDWORD*>(ph);
        Log("%p->SetProperty(%s %s obj=0x%02lX(%s) value=%lu) -> 0x%08lX", static_cast<void*>(self), PropName(prop),
            HowName(ph->dwHow), ph->dwObj, JoyOffsetName(ph->dwObj), d->dwData, static_cast<unsigned long>(hr));
    } else {
        Log("%p->SetProperty(%s) -> 0x%08lX", static_cast<void*>(self), PropName(prop), static_cast<unsigned long>(hr));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Dev_Acquire(IDirectInputDevice2A* self) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice2A*)>(self, 7);
    HRESULT hr = fn(self);
    static LONG calls = 0;
    static LONG failures = 0;
    LONG n = InterlockedIncrement(&calls);
    // 切断中はゲームが毎フレーム Acquire を再試行するので、失敗は最初の 3 回と以降 300 回ごとだけ記録する
    LONG f = FAILED(hr) ? InterlockedIncrement(&failures) : 0;
    if (n <= 5 || (f > 0 && (f <= 3 || f % 300 == 0))) {
        Log("%p->Acquire #%ld -> 0x%08lX%s", static_cast<void*>(self), n, static_cast<unsigned long>(hr),
            f > 0 ? (f == 3 ? "（以降の失敗は 300 回ごと）" : "") : "");
    }
    return hr;
}

void FfbTick(IDirectInputDevice2A* dev);

HRESULT STDMETHODCALLTYPE Dev_GetDeviceState(IDirectInputDevice2A* self, DWORD cb, LPVOID data) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice2A*, DWORD, LPVOID)>(self, 9);
    HRESULT hr = fn(self, cb, data);
    FfbTick(self);  // ゲームは毎フレーム読むので、FFB の出力更新もここで行う
    DeviceState* s = DeviceSlot(self);
    if (!s) {
        return hr;
    }
    ++s->polls;
    if (FAILED(hr)) {
        if (s->polls <= 5 || (s->polls % 600) == 0) {
            Log("%p->GetDeviceState #%ld -> 0x%08lX", static_cast<void*>(self), s->polls, static_cast<unsigned long>(hr));
        }
        return hr;
    }
    if (cb != sizeof(DIJOYSTATE) || !data) {
        return hr;
    }
    RemapState(*s, *static_cast<DIJOYSTATE*>(data));
    if (!GetConfig().logInput) {
        return hr;
    }
    // 以下はログ（ゲームに渡す加工後の値を記録する）
    const auto* js = static_cast<const DIJOYSTATE*>(data);
    auto moved = [](LONG a, LONG b) { return std::labs(a - b) > 6; };
    bool changed = !s->hasLast || moved(js->lX, s->last.lX) || moved(js->lY, s->last.lY) || moved(js->lZ, s->last.lZ) ||
                   moved(js->lRx, s->last.lRx) || moved(js->lRy, s->last.lRy) || moved(js->lRz, s->last.lRz) ||
                   moved(js->rglSlider[0], s->last.rglSlider[0]) || moved(js->rglSlider[1], s->last.rglSlider[1]) ||
                   js->rgdwPOV[0] != s->last.rgdwPOV[0] ||
                   std::memcmp(js->rgbButtons, s->last.rgbButtons, sizeof(js->rgbButtons)) != 0;
    DWORD now = GetTickCount();
    // 変化があっても 50ms に 1 回まで
    if (changed && now - s->lastLogTick >= 50) {
        DWORD buttons = 0;
        for (int i = 0; i < 32; ++i) {
            if (js->rgbButtons[i] & 0x80) {
                buttons |= 1UL << i;
            }
        }
        Log("%p state X=%ld Y=%ld Z=%ld Rx=%ld Ry=%ld Rz=%ld S0=%ld S1=%ld POV=%ld btn=0x%08lX", static_cast<void*>(self),
            js->lX, js->lY, js->lZ, js->lRx, js->lRy, js->lRz, js->rglSlider[0], js->rglSlider[1],
            static_cast<long>(js->rgdwPOV[0]), buttons);
        s->last = *js;
        s->hasLast = true;
        s->lastLogTick = now;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Dev_SetDataFormat(IDirectInputDevice2A* self, LPCDIDATAFORMAT fmt) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice2A*, LPCDIDATAFORMAT)>(self, 11);
    HRESULT hr = fn(self, fmt);
    // dataSize=0x50 objs=44 なら DIJoystick (DIJOYSTATE)
    Log("%p->SetDataFormat(dataSize=0x%lX objs=%lu) -> 0x%08lX", static_cast<void*>(self), fmt ? fmt->dwDataSize : 0,
        fmt ? fmt->dwNumObjs : 0, static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE Dev_SetCooperativeLevel(IDirectInputDevice2A* self, HWND hwnd, DWORD flags) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice2A*, HWND, DWORD)>(self, 13);
    HRESULT hr = fn(self, hwnd, flags);
    Log("%p->SetCooperativeLevel(hwnd=%p flags=0x%lX) -> 0x%08lX", static_cast<void*>(self), static_cast<void*>(hwnd),
        flags, static_cast<unsigned long>(hr));
    return hr;
}

// ---------------------------------------------------------------- IDirectInputEffect（[ForceFeedback] Mode=game）
// ゲームは約 30fps で FF_SetConstantForceA → SetParameters(方向+大きさ) → Start(1, 0) を繰り返す
struct FfbStats {
    LONG sets;
    LONG starts;
    LONG skippedStarts;
    LONG rawMin;
    LONG rawMax;
    LONG outMin;
    LONG outMax;
    DWORD windowStart;
};
FfbStats g_ffb{0, 0, 0, LONG_MAX, LONG_MIN, LONG_MAX, LONG_MIN, 0};
SRWLOCK g_ffbLock = SRWLOCK_INIT;

void FlushFfbStatsLocked() {
    const DWORD now = GetTickCount();
    if (g_ffb.windowStart == 0) {
        g_ffb.windowStart = now;
        return;
    }
    if (now - g_ffb.windowStart < 1000) {
        return;
    }
    if (g_ffb.sets > 0 || g_ffb.starts > 0) {
        Log("[ffb/s] SetParameters=%ld raw[%ld..%ld] out[%ld..%ld] Start=%ld skipped=%ld", g_ffb.sets, g_ffb.rawMin,
            g_ffb.rawMax, g_ffb.outMin, g_ffb.outMax, g_ffb.starts, g_ffb.skippedStarts);
    }
    g_ffb = {0, 0, 0, LONG_MAX, LONG_MIN, LONG_MAX, LONG_MIN, now};
}

// ゲームの力は不定期（1 秒に数回〜数十回、数秒来ないことも）に届き、元は 30ms で消える単発のエフェクトだった。
// ゲームからの値は「目標値」として受け取り、実際の出力は FfbTick が毎フレーム
//  - HoldMs 更新が無ければ 0 へ戻す（一瞬の衝撃が残り続けない）
//  - Smoothing で変化の速さを制限する（0 と最大の急な切り替えを和らげる）
// してから SetParameters で送る。符号付きの大きさを方向 27000 に固定して扱う
struct FfbOutput {
    IDirectInputDevice2A* dev;
    IDirectInputEffect* effect;  // AddRef して保持（ゲームが作り直すと差し替える）
    double target;
    double current;
    LONG sent;
    DWORD lastGameTick;
    DWORD lastTick;
    DWORD fadeStart;  // 途切れた後に力が再開した時刻（FadeInMs の起点）
};
FfbOutput g_ffbOut{nullptr, nullptr, 0.0, 0.0, 0, 0, 0, 0};

// ゲームは種別コード 8 向けに、内部の整数値を ×40000 して送る（実測: 走行中 1〜150 程度。低速で 1〜10、
// 250km/h 超のカーブで 50 以上）。100 を満量とし、カーブで小さい値を持ち上げる
constexpr double kGameUnit = 40000.0;
constexpr double kFullScaleUnits = 100.0;
// これ以上力が来なかったら「途切れた」とみなし、再開時にフェードインする
constexpr DWORD kFadeSilenceMs = 1000;

HRESULT SendFfbMagnitude(LONG magnitude) {
    DICONSTANTFORCE cf{magnitude};
    LONG direction[2] = {27000, 0};
    DIEFFECT e{};
    e.dwSize = sizeof(e);
    e.dwFlags = DIEFF_POLAR | DIEFF_OBJECTOFFSETS;
    e.cAxes = 2;
    e.rglDirection = direction;
    e.cbTypeSpecificParams = sizeof(cf);
    e.lpvTypeSpecificParams = &cf;
    auto set = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectInputEffect*, LPCDIEFFECT, DWORD)>(g_ffbOut.effect, 6);
    return set(g_ffbOut.effect, &e, DIEP_DIRECTION | DIEP_TYPESPECIFICPARAMS);
}

void FfbTick(IDirectInputDevice2A* dev) {
    FfbOutput& o = g_ffbOut;
    if (!o.effect || dev != o.dev) {
        return;
    }
    const Config& cfg = GetConfig();
    const DWORD now = GetTickCount();
    const DWORD dt = o.lastTick ? now - o.lastTick : 0;
    o.lastTick = now;
    if (now - o.lastGameTick > static_cast<DWORD>(cfg.ffbHoldMs)) {
        o.target = 0.0;
    }
    double fade = 1.0;
    if (cfg.ffbFadeInMs > 0 && o.fadeStart != 0 && now - o.fadeStart < static_cast<DWORD>(cfg.ffbFadeInMs)) {
        fade = static_cast<double>(now - o.fadeStart) / cfg.ffbFadeInMs;
    }
    const double desired = o.target * fade;
    const double limit = cfg.ffbMaxForce * 100.0;
    if (cfg.ffbSmoothingMs > 0 && limit > 0) {
        const double step = limit * dt / cfg.ffbSmoothingMs;
        const double d = desired - o.current;
        o.current += d > step ? step : (d < -step ? -step : d);
    } else {
        o.current = desired;
    }
    const LONG magnitude = static_cast<LONG>(std::lround(o.current));
    if (magnitude == o.sent) {
        return;
    }
    HRESULT hr = SendFfbMagnitude(magnitude);
    if (SUCCEEDED(hr)) {
        o.sent = magnitude;
        DWORD status = 0;
        if (magnitude != 0 && SUCCEEDED(o.effect->GetEffectStatus(&status)) && !(status & DIEGES_PLAYING)) {
            auto start = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectInputEffect*, DWORD, DWORD)>(o.effect, 7);
            start(o.effect, 1, 0);  // Acquire を失った後などで止まっていたら再開
        }
    }
    static LONG failures = 0;
    if (FAILED(hr) && cfg.logInput && InterlockedIncrement(&failures) <= 3) {
        Log("FFB output SetParameters(mag=%ld) -> 0x%08lX", magnitude, static_cast<unsigned long>(hr));
    }
}

HRESULT STDMETHODCALLTYPE Eff_SetParameters(IDirectInputEffect* self, LPCDIEFFECT eff, DWORD flags) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectInputEffect*, LPCDIEFFECT, DWORD)>(self, 6);
    if (self != g_ffbOut.effect || !eff || !(flags & DIEP_TYPESPECIFICPARAMS) || !eff->lpvTypeSpecificParams ||
        eff->cbTypeSpecificParams < sizeof(DICONSTANTFORCE)) {
        return fn(self, eff, flags);
    }
    const Config& cfg = GetConfig();
    const LONG raw = static_cast<const DICONSTANTFORCE*>(eff->lpvTypeSpecificParams)->lMagnitude;
    // 満量比 → カーブ → Gain → 上限。符号は方向と Invert から
    const double x = std::fabs(raw / kGameUnit) / kFullScaleUnits;
    double mag = std::pow(x, cfg.ffbCurve / 100.0) * (cfg.ffbGain / 100.0);
    mag = mag > 1.0 ? 1.0 : mag;
    double sign = raw < 0 ? -1.0 : 1.0;
    // ゲームは方向 27000(左) か 9000(右) を指定する。9000 は 27000 の逆向き
    if ((flags & DIEP_DIRECTION) && eff->rglDirection && eff->cAxes >= 1 && eff->rglDirection[0] == 9000) {
        sign = -sign;
    }
    if (cfg.ffbInvert) {
        sign = -sign;
    }
    const double v = sign * mag * (cfg.ffbMaxForce * 100.0);  // 100% = DI_FFNOMINALMAX(10000)
    const DWORD now = GetTickCount();
    if (g_ffbOut.lastGameTick == 0 || now - g_ffbOut.lastGameTick > kFadeSilenceMs) {
        g_ffbOut.fadeStart = now;
    }
    g_ffbOut.target = v;
    g_ffbOut.lastGameTick = now;
    if (cfg.logInput) {
        const LONG out = static_cast<LONG>(std::lround(v));
        AcquireSRWLockExclusive(&g_ffbLock);
        ++g_ffb.sets;
        g_ffb.rawMin = raw < g_ffb.rawMin ? raw : g_ffb.rawMin;
        g_ffb.rawMax = raw > g_ffb.rawMax ? raw : g_ffb.rawMax;
        g_ffb.outMin = out < g_ffb.outMin ? out : g_ffb.outMin;
        g_ffb.outMax = out > g_ffb.outMax ? out : g_ffb.outMax;
        FlushFfbStatsLocked();
        ReleaseSRWLockExclusive(&g_ffbLock);
    }
    return DI_OK;  // 実際の出力は FfbTick
}

// ゲームはシーン遷移で止める。出力もすぐ 0 にする
HRESULT STDMETHODCALLTYPE Eff_Stop(IDirectInputEffect* self) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectInputEffect*)>(self, 8);
    if (self == g_ffbOut.effect) {
        g_ffbOut.target = 0.0;
        g_ffbOut.current = 0.0;
        g_ffbOut.sent = 0;
        SendFfbMagnitude(0);
    }
    return fn(self);
}

// 持続時間を無限にしてあるので、再生中なら Start し直さない（毎フレームの再始動による途切れを防ぐ）。
// Acquire を失うと再生は止まるので、状態を見て止まっていれば Start する
HRESULT STDMETHODCALLTYPE Eff_Start(IDirectInputEffect* self, DWORD count, DWORD flags) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectInputEffect*, DWORD, DWORD)>(self, 7);
    DWORD status = 0;
    const bool playing = SUCCEEDED(self->GetEffectStatus(&status)) && (status & DIEGES_PLAYING);
    HRESULT hr = playing ? DI_OK : fn(self, count, flags);
    if (GetConfig().logInput) {
        AcquireSRWLockExclusive(&g_ffbLock);
        ++(playing ? g_ffb.skippedStarts : g_ffb.starts);
        FlushFfbStatsLocked();
        ReleaseSRWLockExclusive(&g_ffbLock);
        static LONG failures = 0;
        if (FAILED(hr) && InterlockedIncrement(&failures) <= 3) {
            Log("%p->IDirectInputEffect::Start -> 0x%08lX", static_cast<void*>(self), static_cast<unsigned long>(hr));
        }
    }
    return hr;
}

void HookEffect(IDirectInputEffect* effect) {
    static LONG hooked = 0;
    if (InterlockedExchange(&hooked, 1) == 0) {
        Log("hook IDirectInputEffect %p", static_cast<void*>(effect));
        PatchVtable(effect, 6, Eff_SetParameters, "DIEffect::SetParameters");
        PatchVtable(effect, 7, Eff_Start, "DIEffect::Start");
        PatchVtable(effect, 8, Eff_Stop, "DIEffect::Stop");
    }
}

HRESULT STDMETHODCALLTYPE Dev_CreateEffect(IDirectInputDevice2A* self, REFGUID guid, LPCDIEFFECT eff,
                                           LPDIRECTINPUTEFFECT* out, LPUNKNOWN outer) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice2A*, REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT*,
                                               LPUNKNOWN)>(self, 18);
    // ゲームは 30ms の ConstantForce を毎フレーム Start し直すので、30fps だと途切れて振動になる → 無限にする
    const bool game = GetConfig().ffbMode == FfbMode::Game;
    DIEFFECT copy{};
    LPCDIEFFECT pass = eff;
    if (game && eff && guid == GUID_ConstantForce && eff->dwSize <= sizeof(copy)) {
        std::memcpy(&copy, eff, eff->dwSize);
        copy.dwDuration = INFINITE;
        pass = &copy;
    }
    HRESULT hr = fn(self, guid, pass, out, outer);
    if (game && SUCCEEDED(hr) && out && *out && guid == GUID_ConstantForce) {
        HookEffect(*out);
        // 出力先を差し替える（F5 を閉じるなどでゲームはデバイスとエフェクトを作り直す）
        if (g_ffbOut.effect) {
            g_ffbOut.effect->Release();
        }
        (*out)->AddRef();
        g_ffbOut = {self, *out, 0.0, 0.0, 0, 0, 0, 0};
    }
    Log("%p->CreateEffect(%s duration=%lu gain=%lu axes=%lu) -> 0x%08lX", static_cast<void*>(self),
        GuidStr(&guid).c_str(), pass ? pass->dwDuration : 0, pass ? pass->dwGain : 0, pass ? pass->cAxes : 0,
        static_cast<unsigned long>(hr));
    return hr;
}

void HookDevice(IUnknown* dev) {
    Log("hook IDirectInputDevice2A %p", static_cast<void*>(dev));
    PatchVtable(dev, 3, Dev_GetCapabilities, "DIDevice::GetCapabilities");
    PatchVtable(dev, 6, Dev_SetProperty, "DIDevice::SetProperty");
    PatchVtable(dev, 7, Dev_Acquire, "DIDevice::Acquire");
    PatchVtable(dev, 9, Dev_GetDeviceState, "DIDevice::GetDeviceState");
    PatchVtable(dev, 11, Dev_SetDataFormat, "DIDevice::SetDataFormat");
    PatchVtable(dev, 13, Dev_SetCooperativeLevel, "DIDevice::SetCooperativeLevel");
    PatchVtable(dev, 18, Dev_CreateEffect, "DIDevice2::CreateEffect");
}

HRESULT STDMETHODCALLTYPE Dev_QueryInterface(IUnknown* self, REFIID riid, void** out) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IUnknown*, REFIID, void**)>(self, 0);
    HRESULT hr = fn(self, riid, out);
    Log("%p->QueryInterface(%s) -> 0x%08lX obj=%p", static_cast<void*>(self), GuidStr(&riid).c_str(),
        static_cast<unsigned long>(hr), out ? *out : nullptr);
    if (SUCCEEDED(hr) && out && *out && riid == IID_IDirectInputDevice2A) {
        HookDevice(static_cast<IUnknown*>(*out));
    }
    return hr;
}

// ---------------------------------------------------------------- IDirectInputA
struct EnumDevCtx {
    LPDIENUMDEVICESCALLBACKA cb;
    LPVOID ctx;
    int nameMatches;
};

bool ContainsNoCase(const char* s, const std::string& sub) {
    for (; *s; ++s) {
        if (_strnicmp(s, sub.c_str(), sub.size()) == 0) {
            return true;
        }
    }
    return sub.empty();
}

BOOL CALLBACK EnumDevicesWrap(LPCDIDEVICEINSTANCEA d, LPVOID p) {
    auto* c = static_cast<EnumDevCtx*>(p);
    // DeviceName 指定があれば、一致しないゲームコントローラはゲームに見せない
    const Config& cfg = GetConfig();
    if (!cfg.inputDeviceName.empty() && GET_DIDEVICE_TYPE(d->dwDevType) == DIDEVTYPE_JOYSTICK) {
        const bool match = ContainsNoCase(d->tszProductName, cfg.inputDeviceName) &&
                           (++c->nameMatches == cfg.inputDeviceIndex || cfg.inputDeviceIndex == 0);
        if (!match) {
            Log("    device type=0x%08lX product=\"%s\" guidProduct=%s -> hidden (DeviceName)", d->dwDevType,
                d->tszProductName, GuidStr(&d->guidProduct).c_str());
            return DIENUM_CONTINUE;
        }
    }
    // ゲーム (Input_EnumJoystickCb) は dwDevType のサブタイプで F5 Device Settings の選択肢を決める。
    // 設定があれば、サブタイプだけ書き換えたコピーを渡す
    DIDEVICEINSTANCEA copy;
    LPCDIDEVICEINSTANCEA pass = d;
    const int subtype = GetConfig().inputDeviceSubtype;
    const DWORD origType = d->dwDevType;
    if (subtype != 0 && d->dwSize <= sizeof(copy) && GET_DIDEVICE_TYPE(d->dwDevType) == DIDEVTYPE_JOYSTICK) {
        std::memcpy(&copy, d, d->dwSize);
        copy.dwDevType = (d->dwDevType & ~0xFF00UL) | (static_cast<DWORD>(subtype) << 8);
        pass = &copy;
    }
    // [ForceFeedback] Mode=game: ゲームが FFB の強さを更新するのは既知機種の種別コード 3/8 だけ。
    // コード 3（SideWinder Force Feedback Pro）は F5 で「SideWinder 3D Pro Type」＝ジョイスティック扱いになるので、
    // FF ドライバを持つデバイスは コード 8 の "DIforce2 Serial Joystick Device"（名前は前方一致）に見せる。
    // コード 8 は F5 の「Per4mer Racing Wheel」で選べるホイール扱い（0x45B083 は 9 か 8 を探す）
    static constexpr GUID kNullGuid{};
    if (cfg.ffbMode == FfbMode::Game && d->dwSize <= sizeof(copy) && GET_DIDEVICE_TYPE(d->dwDevType) == DIDEVTYPE_JOYSTICK &&
        std::memcmp(&d->guidFFDriver, &kNullGuid, sizeof(GUID)) != 0) {
        if (pass != &copy) {
            std::memcpy(&copy, d, d->dwSize);
            pass = &copy;
        }
        strcpy_s(copy.tszProductName, "DIforce2 Serial Joystick Device");
        Log("    device \"%s\" reported as \"%s\" ([ForceFeedback] Mode=game)", d->tszProductName, copy.tszProductName);
    }
    BOOL r = c->cb(pass, c->ctx);
    Log("    device type=0x%08lX (sub=%lu)%s instance=\"%s\" product=\"%s\" guidProduct=%s -> cb=%d", origType,
        (origType >> 8) & 0xFF, pass->dwDevType != origType ? (" -> overridden sub=" + std::to_string(subtype)).c_str() : "",
        d->tszInstanceName, d->tszProductName, GuidStr(&d->guidProduct).c_str(), r);
    return r;
}

HRESULT STDMETHODCALLTYPE DI_EnumDevices(IDirectInputA* self, DWORD type, LPDIENUMDEVICESCALLBACKA cb, LPVOID ctx,
                                         DWORD flags) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectInputA*, DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD)>(self, 4);
    Log("IDirectInput::EnumDevices(type=%lu flags=0x%lX)", type, flags);
    EnumDevCtx c{cb, ctx, 0};
    HRESULT hr = cb ? fn(self, type, EnumDevicesWrap, &c, flags) : fn(self, type, cb, ctx, flags);
    Log("IDirectInput::EnumDevices -> 0x%08lX", static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE DI_CreateDevice(IDirectInputA* self, REFGUID guid, LPDIRECTINPUTDEVICEA* out, LPUNKNOWN outer) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectInputA*, REFGUID, LPDIRECTINPUTDEVICEA*, LPUNKNOWN)>(self, 3);
    HRESULT hr = fn(self, guid, out, outer);
    Log("IDirectInput::CreateDevice(%s) -> 0x%08lX dev=%p", GuidStr(&guid).c_str(), static_cast<unsigned long>(hr),
        out ? static_cast<void*>(*out) : nullptr);
    if (SUCCEEDED(hr) && out && *out) {
        PatchVtable(*out, 0, Dev_QueryInterface, "DIDevice::QueryInterface");
    }
    return hr;
}

}  // namespace

void HookDirectInput(void* directInput) {
    Log("hook IDirectInputA %p", directInput);
    PatchVtable(directInput, 3, DI_CreateDevice, "IDirectInput::CreateDevice");
    PatchVtable(directInput, 4, DI_EnumDevices, "IDirectInput::EnumDevices");
}

}  // namespace stcc
