#include "common.h"

#include <iterator>

namespace stcc {

Config LoadConfig(const std::wstring& iniPath) {
    Config c;
    const wchar_t* ini = iniPath.c_str();
    c.windowed = GetPrivateProfileIntW(L"Display", L"Windowed", c.windowed ? 1 : 0, ini) != 0;
    c.d3dWindowedVideoMemory =
        GetPrivateProfileIntW(L"Display", L"D3DWindowedVideoMemory", c.d3dWindowedVideoMemory ? 1 : 0, ini) != 0;
    c.logGraphics = GetPrivateProfileIntW(L"Debug", L"LogGraphics", c.logGraphics ? 1 : 0, ini) != 0;
    c.logInput = GetPrivateProfileIntW(L"Debug", L"LogInput", c.logInput ? 1 : 0, ini) != 0;

    wchar_t type[32];
    GetPrivateProfileStringW(L"Input", L"DeviceType", L"auto", type, static_cast<DWORD>(std::size(type)), ini);
    if (_wcsicmp(type, L"joystick") == 0) {
        c.inputDeviceSubtype = 2;  // DIDEVTYPEJOYSTICK_TRADITIONAL
    } else if (_wcsicmp(type, L"wheel") == 0) {
        c.inputDeviceSubtype = 6;  // DIDEVTYPEJOYSTICK_WHEEL
    } else if (_wcsicmp(type, L"gamepad") == 0) {
        c.inputDeviceSubtype = 4;  // DIDEVTYPEJOYSTICK_GAMEPAD
    }
    return c;
}

}  // namespace stcc
