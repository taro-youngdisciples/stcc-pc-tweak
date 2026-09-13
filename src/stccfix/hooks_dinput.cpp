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
struct DeviceLogState {
    IDirectInputDevice2A* dev;
    DIJOYSTATE last;
    bool hasLast;
    DWORD lastLogTick;
    LONG polls;
};
DeviceLogState g_devices[4];
SRWLOCK g_devLock = SRWLOCK_INIT;

DeviceLogState* DeviceSlot(IDirectInputDevice2A* dev) {
    AcquireSRWLockExclusive(&g_devLock);
    DeviceLogState* found = nullptr;
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
                found = &d;
                break;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_devLock);
    return found;
}

BOOL CALLBACK EnumObjectsLog(LPCDIDEVICEOBJECTINSTANCEA o, LPVOID) {
    Log("    object ofs=0x%02lX(%s) type=0x%08lX flags=0x%lX name=\"%s\" guid=%s", o->dwOfs, JoyOffsetName(o->dwOfs),
        o->dwType, o->dwFlags, o->tszName, GuidStr(&o->guidType).c_str());
    return DIENUM_CONTINUE;
}

HRESULT STDMETHODCALLTYPE Dev_GetCapabilities(IDirectInputDevice2A* self, LPDIDEVCAPS caps) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice2A*, LPDIDEVCAPS)>(self, 3);
    HRESULT hr = fn(self, caps);
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
    DeviceLogState* s = DeviceSlot(self);
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
};

BOOL CALLBACK EnumDevicesWrap(LPCDIDEVICEINSTANCEA d, LPVOID p) {
    auto* c = static_cast<EnumDevCtx*>(p);
    BOOL r = c->cb(d, c->ctx);
    Log("    device type=0x%08lX (sub=%lu) instance=\"%s\" product=\"%s\" guidProduct=%s ff=%s -> cb=%d", d->dwDevType,
        (d->dwDevType >> 8) & 0xFF, d->tszInstanceName, d->tszProductName, GuidStr(&d->guidProduct).c_str(),
        GuidStr(&d->guidFFDriver).c_str(), r);
    return r;
}

HRESULT STDMETHODCALLTYPE DI_EnumDevices(IDirectInputA* self, DWORD type, LPDIENUMDEVICESCALLBACKA cb, LPVOID ctx,
                                         DWORD flags) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectInputA*, DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD)>(self, 4);
    Log("IDirectInput::EnumDevices(type=%lu flags=0x%lX)", type, flags);
    EnumDevCtx c{cb, ctx};
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
