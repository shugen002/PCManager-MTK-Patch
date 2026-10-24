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
// Installed by src/install.cpp, which already checked that this is an MT7922
// machine and that the process is HiConnectivityService.exe.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hci_adv.h"
#include "log.h"
#include "patch_util.h"
#include "patches.h"

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

// The EXE wrapper at RVA 0x83660 resets the cached handle to -1.  Verify the
// prologue first so a PC Manager update cannot turn this into a write into
// arbitrary code.
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

void InstallRealtekHooks(void) {
    HMODULE exe = GetModuleHandleA(NULL);
    if (!exe) {
        shimlog::Line("hook", "no main module: nothing patched");
        return;
    }

    g_open  = (FnOpen) PatchImport(exe, "RealtekBTHuawei.dll", "RtkApplOpenDevice",
                                   (void*)HookOpen);
    g_multi = (FnMulti)PatchImport(exe, "RealtekBTHuawei.dll", "RtkSendHciLeSetMultiAdvParam",
                                   (void*)HookMulti);
    g_send  = (FnSend) PatchImport(exe, "RealtekBTHuawei.dll", "RtkApplSendAdv",
                                   (void*)HookSend);

    int patched = (g_open ? 1 : 0) + (g_multi ? 1 : 0) + (g_send ? 1 : 0);
    if (patched == 0) {
        shimlog::Line("hook", "RealtekBTHuawei.dll imports not found: no advertising hooks");
    } else {
        shimlog::Line("hook", "advertising hooks: patched=%d open=%p multi=%p send=%p",
                      patched, (void*)g_open, (void*)g_multi, (void*)g_send);
    }

    PatchCloseWrapper(exe);
}
