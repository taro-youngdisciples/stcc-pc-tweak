#include "common.h"

#include <iterator>

namespace stcc {
namespace {

HMODULE g_self = nullptr;
Config g_config;

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

    g_config = LoadConfig(dir + L"\\stccfix.ini");
    const Config& cfg = g_config;
    Log("config: Windowed=%d D3DWindowedVideoMemory=%d LogGraphics=%d LogInput=%d InputDeviceSubtype=%d",
        cfg.windowed ? 1 : 0, cfg.d3dWindowedVideoMemory ? 1 : 0, cfg.logGraphics ? 1 : 0, cfg.logInput ? 1 : 0,
        cfg.inputDeviceSubtype);
    Log("config: DeviceName=\"%s\" #%d ForceFeedback=%d", cfg.inputDeviceName.c_str(), cfg.inputDeviceIndex,
        cfg.forceFeedback ? 1 : 0);
    Log("config: TriggerPedals=%d AccelAxis=%d%s BrakeAxis=%d%s PedalDeadzone=%d SteerDeadzone=%d SteerLinearity=%d",
        cfg.triggerPedals ? 1 : 0, cfg.accelAxis, cfg.accelInvert ? "(inv)" : "", cfg.brakeAxis,
        cfg.brakeInvert ? "(inv)" : "", cfg.pedalDeadzone, cfg.steerDeadzone, cfg.steerLinearity);
    Log("config: WindowScale=%d KeepAspect=%d RememberWindowSize=%d BorderlessFullscreen=%d DpiAware=%d",
        cfg.windowScale, cfg.keepAspect ? 1 : 0, cfg.rememberWindowSize ? 1 : 0, cfg.borderlessFullscreen ? 1 : 0,
        cfg.dpiAware ? 1 : 0);

    if (cfg.dpiAware) {
        // ゲームが窓を作る前に宣言する。SetProcessDpiAwarenessContext は Win10 1703+（無ければ旧 API）
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        using SetCtxFn = BOOL(WINAPI*)(HANDLE);
        auto setCtx = user32 ? reinterpret_cast<SetCtxFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")) : nullptr;
        const BOOL ok = setCtx ? setCtx(reinterpret_cast<HANDLE>(-4))  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
                               : (user32 ? SetProcessDPIAware() : FALSE);
        Log("dpi: %s -> %d (err %lu)", setCtx ? "SetProcessDpiAwarenessContext(PMv2)" : "SetProcessDPIAware", ok,
            ok ? 0UL : GetLastError());
    }

    Log("config: AspectRatio=%d:%d (widescreen x scale %.4f)", cfg.aspectW, cfg.aspectH, WidescreenScale(cfg));
    if (GraphicsHooksNeeded(cfg)) {
        InstallDirectDrawLogging();  // 版に依存しない（ワイド化の頂点加工もここから）
    }

    const GameVersion ver = DetectGameVersion();
    Log("game version: %s", GameVersionName(ver));
    if (ver != GameVersion::Jp102) {
        Log("未対応の版なのでパッチは当てない（dinput 転送のみ）");
        return;
    }
    if (cfg.windowed) {
        ApplyPatches(kWindowedJp102, std::size(kWindowedJp102));
        InstallWindowHooks();
        if (WidescreenScale(cfg) != 1.0 && cfg.hudAnchorEdges) {
            InstallHudHooks();
        }
    }
    if (cfg.d3dWindowedVideoMemory) {
        ApplyPatches(kD3DWindowedVideoMemoryJp102, std::size(kD3DWindowedVideoMemoryJp102));
    }
    if (cfg.logGraphics) {
        InstallGameTraceHooks();
    }
}

}  // namespace

const Config& GetConfig() {
    return g_config;
}

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
