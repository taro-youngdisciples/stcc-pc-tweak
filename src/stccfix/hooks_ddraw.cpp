// DirectDraw / Direct3D (DX5 世代) の呼び出しログ。調査用。
//
// COM メソッドは vtable の該当スロットを差し替える。元の関数ポインタは (vtable, index) ごとに保持するので、
// ラッパー（dgVoodoo 等）が複数インターフェースで実装を共有していても取り違えない。
// 描画ループで毎フレーム呼ばれるメソッド（Lock/Blt/Flip 等）はフックしない。
#include "common.h"
#include "vtable_hook.h"

#define DIRECT3D_VERSION 0x0500
#include <ddraw.h>
#include <d3d.h>

#include <intrin.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace stcc {
namespace {

// このファイルのフックはワイド化のためにログ無効でも常駐するので、記録は [Debug] LogGraphics=1 のときだけ行う。
// 以降の Log(...) はすべて GLog(...) に置き換わる（(Log) と括弧で書くと本物を呼べる）
template <class... Args>
void GLog(const char* fmt, Args... args) {
    if (GetConfig().logGraphics) {
        (Log)(fmt, args...);
    }
}
#define Log(...) GLog(__VA_ARGS__)

// ---------------------------------------------------------------- ワイド化（横長の描画面）
// Direct3D の描画先（640x480 / 320x240）を表示比率の幅に広げて作り、
//  - ゲームの 2D 書き込み（Lock）は中央 4:3 の位置へずらして渡す → HUD は伸びない
//  - 3D の TL 頂点は x を offset だけ右へずらす（縮めない）→ 横に視野が広がる
//  - 表示用の Blt は広げた幅ごと転送する
struct WideTarget {
    IDirectDrawSurface* surf = nullptr;
    LONG baseW = 0;  // ゲームが要求した幅（640 / 320）
    LONG baseH = 0;
    LONG wideW = 0;   // 実際に作った幅
    LONG offset = 0;  // 左右の余白（ゲーム座標 x=0 が描画面のどこに来るか）
};
WideTarget g_wide;
LONG g_drawsSinceLock = 0;  // 前回の描画先ロック以降の DrawPrimitive 回数（0 なら 2D だけのフレーム）

LONG WideOffsetFor(LONG baseW, LONG baseH) {
    const Config& cfg = GetConfig();
    if (WidescreenScale(cfg) == 1.0) {
        return 0;
    }
    const double wantW = static_cast<double>(baseH) * cfg.aspectW / cfg.aspectH;
    const LONG off = static_cast<LONG>(std::lround((wantW - baseW) / 2.0));
    return off > 0 ? off : 0;
}

bool IsWideSurface(IUnknown* s) {
    return s && g_wide.surf && static_cast<void*>(s) == static_cast<void*>(g_wide.surf);
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
HRESULT STDMETHODCALLTYPE Vp2_SetViewport(IDirect3DViewport2* self, LPD3DVIEWPORT vp);
HRESULT STDMETHODCALLTYPE Vp2_SetViewport2(IDirect3DViewport2* self, LPD3DVIEWPORT2 vp);
HRESULT STDMETHODCALLTYPE Vp2_Clear(IDirect3DViewport2* self, DWORD count, LPD3DRECT rects, DWORD flags);

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

    // ワイド化: Direct3D の描画先（主画面以外で 3DDEVICE 付き、640x480 / 320x240）だけを横に広げて作る
    DDSURFACEDESC wideDesc{};
    LPDDSURFACEDESC pass = desc;
    LONG offset = 0;
    if (desc && (desc->dwFlags & DDSD_WIDTH) && (desc->dwFlags & DDSD_HEIGHT) &&
        (desc->ddsCaps.dwCaps & DDSCAPS_3DDEVICE) && !(desc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) &&
        ((desc->dwWidth == 640 && desc->dwHeight == 480) || (desc->dwWidth == 320 && desc->dwHeight == 240))) {
        offset = WideOffsetFor(static_cast<LONG>(desc->dwWidth), static_cast<LONG>(desc->dwHeight));
        if (offset > 0) {
            wideDesc = *desc;
            wideDesc.dwWidth = desc->dwWidth + 2 * static_cast<DWORD>(offset);
            pass = &wideDesc;
        }
    }
    HRESULT hr = fn(self, pass, out, outer);
    if (pass != desc && SUCCEEDED(hr) && out && *out) {
        g_wide.surf = *out;
        g_wide.baseW = static_cast<LONG>(desc->dwWidth);
        g_wide.baseH = static_cast<LONG>(desc->dwHeight);
        g_wide.wideW = static_cast<LONG>(wideDesc.dwWidth);
        g_wide.offset = offset;
        (Log)("widescreen: render surface %ldx%ld -> %ldx%ld (offset %ld) surf=%p", g_wide.baseW, g_wide.baseH,
              g_wide.wideW, g_wide.baseH, offset, static_cast<void*>(*out));
    }
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
    // ゲームは毎フレーム viewport を作り直す（D3D_SetupViewport_Inner 0x440EE0）ので、最初の数回と失敗だけ記録する
    static LONG calls = 0;
    LONG n = InterlockedIncrement(&calls);
    if (n <= 3 || FAILED(hr)) {
        Log("IDirect3D2::CreateViewport #%ld -> 0x%08lX%s", n, static_cast<unsigned long>(hr),
            n == 3 ? "（以降は失敗時のみ記録）" : "");
    }
    if (SUCCEEDED(hr) && out && *out) {
        PatchVtable(*out, 5, Vp2_SetViewport, "IDirect3DViewport2::SetViewport");
        PatchVtable(*out, 12, Vp2_Clear, "IDirect3DViewport2::Clear");
        PatchVtable(*out, 17, Vp2_SetViewport2, "IDirect3DViewport2::SetViewport2");
    }
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

// ---------------------------------------------------------------- IDirect3DDevice2: 描画統計（ワイド化調査用）
// 毎フレーム呼ばれるメソッドは個別に記録せず、1 秒ごとに集計を出す
struct DrawStats {
    DWORD windowStart = 0;
    long frames = 0;
    long drawPrim = 0;
    long drawIndexed = 0;
    long vertices = 0;
    long byVertexType[4] = {};  // 0:? 1:D3DVT_VERTEX 2:D3DVT_LVERTEX 3:D3DVT_TLVERTEX
    long setTransform = 0;
    long setRenderState = 0;
    long bgQuads = 0;  // ワイド化で「画面幅いっぱいの背景」と判定した数
    long blt = 0;
    long bltFast = 0;
    long bltMinX = 1000000, bltMaxX = -1000000;  // Blt/BltFast の転送先 x 範囲
    long bltMinW = 1000000, bltMaxW = -1000000;  // 転送先の幅
    // Lock の呼び出し元（ゲーム側の戻りアドレス）ごとの回数。HUD 等のソフトウェア描画ルーチン特定用
    struct Caller {
        void* addr;
        long count;
        long w;  // ロックした面の幅（テクスチャか描画先かの目安）
        long h;
    };
    Caller lockCallers[16] = {};
    long locks = 0;
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    float minZ = 1e9f, maxZ = -1e9f, minRhw = 1e9f, maxRhw = -1e9f;
};
DrawStats g_draw;
SRWLOCK g_drawLock = SRWLOCK_INIT;

void AccumulateVertices(D3DVERTEXTYPE type, LPVOID verts, DWORD count) {
    if (type >= 0 && type < 4) {
        g_draw.byVertexType[type] += 1;
    }
    g_draw.vertices += static_cast<long>(count);
    if (type != D3DVT_TLVERTEX || !verts) {
        return;
    }
    const auto* v = static_cast<const D3DTLVERTEX*>(verts);
    for (DWORD i = 0; i < count; ++i) {
        const D3DTLVERTEX& t = v[i];
        g_draw.minX = t.sx < g_draw.minX ? t.sx : g_draw.minX;
        g_draw.maxX = t.sx > g_draw.maxX ? t.sx : g_draw.maxX;
        g_draw.minY = t.sy < g_draw.minY ? t.sy : g_draw.minY;
        g_draw.maxY = t.sy > g_draw.maxY ? t.sy : g_draw.maxY;
        g_draw.minZ = t.sz < g_draw.minZ ? t.sz : g_draw.minZ;
        g_draw.maxZ = t.sz > g_draw.maxZ ? t.sz : g_draw.maxZ;
        g_draw.minRhw = t.rhw < g_draw.minRhw ? t.rhw : g_draw.minRhw;
        g_draw.maxRhw = t.rhw > g_draw.maxRhw ? t.rhw : g_draw.maxRhw;
    }
}

void FlushDrawStatsLocked() {
    const DWORD now = GetTickCount();
    if (g_draw.windowStart == 0) {
        g_draw.windowStart = now;
        return;
    }
    if (now - g_draw.windowStart < 1000) {
        return;
    }
    if (g_draw.frames > 0) {
        Log("[draw/s] frames=%ld DP=%ld DIP=%ld verts=%ld vtype{1:%ld 2:%ld 3:%ld} SetTransform=%ld SetRenderState=%ld "
            "TL x[%.1f..%.1f] y[%.1f..%.1f] z[%.4f..%.4f] rhw[%.5f..%.5f] bgQuads=%ld",
            g_draw.frames, g_draw.drawPrim, g_draw.drawIndexed, g_draw.vertices, g_draw.byVertexType[1],
            g_draw.byVertexType[2], g_draw.byVertexType[3], g_draw.setTransform, g_draw.setRenderState, g_draw.minX,
            g_draw.maxX, g_draw.minY, g_draw.maxY, g_draw.minZ, g_draw.maxZ, g_draw.minRhw, g_draw.maxRhw,
            g_draw.bgQuads);
        Log("[blt/s] Blt=%ld BltFast=%ld dest x[%ld..%ld] w[%ld..%ld]", g_draw.blt, g_draw.bltFast,
            g_draw.bltMinX, g_draw.bltMaxX, g_draw.bltMinW, g_draw.bltMaxW);
        if (g_draw.locks > 0) {
            std::string callers;
            for (const auto& c : g_draw.lockCallers) {
                if (c.addr) {
                    char b[64];
                    std::snprintf(b, sizeof(b), " %p:%ld(%ldx%ld)", c.addr, c.count, c.w, c.h);
                    callers += b;
                }
            }
            Log("[lock/s] Lock=%ld callers:%s", g_draw.locks, callers.c_str());
        }
    }
    g_draw = DrawStats{};
    g_draw.windowStart = now;
}

// ---------------------------------------------------------------- ワイド化: 3D の TL 頂点
// ゲームは自前で 640x480 に投影した TL 頂点だけで描く（SetTransform 0 回、DrawPrimitive + D3DVT_TLVERTEX）。
// 横長の描画面に対して x を offset だけ右へずらせば、中央 4:3 は元どおりで、左右にはみ出していた部分が見えるようになる。
// ゲームのカリングは 4:3 前提なので、画面端で物体が湧く問題はゲーム側の修正で別途対処する。
thread_local std::vector<D3DTLVERTEX> t_wideVerts;

// 画面幅いっぱいの背景ポリゴン（空など）か: 全頂点の x がゲーム座標の左端(0)か右端(baseW)にぴったり乗っている
bool IsFullWidthQuad(const D3DTLVERTEX* v, DWORD count) {
    if (count < 3 || count > 8) {
        return false;
    }
    const float right = static_cast<float>(g_wide.baseW);
    bool hasLeft = false;
    bool hasRight = false;
    for (DWORD i = 0; i < count; ++i) {
        if (std::fabs(v[i].sx) <= 1.01f) {
            hasLeft = true;
        } else if (std::fabs(v[i].sx - right) <= 1.01f) {
            hasRight = true;
        } else {
            return false;
        }
    }
    return hasLeft && hasRight;
}

LPVOID WidenVertices(D3DVERTEXTYPE vtype, LPVOID verts, DWORD count) {
    ++g_drawsSinceLock;
    if (g_wide.offset == 0 || vtype != D3DVT_TLVERTEX || !verts || count == 0) {
        return verts;
    }
    const auto* src = static_cast<const D3DTLVERTEX*>(verts);
    t_wideVerts.assign(src, src + count);
    const float off = static_cast<float>(g_wide.offset);

    if (IsFullWidthQuad(src, count)) {
        // 背景は描画面の端から端まで広げる。extend ならテクスチャの横範囲も同じ比率で広げ、絵を伸ばさない
        const float right = static_cast<float>(g_wide.wideW);
        const float ratio = static_cast<float>(g_wide.wideW) / static_cast<float>(g_wide.baseW);
        float minU = t_wideVerts[0].tu;
        float maxU = t_wideVerts[0].tu;
        for (const auto& v : t_wideVerts) {
            minU = v.tu < minU ? v.tu : minU;
            maxU = v.tu > maxU ? v.tu : maxU;
        }
        const float cu = (minU + maxU) * 0.5f;
        for (auto& v : t_wideVerts) {
            v.sx = std::fabs(v.sx) <= 1.01f ? 0.0f : right;
            if (GetConfig().wideBackgroundExtend) {
                v.tu = cu + (v.tu - cu) * ratio;
            }
        }
        if (GetConfig().logGraphics) {
            ++g_draw.bgQuads;  // 統計用なので厳密な排他は不要
        }
        return t_wideVerts.data();
    }

    for (auto& v : t_wideVerts) {
        v.sx += off;
    }
    return t_wideVerts.data();
}

HRESULT STDMETHODCALLTYPE Dev2_EndScene(IDirect3DDevice2* self) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice2*)>(self, 11);
    HRESULT hr = fn(self);
    if (!GetConfig().logGraphics) {
        return hr;
    }
    AcquireSRWLockExclusive(&g_drawLock);
    ++g_draw.frames;
    FlushDrawStatsLocked();
    ReleaseSRWLockExclusive(&g_drawLock);
    return hr;
}

HRESULT STDMETHODCALLTYPE Dev2_SetRenderState(IDirect3DDevice2* self, D3DRENDERSTATETYPE state, DWORD value) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice2*, D3DRENDERSTATETYPE, DWORD)>(self, 23);
    if (GetConfig().logGraphics) {
        AcquireSRWLockExclusive(&g_drawLock);
        ++g_draw.setRenderState;
        ReleaseSRWLockExclusive(&g_drawLock);
    }
    return fn(self, state, value);
}

HRESULT STDMETHODCALLTYPE Dev2_SetTransform(IDirect3DDevice2* self, D3DTRANSFORMSTATETYPE state, LPD3DMATRIX m) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice2*, D3DTRANSFORMSTATETYPE, LPD3DMATRIX)>(self, 26);
    AcquireSRWLockExclusive(&g_drawLock);
    long n = ++g_draw.setTransform;
    ReleaseSRWLockExclusive(&g_drawLock);
    static LONG logged = 0;
    if (m && n == 1 && InterlockedIncrement(&logged) <= 6) {
        Log("IDirect3DDevice2::SetTransform(state=%d) [%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]",
            static_cast<int>(state), m->_11, m->_12, m->_13, m->_14, m->_21, m->_22, m->_23, m->_24, m->_31, m->_32,
            m->_33, m->_34, m->_41, m->_42, m->_43, m->_44);
    }
    return fn(self, state, m);
}

HRESULT STDMETHODCALLTYPE Dev2_DrawPrimitive(IDirect3DDevice2* self, D3DPRIMITIVETYPE prim, D3DVERTEXTYPE vtype,
                                             LPVOID verts, DWORD count, DWORD flags) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice2*, D3DPRIMITIVETYPE, D3DVERTEXTYPE, LPVOID, DWORD, DWORD)>(self, 29);
    if (GetConfig().logGraphics) {
        AcquireSRWLockExclusive(&g_drawLock);
        ++g_draw.drawPrim;
        AccumulateVertices(vtype, verts, count);
        ReleaseSRWLockExclusive(&g_drawLock);
    }
    return fn(self, prim, vtype, WidenVertices(vtype, verts, count), count, flags);
}

HRESULT STDMETHODCALLTYPE Dev2_DrawIndexedPrimitive(IDirect3DDevice2* self, D3DPRIMITIVETYPE prim, D3DVERTEXTYPE vtype,
                                                    LPVOID verts, DWORD vcount, LPWORD idx, DWORD icount, DWORD flags) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice2*, D3DPRIMITIVETYPE, D3DVERTEXTYPE, LPVOID, DWORD, LPWORD,
                                               DWORD, DWORD)>(self, 30);
    if (GetConfig().logGraphics) {
        AcquireSRWLockExclusive(&g_drawLock);
        ++g_draw.drawIndexed;
        AccumulateVertices(vtype, verts, vcount);
        ReleaseSRWLockExclusive(&g_drawLock);
    }
    return fn(self, prim, vtype, WidenVertices(vtype, verts, vcount), vcount, idx, icount, flags);
}

// ---------------------------------------------------------------- IDirect3DViewport2（値が変わったときだけ記録）
HRESULT STDMETHODCALLTYPE Vp2_SetViewport2(IDirect3DViewport2* self, LPD3DVIEWPORT2 vp) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3DViewport2*, LPD3DVIEWPORT2)>(self, 17);
    // ワイド化: 640/639 幅の viewport を描画面の幅まで広げる（TL 頂点のクリップ範囲になる）
    D3DVIEWPORT2 wide;
    LPD3DVIEWPORT2 pass = vp;
    if (vp && g_wide.offset > 0 && vp->dwX == 0 && vp->dwWidth + 1 >= static_cast<DWORD>(g_wide.baseW) &&
        vp->dwWidth <= static_cast<DWORD>(g_wide.baseW)) {
        wide = *vp;
        wide.dwWidth += 2 * static_cast<DWORD>(g_wide.offset);
        pass = &wide;
    }
    HRESULT hr = fn(self, pass);
    static D3DVIEWPORT2 last{};
    if (vp && std::memcmp(vp, &last, sizeof(last)) != 0) {
        last = *vp;
        Log("IDirect3DViewport2::SetViewport2 x=%lu y=%lu w=%lu h=%lu clip[%.3f %.3f %.3f %.3f] z[%.3f..%.3f] -> 0x%08lX",
            vp->dwX, vp->dwY, vp->dwWidth, vp->dwHeight, vp->dvClipX, vp->dvClipY, vp->dvClipWidth, vp->dvClipHeight,
            vp->dvMinZ, vp->dvMaxZ, static_cast<unsigned long>(hr));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Vp2_SetViewport(IDirect3DViewport2* self, LPD3DVIEWPORT vp) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3DViewport2*, LPD3DVIEWPORT)>(self, 5);
    D3DVIEWPORT wide;
    LPD3DVIEWPORT pass = vp;
    if (vp && g_wide.offset > 0 && vp->dwX == 0 && vp->dwWidth + 1 >= static_cast<DWORD>(g_wide.baseW) &&
        vp->dwWidth <= static_cast<DWORD>(g_wide.baseW)) {
        wide = *vp;
        wide.dwWidth += 2 * static_cast<DWORD>(g_wide.offset);
        pass = &wide;
    }
    HRESULT hr = fn(self, pass);
    static D3DVIEWPORT last{};
    if (vp && std::memcmp(vp, &last, sizeof(last)) != 0) {
        last = *vp;
        Log("IDirect3DViewport2::SetViewport x=%lu y=%lu w=%lu h=%lu scale[%.3f %.3f] max[%.3f %.3f] z[%.3f..%.3f] -> 0x%08lX",
            vp->dwX, vp->dwY, vp->dwWidth, vp->dwHeight, vp->dvScaleX, vp->dvScaleY, vp->dvMaxX, vp->dvMaxY,
            vp->dvMinZ, vp->dvMaxZ, static_cast<unsigned long>(hr));
    }
    return hr;
}

// IDirect3DViewport2::Clear (index 12): ゲーム座標で全幅の矩形は描画面の全幅に広げる
HRESULT STDMETHODCALLTYPE Vp2_Clear(IDirect3DViewport2* self, DWORD count, LPD3DRECT rects, DWORD flags) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3DViewport2*, DWORD, LPD3DRECT, DWORD)>(self, 12);
    if (g_wide.offset == 0 || !rects || count == 0 || count > 16) {
        return fn(self, count, rects, flags);
    }
    D3DRECT wide[16];
    for (DWORD i = 0; i < count; ++i) {
        wide[i] = rects[i];
        if (wide[i].x1 <= 0 && wide[i].x2 >= g_wide.baseW - 1) {
            wide[i].x1 = 0;
            wide[i].x2 = g_wide.wideW;
        } else {
            wide[i].x1 += g_wide.offset;
            wide[i].x2 += g_wide.offset;
        }
    }
    return fn(self, count, wide, flags);
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
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice2*, LPD3DENUMTEXTUREFORMATSCALLBACK, LPVOID)>(self, 9);
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

// Blt / BltFast は HUD 等の 2D 描画の調査用に件数と転送先範囲だけを集計する
void AccumulateBlt(bool fast, long x, long w) {
    if (!GetConfig().logGraphics) {
        return;
    }
    AcquireSRWLockExclusive(&g_drawLock);
    (fast ? g_draw.bltFast : g_draw.blt) += 1;
    g_draw.bltMinX = x < g_draw.bltMinX ? x : g_draw.bltMinX;
    g_draw.bltMaxX = x > g_draw.bltMaxX ? x : g_draw.bltMaxX;
    g_draw.bltMinW = w < g_draw.bltMinW ? w : g_draw.bltMinW;
    g_draw.bltMaxW = w > g_draw.bltMaxW ? w : g_draw.bltMaxW;
    ReleaseSRWLockExclusive(&g_drawLock);
}

// IDirectDrawSurface::Lock (index 25)。呼び出し元アドレスを集計する（__declspec(noinline) で戻りアドレスを正しく取る）
__declspec(noinline) HRESULT STDMETHODCALLTYPE Surf_Lock(IDirectDrawSurface* self, LPRECT rect, LPDDSURFACEDESC desc,
                                                         DWORD flags, HANDLE ev) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectDrawSurface*, LPRECT, LPDDSURFACEDESC, DWORD, HANDLE)>(self, 25);
    HRESULT hr = fn(self, rect, desc, flags, ev);

    // ワイド化: 描画先のロックは、ゲームから見て中央 4:3 部分だけの 640x480（320x240）に見せる
    if (SUCCEEDED(hr) && desc && !rect && IsWideSurface(self) && desc->lpSurface &&
        desc->dwWidth == static_cast<DWORD>(g_wide.wideW)) {
        const LONG bpp = static_cast<LONG>(desc->ddpfPixelFormat.dwRGBBitCount / 8);
        const LONG pitch = desc->lPitch;
        auto* base = static_cast<BYTE*>(desc->lpSurface);
        // 前回ロック以降 3D を描いていない = 2D だけの画面（メニュー等）。左右の余白に前の画面が残らないよう黒で塗る
        if (InterlockedExchange(&g_drawsSinceLock, 0) == 0 && bpp > 0) {
            const size_t leftBytes = static_cast<size_t>(g_wide.offset * bpp);
            const size_t rightStart = static_cast<size_t>((g_wide.offset + g_wide.baseW) * bpp);
            const size_t rightBytes = static_cast<size_t>((g_wide.wideW - g_wide.offset - g_wide.baseW) * bpp);
            for (DWORD y = 0; y < desc->dwHeight; ++y) {
                BYTE* row = base + static_cast<ptrdiff_t>(y) * pitch;
                std::memset(row, 0, leftBytes);
                std::memset(row + rightStart, 0, rightBytes);
            }
        }
        desc->lpSurface = base + g_wide.offset * bpp;
        desc->dwWidth = static_cast<DWORD>(g_wide.baseW);
    }
    if (GetConfig().logGraphics && SUCCEEDED(hr) && desc) {
        void* caller = _ReturnAddress();
        AcquireSRWLockExclusive(&g_drawLock);
        ++g_draw.locks;
        for (auto& c : g_draw.lockCallers) {
            if (c.addr == caller || !c.addr) {
                c.addr = caller;
                ++c.count;
                c.w = static_cast<long>(desc->dwWidth);
                c.h = static_cast<long>(desc->dwHeight);
                break;
            }
        }
        ReleaseSRWLockExclusive(&g_drawLock);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Surf_Blt(IDirectDrawSurface* self, LPRECT dest, LPDIRECTDRAWSURFACE src, LPRECT srcRect,
                                   DWORD flags, LPDDBLTFX fx) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectDrawSurface*, LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD, LPDDBLTFX)>(self, 5);
    if (dest) {
        AccumulateBlt(false, dest->left, dest->right - dest->left);
    }
    if (g_wide.offset > 0) {
        RECT d;
        RECT s;
        if (IsWideSurface(src) && !IsWideSurface(self) && srcRect && srcRect->left <= 0 &&
            srcRect->right >= g_wide.baseW - 1 && srcRect->right <= g_wide.baseW) {
            // 表示用の転送: 広げた描画面の全幅を送る
            s = *srcRect;
            s.left = 0;
            s.right = g_wide.wideW;
            return fn(self, dest, src, &s, flags, fx);
        }
        if (IsWideSurface(self) && dest) {
            d = *dest;
            if ((flags & DDBLT_COLORFILL) && d.left <= 0 && d.right >= g_wide.baseW - 1) {
                d.left = 0;
                d.right = g_wide.wideW;  // 全画面の塗りつぶしは余白ごと
            } else {
                d.left += g_wide.offset;
                d.right += g_wide.offset;
            }
            return fn(self, &d, src, srcRect, flags, fx);
        }
    }
    return fn(self, dest, src, srcRect, flags, fx);
}

HRESULT STDMETHODCALLTYPE Surf_BltFast(IDirectDrawSurface* self, DWORD x, DWORD y, LPDIRECTDRAWSURFACE src, LPRECT srcRect,
                                       DWORD trans) {
    auto fn = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirectDrawSurface*, DWORD, DWORD, LPDIRECTDRAWSURFACE, LPRECT, DWORD)>(self, 7);
    AccumulateBlt(true, static_cast<long>(x), srcRect ? srcRect->right - srcRect->left : -1);
    if (g_wide.offset > 0 && IsWideSurface(self)) {
        x += static_cast<DWORD>(g_wide.offset);
    }
    return fn(self, x, y, src, srcRect, trans);
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

// IDirect3DDevice2 の vtable 順（d3d.h）:
//  0 QueryInterface  1 AddRef  2 Release  3 GetCaps  4 SwapTextureHandles  5 GetStats
//  6 AddViewport  7 DeleteViewport  8 NextViewport  9 EnumTextureFormats  10 BeginScene  11 EndScene
// 12 GetDirect3D 13 SetCurrentViewport 14 GetCurrentViewport 15 SetRenderTarget 16 GetRenderTarget
// 17 Begin 18 BeginIndexed 19 Vertex 20 Index 21 End 22 GetRenderState 23 SetRenderState
// 24 GetLightState 25 SetLightState 26 SetTransform 27 GetTransform 28 MultiplyTransform
// 29 DrawPrimitive 30 DrawIndexedPrimitive 31 SetClipStatus 32 GetClipStatus
// （ゲーム側 0x440EE0 の +0x18=AddViewport / +0x1C=DeleteViewport / +0x34=SetCurrentViewport と一致）
void HookDevice2(IDirect3DDevice2* dev) {
    Log("hook IDirect3DDevice2 %p", static_cast<void*>(dev));
    PatchVtable(dev, 0, Any_QueryInterface, "QueryInterface");
    PatchVtable(dev, 9, Dev2_EnumTextureFormats, "IDirect3DDevice2::EnumTextureFormats");
    PatchVtable(dev, 11, Dev2_EndScene, "IDirect3DDevice2::EndScene");
    PatchVtable(dev, 23, Dev2_SetRenderState, "IDirect3DDevice2::SetRenderState");
    PatchVtable(dev, 26, Dev2_SetTransform, "IDirect3DDevice2::SetTransform");
    PatchVtable(dev, 29, Dev2_DrawPrimitive, "IDirect3DDevice2::DrawPrimitive");
    PatchVtable(dev, 30, Dev2_DrawIndexedPrimitive, "IDirect3DDevice2::DrawIndexedPrimitive");
}

void HookSurface(IDirectDrawSurface* s) {
    PatchVtable(s, 0, Any_QueryInterface, "QueryInterface");
    PatchVtable(s, 3, Surf_AddAttachedSurface, "Surface::AddAttachedSurface");
    PatchVtable(s, 5, Surf_Blt, "Surface::Blt");
    PatchVtable(s, 7, Surf_BltFast, "Surface::BltFast");
    PatchVtable(s, 25, Surf_Lock, "Surface::Lock");
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
