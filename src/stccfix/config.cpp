#include "common.h"

namespace stcc {

Config LoadConfig(const std::wstring& iniPath) {
    Config c;
    const wchar_t* ini = iniPath.c_str();
    c.windowed = GetPrivateProfileIntW(L"Display", L"Windowed", c.windowed ? 1 : 0, ini) != 0;
    c.logGraphics = GetPrivateProfileIntW(L"Debug", L"LogGraphics", c.logGraphics ? 1 : 0, ini) != 0;
    return c;
}

}  // namespace stcc
