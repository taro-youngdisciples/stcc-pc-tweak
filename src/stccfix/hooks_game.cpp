// ゲーム内関数（jp-1.02）の戻り値ログ。調査用。
//
// 対象は「引数なし・戻り値 EAX」の関数だけ。デトアは naked にして、元の関数を呼ぶまで
// レジスタに一切触れない（逆コンパイル結果に unaff_EDI 等が出る関数もあるため）。
// 引数を取る関数にこのマクロを使うと、元関数から見たスタック位置が 4 バイトずれるので不可。
#include "common.h"

#include <MinHook.h>

namespace stcc {
namespace {

#define STCC_RET_LOGGER(NAME)                                                    \
    void* g_orig_##NAME = nullptr;                                               \
    void __cdecl Report_##NAME(DWORD ret) { Log("[game] %s() -> 0x%lX", #NAME, ret); } \
    __declspec(naked) void Detour_##NAME() {                                     \
        __asm call dword ptr [g_orig_##NAME]                                     \
        __asm push eax                                                           \
        __asm push eax                                                           \
        __asm call Report_##NAME                                                 \
        __asm add esp, 4                                                         \
        __asm pop eax                                                            \
        __asm ret                                                                \
    }

// data/addresses/stcc_jp-1.02.toml 参照
// Gfx_InitDirectDraw (0x435390) は hooks_window.cpp が後処理フックを掛け、戻り値ログもそちらで出す
STCC_RET_LOGGER(D3D_Init)             // 0x433B20
STCC_RET_LOGGER(D3D_SelectDevice)     // 0x433DD0
STCC_RET_LOGGER(D3D_PickTextureFormat)  // 0x434DE0
STCC_RET_LOGGER(D3D_SetupViewport)    // 0x433ED0
STCC_RET_LOGGER(D3D_CreateResources)  // 0x434960
STCC_RET_LOGGER(D3D_CreateTextureSurface)  // 0x4348A0

struct Target {
    std::uintptr_t addr;
    void* detour;
    void** original;
    const char* name;
};

#undef STCC_RET_LOGGER

}  // namespace

void InstallGameTraceHooks() {
    const Target targets[] = {
        {0x00433B20, Detour_D3D_Init, &g_orig_D3D_Init, "D3D_Init"},
        {0x00433DD0, Detour_D3D_SelectDevice, &g_orig_D3D_SelectDevice, "D3D_SelectDevice"},
        {0x00434DE0, Detour_D3D_PickTextureFormat, &g_orig_D3D_PickTextureFormat, "D3D_PickTextureFormat"},
        {0x00433ED0, Detour_D3D_SetupViewport, &g_orig_D3D_SetupViewport, "D3D_SetupViewport"},
        {0x00434960, Detour_D3D_CreateResources, &g_orig_D3D_CreateResources, "D3D_CreateResources"},
        {0x004348A0, Detour_D3D_CreateTextureSurface, &g_orig_D3D_CreateTextureSurface, "D3D_CreateTextureSurface"},
    };

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        Log("MH_Initialize 失敗: %s", MH_StatusToString(st));
        return;
    }
    for (const auto& t : targets) {
        auto target = reinterpret_cast<LPVOID>(t.addr);
        st = MH_CreateHook(target, t.detour, t.original);
        if (st == MH_OK) {
            st = MH_EnableHook(target);
        }
        Log("game hook %s @0x%08X: %s", t.name, static_cast<unsigned>(t.addr), MH_StatusToString(st));
    }
}

}  // namespace stcc
