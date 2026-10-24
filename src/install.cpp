// install.cpp: the startup gate and the single entry point for every patch.
//
// Nothing is touched unless this is a MediaTek MT7922-class machine (the BT
// filter driver's device object exists) and the host process is
// HiConnectivityService.exe.  Everywhere else the DLL stays a pure winmm
// pass-through, which is what makes it safe to drop next to PC Manager.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "log.h"
#include "mtk_hci.h"
#include "patches.h"

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

    InstallRealtekHooks();
    InstallWlanFix();
}
