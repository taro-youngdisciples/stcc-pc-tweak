#pragma once

// COM インターフェースの vtable スロット差し替え。
// 元の関数ポインタは (vtable, index) ごとに保持するので、実装を共有する複数インターフェースでも取り違えない。
namespace stcc {

// iface の vtable[index] を detour に差し替える。同じ (vtable, index) は一度だけ。
void PatchVtable(void* iface, int index, void* detour, const char* name);
// self の vtable[index] の差し替え前の関数。未フックなら nullptr
void* FindVtableOriginal(void* self, int index);

template <class Fn>
Fn Orig(void* self, int index) {
    return reinterpret_cast<Fn>(FindVtableOriginal(self, index));
}

}  // namespace stcc
