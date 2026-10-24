#pragma once
// Import-table patching helpers shared by the patch modules.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Replace one pointer, keeping the page protection intact.
static inline void* PatchSlot(void** slot, void* replacement) {
    DWORD old = 0;
    void* prev = *slot;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        *slot = replacement;
        VirtualProtect(slot, sizeof(void*), old, &old);
    }
    return prev;
}

// Replace one function that `module` imports from `dll`.  Returns the previous
// pointer, or NULL when that import is not there (or has no name table).
static inline void* PatchImport(HMODULE module, const char* dll, const char* func,
                                void* replacement) {
    auto base = (unsigned char*)module;
    if (!base) return NULL;
    auto dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto dir = nt->OptionalHeader.DataDirectory + IMAGE_DIRECTORY_ENTRY_IMPORT;
    if (!dir->VirtualAddress) return NULL;

    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir->VirtualAddress); imp->Name; ++imp) {
        if (lstrcmpiA((const char*)(base + imp->Name), dll) != 0) continue;

        // Without the name table there is nothing to match on, and walking
        // FirstThunk while patching it would step over our own replacements.
        if (!imp->OriginalFirstThunk) return NULL;

        auto thunk = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        auto slot = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; thunk->u1.AddressOfData; ++thunk, ++slot) {
            if (IMAGE_SNAP_BY_ORDINAL(thunk->u1.Ordinal)) continue;
            auto byName = (IMAGE_IMPORT_BY_NAME*)(base + thunk->u1.AddressOfData);
            if (!lstrcmpA((const char*)byName->Name, func)) {
                return PatchSlot((void**)&slot->u1.Function, replacement);
            }
        }
    }
    return NULL;
}
