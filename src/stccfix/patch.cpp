#include "common.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace stcc {
namespace {

// data/addresses/versions.toml の jp-1.02 に対応。
// DllMain 内で重いハッシュ計算やライブラリロードをしないよう、PE ヘッダの値で判定し、
// 実際に書き換える前に元のバイト列を必ず照合する。
constexpr DWORD kJp102TimeDateStamp = 0x3533084F;
constexpr DWORD kJp102SizeOfImage = 0x00EC0000;

std::vector<std::uint8_t> ParseHex(const char* s) {
    std::vector<std::uint8_t> out;
    while (*s) {
        if (*s == ' ') {
            ++s;
            continue;
        }
        char* end = nullptr;
        out.push_back(static_cast<std::uint8_t>(std::strtoul(s, &end, 16)));
        s = end;
    }
    return out;
}

}  // namespace

GameVersion DetectGameVersion() {
    auto base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    Log("exe: TimeDateStamp=0x%08lX SizeOfImage=0x%08lX ImageBase=0x%08lX", nt->FileHeader.TimeDateStamp,
        nt->OptionalHeader.SizeOfImage, nt->OptionalHeader.ImageBase);
    if (nt->FileHeader.TimeDateStamp == kJp102TimeDateStamp && nt->OptionalHeader.SizeOfImage == kJp102SizeOfImage) {
        return GameVersion::Jp102;
    }
    return GameVersion::Unknown;
}

const char* GameVersionName(GameVersion v) {
    switch (v) {
        case GameVersion::Jp102: return "jp-1.02";
        default: return "unknown";
    }
}

bool ApplyPatches(const BytePatch* patches, std::size_t count) {
    std::vector<std::vector<std::uint8_t>> olds, news;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& p = patches[i];
        olds.push_back(ParseHex(p.original));
        news.push_back(ParseHex(p.replacement));
        if (olds.back().size() != news.back().size()) {
            Log("patch 0x%08X: 長さ不一致（定義ミス）", p.va);
            return false;
        }
        if (std::memcmp(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(p.va)), olds.back().data(),
                        olds.back().size()) != 0) {
            Log("patch 0x%08X: 元のバイト列が一致しないため全パッチを中止 (%s)", p.va, p.description);
            return false;
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto& p = patches[i];
        auto addr = reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(p.va));
        DWORD oldProtect = 0;
        if (!VirtualProtect(addr, news[i].size(), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            Log("patch 0x%08X: VirtualProtect 失敗 (%lu)", p.va, GetLastError());
            return false;
        }
        std::memcpy(addr, news[i].data(), news[i].size());
        VirtualProtect(addr, news[i].size(), oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), addr, news[i].size());
        Log("patch 0x%08X: 適用 %s", p.va, p.description);
    }
    return true;
}

}  // namespace stcc
