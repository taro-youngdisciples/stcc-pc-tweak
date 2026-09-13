// ウィンドウモードの窓サイズ管理（jp-1.02）。
//
// ゲームは Gfx_InitDirectDraw (0x435390) のたびに窓を 640x480 クライアントに戻す（シーン切替・レース開始でも呼ばれる）。
// その直後にフックして、設定サイズ（または利用者が最後にリサイズしたサイズ）へ広げ直す。
// さらに窓プロシージャをサブクラス化し、ドラッグでのリサイズを 4:3 に拘束する
// （ウィンドウモードの dgVoodoo はクライアント全体に引き伸ばすので、比率を崩すと絵が歪む）。
#include "common.h"

#include <MinHook.h>

#include <intrin.h>

#include <cstdlib>

namespace stcc {
namespace {

constexpr std::uintptr_t kGfxInitDirectDraw = 0x00435390;
constexpr std::uintptr_t kHWndMain = 0x0056C184;  // g_hWndMain
constexpr std::uintptr_t kFullscreenFlag = 0x00568930;  // g_bFullscreen
constexpr int kBaseW = 640;
constexpr int kBaseH = 480;

void* g_origGfxInit = nullptr;
WNDPROC g_origWndProc = nullptr;
HWND g_hwnd = nullptr;
bool g_applying = false;  // 自分で SetWindowPos している最中は WM_SIZE を記録しない
int g_userClientW = 0;    // 利用者がリサイズしたクライアントサイズ（0 = 未設定）
int g_userClientH = 0;
bool g_inSizeMove = false;

// 枠なし全画面（ゲームはウィンドウモードのまま、枠とメニューバーを外してモニタ全体を覆う）。
// 起動時の設定でだけ有効にする: 実行中に窓表示から広げても dgVoodoo の表示先が元の大きさのまま追従しない
// （2026-09-13 に Alt+Enter 切り替えを試して確認。縮めるのは追従する）
bool g_borderless = false;
bool g_styleSaved = false;  // 窓表示に戻すための元のスタイルとメニューを退避済みか
LONG g_savedStyle = 0;
LONG g_savedExStyle = 0;
HMENU g_savedMenu = nullptr;

// 640x480 への要求を差し替えた少し後に、利用者のリサイズと同じ通知をゲームへ届け直すためのタイマー
constexpr UINT_PTR kNudgeTimerId = 0x53545743;  // 'STWC'
constexpr UINT kNudgeDelayMs = 300;

// ---- 調査用: user32 の窓サイズ変更 API を呼び出し元つきで記録（[Debug] LogWindow）
using SetWindowPosFn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using MoveWindowFn = BOOL(WINAPI*)(HWND, int, int, int, int, BOOL);
SetWindowPosFn g_origSetWindowPos = nullptr;
MoveWindowFn g_origMoveWindow = nullptr;

BOOL WINAPI Hook_SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (hwnd == g_hwnd && !g_applying && GetConfig().logWindow) {
        Log("window: SetWindowPos(%d,%d %dx%d flags=0x%X) caller=%p", x, y, cx, cy, flags, _ReturnAddress());
    }
    return g_origSetWindowPos(hwnd, after, x, y, cx, cy, flags);
}

// 枠なし全画面中にゲーム（MFC）がメニューを付け直すと、メニューバーが画面上端に出てクライアントが縮む。
// 付け直しは保留して、窓表示に戻すときに付ける
using SetMenuFn = BOOL(WINAPI*)(HWND, HMENU);
SetMenuFn g_origSetMenu = nullptr;
bool g_inStyleChange = false;  // SetBorderlessStyle 自身の SetMenu は通す

BOOL WINAPI Hook_SetMenu(HWND hwnd, HMENU menu) {
    if (g_borderless && !g_inStyleChange && menu && hwnd && hwnd == g_hwnd) {
        g_savedMenu = menu;
        static LONG logged = 0;
        if (InterlockedIncrement(&logged) <= 5) {
            Log("window: SetMenu(%p) deferred while borderless, caller=%p", static_cast<void*>(menu), _ReturnAddress());
        }
        return TRUE;
    }
    return g_origSetMenu(hwnd, menu);
}

BOOL WINAPI Hook_MoveWindow(HWND hwnd, int x, int y, int cx, int cy, BOOL repaint) {
    if (hwnd == g_hwnd && !g_applying && GetConfig().logWindow) {
        Log("window: MoveWindow(%d,%d %dx%d) caller=%p", x, y, cx, cy, _ReturnAddress());
    }
    return g_origMoveWindow(hwnd, x, y, cx, cy, repaint);
}

HWND GameWindow() {
    return *reinterpret_cast<HWND*>(kHWndMain);
}

bool GameIsFullscreen() {
    return *reinterpret_cast<int*>(kFullscreenFlag) != 0;
}

// クライアントサイズ (cw, ch) に対する窓全体サイズ（メニューバーの折り返しは SetWindowPos 後に補正する）
SIZE WindowSizeForClient(HWND hwnd, int cw, int ch) {
    RECT rc{0, 0, cw, ch};
    const DWORD style = static_cast<DWORD>(GetWindowLongA(hwnd, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongA(hwnd, GWL_EXSTYLE));
    AdjustWindowRectEx(&rc, style, GetMenu(hwnd) != nullptr, exStyle);
    return {rc.right - rc.left, rc.bottom - rc.top};
}

// 表示アスペクト比に合わせたクライアント幅（高さ基準。4:3 なら 640x480 の整数倍）
int ClientWidthForHeight(int ch) {
    const Config& cfg = GetConfig();
    return (ch * cfg.aspectW + cfg.aspectH / 2) / cfg.aspectH;
}

RECT MonitorRect(HWND hwnd) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcMonitor;
}

void DesiredClientSize(HWND hwnd, int* cw, int* ch) {
    const Config& cfg = GetConfig();
    if (g_borderless) {
        // 比率が合わないモニタでも dgVoodoo（ScalingMode=stretched_ar）が黒帯で比率を保つ
        const RECT m = MonitorRect(hwnd);
        *cw = m.right - m.left;
        *ch = m.bottom - m.top;
        return;
    }
    if (cfg.rememberWindowSize && g_userClientW > 0) {
        *cw = g_userClientW;
        *ch = g_userClientH;
        return;
    }
    int scale = cfg.windowScale;
    if (scale <= 0) {
        // 自動: モニタ作業領域（タスクバーを除く）に窓全体が収まる最大の整数倍（高さ 480 基準）
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        const int availW = mi.rcWork.right - mi.rcWork.left;
        const int availH = mi.rcWork.bottom - mi.rcWork.top;
        const SIZE extra = WindowSizeForClient(hwnd, 0, 0);
        scale = 1;
        while (ClientWidthForHeight(kBaseH * (scale + 1)) + extra.cx <= availW &&
               kBaseH * (scale + 1) + extra.cy <= availH) {
            ++scale;
        }
    }
    *ch = kBaseH * scale;
    *cw = ClientWidthForHeight(*ch);
}

// 枠なし全画面用に枠とメニューバーを外す / 窓表示用に元へ戻す
void SetBorderlessStyleImpl(HWND hwnd, bool on);
void SetBorderlessStyle(HWND hwnd, bool on) {
    g_inStyleChange = true;
    SetBorderlessStyleImpl(hwnd, on);
    g_inStyleChange = false;
}

void SetBorderlessStyleImpl(HWND hwnd, bool on) {
    if (on) {
        if (!g_styleSaved) {
            g_savedStyle = GetWindowLongA(hwnd, GWL_STYLE);
            g_savedExStyle = GetWindowLongA(hwnd, GWL_EXSTYLE);
            g_savedMenu = GetMenu(hwnd);
            g_styleSaved = true;
        }
        SetWindowLongA(hwnd, GWL_STYLE,
                       g_savedStyle & ~static_cast<LONG>(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX));
        SetWindowLongA(hwnd, GWL_EXSTYLE,
                       g_savedExStyle & ~static_cast<LONG>(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE));
        if (GetMenu(hwnd)) {
            SetMenu(hwnd, nullptr);  // メニューは破棄しない（窓表示で戻す）
        }
    } else if (g_styleSaved) {
        SetWindowLongA(hwnd, GWL_STYLE, g_savedStyle);
        SetWindowLongA(hwnd, GWL_EXSTYLE, g_savedExStyle);
        if (!GetMenu(hwnd) && g_savedMenu) {
            SetMenu(hwnd, g_savedMenu);
        }
        g_styleSaved = false;
    }
}

void ApplyWindowSize(HWND hwnd) {
    if (g_borderless) {
        if (IsZoomed(hwnd)) {
            ShowWindow(hwnd, SW_RESTORE);
        }
        SetBorderlessStyle(hwnd, true);
        const RECT m = MonitorRect(hwnd);
        g_applying = true;
        SetWindowPos(hwnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
        g_applying = false;
        RECT client{};
        GetClientRect(hwnd, &client);
        Log("window: borderless fullscreen, client %ldx%ld", client.right, client.bottom);
        return;
    }
    SetBorderlessStyle(hwnd, false);

    int cw = 0;
    int ch = 0;
    DesiredClientSize(hwnd, &cw, &ch);

    // 利用者がサイズを変えられるよう、枠と最大化ボタンを付ける
    LONG style = GetWindowLongA(hwnd, GWL_STYLE);
    if ((style & (WS_THICKFRAME | WS_MAXIMIZEBOX)) != (WS_THICKFRAME | WS_MAXIMIZEBOX)) {
        SetWindowLongA(hwnd, GWL_STYLE, style | WS_THICKFRAME | WS_MAXIMIZEBOX);
    }

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;

    g_applying = true;
    SIZE ws = WindowSizeForClient(hwnd, cw, ch);
    RECT cur{};
    GetWindowRect(hwnd, &cur);
    // 今の左上を保ちつつ、はみ出すなら作業領域内に押し戻す（収まらなければ左上揃え）
    int x = cur.left;
    int y = cur.top;
    if (x + ws.cx > work.right) x = work.right - ws.cx;
    if (y + ws.cy > work.bottom) y = work.bottom - ws.cy;
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;
    SetWindowPos(hwnd, nullptr, x, y, ws.cx, ws.cy, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    // メニューバーが折り返すと AdjustWindowRectEx の計算とずれるので、実測で 1 回補正する
    RECT client{};
    GetClientRect(hwnd, &client);
    const int dh = ch - (client.bottom - client.top);
    const int dw = cw - (client.right - client.left);
    if (dh != 0 || dw != 0) {
        SetWindowPos(hwnd, nullptr, 0, 0, ws.cx + dw, ws.cy + dh, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
        GetClientRect(hwnd, &client);
    }
    g_applying = false;
    Log("window: client %ldx%ld (target %dx%d)", client.right - client.left, client.bottom - client.top, cw, ch);
}

// ドラッグ中の窓矩形を、クライアントが表示アスペクト比（既定 4:3）になるよう補正する
void ConstrainSizing(HWND hwnd, WPARAM edge, RECT* r) {
    const Config& cfg = GetConfig();
    const SIZE extra = WindowSizeForClient(hwnd, 0, 0);
    int cw = (r->right - r->left) - extra.cx;
    int ch = (r->bottom - r->top) - extra.cy;
    const bool horizontalEdge = edge == WMSZ_TOP || edge == WMSZ_BOTTOM;
    if (horizontalEdge) {
        cw = ClientWidthForHeight(ch);
    } else {
        ch = cw * cfg.aspectH / cfg.aspectW;
    }
    const int ww = cw + extra.cx;
    const int wh = ch + extra.cy;
    // 動かしている辺の反対側を固定する
    if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) {
        r->left = r->right - ww;
    } else {
        r->right = r->left + ww;
    }
    if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) {
        r->top = r->bottom - wh;
    } else {
        r->bottom = r->top + wh;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const Config& cfg = GetConfig();
    if (!GameIsFullscreen()) {
        switch (msg) {
            case WM_SIZING:
                if (cfg.keepAspect && lp) {
                    ConstrainSizing(hwnd, wp, reinterpret_cast<RECT*>(lp));
                    return TRUE;
                }
                break;
            case WM_WINDOWPOSCHANGING: {
                // ゲームは画面モードが変わるたびに（0x436750: メニューは 640x480、レースは Screen Mode 設定の
                // 640x480 か 320x240）窓をそのクライアントサイズへ戻す。
                // 出どころを問わず「ゲームの画面モードと同じクライアントサイズへのリサイズ要求」だけを設定サイズに差し替える
                // （最大化や利用者のドラッグなど、他の大きさへの変更には触れない）。
                // 幅が狭いとメニューバーが折り返して高さが変わるので、判定は幅を主に使う
                auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
                if (g_borderless && g_hwnd == hwnd && !g_applying && pos &&
                    (pos->flags & (SWP_NOSIZE | SWP_NOMOVE)) != (SWP_NOSIZE | SWP_NOMOVE)) {
                    // 枠なし全画面では利用者はサイズを変えられないので、位置・サイズの変更はすべてゲーム由来。
                    // ゲームは枠とメニュー込みで窓サイズを計算するので、スタイルからの逆算に頼らずモニタ全体へ固定する
                    const RECT m = MonitorRect(hwnd);
                    if (pos->x != m.left || pos->y != m.top || pos->cx != m.right - m.left || pos->cy != m.bottom - m.top) {
                        Log("window: game requested %d,%d %dx%d while borderless, kept fullscreen", pos->x, pos->y,
                            pos->cx, pos->cy);
                        pos->x = m.left;
                        pos->y = m.top;
                        pos->cx = m.right - m.left;
                        pos->cy = m.bottom - m.top;
                        pos->flags &= ~static_cast<UINT>(SWP_NOSIZE | SWP_NOMOVE);
                        SetTimer(hwnd, kNudgeTimerId, kNudgeDelayMs, nullptr);
                    }
                } else if (g_hwnd == hwnd && !g_applying && pos && !(pos->flags & SWP_NOSIZE)) {
                    auto matchesMode = [&](int cw, int ch) {
                        const SIZE s = WindowSizeForClient(hwnd, cw, ch);
                        return std::abs(pos->cx - s.cx) <= 2 && pos->cy >= s.cy - 2 && pos->cy <= s.cy + 80;
                    };
                    if (matchesMode(kBaseW, kBaseH) || matchesMode(kBaseW / 2, kBaseH / 2)) {
                        int cw = 0;
                        int ch = 0;
                        DesiredClientSize(hwnd, &cw, &ch);
                        const SIZE want = WindowSizeForClient(hwnd, cw, ch);
                        if (want.cx != pos->cx || want.cy != pos->cy) {
                            Log("window: game requested %dx%d, replaced with %ldx%ld", pos->cx, pos->cy, want.cx, want.cy);
                            pos->cx = want.cx;
                            pos->cy = want.cy;
                            // ゲームは初期化中に自前の 640x480 で描画先を決めることがあるので、落ち着いた頃に通知し直す
                            SetTimer(hwnd, kNudgeTimerId, kNudgeDelayMs, nullptr);
                        }
                    }
                }
                break;
            }
            case WM_TIMER:
                if (wp == kNudgeTimerId) {
                    KillTimer(hwnd, kNudgeTimerId);
                    // 1px 縮めて戻す。利用者がドラッグしたときと同じ WM_SIZE がゲームと dgVoodoo に届く
                    RECT wr{};
                    GetWindowRect(hwnd, &wr);
                    const int w = wr.right - wr.left;
                    const int h = wr.bottom - wr.top;
                    g_applying = true;
                    SetWindowPos(hwnd, nullptr, 0, 0, w, h - 1, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
                    SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
                    g_applying = false;
                    RECT client{};
                    GetClientRect(hwnd, &client);
                    Log("window: nudged size notification (client %ldx%ld)", client.right, client.bottom);
                    return 0;
                }
                break;
            case WM_ENTERSIZEMOVE:
                g_inSizeMove = true;
                break;
            case WM_SIZE:
                if (cfg.logWindow && !g_inSizeMove) {
                    Log("window: WM_SIZE type=%u %ux%u%s", static_cast<unsigned>(wp), LOWORD(lp), HIWORD(lp),
                        g_applying ? " (stccfix)" : "");
                }
                break;
            case WM_EXITSIZEMOVE: {
                g_inSizeMove = false;
                // 利用者のドラッグ操作が終わったときだけ記録する
                // （ゲーム自身の再初期化による 640x480 への SetWindowPos を「利用者の大きさ」と誤認しないため）
                RECT client{};
                if (!g_applying && !IsZoomed(hwnd) && GetClientRect(hwnd, &client) && client.right > 0 && client.bottom > 0) {
                    g_userClientW = client.right;
                    g_userClientH = client.bottom;
                    Log("window: user resized to %ldx%ld", client.right, client.bottom);
                }
                break;
            }
            default:
                break;
        }
    }
    return CallWindowProcA(g_origWndProc, hwnd, msg, wp, lp);
}

void EnsureSubclassed(HWND hwnd) {
    if (hwnd != g_hwnd) {
        g_hwnd = hwnd;
        g_origWndProc = reinterpret_cast<WNDPROC>(
            static_cast<LONG_PTR>(SetWindowLongA(hwnd, GWL_WNDPROC, static_cast<LONG>(reinterpret_cast<LONG_PTR>(&WndProc)))));
        Log("window: subclassed hwnd=%p", static_cast<void*>(hwnd));
    }
}

// DirectDraw 初期化の前: dgVoodoo は SetCooperativeLevel 時点の窓の形（枠・メニューバー）で表示先を決め、
// 後から枠を外しても追従しない（メニューバーの跡が残る）。枠なし全画面は初期化の前に形を整えておく
void __cdecl OnBeforeGfxInitDirectDraw() {
    if (!g_borderless || GameIsFullscreen()) {
        return;
    }
    HWND hwnd = GameWindow();
    if (!hwnd) {
        return;
    }
    EnsureSubclassed(hwnd);
    ApplyWindowSize(hwnd);
}

void __cdecl OnGfxInitDirectDraw(DWORD ret) {
    if (GetConfig().logGraphics) {
        Log("[game] Gfx_InitDirectDraw() -> 0x%lX", ret);
    }
    if (ret == 0 || GameIsFullscreen()) {
        return;
    }
    HWND hwnd = GameWindow();
    if (!hwnd) {
        return;
    }
    EnsureSubclassed(hwnd);
    ApplyWindowSize(hwnd);
}

// 引数なし関数の前後処理フック。前処理はレジスタとフラグを保存して呼ぶ
__declspec(naked) void Detour_GfxInitDirectDraw() {
    __asm pushad
    __asm pushfd
    __asm call OnBeforeGfxInitDirectDraw
    __asm popfd
    __asm popad
    __asm call dword ptr [g_origGfxInit]
    __asm push eax
    __asm push eax
    __asm call OnGfxInitDirectDraw
    __asm add esp, 4
    __asm pop eax
    __asm ret
}

}  // namespace

void InstallWindowHooks() {
    g_borderless = GetConfig().borderlessFullscreen;
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        Log("MH_Initialize 失敗: %s", MH_StatusToString(st));
        return;
    }
    auto target = reinterpret_cast<LPVOID>(kGfxInitDirectDraw);
    st = MH_CreateHook(target, &Detour_GfxInitDirectDraw, &g_origGfxInit);
    if (st == MH_OK) {
        st = MH_EnableHook(target);
    }
    Log("window hook Gfx_InitDirectDraw @0x%08X: %s", static_cast<unsigned>(kGfxInitDirectDraw), MH_StatusToString(st));

    // user32 は exe の静的 import なのでロード済み
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto hookApi = [&](const char* name, LPVOID detour, LPVOID* orig) {
        LPVOID fn = user32 ? static_cast<LPVOID>(GetProcAddress(user32, name)) : nullptr;
        MH_STATUS s = fn ? MH_CreateHook(fn, detour, orig) : MH_ERROR_FUNCTION_NOT_FOUND;
        if (s == MH_OK) {
            s = MH_EnableHook(fn);
        }
        Log("window hook user32!%s: %s", name, MH_StatusToString(s));
    };
    if (g_borderless) {
        hookApi("SetMenu", &Hook_SetMenu, reinterpret_cast<LPVOID*>(&g_origSetMenu));
    }
    if (GetConfig().logWindow) {
        hookApi("SetWindowPos", &Hook_SetWindowPos, reinterpret_cast<LPVOID*>(&g_origSetWindowPos));
        hookApi("MoveWindow", &Hook_MoveWindow, reinterpret_cast<LPVOID*>(&g_origMoveWindow));
    }
}

}  // namespace stcc
