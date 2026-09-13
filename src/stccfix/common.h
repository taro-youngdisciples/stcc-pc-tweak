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
    bool logGraphics = false;  // DirectDraw/Direct3D 呼び出しとゲーム側 D3D 初期化関数の戻り値をログ
};
Config LoadConfig(const std::wstring& iniPath);

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
