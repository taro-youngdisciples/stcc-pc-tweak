// フレームの計測と上限の差し替え（jp-1.02）。
//
// ゲームは 1 ループで 1 ステップ（30Hz 固定）進め、Frame_Limit30 (0x417130) が GetTickCount の
// ビジーウェイトで待つ（15.6ms 刻みのため約 28 FPS）。詳細は stcc_jp-1.02.toml の「フレームの流れとタイミング」。
//  - [Debug] LogFrame=1: 1 秒ごとにステップ数（÷30 = 本来の速さに対する割合）、描画/省略回数、上限の待ち時間を記録
//  - [Frame] TargetFps=N: Frame_Limit30 を QueryPerformanceCounter と高精度タイマーによる N FPS の待ちに差し替える
#include "common.h"

#include <MinHook.h>

#include <intrin.h>

namespace stcc {
namespace {

constexpr std::uintptr_t kGameFrame = 0x00417400;              // Game_Frame
constexpr std::uintptr_t kFrameLimit30 = 0x00417130;           // Frame_Limit30
constexpr std::uintptr_t kFrameSkipShouldRender = 0x00445EA0;  // FrameSkip_ShouldRender のラッパー（ecx = g_FrameSkip）
// Frame_RenderA / Frame_RenderB の先頭で「描画するか」を決める呼び出しの戻り先（2 回目の呼び出しは数えない）
constexpr std::uintptr_t kRenderDecisionA = 0x00416FA9;
constexpr std::uintptr_t kRenderDecisionB = 0x004171E9;
constexpr std::uintptr_t kGameState = 0x01101060;     // g_GameState
constexpr std::uintptr_t kRaceSubstate = 0x01101064;  // g_RaceSubstateTable の添字
constexpr std::uintptr_t kFrameFlags = 0x010F61A0;    // g_FrameFlags（bit0 = 上限を使う）

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

using GameFrameFn = void(__cdecl*)();
using ShouldRenderFn = int(__cdecl*)();
using LimitFn = DWORD(__cdecl*)();
GameFrameFn g_origGameFrame = nullptr;
ShouldRenderFn g_origShouldRender = nullptr;
LimitFn g_origLimit = nullptr;

LARGE_INTEGER g_freq{};

double Now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(g_freq.QuadPart);
}

// ゲームのスレッドからしか呼ばれないのでロックしない
struct FrameStats {
    double windowStart;
    double lastStep;
    double maxInterval;
    double limiterWait;
    long steps;
    long renders;
    long skips;
    long limiterCalls;
};
FrameStats g_stats{};

void FlushStats(double now) {
    if (g_stats.windowStart == 0.0) {
        g_stats.windowStart = now;
        return;
    }
    const double elapsed = now - g_stats.windowStart;
    if (elapsed < 1.0) {
        return;
    }
    const double stepsPerSec = g_stats.steps / elapsed;
    Log("[frame/s] steps=%.1f (speed %.0f%%) rendered=%.1f skipped=%.1f limiter=%.1f wait=%.1fms/frame "
        "maxInterval=%.1fms state=%d substate=%d flags=0x%02X",
        stepsPerSec, stepsPerSec / 30.0 * 100.0, g_stats.renders / elapsed, g_stats.skips / elapsed,
        g_stats.limiterCalls / elapsed, g_stats.limiterCalls ? g_stats.limiterWait * 1000.0 / g_stats.limiterCalls : 0.0,
        g_stats.maxInterval * 1000.0, *reinterpret_cast<int*>(kGameState), *reinterpret_cast<int*>(kRaceSubstate),
        *reinterpret_cast<unsigned char*>(kFrameFlags));
    const double lastStep = g_stats.lastStep;
    g_stats = {};
    g_stats.windowStart = now;
    g_stats.lastStep = lastStep;
}

void __cdecl Hook_GameFrame() {
    if (GetConfig().logFrame) {
        const double now = Now();
        if (g_stats.lastStep != 0.0 && now - g_stats.lastStep > g_stats.maxInterval) {
            g_stats.maxInterval = now - g_stats.lastStep;
        }
        g_stats.lastStep = now;
        ++g_stats.steps;
        FlushStats(now);
    }
    g_origGameFrame();
}

int __cdecl Hook_ShouldRender() {
    const int render = g_origShouldRender();
    const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (ret == kRenderDecisionA || ret == kRenderDecisionB) {
        ++(render ? g_stats.renders : g_stats.skips);
    }
    return render;
}

// ---- 高精度の待ち
HANDLE g_timer = nullptr;
bool g_timerTried = false;
double g_nextFrame = 0.0;

void SleepSeconds(double sec) {
    if (!g_timerTried) {
        g_timerTried = true;  // DllMain ではなく最初の待ちで作る（Win10 1803 以降。無ければ Sleep(1)）
        g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        Log("frame: high resolution waitable timer %s", g_timer ? "available" : "unavailable, using Sleep(1)");
    }
    if (g_timer) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(sec * 1e7);  // 100ns 単位の相対時間
        if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(g_timer, INFINITE);
            return;
        }
    }
    Sleep(1);
}

// 前回の締め切りから 1 周期ずつ進める（遅れが 1 周期を超えたら取り戻さずに今から数え直す）
void WaitForNextFrame(double period) {
    double now = Now();
    if (g_nextFrame == 0.0 || now - g_nextFrame > period) {
        g_nextFrame = now + period;
        return;
    }
    for (double remain = g_nextFrame - now; remain > 0.0; remain = g_nextFrame - Now()) {
        if (remain > 0.002) {
            SleepSeconds(remain - 0.0015);  // 最後の 1.5ms はスピンで合わせる
        } else {
            YieldProcessor();
        }
    }
    g_nextFrame += period;
}

// 呼び出し側は戻り値を使わないが、元の関数と同じく GetTickCount の値を返しておく
DWORD __cdecl Hook_FrameLimit30() {
    const int fps = GetConfig().targetFps;
    const double t0 = Now();
    DWORD ret;
    if (fps > 0) {
        WaitForNextFrame(1.0 / fps);
        ret = GetTickCount();
    } else {
        ret = g_origLimit();
    }
    ++g_stats.limiterCalls;
    g_stats.limiterWait += Now() - t0;
    return ret;
}

void Hook(std::uintptr_t addr, LPVOID detour, LPVOID* orig, const char* name) {
    MH_STATUS st = MH_CreateHook(reinterpret_cast<LPVOID>(addr), detour, orig);
    if (st == MH_OK) {
        st = MH_EnableHook(reinterpret_cast<LPVOID>(addr));
    }
    Log("frame hook %s @0x%08X: %s", name, static_cast<unsigned>(addr), MH_StatusToString(st));
}

}  // namespace

void InstallFrameHooks() {
    QueryPerformanceFrequency(&g_freq);
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        Log("MH_Initialize 失敗: %s", MH_StatusToString(st));
        return;
    }
    Hook(kGameFrame, &Hook_GameFrame, reinterpret_cast<LPVOID*>(&g_origGameFrame), "Game_Frame");
    Hook(kFrameSkipShouldRender, &Hook_ShouldRender, reinterpret_cast<LPVOID*>(&g_origShouldRender),
         "FrameSkip_ShouldRender");
    Hook(kFrameLimit30, &Hook_FrameLimit30, reinterpret_cast<LPVOID*>(&g_origLimit), "Frame_Limit30");
}

}  // namespace stcc
