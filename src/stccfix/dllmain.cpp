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

void Startup() {
    const std::wstring dir = ModuleDirectory();
    LogInit(dir + L"\\stccfix.log");
    Log("stccfix build %s %s", __DATE__, __TIME__);

    const Config cfg = LoadConfig(dir + L"\\stccfix.ini");
    Log("config: Windowed=%d", cfg.windowed ? 1 : 0);

    const GameVersion ver = DetectGameVersion();
    Log("game version: %s", GameVersionName(ver));
    if (ver != GameVersion::Jp102) {
        Log("未対応の版なのでパッチは当てない（dinput 転送のみ）");
        return;
    }
    if (cfg.windowed) {
        ApplyPatches(kWindowedJp102, std::size(kWindowedJp102));
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
