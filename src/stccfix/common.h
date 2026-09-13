#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace stcc {

// ---------------------------------------------------------------- log.cpp
void LogInit(const std::wstring& path);
void Log(const char* fmt, ...);

// ---------------------------------------------------------------- config.cpp
struct Config {
    bool windowed = true;      // g_bFullscreen の初期値を 0 にしてウィンドウで起動する
    bool d3dWindowedVideoMemory = true;  // ウィンドウ時の D3D 描画先を VIDEOMEMORY で作る（HAL デバイス作成失敗の修正）
    int windowScale = 0;             // ウィンドウのクライアントを 640x480 の何倍にするか。0 = モニタに収まる最大の整数倍
    bool keepAspect = true;          // ドラッグでのリサイズを 4:3 に拘束
    bool rememberWindowSize = true;  // 利用者がリサイズした大きさを、ゲームの再初期化後も維持
    // 表示アスペクト比。4:3 以外なら Direct3D の描画先を横長に作ってワイド化する（hooks_ddraw.cpp）:
    // 3D の TL 頂点は右へずらし、ゲームの 2D 書き込みは中央 4:3 に置くので HUD は伸びない。窓もこの比率にする
    int aspectW = 4;
    int aspectH = 3;
    // 画面幅いっぱいの背景（空など）の扱い。true = テクスチャ座標を広げて比率を保つ / false = 横に引き伸ばす
    bool wideBackgroundExtend = true;
    // レース中の HUD を画面の左右端へ寄せる（false = 中央 4:3 のまま）
    bool hudAnchorEdges = true;
    bool logGraphics = false;  // DirectDraw/Direct3D 呼び出しとゲーム側 D3D 初期化関数の戻り値をログ
    bool logInput = false;     // DirectInput のデバイス列挙・プロパティ・GetDeviceState の変化をログ
    bool logWindow = false;    // 窓サイズ変更（SetWindowPos/MoveWindow の呼び出し元、WM_SIZE）をログ
    // ゲームに見せる DIDEVTYPE サブタイプ。0 = 変更しない。
    // ゲームは サブタイプ 4(GAMEPAD)→Game Pad、6(WHEEL)→Steering Wheel、それ以外→Joystick として選択肢を出す
    int inputDeviceSubtype = 0;

    // ---- 軸の加工（GetDeviceState の結果を書き換える）
    // Joystick / Steering Wheel モードのゲームは Y 軸 1 本を「上=アクセル、下=ブレーキ」として読む。
    // triggerPedals で、アクセル/ブレーキ用の別々の軸（DS4 なら R2=Ry, L2=Rx）から Y を合成する。
    bool triggerPedals = false;
    int accelAxis = 4;  // DIJOYSTATE の軸番号 0:X 1:Y 2:Z 3:Rx 4:Ry 5:Rz 6:Slider0 7:Slider1
    int brakeAxis = 3;
    bool accelInvert = false;  // 離したときに最大値になる軸（ホイールのペダル等）なら 1
    bool brakeInvert = false;
    int pedalDeadzone = 5;     // %。これ未満の踏み込みは 0 とみなし、両方 0 ならスティックの Y をそのまま通す
    int steerDeadzone = 0;     // %。X 軸中央の遊び
    int steerLinearity = 100;  // %。100 = 線形、>100 で中央付近が鈍く（出力 = 入力^(値/100)）
};
// DirectInput フックが必要な設定か
bool InputHooksNeeded(const Config& c);
// ワイド化の横方向縮小率（4:3 なら 1.0）
double WidescreenScale(const Config& c);
// DirectDraw/Direct3D フックが必要な設定か
bool GraphicsHooksNeeded(const Config& c);
Config LoadConfig(const std::wstring& iniPath);
// DllMain で読み込んだ設定（DllMain 以降はいつでも参照可）
const Config& GetConfig();

// ---------------------------------------------------------------- hooks_dinput.cpp
// DirectInputCreate* が返した IDirectInput インターフェースにフックを掛ける（初回呼び出し時、DllMain 外）
void HookDirectInput(void* directInput);

// ---------------------------------------------------------------- patch.cpp
struct BytePatch {
    std::uint32_t va;
    const char* original;     // 16進文字列（空白区切り）
    const char* replacement;  // 同上。original と同じ長さ
    const char* description;
};

enum class GameVersion { Unknown, Jp102 };

GameVersion DetectGameVersion();
const char* GameVersionName(GameVersion v);
// 元のバイト列を照合してから書き換える。1 つでも不一致なら何も書かずに false
bool ApplyPatches(const BytePatch* patches, std::size_t count);

// ---------------------------------------------------------------- hooks_ddraw.cpp
// exe の IAT の DDRAW!DirectDrawCreate を差し替え、以後生成される COM オブジェクトの vtable をフックする。
// DllMain から呼んでよい（IAT 書き換えのみ）
void InstallDirectDrawLogging();

// ワイド化で作った横長の描画面の情報（作っていなければ false）
struct WideInfo {
    LONG baseW;   // ゲームが要求した幅（640 / 320）
    LONG baseH;
    LONG wideW;   // 実際の幅
    LONG offset;  // 左右の余白
};
bool GetWideInfo(WideInfo* out);
// 直近の描画先ロックの前に 3D を描いていたか（レース中など）
bool LastLockHad3D();

// ---------------------------------------------------------------- hooks_hud.cpp
// レース中の HUD スプライトを横長画面の左右端へ寄せる。jp-1.02 専用。DllMain から呼ぶ
void InstallHudHooks();

// ---------------------------------------------------------------- hooks_window.cpp
// ウィンドウモードの窓サイズ維持と 4:3 リサイズ。jp-1.02 専用。DllMain から呼ぶ
void InstallWindowHooks();

// ---------------------------------------------------------------- hooks_game.cpp
// ゲーム内の D3D 初期化関数（引数なし）の戻り値をログする。jp-1.02 専用。DllMain から呼ぶ
void InstallGameTraceHooks();

// ---------------------------------------------------------------- dllmain.cpp
std::wstring ModuleDirectory();

}  // namespace stcc
