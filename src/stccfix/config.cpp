#include "common.h"

#include <iterator>

namespace stcc {
namespace {

int ParseAxis(const wchar_t* name, int fallback) {
    static constexpr const wchar_t* kNames[] = {L"X", L"Y", L"Z", L"Rx", L"Ry", L"Rz", L"Slider0", L"Slider1"};
    for (int i = 0; i < static_cast<int>(std::size(kNames)); ++i) {
        if (_wcsicmp(name, kNames[i]) == 0) {
            return i;
        }
    }
    return fallback;
}

}  // namespace

bool InputHooksNeeded(const Config& c) {
    return c.logInput || c.inputDeviceSubtype != 0 || c.triggerPedals || c.steerDeadzone > 0 || c.steerLinearity != 100;
}

Config LoadConfig(const std::wstring& iniPath) {
    Config c;
    const wchar_t* ini = iniPath.c_str();
    c.windowed = GetPrivateProfileIntW(L"Display", L"Windowed", c.windowed ? 1 : 0, ini) != 0;
    c.d3dWindowedVideoMemory =
        GetPrivateProfileIntW(L"Display", L"D3DWindowedVideoMemory", c.d3dWindowedVideoMemory ? 1 : 0, ini) != 0;
    c.windowScale = static_cast<int>(GetPrivateProfileIntW(L"Display", L"WindowScale", c.windowScale, ini));
    c.keepAspect = GetPrivateProfileIntW(L"Display", L"KeepAspect", c.keepAspect ? 1 : 0, ini) != 0;
    c.rememberWindowSize =
        GetPrivateProfileIntW(L"Display", L"RememberWindowSize", c.rememberWindowSize ? 1 : 0, ini) != 0;
    c.logGraphics = GetPrivateProfileIntW(L"Debug", L"LogGraphics", c.logGraphics ? 1 : 0, ini) != 0;
    c.logInput = GetPrivateProfileIntW(L"Debug", L"LogInput", c.logInput ? 1 : 0, ini) != 0;
    c.logWindow = GetPrivateProfileIntW(L"Debug", L"LogWindow", c.logWindow ? 1 : 0, ini) != 0;

    wchar_t type[32];
    GetPrivateProfileStringW(L"Input", L"DeviceType", L"auto", type, static_cast<DWORD>(std::size(type)), ini);
    if (_wcsicmp(type, L"joystick") == 0) {
        c.inputDeviceSubtype = 2;  // DIDEVTYPEJOYSTICK_TRADITIONAL
    } else if (_wcsicmp(type, L"wheel") == 0) {
        c.inputDeviceSubtype = 6;  // DIDEVTYPEJOYSTICK_WHEEL
    } else if (_wcsicmp(type, L"gamepad") == 0) {
        c.inputDeviceSubtype = 4;  // DIDEVTYPEJOYSTICK_GAMEPAD
    }

    c.triggerPedals = GetPrivateProfileIntW(L"Input", L"TriggerPedals", 0, ini) != 0;
    wchar_t axis[16];
    GetPrivateProfileStringW(L"Input", L"AccelAxis", L"Ry", axis, static_cast<DWORD>(std::size(axis)), ini);
    c.accelAxis = ParseAxis(axis, c.accelAxis);
    GetPrivateProfileStringW(L"Input", L"BrakeAxis", L"Rx", axis, static_cast<DWORD>(std::size(axis)), ini);
    c.brakeAxis = ParseAxis(axis, c.brakeAxis);
    c.accelInvert = GetPrivateProfileIntW(L"Input", L"AccelInvert", 0, ini) != 0;
    c.brakeInvert = GetPrivateProfileIntW(L"Input", L"BrakeInvert", 0, ini) != 0;
    c.pedalDeadzone = static_cast<int>(GetPrivateProfileIntW(L"Input", L"PedalDeadzone", c.pedalDeadzone, ini));
    c.steerDeadzone = static_cast<int>(GetPrivateProfileIntW(L"Input", L"SteerDeadzone", c.steerDeadzone, ini));
    c.steerLinearity = static_cast<int>(GetPrivateProfileIntW(L"Input", L"SteerLinearity", c.steerLinearity, ini));
    if (c.steerLinearity < 10) {
        c.steerLinearity = 10;
    }
    return c;
}

}  // namespace stcc
