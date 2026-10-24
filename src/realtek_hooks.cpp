// realtek_hooks.cpp: hand Huawei's advertising payload to the MT7922.
//
// PC Manager has no generic BLE advertiser.  HiConnectivityService.exe picks a
// vendor DLL from wifiType; on this combo card that is RealtekBTHuawei.dll, and
// its RtkApplOpenDevice cannot succeed without Realtek hardware, so the cached
// device handle stays -1.  RealtekApplSendAdv then returns before it builds
// anything -- and the call order Send -> Close -> Open resets that handle on
// every round anyway, so the payload builder is never reached.
//
// Three minimal interventions, none of them touching the payload Huawei writes:
//   * IAT-hook RtkApplOpenDevice      -> hand out a valid-looking handle
//   * patch RealtekApplCloseDevice (EXE RVA 0x83660) -> `xor eax,eax; ret`, so
//     the cached handle is not reset between Send and Open
//   * IAT-hook RtkSendHciLeSetMultiAdvParam -> report success; the real call
//     cannot work without Realtek hardware and would abort the sequence
// then RtkApplSendAdv is reached and its 31-byte AD payload is published through
// src/hci_adv.cpp.
//
// Installed only when \\.\MTKBTFilter exists (MT7922 class adapter) and
// only inside HiConnectivityService.exe; on anything else nothing is patched.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hci_adv.h"
#include "log.h"
#include "mtk_hci.h"

#define FAKE_DEVICE ((HANDLE)(ULONG_PTR)0x524B0001)
#define RVA_REALTEK_APPL_CLOSE_DEVICE 0x83660u
#define MAX_AD_LEN 31

// Signatures of the RealtekBTHuawei.dll entry points, as the EXE calls them.
typedef long (WINAPI* FnOpen)(HANDLE*);
typedef long (WINAPI* FnMulti)(UINT64, void*);
typedef long (WINAPI* FnSend)(UINT64, UINT64, void*, UINT64, UINT64, UINT64);

static FnOpen  g_open = NULL;
static FnMulti g_multi = NULL;
static FnSend  g_send = NULL;
static int     g_opens = 0;
static int     g_sends = 0;

static void* PatchSlot(void** slot, void* replacement) {
    DWORD old = 0;
    void* prev = *slot;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        *slot = replacement;
        VirtualProtect(slot, sizeof(void*), old, &old);
    }
    return prev;
}

// ------------------------------------------------------------------- hooks
static long WINAPI HookOpen(HANDLE* device) {
    if (device) *device = FAKE_DEVICE;
    if (g_opens++ == 0) {
        shimlog::Line("hook", "RtkApplOpenDevice -> faked success (handle=%p)",
                      (void*)FAKE_DEVICE);
    }
    return 0;
}

static long WINAPI HookMulti(UINT64 device, void* params) {
    (void)device;
    (void)params;
    return 0;
}

static long WINAPI HookSend(UINT64 instance, UINT64 device, void* ad, UINT64 adLen,
                            UINT64 p5, UINT64 p6) {
    (void)device;
    (void)p5;
    (void)p6;
    if (!ad || adLen == 0) return 0;

    if (g_sends++ == 0) {
        char hex[MAX_AD_LEN * 3 + 1];
        char* w = hex;
        int n = (int)adLen;
        if (n > MAX_AD_LEN) n = MAX_AD_LEN;
        for (int i = 0; i < n; ++i) {
            w += _snprintf_s(w, 3, _TRUNCATE, "%02x", ((const unsigned char*)ad)[i]);
        }
        shimlog::Line("hook", "RtkApplSendAdv instance=%u len=%d data=%s",
                      (unsigned)instance, (int)adLen, hex);
    }

    adv::Publish((const unsigned char*)ad, (int)adLen);
    return 0;
}

static void HookImports(HMODULE exe) {
    auto base = (unsigned char*)exe;
    auto dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto dir = nt->OptionalHeader.DataDirectory + IMAGE_DIRECTORY_ENTRY_IMPORT;
    if (!dir->VirtualAddress) return;

    int patched = 0;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir->VirtualAddress); imp->Name; ++imp) {
        if (lstrcmpiA((const char*)(base + imp->Name), "RealtekBTHuawei.dll") != 0) continue;

        // Without the name table there is nothing to match on, and iterating
        // FirstThunk while patching it would walk over our own replacements.
        if (!imp->OriginalFirstThunk) {
            shimlog::Line("hook", "RealtekBTHuawei.dll has no import-by-name table: skipped");
            continue;
        }

        auto thunk = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        auto slot = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; thunk->u1.AddressOfData; ++thunk, ++slot) {
            if (IMAGE_SNAP_BY_ORDINAL(thunk->u1.Ordinal)) continue;
            auto byName = (IMAGE_IMPORT_BY_NAME*)(base + thunk->u1.AddressOfData);
            const char* fn = (const char*)byName->Name;
            void** at = (void**)&slot->u1.Function;

            if (!lstrcmpA(fn, "RtkApplOpenDevice")) {
                g_open = (FnOpen)PatchSlot(at, (void*)HookOpen);
                ++patched;
            } else if (!lstrcmpA(fn, "RtkSendHciLeSetMultiAdvParam")) {
                g_multi = (FnMulti)PatchSlot(at, (void*)HookMulti);
                ++patched;
            } else if (!lstrcmpA(fn, "RtkApplSendAdv")) {
                g_send = (FnSend)PatchSlot(at, (void*)HookSend);
                ++patched;
            }
        }
    }

    if (patched == 0) {
        shimlog::Line("hook", "RealtekBTHuawei.dll imports not found: no hooks installed");
    } else {
        shimlog::Line("hook", "patched=%d open=%p multi=%p send=%p",
                      patched, (void*)g_open, (void*)g_multi, (void*)g_send);
    }
}

// The EXE wrapper at RVA 0x83660 resets the cached handle to -1.  Guard on the
// module name and the exact prologue so a PC Manager update cannot turn this
// into a write to arbitrary code.
static void PatchCloseWrapper(HMODULE exe) {
    unsigned char* p = (unsigned char*)exe + RVA_REALTEK_APPL_CLOSE_DEVICE;

    DWORD old = 0;
    if (!VirtualProtect(p, 3, PAGE_EXECUTE_READWRITE, &old)) {
        shimlog::Line("hook", "RealtekApplCloseDevice: VirtualProtect failed");
        return;
    }

    unsigned char b0 = p[0], b1 = p[1], b2 = p[2];
    if (b0 == 0x33 && b1 == 0xC0 && b2 == 0xC3) {
        shimlog::Line("hook", "RealtekApplCloseDevice already neutralised");
    } else if (b0 == 0x48 && b1 == 0x89) {
        // expected prologue: 48 89 4C 24 08 (mov qword ptr [rsp+8], rcx)
        p[0] = 0x33;   // xor eax, eax
        p[1] = 0xC0;
        p[2] = 0xC3;   // ret
        FlushInstructionCache(GetCurrentProcess(), p, 3);
        shimlog::Line("hook", "RealtekApplCloseDevice neutralised at %p (%02x %02x %02x)",
                      p, b0, b1, b2);
    } else {
        shimlog::Line("hook",
                      "RealtekApplCloseDevice SKIPPED: unexpected prologue %02x %02x %02x at %p",
                      b0, b1, b2, p);
    }
    VirtualProtect(p, 3, old, &old);
}

static bool IsTargetProcess(void) {
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, path, MAX_PATH);
    const char* name = path;
    for (const char* q = path; *q; ++q) {
        if (*q == '\\') name = q + 1;
    }
    return lstrcmpiA(name, "HiConnectivityService.exe") == 0;
}

// Called once from the worker thread that src/proxy.cpp starts in DllMain.
extern "C" void MtkPatchInstall(void) {
    if (!mtk::Present()) {
        shimlog::Line("patch", "no \\\\.\\MTKBTFilter: nothing patched");
        return;
    }
    if (!IsTargetProcess()) {
        shimlog::Line("patch", "not HiConnectivityService.exe: nothing patched");
        return;
    }

    HMODULE exe = GetModuleHandleA(NULL);
    if (!exe) {
        shimlog::Line("patch", "no main module: nothing patched");
        return;
    }

    HookImports(exe);
    PatchCloseWrapper(exe);
}
