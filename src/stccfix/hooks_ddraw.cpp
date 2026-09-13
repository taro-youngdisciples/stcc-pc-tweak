// DirectDraw / Direct3D (DX5 世代) の呼び出しログ。調査用。
//
// COM メソッドは vtable の該当スロットを差し替える。元の関数ポインタは (vtable, index) ごとに保持するので、
// ラッパー（dgVoodoo 等）が複数インターフェースで実装を共有していても取り違えない。
// 描画ループで毎フレーム呼ばれるメソッド（Lock/Blt/Flip 等）はフックしない。
#include "common.h"

#define DIRECT3D_VERSION 0x0500
#include <ddraw.h>
#include <d3d.h>

#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

namespace stcc {
namespace {

// ---------------------------------------------------------------- vtable 差し替え
CRITICAL_SECTION g_cs;

struct VtblSlot {
    void** vtbl;
    int index;
    void* original;
};
VtblSlot g_slots[128];
int g_slotCount = 0;

void* FindOriginalLocked(void** vtbl, int index) {
    for (int i = 0; i < g_slotCount; ++i) {
        if (g_slots[i].vtbl == vtbl && g_slots[i].index == index) {
            return g_slots[i].original;
        }
    }
    return nullptr;
}

void PatchVtable(IUnknown* iface, int index, void* detour, const char* name) {
    if (!iface) {
        return;
    }
    void** vtbl = *reinterpret_cast<void***>(iface);
    EnterCriticalSection(&g_cs);
    if (!FindOriginalLocked(vtbl, index) && g_slotCount < static_cast<int>(std::size(g_slots))) {
        DWORD oldProtect = 0;
        if (VirtualProtect(&vtbl[index], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            g_slots[g_slotCount++] = {vtbl, index, vtbl[index]};
            vtbl[index] = detour;
            VirtualProtect(&vtbl[index], sizeof(void*), oldProtect, &oldProtect);
            Log("  hook %s: vtbl=%p[%d] orig=%p", name, static_cast<void*>(vtbl), index, g_slots[g_slotCount - 1].original);
        } else {
            Log("  hook %s: VirtualProtect 失敗 (%lu)", name, GetLastError());
        }
    }
    LeaveCriticalSection(&g_cs);
}

template <class Fn>
Fn Orig(void* self, int index) {
    void** vtbl = *reinterpret_cast<void***>(self);
    EnterCriticalSection(&g_cs);
    void* p = FindOriginalLocked(vtbl, index);
    LeaveCriticalSection(&g_cs);
    return reinterpret_cast<Fn>(p);
}

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

const char* IidName(REFIID r) {
    if (r == IID_IUnknown) return "IUnknown";
    if (r == IID_IDirectDraw) return "IDirectDraw";
    if (r == IID_IDirectDraw2) return "IDirectDraw2";
    if (r == IID_IDirectDrawSurface) return "IDirectDrawSurface";
    if (r == IID_IDirectDrawSurface2) return "IDirectDrawSurface2";
    if (r == IID_IDirectDrawSurface3) return "IDirectDrawSurface3";
    if (r == IID_IDirect3D) return "IDirect3D";
    if (r == IID_IDirect3D2) return "IDirect3D2";
    if (r == IID_IDirect3DTexture) return "IDirect3DTexture";
    if (r == IID_IDirect3DTexture2) return "IDirect3DTexture2";
    if (r == IID_IDirect3DHALDevice) return "IID_IDirect3DHALDevice";
    if (r == IID_IDirect3DRGBDevice) return "IID_IDirect3DRGBDevice";
    if (r == IID_IDirect3DRampDevice) return "IID_IDirect3DRampDevice";
    if (r == IID_IDirect3DMMXDevice) return "IID_IDirect3DMMXDevice";
    return "?";
}

void LogDesc(const char* tag, const DDSURFACEDESC* d) {
    if (!d) {
        Log("    %s: null", tag);
        return;
    }
    const DDPIXELFORMAT& pf = d->ddpfPixelFormat;
    Log("    %s: size=%lu flags=0x%08lX %lux%lu caps=0x%08lX backbuf=%lu zbits=%lu "
        "pf{flags=0x%lX bits=%lu R=%08lX G=%08lX B=%08lX A=%08lX}",
        tag, d->dwSize, d->dwFlags, d->dwWidth, d->dwHeight, d->ddsCaps.dwCaps, d->dwBackBufferCount,
        d->dwZBufferBitDepth, pf.dwFlags, pf.dwRGBBitCount, pf.dwRBitMask, pf.dwGBitMask, pf.dwBBitMask,
        pf.dwRGBAlphaBitMask);
}

void LogSurface(const char* tag, IDirectDrawSurface* s) {
    if (!s) {
        Log("    %s: null surface", tag);
        return;
    }
    DDSURFACEDESC d{};
    d.dwSize = sizeof(d);
    HRESULT hr = s->GetSurfaceDesc(&d);
    if (SUCCEEDED(hr)) {
        LogDesc(tag, &d);
    } else {
        Log("    %s: GetSurfaceDesc -> 0x%08lX", tag, static_cast<unsigned long>(hr));
    }
}

// ---------------------------------------------------------------- 前方宣言
void HookDirectDraw(IUnknown* dd, const char* kind);
void HookDirectDraw2(IDirectDraw2* dd);
void HookDirect3D2(IDirect3D2* d3d);
void HookDevice2(IDirect3DDevice2* dev);
void HookSurface(IDirectDrawSurface* s);

void HookByIid(REFIID riid, void* obj) {
    if (!obj) {
        return;
    }
    if (riid == IID_IDirectDraw2) {
        HookDirectDraw2(static_cast<IDirectDraw2*>(obj));
    } else if (riid == IID_IDirect3D2) {
        HookDirect3D2(static_cast<IDirect3D2*>(obj));
    } else if (riid == IID_IDirectDrawSurface || riid == IID_IDirectDrawSurface2 || riid == IID_IDirectDrawSurface3) {
        HookSurface(static_cast<IDirectDrawSurface*>(obj));
    }
}

// ---------------------------------------------------------------- 共通: QueryInterface (index 0)
HRESULT STDMETHODCALLTYPE Any_QueryInterface(IUnknown* self, REFIID riid, void** out) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IUnknown*, REFIID, void**)>(self, 0);
    HRESULT hr = fn(self, riid, out);
    Log("%p->QueryInterface(%s %s) -> 0x%08lX obj=%p", static_cast<void*>(self), IidName(riid), GuidStr(&riid).c_str(),
        static_cast<unsigned long>(hr), out ? *out : nullptr);
    if (SUCCEEDED(hr) && out) {
        HookByIid(riid, *out);
    }
    return hr;
}

// ---------------------------------------------------------------- IDirectDraw2
HRESULT STDMETHODCALLTYPE DD2_CreateClipper(IDirectDraw2* self, DWORD flags, LPDIRECTDRAWCLIPPER* out, IUnknown* outer) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectDraw2*, DWORD, LPDIRECTDRAWCLIPPER*, IUnknown*)>(self, 4);
    HRESULT hr = fn(self, flags, out, outer);
    Log("IDirectDraw2::CreateClipper(flags=0x%lX) -> 0x%08lX", flags, static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE DD2_CreateSurface(IDirectDraw2* self, LPDDSURFACEDESC desc, LPDIRECTDRAWSURFACE* out,
                                            IUnknown* outer) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectDraw2*, LPDDSURFACEDESC, LPDIRECTDRAWSURFACE*, IUnknown*)>(self, 6);
    HRESULT hr = fn(self, desc, out, outer);
    Log("IDirectDraw2::CreateSurface -> 0x%08lX surf=%p", static_cast<unsigned long>(hr),
        out ? static_cast<void*>(*out) : nullptr);
    LogDesc("request", desc);
    if (SUCCEEDED(hr) && out && *out) {
        LogSurface("created", *out);
        HookSurface(*out);
    }
    return hr;
}

struct EnumModesCtx {
    LPDDENUMMODESCALLBACK cb;
    LPVOID ctx;
    int count;
};

HRESULT CALLBACK EnumModesWrap(LPDDSURFACEDESC d, LPVOID p) {
    auto* c = static_cast<EnumModesCtx*>(p);
    ++c->count;
    HRESULT r = c->cb(d, c->ctx);
    if (d && d->ddpfPixelFormat.dwRGBBitCount <= 16) {
        Log("    mode %lux%lu %lubpp -> cb=%ld", d->dwWidth, d->dwHeight, d->ddpfPixelFormat.dwRGBBitCount,
            static_cast<long>(r));
    }
    return r;
}

HRESULT STDMETHODCALLTYPE DD2_EnumDisplayModes(IDirectDraw2* self, DWORD flags, LPDDSURFACEDESC desc, LPVOID ctx,
                                               LPDDENUMMODESCALLBACK cb) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectDraw2*, DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMMODESCALLBACK)>(self, 8);
    Log("IDirectDraw2::EnumDisplayModes(flags=0x%lX)", flags);
    EnumModesCtx c{cb, ctx, 0};
    HRESULT hr = cb ? fn(self, flags, desc, &c, EnumModesWrap) : fn(self, flags, desc, ctx, cb);
    Log("IDirectDraw2::EnumDisplayModes -> 0x%08lX (%d modes)", static_cast<unsigned long>(hr), c.count);
    return hr;
}

HRESULT STDMETHODCALLTYPE DD2_SetCooperativeLevel(IDirectDraw2* self, HWND hwnd, DWORD flags) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectDraw2*, HWND, DWORD)>(self, 20);
    HRESULT hr = fn(self, hwnd, flags);
    Log("IDirectDraw2::SetCooperativeLevel(hwnd=%p flags=0x%lX) -> 0x%08lX", static_cast<void*>(hwnd), flags,
        static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE DD2_SetDisplayMode(IDirectDraw2* self, DWORD w, DWORD h, DWORD bpp, DWORD refresh, DWORD flags) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectDraw2*, DWORD, DWORD, DWORD, DWORD, DWORD)>(self, 21);
    HRESULT hr = fn(self, w, h, bpp, refresh, flags);
    Log("IDirectDraw2::SetDisplayMode(%lux%lu %lubpp @%lu flags=0x%lX) -> 0x%08lX", w, h, bpp, refresh, flags,
        static_cast<unsigned long>(hr));
    return hr;
}

// ---------------------------------------------------------------- IDirect3D2
struct EnumDevCtx {
    LPD3DENUMDEVICESCALLBACK cb;
    LPVOID ctx;
};

HRESULT CALLBACK EnumDevicesWrap(GUID* guid, LPSTR desc, LPSTR name, LPD3DDEVICEDESC hw, LPD3DDEVICEDESC hel, LPVOID p) {
    auto* c = static_cast<EnumDevCtx*>(p);
    HRESULT r = c->cb(guid, desc, name, hw, hel, c->ctx);
    Log("    device %s name=\"%s\" desc=\"%s\" -> cb=%ld", GuidStr(guid).c_str(), name ? name : "", desc ? desc : "",
        static_cast<long>(r));
    if (hw) {
        Log("      hw : size=%lu flags=0x%lX color=%lu renderBits=0x%lX zBits=0x%lX", hw->dwSize, hw->dwFlags,
            static_cast<unsigned long>(hw->dcmColorModel), hw->dwDeviceRenderBitDepth, hw->dwDeviceZBufferBitDepth);
    }
    if (hel) {
        Log("      hel: size=%lu flags=0x%lX color=%lu renderBits=0x%lX zBits=0x%lX", hel->dwSize, hel->dwFlags,
            static_cast<unsigned long>(hel->dcmColorModel), hel->dwDeviceRenderBitDepth, hel->dwDeviceZBufferBitDepth);
    }
    return r;
}

HRESULT STDMETHODCALLTYPE D3D2_EnumDevices(IDirect3D2* self, LPD3DENUMDEVICESCALLBACK cb, LPVOID ctx) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3D2*, LPD3DENUMDEVICESCALLBACK, LPVOID)>(self, 3);
    Log("IDirect3D2::EnumDevices");
    EnumDevCtx c{cb, ctx};
    HRESULT hr = cb ? fn(self, EnumDevicesWrap, &c) : fn(self, cb, ctx);
    Log("IDirect3D2::EnumDevices -> 0x%08lX", static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D2_CreateViewport(IDirect3D2* self, LPDIRECT3DVIEWPORT2* out, IUnknown* outer) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3D2*, LPDIRECT3DVIEWPORT2*, IUnknown*)>(self, 6);
    HRESULT hr = fn(self, out, outer);
    Log("IDirect3D2::CreateViewport -> 0x%08lX", static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D2_CreateDevice(IDirect3D2* self, REFCLSID clsid, LPDIRECTDRAWSURFACE surf,
                                            LPDIRECT3DDEVICE2* out) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3D2*, REFCLSID, LPDIRECTDRAWSURFACE, LPDIRECT3DDEVICE2*)>(self, 8);
    Log("IDirect3D2::CreateDevice(%s %s surf=%p)", IidName(clsid), GuidStr(&clsid).c_str(), static_cast<void*>(surf));
    LogSurface("target", surf);
    HRESULT hr = fn(self, clsid, surf, out);
    Log("IDirect3D2::CreateDevice -> 0x%08lX dev=%p", static_cast<unsigned long>(hr),
        out ? static_cast<void*>(*out) : nullptr);
    if (SUCCEEDED(hr) && out && *out) {
        HookDevice2(*out);
    }
    return hr;
}

// ---------------------------------------------------------------- IDirect3DDevice2
struct EnumTexCtx {
    LPD3DENUMTEXTUREFORMATSCALLBACK cb;
    LPVOID ctx;
};

HRESULT CALLBACK EnumTexWrap(LPDDSURFACEDESC d, LPVOID p) {
    auto* c = static_cast<EnumTexCtx*>(p);
    HRESULT r = c->cb(d, c->ctx);
    if (d) {
        const DDPIXELFORMAT& pf = d->ddpfPixelFormat;
        Log("    texfmt flags=0x%lX bits=%lu R=%08lX G=%08lX B=%08lX A=%08lX -> cb=%ld", pf.dwFlags, pf.dwRGBBitCount,
            pf.dwRBitMask, pf.dwGBitMask, pf.dwBBitMask, pf.dwRGBAlphaBitMask, static_cast<long>(r));
    }
    return r;
}

HRESULT STDMETHODCALLTYPE Dev2_EnumTextureFormats(IDirect3DDevice2* self, LPD3DENUMTEXTUREFORMATSCALLBACK cb, LPVOID ctx) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice2*, LPD3DENUMTEXTUREFORMATSCALLBACK, LPVOID)>(self, 4);
    Log("IDirect3DDevice2::EnumTextureFormats");
    EnumTexCtx c{cb, ctx};
    HRESULT hr = cb ? fn(self, EnumTexWrap, &c) : fn(self, cb, ctx);
    Log("IDirect3DDevice2::EnumTextureFormats -> 0x%08lX", static_cast<unsigned long>(hr));
    return hr;
}

// ---------------------------------------------------------------- IDirectDrawSurface
HRESULT STDMETHODCALLTYPE Surf_AddAttachedSurface(IDirectDrawSurface* self, LPDIRECTDRAWSURFACE att) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectDrawSurface*, LPDIRECTDRAWSURFACE)>(self, 3);
    HRESULT hr = fn(self, att);
    Log("%p->AddAttachedSurface(%p) -> 0x%08lX", static_cast<void*>(self), static_cast<void*>(att),
        static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE Surf_GetAttachedSurface(IDirectDrawSurface* self, LPDDSCAPS caps, LPDIRECTDRAWSURFACE* out) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectDrawSurface*, LPDDSCAPS, LPDIRECTDRAWSURFACE*)>(self, 12);
    HRESULT hr = fn(self, caps, out);
    Log("%p->GetAttachedSurface(caps=0x%lX) -> 0x%08lX surf=%p", static_cast<void*>(self), caps ? caps->dwCaps : 0,
        static_cast<unsigned long>(hr), out ? static_cast<void*>(*out) : nullptr);
    if (SUCCEEDED(hr) && out && *out) {
        HookSurface(*out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Surf_SetClipper(IDirectDrawSurface* self, LPDIRECTDRAWCLIPPER clip) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectDrawSurface*, LPDIRECTDRAWCLIPPER)>(self, 28);
    HRESULT hr = fn(self, clip);
    Log("%p->SetClipper(%p) -> 0x%08lX", static_cast<void*>(self), static_cast<void*>(clip), static_cast<unsigned long>(hr));
    return hr;
}

// ---------------------------------------------------------------- フック設置
void HookDirectDraw(IUnknown* dd, const char* kind) {
    Log("hook %s %p", kind, static_cast<void*>(dd));
    PatchVtable(dd, 0, Any_QueryInterface, "QueryInterface");
}

void HookDirectDraw2(IDirectDraw2* dd) {
    HookDirectDraw(dd, "IDirectDraw2");
    PatchVtable(dd, 4, DD2_CreateClipper, "IDirectDraw2::CreateClipper");
    PatchVtable(dd, 6, DD2_CreateSurface, "IDirectDraw2::CreateSurface");
    PatchVtable(dd, 8, DD2_EnumDisplayModes, "IDirectDraw2::EnumDisplayModes");
    PatchVtable(dd, 20, DD2_SetCooperativeLevel, "IDirectDraw2::SetCooperativeLevel");
    PatchVtable(dd, 21, DD2_SetDisplayMode, "IDirectDraw2::SetDisplayMode");
}

void HookDirect3D2(IDirect3D2* d3d) {
    Log("hook IDirect3D2 %p", static_cast<void*>(d3d));
    PatchVtable(d3d, 0, Any_QueryInterface, "QueryInterface");
    PatchVtable(d3d, 3, D3D2_EnumDevices, "IDirect3D2::EnumDevices");
    PatchVtable(d3d, 6, D3D2_CreateViewport, "IDirect3D2::CreateViewport");
    PatchVtable(d3d, 8, D3D2_CreateDevice, "IDirect3D2::CreateDevice");
}

void HookDevice2(IDirect3DDevice2* dev) {
    Log("hook IDirect3DDevice2 %p", static_cast<void*>(dev));
    PatchVtable(dev, 0, Any_QueryInterface, "QueryInterface");
    PatchVtable(dev, 4, Dev2_EnumTextureFormats, "IDirect3DDevice2::EnumTextureFormats");
}

void HookSurface(IDirectDrawSurface* s) {
    PatchVtable(s, 0, Any_QueryInterface, "QueryInterface");
    PatchVtable(s, 3, Surf_AddAttachedSurface, "Surface::AddAttachedSurface");
    PatchVtable(s, 12, Surf_GetAttachedSurface, "Surface::GetAttachedSurface");
    PatchVtable(s, 28, Surf_SetClipper, "Surface::SetClipper");
}

// ---------------------------------------------------------------- IAT: DDRAW!DirectDrawCreate
using DirectDrawCreateFn = HRESULT(WINAPI*)(GUID*, LPDIRECTDRAW*, IUnknown*);
DirectDrawCreateFn g_realDirectDrawCreate = nullptr;

HRESULT WINAPI Hook_DirectDrawCreate(GUID* guid, LPDIRECTDRAW* out, IUnknown* outer) {
    HRESULT hr = g_realDirectDrawCreate(guid, out, outer);
    Log("DirectDrawCreate(guid=%s) -> 0x%08lX dd=%p", GuidStr(guid).c_str(), static_cast<unsigned long>(hr),
        out ? static_cast<void*>(*out) : nullptr);
    if (SUCCEEDED(hr) && out && *out) {
        HookDirectDraw(*out, "IDirectDraw");
    }
    return hr;
}

bool PatchIat(HMODULE module, const char* dllName, void* target, void* detour) {
    auto base = reinterpret_cast<BYTE*>(module);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<char*>(base + imp->Name), dllName) != 0) {
            continue;
        }
        for (auto thunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + imp->FirstThunk); thunk->u1.Function; ++thunk) {
            if (reinterpret_cast<void*>(static_cast<std::uintptr_t>(thunk->u1.Function)) != target) {
                continue;
            }
            DWORD oldProtect = 0;
            VirtualProtect(&thunk->u1.Function, sizeof(DWORD), PAGE_READWRITE, &oldProtect);
            thunk->u1.Function = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(detour));
            VirtualProtect(&thunk->u1.Function, sizeof(DWORD), oldProtect, &oldProtect);
            return true;
        }
    }
    return false;
}

}  // namespace

void InstallDirectDrawLogging() {
    InitializeCriticalSection(&g_cs);
    HMODULE ddraw = GetModuleHandleW(L"ddraw.dll");  // exe の静的 import なので既にマップ済み
    if (!ddraw) {
        Log("ddraw.dll が未ロードのため DirectDraw ログは無効");
        return;
    }
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(ddraw, path, MAX_PATH);
    char pathA[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, pathA, sizeof(pathA), nullptr, nullptr);
    Log("ddraw.dll: %s", pathA);

    g_realDirectDrawCreate = reinterpret_cast<DirectDrawCreateFn>(GetProcAddress(ddraw, "DirectDrawCreate"));
    if (g_realDirectDrawCreate &&
        PatchIat(GetModuleHandleW(nullptr), "DDRAW.dll", static_cast<void*>(g_realDirectDrawCreate),
                 static_cast<void*>(Hook_DirectDrawCreate))) {
        Log("IAT hook: DDRAW!DirectDrawCreate");
    } else {
        Log("IAT hook: DDRAW!DirectDrawCreate が見つからない");
    }
}

}  // namespace stcc
