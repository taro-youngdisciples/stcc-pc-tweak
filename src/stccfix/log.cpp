#include "common.h"

#include <cstdarg>
#include <cstdio>

namespace stcc {
namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_lock;
DWORD g_startTick = 0;

}  // namespace

void LogInit(const std::wstring& path) {
    InitializeCriticalSection(&g_lock);
    g_startTick = GetTickCount();
    // 起動ごとに上書き（前回分は必要なら run スクリプト側で退避する）
    g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
}

void Log(const char* fmt, ...) {
    if (g_file == INVALID_HANDLE_VALUE) {
        return;
    }
    char buf[1024];
    int head = std::snprintf(buf, sizeof(buf), "[%8lu ms] [T%05lu] ", GetTickCount() - g_startTick,
                             GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    int body = std::vsnprintf(buf + head, sizeof(buf) - head - 2, fmt, ap);
    va_end(ap);
    if (body < 0) {
        body = 0;
    }
    int len = head + body;
    if (len > static_cast<int>(sizeof(buf)) - 2) {
        len = sizeof(buf) - 2;
    }
    buf[len++] = '\r';
    buf[len++] = '\n';

    EnterCriticalSection(&g_lock);
    DWORD written = 0;
    // FlushFileBuffers は呼ばない。WriteFile の時点で OS のキャッシュに入るので、
    // ゲームプロセスが落ちてもログは残る（毎行フラッシュはシーン切替時のラグ要因だった）
    WriteFile(g_file, buf, static_cast<DWORD>(len), &written, nullptr);
    LeaveCriticalSection(&g_lock);
}

}  // namespace stcc
