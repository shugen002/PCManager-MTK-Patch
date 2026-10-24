// proxy.cpp: app-local winmm.dll -- pass-through plus the MT7922 HCI patch.
//
// HiConnectivityService.exe resolves winmm.dll through the normal search order,
// so this DLL in the PC Manager directory wins over System32.  Every one of the
// 181 exports is a thunk (generated from generator/winmm_exports.txt) that jumps
// through a pointer resolved here from the system winmm.dll, so the rest of PC
// Manager still sees an unchanged winmm.
//
// The only thing layered on top is src/realtek_hooks.cpp.  It is installed from a
// worker thread, once the pass-through is live, and only when \\.\MTKBTFilter
// exists -- on a machine without the MediaTek adapter this DLL does nothing but
// forward.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "log.h"
#include "winmm_ptrs.h"
#include "winmm_table.h"

extern "C" void MtkPatchInstall(void);

static HMODULE g_real = NULL;

// The genuine winmm lives in System32; load it by full path so the app-local
// search order cannot return our own module.
static HMODULE LoadSystemWinmm(void) {
    wchar_t dir[MAX_PATH] = {0};
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return NULL;

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\winmm.dll", dir);
    return LoadLibraryW(path);
}

static DWORD WINAPI InstallWorker(LPVOID) {
    MtkPatchInstall();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(module);

    g_real = LoadSystemWinmm();
    if (!g_real) {
        shimlog::Line("proxy", "FATAL: cannot load the system winmm.dll");
        return TRUE;
    }

    int unresolved = 0;
    for (int i = 0; i < kEntCount; ++i) {
        FARPROC p = kEnts[i].name
            ? GetProcAddress(g_real, kEnts[i].name)
            : GetProcAddress(g_real, (LPCSTR)(ULONG_PTR)kEnts[i].ord);
        if (p) {
            *kEnts[i].p = (UINT64)(ULONG_PTR)p;
        } else {
            ++unresolved;
        }
    }
    shimlog::Line("proxy", "pass-through ready: %d/%d exports resolved, %d unresolved",
                  kEntCount - unresolved, kEntCount, unresolved);

    // Deliberately off the loader lock: the patch install opens a device and
    // rewrites code, which is not DllMain material.
    HANDLE worker = CreateThread(NULL, 0, InstallWorker, NULL, 0, NULL);
    if (worker) CloseHandle(worker);
    return TRUE;
}
