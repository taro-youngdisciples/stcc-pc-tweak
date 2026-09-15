// フレームの計測とペース配分（jp-1.02）。
//
// ゲームは 1 ループで 1 ステップ（30Hz 固定）進め、Frame_Limit30 (0x417130) が GetTickCount の
// ビジーウェイトで待つ（15.6ms 刻みのため描画は約 28 FPS）。遅れを取り戻す FrameSkip は「直近 4 フレーム ≥ 132ms」で
// 描画を省くが、正確な 30 FPS でもこの基準を超えるため省略が止まらず、ロジックは約 34 ステップ/秒（113%）で
// コマ飛びする。詳細は stcc_jp-1.02.toml の「フレームの流れとタイミング」。
//  - [Debug] LogFrame=1: 1 秒ごとにステップ数（÷30 = 本来の速さに対する割合）、描画/省略回数、待ち時間を記録
//  - [Frame] TargetFps=N: ステップの先頭（Game_Frame）で 1/N 秒ごとに正確に待ち、元の上限は待たせない。
//    描画の省略は「1 周期以上遅れて始まったステップ」だけにする（ゲームの連続省略上限と描画停止要求は守る）
#include "common.h"

#include <MinHook.h>

#include <intrin.h>

namespace stcc {
namespace {

constexpr std::uintptr_t kGameFrame = 0x00417400;              // Game_Frame
constexpr std::uintptr_t kFrameLimit30 = 0x00417130;           // Frame_Limit30
constexpr std::uintptr_t kFrameSkipShouldRender = 0x00445EA0;  // FrameSkip_ShouldRender のラッパー（ecx = g_FrameSkip）
constexpr std::uintptr_t kFrameSkipMaxSkip = 0x007F5218;       // g_FrameSkip+8: 連続して描画を省いてよい回数
constexpr std::uintptr_t kFrameSkipHold = 0x007F5248;          // g_FrameSkip+0x38: 非 0 なら描画しない
// Frame_RenderA / Frame_RenderB の先頭で「描画するか」を決める呼び出しの戻り先（描画後の 2 回目は数えない）
constexpr std::uintptr_t kRenderDecisionA = 0x00416FA9;
constexpr std::uintptr_t kRenderDecisionB = 0x004171E9;
constexpr std::uintptr_t kGameState = 0x01101060;     // g_GameState
constexpr std::uintptr_t kRaceSubstate = 0x01101064;  // g_RaceSubstateTable の添字
constexpr std::uintptr_t kFrameFlags = 0x010F61A0;    // g_FrameFlags（bit0 = 上限を使う）

// これ以上遅れたら（ロード中など）取り戻さずに今から数え直す周期数
constexpr double kResyncPeriods = 4.0;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

constexpr std::uintptr_t kFrameRenderA = 0x00416FA0;  // Frame_RenderA
constexpr std::uintptr_t kFrameRenderB = 0x004171E0;  // Frame_RenderB

using GameFrameFn = void(__cdecl*)();
using ShouldRenderFn = int(__cdecl*)();
using LimitFn = DWORD(__cdecl*)();
using RenderFn = void(__cdecl*)();
GameFrameFn g_origGameFrame = nullptr;
ShouldRenderFn g_origShouldRender = nullptr;
LimitFn g_origLimit = nullptr;
RenderFn g_origRenderA = nullptr;
RenderFn g_origRenderB = nullptr;
int g_renderDepth = 0;  // Frame_RenderA/B の実行中なら 1 以上（ゲームのスレッドだけが触る）

void __cdecl Hook_FrameRenderA() {
    ++g_renderDepth;
    g_origRenderA();
    --g_renderDepth;
}

void __cdecl Hook_FrameRenderB() {
    ++g_renderDepth;
    g_origRenderB();
    --g_renderDepth;
}

LARGE_INTEGER g_freq{};

double Now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(g_freq.QuadPart);
}

// ---- 計測（ゲームのスレッドからしか呼ばれないのでロックしない）
struct FrameStats {
    double windowStart;
    double lastStep;
    double maxInterval;
    double waitTotal;
    long waits;
    long steps;
    long renders;
    long skips;
    long lateSteps;
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
    Log("[frame/s] steps=%.1f (speed %.0f%%) rendered=%.1f skipped=%.1f late=%.1f limiter=%.1f wait=%.1fms "
        "maxInterval=%.1fms state=%d substate=%d flags=0x%02X",
        stepsPerSec, stepsPerSec / 30.0 * 100.0, g_stats.renders / elapsed, g_stats.skips / elapsed,
        g_stats.lateSteps / elapsed, g_stats.limiterCalls / elapsed,
        g_stats.waits ? g_stats.waitTotal * 1000.0 / g_stats.waits : 0.0, g_stats.maxInterval * 1000.0,
        *reinterpret_cast<int*>(kGameState), *reinterpret_cast<int*>(kRaceSubstate),
        *reinterpret_cast<unsigned char*>(kFrameFlags));
    const double lastStep = g_stats.lastStep;
    g_stats = {};
    g_stats.windowStart = now;
    g_stats.lastStep = lastStep;
}

// ---- 高精度の待ち
HANDLE g_timer = nullptr;
bool g_timerTried = false;

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

// ---- ペース配分（TargetFps > 0）
struct Pacer {
    double next;           // このステップを始めてよい時刻
    double lateness;       // 今のステップが締め切りから何秒遅れて始まったか
    int consecutiveSkips;  // 遅れのために続けて描画を省いた回数
    int render;            // 今のステップの描画判定
    bool decided;
};
Pacer g_pacer{};

// 締め切りまで待ち、遅れ（秒）を返す。締め切りは 1 周期ずつ進めるので、少しの遅れは次のステップで取り戻す
double WaitStep(double period) {
    double now = Now();
    if (g_pacer.next == 0.0 || now - g_pacer.next > period * kResyncPeriods) {
        g_pacer.next = now + period;
        return 0.0;
    }
    const double lateness = now - g_pacer.next;
    for (double remain = -lateness; remain > 0.0; remain = g_pacer.next - Now()) {
        if (remain > 0.002) {
            SleepSeconds(remain - 0.0015);  // 最後の 1.5ms はスピンで合わせる
        } else {
            YieldProcessor();
        }
    }
    g_pacer.next += period;
    return lateness > 0.0 ? lateness : 0.0;
}

void __cdecl Hook_GameFrame() {
    const Config& cfg = GetConfig();
    if (cfg.targetFps > 0) {
        const double period = 1.0 / cfg.targetFps;
        const double t0 = Now();
        g_pacer.lateness = WaitStep(period);
        g_pacer.decided = false;
        g_stats.waitTotal += Now() - t0;
        ++g_stats.waits;
        if (g_pacer.lateness >= period) {
            ++g_stats.lateSteps;
        }
    }
    if (cfg.logFrame) {
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

int DecideRender(int fps) {
    if (*reinterpret_cast<int*>(kFrameSkipHold) != 0) {
        return 0;  // ゲーム自身の描画停止要求（元の FrameSkip_ShouldRender と同じ条件）
    }
    const int maxSkip = *reinterpret_cast<int*>(kFrameSkipMaxSkip);
    if (g_pacer.lateness >= 1.0 / fps && g_pacer.consecutiveSkips < maxSkip) {
        ++g_pacer.consecutiveSkips;
        return 0;
    }
    g_pacer.consecutiveSkips = 0;
    return 1;
}

// 「描画するか」を決める 1 回目の呼び出しか。Frame_RenderA/B をフックしていると、先頭付近の call は
// MinHook のトランポリンへ移されるので、戻り先がトランポリン内のときも 1 回目とみなす
bool IsRenderDecisionCall(std::uintptr_t ret) {
    if (ret == kRenderDecisionA || ret == kRenderDecisionB) {
        return true;
    }
    for (auto* tramp : {static_cast<void*>(g_origRenderA), static_cast<void*>(g_origRenderB)}) {
        const auto t = reinterpret_cast<std::uintptr_t>(tramp);
        if (t && ret > t && ret < t + 64) {
            return true;
        }
    }
    return false;
}

int __cdecl Hook_ShouldRender() {
    const int fps = GetConfig().targetFps;
    int render;
    if (fps > 0) {
        // 1 フレームに 2 回呼ばれる（描画するか / 描画後に上限へ進むか）。どちらにも同じ値を返す
        if (!g_pacer.decided) {
            g_pacer.render = DecideRender(fps);
            g_pacer.decided = true;
        }
        render = g_pacer.render;
    } else {
        render = g_origShouldRender();
    }
    if (IsRenderDecisionCall(reinterpret_cast<std::uintptr_t>(_ReturnAddress()))) {
        ++(render ? g_stats.renders : g_stats.skips);
    }
    return render;
}

// 呼び出し側は戻り値を使わないが、元の関数と同じく GetTickCount の値を返しておく
DWORD __cdecl Hook_FrameLimit30() {
    ++g_stats.limiterCalls;
    if (GetConfig().targetFps > 0) {
        return GetTickCount();  // 待ちは Hook_GameFrame で行う
    }
    const double t0 = Now();
    const DWORD ret = g_origLimit();
    g_stats.waitTotal += Now() - t0;
    ++g_stats.waits;
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
    if (GetConfig().logDrawList) {
        // Frame_RenderA/B は引数なし・戻り値なし（呼び出し元 0x417471 / 0x41746A は結果を使わない）
        Hook(kFrameRenderA, &Hook_FrameRenderA, reinterpret_cast<LPVOID*>(&g_origRenderA), "Frame_RenderA");
        Hook(kFrameRenderB, &Hook_FrameRenderB, reinterpret_cast<LPVOID*>(&g_origRenderB), "Frame_RenderB");
    }
}

bool InRenderPhase() {
    return g_renderDepth > 0;
}

}  // namespace stcc
