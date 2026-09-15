// 補間（60fps 表示）の可否を調べるための描画命令ログ（[Debug] LogDrawList=1）。
//
// ゲームは自前で投影した TL 頂点を DrawPrimitive で渡す。フレーム間補間の方式を決めるために、1 秒ごとに次を記録する:
//  - 3D の描画命令が Frame_RenderA/B（描画処理）の中から呼ばれたか、ゲームの処理（状態関数）の中か、と呼び出し元
//  - 前フレームと同じ位置に同じ命令（プリミティブ種別・頂点数・テクスチャ・呼び出し元）が来た割合
//  - 対応がとれた命令の頂点が 1 フレームで画面上をどれだけ動いたか
// D3D の呼び出しはゲームのスレッドからだけなのでロックしない。
#include "common.h"

#include <MinHook.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace stcc {
namespace {

constexpr unsigned kTlVertexType = 3;  // D3DVT_TLVERTEX
constexpr std::uintptr_t kPolyListFlush = 0x00443A00;  // PolyList_Flush (thiscall)
constexpr std::uintptr_t kQuadXYScale = 0x004A7A0C;    // float: 記録の整数座標 → 画面座標
constexpr float kSmallMovePx = 48.0f;  // これ未満の移動なら単純な線形補間でも破綻しにくいとみなす

struct Entry {
    unsigned prim;
    unsigned count;
    unsigned texture;
    void* caller;
    size_t xyOffset;  // g_*XY の先頭（TL 以外は SIZE_MAX）
};

std::vector<Entry> g_cur;
std::vector<Entry> g_prev;
std::vector<float> g_curXY;  // sx, sy の組
std::vector<float> g_prevXY;
unsigned g_texture = 0;

struct CallerCount {
    void* caller;
    long inRender;
    long outside;
};

struct Stats {
    DWORD windowStart;
    long frames;
    long draws;
    long drawsInRender;
    long comparedFrames;
    long sameCountFrames;
    long comparable;      // 比較した命令数（両フレームの短い方）
    long matched;         // 同じ位置で一致した命令数
    long prefix;          // 先頭から連続して一致した命令数の合計
    long movedEntries;    // 移動量を測った命令数（TL の一致した命令）
    long smallMoves;      // その中で最大移動が kSmallMovePx 未満
    double moveSum;       // 命令ごとの最大移動の合計
    float moveMax;
    CallerCount callers[24];
};
Stats g_stats{};

void CountCaller(void* caller, bool inRender) {
    for (auto& c : g_stats.callers) {
        if (c.caller == caller || !c.caller) {
            c.caller = caller;
            ++(inRender ? c.inRender : c.outside);
            return;
        }
    }
}

void Compare() {
    if (g_prev.empty() || g_cur.empty()) {
        return;
    }
    ++g_stats.comparedFrames;
    if (g_prev.size() == g_cur.size()) {
        ++g_stats.sameCountFrames;
    }
    const size_t n = g_prev.size() < g_cur.size() ? g_prev.size() : g_cur.size();
    g_stats.comparable += static_cast<long>(n);
    bool inPrefix = true;
    for (size_t i = 0; i < n; ++i) {
        const Entry& a = g_prev[i];
        const Entry& b = g_cur[i];
        const bool same = a.prim == b.prim && a.count == b.count && a.texture == b.texture && a.caller == b.caller;
        if (!same) {
            inPrefix = false;
            continue;
        }
        ++g_stats.matched;
        if (inPrefix) {
            ++g_stats.prefix;
        }
        if (a.xyOffset == SIZE_MAX || b.xyOffset == SIZE_MAX) {
            continue;
        }
        float maxMove = 0.0f;
        for (unsigned v = 0; v < a.count; ++v) {
            const float dx = g_curXY[b.xyOffset + v * 2] - g_prevXY[a.xyOffset + v * 2];
            const float dy = g_curXY[b.xyOffset + v * 2 + 1] - g_prevXY[a.xyOffset + v * 2 + 1];
            const float d = std::sqrt(dx * dx + dy * dy);
            maxMove = d > maxMove ? d : maxMove;
        }
        ++g_stats.movedEntries;
        g_stats.moveSum += maxMove;
        g_stats.moveMax = maxMove > g_stats.moveMax ? maxMove : g_stats.moveMax;
        if (maxMove < kSmallMovePx) {
            ++g_stats.smallMoves;
        }
    }
}

// ---- ポリゴン一覧の積み込み順での比較（並べ替え前）
// 記録（16 バイト）: +0 材質*, +4 奥行き, +8 画面座標 int (x,y)×4 へのポインタ, +0xC フラグ
struct PolyRecord {
    void* material;
    int depth;
    int* xy;
    unsigned flags;
};

struct PolyListState {
    void* list;
    std::vector<void*> material;
    std::vector<float> xy;  // 1 件につき 8 個（4 頂点の x, y）
};
PolyListState g_polyLists[4];

struct PolyStats {
    long flushes;
    long records;
    long compared;
    long sameCount;
    long comparable;
    long matched;
    long movedEntries;
    long smallMoves;
    double moveSum;
    float moveMax;
};
PolyStats g_poly{};

void MeasurePolyList(void* self) {
    if (!GetConfig().logDrawList || !self) {
        return;
    }
    const auto* base = static_cast<const unsigned char*>(self);
    const auto* begin = *reinterpret_cast<PolyRecord* const*>(base + 0x9C);
    const auto* end = *reinterpret_cast<PolyRecord* const*>(base + 0xA0);
    if (!begin || end < begin || end - begin > 20000) {
        return;
    }
    PolyListState* st = nullptr;
    for (auto& s : g_polyLists) {
        if (s.list == self || !s.list) {
            s.list = self;
            st = &s;
            break;
        }
    }
    if (!st) {
        return;
    }
    const float scale = *reinterpret_cast<const float*>(kQuadXYScale);
    const size_t n = static_cast<size_t>(end - begin);
    std::vector<void*> material(n);
    std::vector<float> xy(n * 8);
    for (size_t i = 0; i < n; ++i) {
        material[i] = begin[i].material;
        for (int k = 0; k < 8; ++k) {
            xy[i * 8 + k] = begin[i].xy ? begin[i].xy[k] * scale : 0.0f;
        }
    }
    ++g_poly.flushes;
    g_poly.records += static_cast<long>(n);
    if (!st->material.empty() && n > 0) {
        ++g_poly.compared;
        if (st->material.size() == n) {
            ++g_poly.sameCount;
        }
        const size_t m = st->material.size() < n ? st->material.size() : n;
        g_poly.comparable += static_cast<long>(m);
        for (size_t i = 0; i < m; ++i) {
            if (st->material[i] != material[i]) {
                continue;
            }
            ++g_poly.matched;
            float maxMove = 0.0f;
            for (int v = 0; v < 4; ++v) {
                const float dx = xy[i * 8 + v * 2] - st->xy[i * 8 + v * 2];
                const float dy = xy[i * 8 + v * 2 + 1] - st->xy[i * 8 + v * 2 + 1];
                const float d = std::sqrt(dx * dx + dy * dy);
                maxMove = d > maxMove ? d : maxMove;
            }
            ++g_poly.movedEntries;
            g_poly.moveSum += maxMove;
            g_poly.moveMax = maxMove > g_poly.moveMax ? maxMove : g_poly.moveMax;
            if (maxMove < kSmallMovePx) {
                ++g_poly.smallMoves;
            }
        }
    }
    st->material.swap(material);
    st->xy.swap(xy);
}

using PolyFlushFn = void(__fastcall*)(void* self, void* edx);
PolyFlushFn g_origPolyFlush = nullptr;

void __fastcall Hook_PolyListFlush(void* self, void* edx) {
    MeasurePolyList(self);
    g_origPolyFlush(self, edx);
}

void Flush() {
    const DWORD now = GetTickCount();
    if (g_stats.windowStart == 0) {
        g_stats.windowStart = now;
        return;
    }
    if (now - g_stats.windowStart < 1000) {
        return;
    }
    if (g_stats.frames > 0) {
        auto pct = [](long a, long b) { return b ? 100.0 * a / b : 0.0; };
        Log("[drawlist/s] frames=%ld draws/frame=%.0f inRender=%.0f%% sameCount=%.0f%% matched=%.1f%% prefix=%.1f%% "
            "move avg=%.1fpx max=%.0fpx small(<%.0fpx)=%.1f%%",
            g_stats.frames, static_cast<double>(g_stats.draws) / g_stats.frames,
            pct(g_stats.drawsInRender, g_stats.draws), pct(g_stats.sameCountFrames, g_stats.comparedFrames),
            pct(g_stats.matched, g_stats.comparable), pct(g_stats.prefix, g_stats.comparable),
            g_stats.movedEntries ? g_stats.moveSum / g_stats.movedEntries : 0.0, g_stats.moveMax, kSmallMovePx,
            pct(g_stats.smallMoves, g_stats.movedEntries));
        std::string callers;
        for (const auto& c : g_stats.callers) {
            if (c.caller) {
                char b[64];
                std::snprintf(b, sizeof(b), " %p:%ld/%ld", c.caller, c.inRender, c.outside);
                callers += b;
            }
        }
        Log("[drawlist/s] callers (inRender/outside):%s", callers.c_str());
        if (g_poly.flushes > 0) {
            Log("[polylist/s] flushes=%ld records/flush=%.0f sameCount=%.0f%% matched(insertion order)=%.1f%% "
                "move avg=%.1fpx max=%.0fpx small(<%.0fpx)=%.1f%%",
                g_poly.flushes, static_cast<double>(g_poly.records) / g_poly.flushes,
                pct(g_poly.sameCount, g_poly.compared), pct(g_poly.matched, g_poly.comparable),
                g_poly.movedEntries ? g_poly.moveSum / g_poly.movedEntries : 0.0, g_poly.moveMax, kSmallMovePx,
                pct(g_poly.smallMoves, g_poly.movedEntries));
        }
    }
    g_poly = PolyStats{};
    g_stats = Stats{};
    g_stats.windowStart = now;
}

}  // namespace

void DrawList_OnTexture(unsigned handle) {
    g_texture = handle;
}

void DrawList_OnDraw(unsigned prim, unsigned vertexType, const void* verts, unsigned count, void* caller) {
    if (!GetConfig().logDrawList) {
        return;
    }
    const bool inRender = InRenderPhase();
    ++g_stats.draws;
    if (inRender) {
        ++g_stats.drawsInRender;
    }
    CountCaller(caller, inRender);
    Entry e{prim, count, g_texture, caller, SIZE_MAX};
    if (vertexType == kTlVertexType && verts && count > 0 && count < 4096) {
        // D3DTLVERTEX は 32 バイトで、先頭が sx, sy（float）
        e.xyOffset = g_curXY.size();
        const auto* p = static_cast<const unsigned char*>(verts);
        for (unsigned i = 0; i < count; ++i) {
            float xy[2];
            std::memcpy(xy, p + i * 32, sizeof(xy));
            g_curXY.push_back(xy[0]);
            g_curXY.push_back(xy[1]);
        }
    }
    g_cur.push_back(e);
}

void InstallDrawListHooks() {
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        Log("MH_Initialize 失敗: %s", MH_StatusToString(st));
        return;
    }
    auto target = reinterpret_cast<LPVOID>(kPolyListFlush);
    st = MH_CreateHook(target, &Hook_PolyListFlush, reinterpret_cast<LPVOID*>(&g_origPolyFlush));
    if (st == MH_OK) {
        st = MH_EnableHook(target);
    }
    Log("drawlist hook PolyList_Flush @0x%08X: %s", static_cast<unsigned>(kPolyListFlush), MH_StatusToString(st));
}

void DrawList_OnEndScene() {
    if (!GetConfig().logDrawList) {
        return;
    }
    if (!g_cur.empty()) {
        ++g_stats.frames;
        Compare();
        g_prev.swap(g_cur);
        g_prevXY.swap(g_curXY);
    }
    g_cur.clear();
    g_curXY.clear();
    Flush();
}

}  // namespace stcc
