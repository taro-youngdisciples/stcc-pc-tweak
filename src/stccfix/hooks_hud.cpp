// HUD（ゲームのソフトウェア 2D スプライト）を横長画面の左右端へ寄せる。jp-1.02 専用。
//
// ワイド化（hooks_ddraw.cpp）では、ゲームの 2D 書き込みを中央 4:3 部分へずらして渡しているので、
// HUD は伸びないが画面中央に集まる。レース中（3D を描いたフレーム）だけ、Spr_RenderQueue (0x46D020) の直前に
// スプライトキューの x を「左の塊は左端、右の塊は右端、中央の塊はそのまま中央」へ動かし、
// 描画関数には広い描画面全体と広い画面幅（g_SprScreenW）を見せる。
//
// 数字の列（ラップタイムなど）が桁ごとにバラけないよう、横に近接するスプライトを 1 つの塊として判定する。
#include "common.h"

#include <MinHook.h>
#include <ddraw.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <vector>

namespace stcc {
namespace {

constexpr std::uintptr_t kSprRenderQueue = 0x0046D020;  // Spr_RenderQueue(DDSURFACEDESC*)
constexpr std::uintptr_t kSprScreenW = 0x010E4C40;      // g_SprScreenW
constexpr std::uintptr_t kSprScreenH = 0x010E4C44;      // g_SprScreenH
constexpr std::uintptr_t kSprPatterns = 0x010E4C48;     // g_SprPatterns（12 バイト/件、+4 幅）
constexpr std::uintptr_t kSprQueueCount = 0x010EBCC8;   // g_SprQueueCount
constexpr std::uintptr_t kSprQueue = 0x010EBCCC;        // g_SprQueue（16 バイト/件）
// レース画面モード中か（Gfx_GetModeSize がメニューの 640x480 固定か Screen Mode の解像度かを決めるフラグ）。
// 車選択など 3D を描くメニューで HUD 寄せが働かないよう、3D 描画と合わせて判定する
constexpr std::uintptr_t kRaceScreenFlag = 0x0056C190;

#pragma pack(push, 1)
struct SprEntry {
    WORD pattern;
    short x;
    short y;
    short srcY;
    short h;
    short clipA;
    short clipB;
    WORD flags;  // bit0 = はみ出しあり（クリップ描画）
};
#pragma pack(pop)

using RenderQueueFn = void(__cdecl*)(LPDDSURFACEDESC);
RenderQueueFn g_origRenderQueue = nullptr;

short PatternWidth(WORD id) {
    return *reinterpret_cast<const short*>(kSprPatterns + static_cast<std::uintptr_t>(id) * 12 + 4);
}

int FindRoot(std::vector<int>& parent, int i) {
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}

// キュー内の各スプライトを、塊ごとに左端 / 中央 / 右端へ動かす（ゲーム座標 → 広い描画面の座標）
void AnchorQueue(const WideInfo& wi) {
    const int count = *reinterpret_cast<const int*>(kSprQueueCount);
    if (count <= 0 || count > 2048) {
        return;
    }
    auto* q = reinterpret_cast<SprEntry*>(kSprQueue);

    struct Box {
        int x0, y0, x1, y1;
    };
    static thread_local std::vector<Box> boxes;
    static thread_local std::vector<int> parent;
    boxes.resize(static_cast<size_t>(count));
    parent.resize(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const int w = PatternWidth(q[i].pattern);
        boxes[i] = {q[i].x, q[i].y, q[i].x + w, q[i].y + q[i].h};
        parent[i] = i;
    }

    // 同じ行（縦に高さの半分以上重なる）で、横に gapX 以内に並んでいれば同じ塊。
    // 上下に 1px 接しているだけの別の表示（コースレコードの数字と大きなラップタイム等）はつなげない
    const int gapX = std::max(2, static_cast<int>(wi.baseW / 80));
    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j) {
            const Box& a = boxes[i];
            const Box& b = boxes[j];
            const int overlapY = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
            const int minH = std::min(a.y1 - a.y0, b.y1 - b.y0);
            if (a.x0 - gapX < b.x1 && b.x0 - gapX < a.x1 && overlapY * 2 >= minH && minH > 0) {
                const int ra = FindRoot(parent, i);
                const int rb = FindRoot(parent, j);
                if (ra != rb) {
                    parent[rb] = ra;
                }
            }
        }
    }

    static thread_local std::vector<Box> clusters;
    clusters.assign(static_cast<size_t>(count), Box{INT_MAX, INT_MAX, INT_MIN, INT_MIN});
    for (int i = 0; i < count; ++i) {
        Box& c = clusters[FindRoot(parent, i)];
        c.x0 = std::min(c.x0, boxes[i].x0);
        c.y0 = std::min(c.y0, boxes[i].y0);
        c.x1 = std::max(c.x1, boxes[i].x1);
        c.y1 = std::max(c.y1, boxes[i].y1);
    }

    const int screenW = static_cast<int>(wi.wideW);
    const int screenH = *reinterpret_cast<const int*>(kSprScreenH);
    const int center = static_cast<int>(wi.baseW) / 2;
    for (int i = 0; i < count; ++i) {
        const Box& c = clusters[FindRoot(parent, i)];
        const int cw = c.x1 - c.x0;
        const int cc = (c.x0 + c.x1) / 2;
        int shift = static_cast<int>(wi.offset);  // 既定: 中央
        if (cw < wi.baseW * 6 / 10 && std::abs(cc - center) > wi.baseW * 12 / 100) {
            shift = cc < center ? 0 : static_cast<int>(wi.offset) * 2;
        }
        q[i].x = static_cast<short>(q[i].x + shift);
        const bool clip = q[i].x < 0 || q[i].y < 0 || q[i].x + PatternWidth(q[i].pattern) >= screenW ||
                          q[i].y + q[i].h >= screenH;
        q[i].flags = static_cast<WORD>((q[i].flags & ~1u) | (clip ? 1u : 0u));
    }
}

void __cdecl Hook_SprRenderQueue(LPDDSURFACEDESC desc) {
    WideInfo wi{};
    const bool raceScreen = *reinterpret_cast<const int*>(kRaceScreenFlag) != 0;
    if (!desc || !raceScreen || !GetWideInfo(&wi) || !LastLockHad3D() ||
        desc->dwWidth != static_cast<DWORD>(wi.baseW)) {
        g_origRenderQueue(desc);
        return;
    }
    const LONG bpp = static_cast<LONG>(desc->ddpfPixelFormat.dwRGBBitCount / 8);
    if (bpp <= 0) {
        g_origRenderQueue(desc);
        return;
    }
    // Lock フックで中央 4:3 へずらされた desc を、広い描画面の先頭に戻して渡す
    DDSURFACEDESC wide = *desc;
    wide.lpSurface = static_cast<BYTE*>(desc->lpSurface) - wi.offset * bpp;
    wide.dwWidth = static_cast<DWORD>(wi.wideW);

    AnchorQueue(wi);
    int& sprScreenW = *reinterpret_cast<int*>(kSprScreenW);
    const int saved = sprScreenW;
    sprScreenW = static_cast<int>(wi.wideW);
    g_origRenderQueue(&wide);
    sprScreenW = saved;
}

}  // namespace

void InstallHudHooks() {
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        Log("MH_Initialize 失敗: %s", MH_StatusToString(st));
        return;
    }
    auto target = reinterpret_cast<LPVOID>(kSprRenderQueue);
    st = MH_CreateHook(target, &Hook_SprRenderQueue, reinterpret_cast<LPVOID*>(&g_origRenderQueue));
    if (st == MH_OK) {
        st = MH_EnableHook(target);
    }
    Log("hud hook Spr_RenderQueue @0x%08X: %s", static_cast<unsigned>(kSprRenderQueue), MH_StatusToString(st));
}

}  // namespace stcc
