// dinput.dll プロキシ。本物（System32 / SysWOW64 の dinput.dll）へ転送しつつ呼び出しを記録する。
// DllMain では LoadLibrary しない（ローダーロック回避）。最初の呼び出し時に遅延ロードする。
#include "common.h"

#include <unknwn.h>  // WIN32_LEAN_AND_MEAN では IUnknown が入らない

namespace stcc {
namespace {

INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
HMODULE g_real = nullptr;

BOOL CALLBACK LoadReal(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);  // WOW64 では自動で SysWOW64 にリダイレクトされる
    wcscpy_s(path + n, MAX_PATH - n, L"\\dinput.dll");
    g_real = LoadLibraryW(path);
    Log("real dinput.dll: %s", g_real ? "loaded" : "FAILED");
    return TRUE;
}

FARPROC Real(const char* name) {
    InitOnceExecuteOnce(&g_once, LoadReal, nullptr, nullptr);
    return g_real ? GetProcAddress(g_real, name) : nullptr;
}

}  // namespace
}  // namespace stcc

using stcc::Log;
using stcc::Real;

extern "C" {

HRESULT WINAPI Proxy_DirectInputCreateA(HINSTANCE hinst, DWORD version, LPVOID* out, LPUNKNOWN outer) {
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, LPVOID*, LPUNKNOWN);
    auto fn = static_cast<Fn>(static_cast<LPVOID>(Real("DirectInputCreateA")));
    HRESULT hr = fn ? fn(hinst, version, out, outer) : E_FAIL;
    Log("DirectInputCreateA(version=0x%lX) -> 0x%08lX iface=%p", version, static_cast<unsigned long>(hr),
        out ? *out : nullptr);
    const auto& cfg = stcc::GetConfig();
    if (SUCCEEDED(hr) && out && *out && stcc::InputHooksNeeded(cfg)) {
        stcc::HookDirectInput(*out);
    }
    return hr;
}

HRESULT WINAPI Proxy_DirectInputCreateW(HINSTANCE hinst, DWORD version, LPVOID* out, LPUNKNOWN outer) {
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, LPVOID*, LPUNKNOWN);
    auto fn = static_cast<Fn>(static_cast<LPVOID>(Real("DirectInputCreateW")));
    HRESULT hr = fn ? fn(hinst, version, out, outer) : E_FAIL;
    Log("DirectInputCreateW(version=0x%lX) -> 0x%08lX", version, static_cast<unsigned long>(hr));
    return hr;
}

HRESULT WINAPI Proxy_DirectInputCreateEx(HINSTANCE hinst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto fn = static_cast<Fn>(static_cast<LPVOID>(Real("DirectInputCreateEx")));
    HRESULT hr = fn ? fn(hinst, version, riid, out, outer) : E_FAIL;
    Log("DirectInputCreateEx(version=0x%lX) -> 0x%08lX", version, static_cast<unsigned long>(hr));
    return hr;
}

HRESULT WINAPI Proxy_DllCanUnloadNow() {
    using Fn = HRESULT(WINAPI*)();
    auto fn = static_cast<Fn>(static_cast<LPVOID>(Real("DllCanUnloadNow")));
    return fn ? fn() : S_FALSE;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    auto fn = static_cast<Fn>(static_cast<LPVOID>(Real("DllGetClassObject")));
    return fn ? fn(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI Proxy_DllRegisterServer() {
    using Fn = HRESULT(WINAPI*)();
    auto fn = static_cast<Fn>(static_cast<LPVOID>(Real("DllRegisterServer")));
    return fn ? fn() : E_FAIL;
}

HRESULT WINAPI Proxy_DllUnregisterServer() {
    using Fn = HRESULT(WINAPI*)();
    auto fn = static_cast<Fn>(static_cast<LPVOID>(Real("DllUnregisterServer")));
    return fn ? fn() : E_FAIL;
}

}  // extern "C"
