#include "common.h"

#include <iterator>

namespace stcc {
namespace {

HMODULE g_self = nullptr;

// data/addresses/stcc_jp-1.02.toml の App_InitInstance_FullscreenInit
constexpr BytePatch kWindowedJp102[] = {
    {0x0043964D, "C7 05 30 89 56 00 01 00 00 00", "C7 05 30 89 56 00 00 00 00 00",
     "InitInstance: g_bFullscreen = 0（ウィンドウ起動）"},
};

// Gfx_CreateRenderSurface (0x4364F0) の D3D 分岐。
// ウィンドウ時の描画先は SYSTEMMEMORY(0x800) で作られ、HAL デバイスの CreateDevice が
// D3DERR_SURFACENOTINVIDMEM で失敗する。caps の上位バイトを {+VIDEOMEMORY|3DDEVICE, -SYSTEMMEMORY} にする。
//   元: mov eax,[esp+7C] / or ah,20h / mov [esp+7C],eax
//   新: or byte [esp+7D],60h / and byte [esp+7D],0F7h / nop
constexpr BytePatch kD3DWindowedVideoMemoryJp102[] = {
    {0x00436548, "8B 44 24 7C 80 CC 20 89 44 24 7C", "80 4C 24 7D 60 80 64 24 7D F7 90",
     "Gfx_CreateRenderSurface: D3D 描画先を VIDEOMEMORY|3DDEVICE で作成"},
};

void Startup() {
    const std::wstring dir = ModuleDirectory();
    LogInit(dir + L"\\stccfix.log");
    Log("stccfix build %s %s", __DATE__, __TIME__);

    const Config cfg = LoadConfig(dir + L"\\stccfix.ini");
    Log("config: Windowed=%d D3DWindowedVideoMemory=%d LogGraphics=%d", cfg.windowed ? 1 : 0,
        cfg.d3dWindowedVideoMemory ? 1 : 0, cfg.logGraphics ? 1 : 0);

    if (cfg.logGraphics) {
        InstallDirectDrawLogging();  // 版に依存しない
    }

    const GameVersion ver = DetectGameVersion();
    Log("game version: %s", GameVersionName(ver));
    if (ver != GameVersion::Jp102) {
        Log("未対応の版なのでパッチは当てない（dinput 転送のみ）");
        return;
    }
    if (cfg.windowed) {
        ApplyPatches(kWindowedJp102, std::size(kWindowedJp102));
    }
    if (cfg.d3dWindowedVideoMemory) {
        ApplyPatches(kD3DWindowedVideoMemoryJp102, std::size(kD3DWindowedVideoMemoryJp102));
    }
    if (cfg.logGraphics) {
        InstallGameTraceHooks();
    }
}

}  // namespace

std::wstring ModuleDirectory() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    std::wstring s(path, n);
    return s.substr(0, s.find_last_of(L"\\/"));
}

}  // namespace stcc

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        stcc::g_self = module;
        DisableThreadLibraryCalls(module);
        // 静的 import で読まれるため、ここはゲームの WinMain より前に走る。
        // メモリパッチはここで当てる（LoadLibrary 等のローダーロックを要する処理はしない）
        stcc::Startup();
    }
    return TRUE;
}
