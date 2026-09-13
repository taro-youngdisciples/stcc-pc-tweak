// joylog: ゲームとは独立に DirectInput8 でゲームコントローラの入力と切断/再接続を記録する診断ツール。
//
//   joylog.exe [秒数=600] [出力ファイル=joylog.txt]
//
// - 非排他・バックグラウンドで読むので、ウィンドウにフォーカスが無くても記録できる
// - 値の変化（軸は生値で ±3000 超）、押された/離されたボタン番号、POV を記録
// - 切断（GetDeviceState 失敗）を検出したら 1 秒ごとに再列挙し、再接続までの時間を記録
// - 10 秒ごとに接続状態と steam.exe の起動有無を記録（Steam Input の影響切り分け用）
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <share.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

FILE* g_out = nullptr;
DWORD g_start = 0;

void Log(const char* fmt, ...) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char head[64];
    std::snprintf(head, sizeof(head), "%02u:%02u:%02u.%03u [%5lus] ", st.wHour, st.wMinute, st.wSecond,
                  st.wMilliseconds, (GetTickCount() - g_start) / 1000);
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    std::printf("%s%s\n", head, body);
    if (g_out) {
        std::fprintf(g_out, "%s%s\n", head, body);
        std::fflush(g_out);
    }
}

std::string Utf8(const wchar_t* w) {
    char b[512];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof(b), nullptr, nullptr);
    return b;
}

bool SteamRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"steam.exe") == 0) {
            found = true;
            break;
        }
    }
    CloseHandle(snap);
    return found;
}

struct Found {
    GUID guid;
    bool ok;
    wchar_t name[MAX_PATH];
};

BOOL CALLBACK EnumDevicesCb(LPCDIDEVICEINSTANCEW d, LPVOID p) {
    auto* f = static_cast<Found*>(p);
    f->guid = d->guidInstance;
    wcscpy_s(f->name, d->tszProductName);
    f->ok = true;
    return DIENUM_STOP;
}

BOOL CALLBACK EnumObjectsCb(LPCDIDEVICEOBJECTINSTANCEW o, LPVOID) {
    Log("  object ofs=0x%02lX type=0x%08lX name=\"%s\"", o->dwOfs, o->dwType, Utf8(o->tszName).c_str());
    return DIENUM_CONTINUE;
}

// 接続失敗の理由は、前回と変わったときだけ記録する
void LogOpenFailure(const char* reason, HRESULT hr) {
    static std::string last;
    char b[128];
    std::snprintf(b, sizeof(b), "%s (0x%08lX)", reason, static_cast<unsigned long>(hr));
    if (last != b) {
        Log("open failed: %s", b);
        last = b;
    }
}

IDirectInputDevice8W* Open(IDirectInput8W* di, HWND hwnd, bool verbose) {
    Found f{};
    HRESULT hr = di->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumDevicesCb, &f, DIEDFL_ATTACHEDONLY);
    if (FAILED(hr) || !f.ok) {
        LogOpenFailure(FAILED(hr) ? "EnumDevices failed" : "no attached game controller", hr);
        return nullptr;
    }
    IDirectInputDevice8W* dev = nullptr;
    hr = di->CreateDevice(f.guid, &dev, nullptr);
    if (FAILED(hr)) {
        LogOpenFailure("CreateDevice failed", hr);
        return nullptr;
    }
    hr = dev->SetDataFormat(&c_dfDIJoystick);
    if (FAILED(hr)) {
        LogOpenFailure("SetDataFormat failed", hr);
        dev->Release();
        return nullptr;
    }
    hr = dev->SetCooperativeLevel(hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    if (FAILED(hr)) {
        LogOpenFailure("SetCooperativeLevel failed", hr);
        dev->Release();
        return nullptr;
    }
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    dev->GetCapabilities(&caps);
    Log("CONNECTED \"%s\" axes=%lu buttons=%lu povs=%lu", Utf8(f.name).c_str(), caps.dwAxes, caps.dwButtons, caps.dwPOVs);
    if (verbose) {
        dev->EnumObjects(EnumObjectsCb, nullptr, DIDFT_AXIS | DIDFT_POV);
    }
    dev->Acquire();
    return dev;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const int seconds = argc > 1 ? _wtoi(argv[1]) : 600;
    const wchar_t* path = argc > 2 ? argv[2] : L"joylog.txt";
    // 実行中も他プロセスから読めるよう、書き込みだけを拒否する共有モードで開く
    g_out = _wfsopen(path, L"w", _SH_DENYWR);
    g_start = GetTickCount();

    // DirectInput の協調レベル設定にはこのプロセスのトップレベルウィンドウが要る（表示はしない）
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"joylog", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    IDirectInput8W* di = nullptr;
    HRESULT hr = DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                    static_cast<LPVOID*>(static_cast<void*>(&di)), nullptr);
    if (FAILED(hr)) {
        Log("DirectInput8Create failed 0x%08lX", static_cast<unsigned long>(hr));
        return 1;
    }

    bool steam = SteamRunning();
    Log("joylog start: duration=%d s steam=%d", seconds, steam ? 1 : 0);

    IDirectInputDevice8W* dev = nullptr;
    DIJOYSTATE last{};
    bool hasLast = false;
    bool firstOpen = true;
    DWORD lostAt = 0;
    DWORD lastEnum = 0;
    DWORD lastBeat = GetTickCount();
    unsigned long polls = 0;
    int disconnects = 0;

    while (GetTickCount() - g_start < static_cast<DWORD>(seconds) * 1000) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            DispatchMessageW(&msg);
        }
        const DWORD now = GetTickCount();

        if (!dev) {
            if (now - lastEnum >= 1000) {
                lastEnum = now;
                dev = Open(di, hwnd, firstOpen);
                if (dev) {
                    firstOpen = false;
                    hasLast = false;
                    if (lostAt) {
                        Log("RECONNECTED after %.1f s", (now - lostAt) / 1000.0);
                        lostAt = 0;
                    }
                }
            }
        } else {
            dev->Poll();
            DIJOYSTATE js{};
            hr = dev->GetDeviceState(sizeof(js), &js);
            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
                HRESULT a = dev->Acquire();
                hr = FAILED(a) ? a : dev->GetDeviceState(sizeof(js), &js);
            }
            if (FAILED(hr)) {
                ++disconnects;
                Log("LOST #%d: 0x%08lX steam=%d", disconnects, static_cast<unsigned long>(hr), SteamRunning() ? 1 : 0);
                dev->Unacquire();
                dev->Release();
                dev = nullptr;
                lostAt = now;
            } else {
                ++polls;
                auto moved = [](LONG a, LONG b) { return std::labs(a - b) > 3000; };
                const bool buttonsChanged = std::memcmp(js.rgbButtons, last.rgbButtons, sizeof(js.rgbButtons)) != 0;
                const bool changed = !hasLast || moved(js.lX, last.lX) || moved(js.lY, last.lY) ||
                                     moved(js.lZ, last.lZ) || moved(js.lRx, last.lRx) || moved(js.lRy, last.lRy) ||
                                     moved(js.lRz, last.lRz) || js.rgdwPOV[0] != last.rgdwPOV[0] || buttonsChanged;
                if (changed) {
                    std::string edges;
                    if (hasLast && buttonsChanged) {
                        for (int i = 0; i < 32; ++i) {
                            const bool was = (last.rgbButtons[i] & 0x80) != 0;
                            const bool is = (js.rgbButtons[i] & 0x80) != 0;
                            if (was != is) {
                                edges += (is ? " +b" : " -b") + std::to_string(i);
                            }
                        }
                    }
                    Log("X=%5ld Y=%5ld Z=%5ld Rx=%5ld Ry=%5ld Rz=%5ld POV=%5ld%s", js.lX, js.lY, js.lZ, js.lRx, js.lRy,
                        js.lRz, static_cast<long>(js.rgdwPOV[0]), edges.c_str());
                    last = js;
                    hasLast = true;
                }
            }
        }

        if (now - lastBeat >= 10000) {
            const bool s = SteamRunning();
            if (s != steam) {
                Log("STEAM %s", s ? "started" : "exited");
                steam = s;
            }
            Log("heartbeat: %s polls=%lu disconnects=%d steam=%d", dev ? "connected" : "DISCONNECTED", polls,
                disconnects, s ? 1 : 0);
            lastBeat = now;
        }
        Sleep(10);
    }

    Log("joylog end: disconnects=%d", disconnects);
    if (dev) {
        dev->Unacquire();
        dev->Release();
    }
    di->Release();
    if (g_out) {
        std::fclose(g_out);
    }
    return 0;
}
