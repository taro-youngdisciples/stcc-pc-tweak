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
    bool logGraphics = false;  // DirectDraw/Direct3D 呼び出しとゲーム側 D3D 初期化関数の戻り値をログ
    bool logInput = false;     // DirectInput のデバイス列挙・プロパティ・GetDeviceState の変化をログ
};
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

// ---------------------------------------------------------------- hooks_game.cpp
// ゲーム内の D3D 初期化関数（引数なし）の戻り値をログする。jp-1.02 専用。DllMain から呼ぶ
void InstallGameTraceHooks();

// ---------------------------------------------------------------- dllmain.cpp
std::wstring ModuleDirectory();

}  // namespace stcc
