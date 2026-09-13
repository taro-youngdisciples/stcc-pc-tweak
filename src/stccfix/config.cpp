#include "common.h"

namespace stcc {

Config LoadConfig(const std::wstring& iniPath) {
    Config c;
    c.windowed = GetPrivateProfileIntW(L"Display", L"Windowed", c.windowed ? 1 : 0, iniPath.c_str()) != 0;
    return c;
}

}  // namespace stcc
