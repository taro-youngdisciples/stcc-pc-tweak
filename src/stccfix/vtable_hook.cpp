#include "vtable_hook.h"

#include "common.h"

#include <iterator>

namespace stcc {
namespace {

SRWLOCK g_lock = SRWLOCK_INIT;

struct VtblSlot {
    void** vtbl;
    int index;
    void* original;
};
VtblSlot g_slots[256];
int g_slotCount = 0;

void* FindLocked(void** vtbl, int index) {
    for (int i = 0; i < g_slotCount; ++i) {
        if (g_slots[i].vtbl == vtbl && g_slots[i].index == index) {
            return g_slots[i].original;
        }
    }
    return nullptr;
}

}  // namespace

void PatchVtable(void* iface, int index, void* detour, const char* name) {
    if (!iface) {
        return;
    }
    void** vtbl = *static_cast<void***>(iface);
    AcquireSRWLockExclusive(&g_lock);
    if (!FindLocked(vtbl, index)) {
        if (g_slotCount >= static_cast<int>(std::size(g_slots))) {
            Log("  hook %s: スロット表が満杯", name);
        } else {
            DWORD oldProtect = 0;
            if (VirtualProtect(&vtbl[index], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                g_slots[g_slotCount++] = {vtbl, index, vtbl[index]};
                vtbl[index] = detour;
                VirtualProtect(&vtbl[index], sizeof(void*), oldProtect, &oldProtect);
                Log("  hook %s: vtbl=%p[%d] orig=%p", name, static_cast<void*>(vtbl), index,
                    g_slots[g_slotCount - 1].original);
            } else {
                Log("  hook %s: VirtualProtect 失敗 (%lu)", name, GetLastError());
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void* FindVtableOriginal(void* self, int index) {
    void** vtbl = *static_cast<void***>(self);
    AcquireSRWLockShared(&g_lock);
    void* p = FindLocked(vtbl, index);
    ReleaseSRWLockShared(&g_lock);
    return p;
}

}  // namespace stcc
