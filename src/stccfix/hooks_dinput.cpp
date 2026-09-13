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
    if (SUCCEEDED(hr) && caps && !GetConfig().forceFeedback && (caps->dwFlags & DIDC_FORCEFEEDBACK)) {
        caps->dwFlags &= ~DIDC_FORCEFEEDBACK;
        Log("%p->GetCapabilities: DIDC_FORCEFEEDBACK hidden (ForceFeedback=0)", static_cast<void*>(self));
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
    HRESULT hr = fn(self, prop, ph);
    const auto id = reinterpret_cast<std::uintptr_t>(prop);
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

HRESULT STDMETHODCALLTYPE Dev_GetDeviceState(IDirectInputDevice2A* self, DWORD cb, LPVOID data) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice2A*, DWORD, LPVOID)>(self, 9);
    HRESULT hr = fn(self, cb, data);
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

HRESULT STDMETHODCALLTYPE Dev_CreateEffect(IDirectInputDevice2A* self, REFGUID guid, LPCDIEFFECT eff,
                                           LPDIRECTINPUTEFFECT* out, LPUNKNOWN outer) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice2A*, REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT*,
                                               LPUNKNOWN)>(self, 18);
    HRESULT hr = fn(self, guid, eff, out, outer);
    Log("%p->CreateEffect(%s duration=%lu gain=%lu axes=%lu) -> 0x%08lX", static_cast<void*>(self),
        GuidStr(&guid).c_str(), eff ? eff->dwDuration : 0, eff ? eff->dwGain : 0, eff ? eff->cAxes : 0,
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
    BOOL r = c->cb(pass, c->ctx);
    Log("    device type=0x%08lX (sub=%lu)%s instance=\"%s\" product=\"%s\" guidProduct=%s -> cb=%d", origType,
        (origType >> 8) & 0xFF, pass != d ? (" -> overridden sub=" + std::to_string(subtype)).c_str() : "",
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
