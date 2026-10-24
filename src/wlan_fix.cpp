// wlan_fix.cpp: repair the WLAN switch-state query inside HiConnectivityService.
//
// Measured problem: WifiBaseAdapter::GetWifiSwitchState calls WlanQueryInterface
// and gets ERROR_NOT_FOUND (1168), so PC Manager concludes the WLAN is off: the
// super-device panel shows "click to enable WLAN" while the WLAN is connected and
// the radio is on.  The identical query from another process succeeds, so the API
// is fine -- the caller is handing WlanQueryInterface a wrong interface GUID (or
// a stale client handle).
//
// The hook calls the real API first and only on ERROR_NOT_FOUND retries with:
//   1. the caller's handle and a GUID taken from WlanEnumInterfaces
//   2. a freshly opened client handle and a freshly enumerated GUID
// The caller then gets the data it asked for, or the original error.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wlanapi.h>

#include "log.h"
#include "patch_util.h"
#include "patches.h"

typedef DWORD (WINAPI* FnQuery)(HANDLE, const GUID*, WLAN_INTF_OPCODE, PVOID, PDWORD,
                                PVOID*, PWLAN_OPCODE_VALUE_TYPE);
typedef DWORD (WINAPI* FnEnum)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST*);
typedef VOID  (WINAPI* FnFree)(PVOID);
typedef DWORD (WINAPI* FnOpen)(DWORD, PVOID, PDWORD, PHANDLE);
typedef DWORD (WINAPI* FnClose)(HANDLE, PVOID);

static FnQuery g_query = NULL;
static FnEnum  g_enum  = NULL;
static FnFree  g_free  = NULL;
static FnOpen  g_open  = NULL;
static FnClose g_close = NULL;

static GUID g_guid = {};
static bool g_haveGuid = false;
static int  g_repairs = 0;

static bool EnumerateGuid(HANDLE client) {
    if (g_haveGuid) return true;
    if (!g_enum || !client) return false;

    PWLAN_INTERFACE_INFO_LIST list = NULL;
    if (g_enum(client, NULL, &list) != ERROR_SUCCESS || !list) return false;
    if (list->dwNumberOfItems > 0) {
        g_guid = list->InterfaceInfo[0].InterfaceGuid;
        g_haveGuid = true;
    }
    if (g_free) g_free(list);
    return g_haveGuid;
}

static DWORD WINAPI HookQuery(HANDLE client, const GUID* guid, WLAN_INTF_OPCODE opcode,
                              PVOID reserved, PDWORD size, PVOID* data,
                              PWLAN_OPCODE_VALUE_TYPE type) {
    DWORD rc = g_query ? g_query(client, guid, opcode, reserved, size, data, type)
                       : ERROR_PROC_NOT_FOUND;
    if (rc != ERROR_NOT_FOUND) return rc;

    // repair 1: the caller's handle, GUID from the interface list
    if (EnumerateGuid(client) && (guid == NULL || !IsEqualGUID(*guid, g_guid))) {
        DWORD retry = g_query(client, &g_guid, opcode, reserved, size, data, type);
        if (retry == ERROR_SUCCESS) {
            if (g_repairs++ == 0) {
                shimlog::Line("wlan", "repaired the switch-state query with the enumerated GUID");
            }
            return retry;
        }
        rc = retry;
    }

    // repair 2: our own client handle and a freshly enumerated GUID
    if (g_open && g_enum) {
        DWORD negotiated = 0;
        HANDLE fresh = NULL;
        if (g_open(2, NULL, &negotiated, &fresh) == ERROR_SUCCESS && fresh) {
            PWLAN_INTERFACE_INFO_LIST list = NULL;
            if (g_enum(fresh, NULL, &list) == ERROR_SUCCESS && list &&
                list->dwNumberOfItems > 0) {
                GUID g = list->InterfaceInfo[0].InterfaceGuid;
                DWORD retry = g_query(fresh, &g, opcode, reserved, size, data, type);
                if (retry == ERROR_SUCCESS) {
                    g_guid = g;
                    g_haveGuid = true;
                    if (g_repairs++ == 0) {
                        shimlog::Line("wlan", "repaired the switch-state query with a fresh handle");
                    }
                    if (g_free) g_free(list);
                    if (g_close) g_close(fresh, NULL);
                    return retry;
                }
                rc = retry;
            }
            if (list && g_free) g_free(list);
            if (g_close) g_close(fresh, NULL);
        }
    }

    if (g_repairs == 0) {
        shimlog::Line("wlan", "switch-state query still fails (err=%lu)", rc);
    }
    return rc;
}

void InstallWlanFix(void) {
    HMODULE api = GetModuleHandleW(L"wlanapi.dll");
    if (!api) api = LoadLibraryW(L"wlanapi.dll");
    if (!api) {
        shimlog::Line("wlan", "wlanapi not loadable: nothing patched");
        return;
    }

    g_query = (FnQuery)GetProcAddress(api, "WlanQueryInterface");
    g_enum  = (FnEnum) GetProcAddress(api, "WlanEnumInterfaces");
    g_free  = (FnFree) GetProcAddress(api, "WlanFreeMemory");
    g_open  = (FnOpen) GetProcAddress(api, "WlanOpenHandle");
    g_close = (FnClose)GetProcAddress(api, "WlanCloseHandle");
    if (!g_query) {
        shimlog::Line("wlan", "WlanQueryInterface not found: nothing patched");
        return;
    }

    HMODULE exe = GetModuleHandleA(NULL);
    void* previous = exe ? PatchImport(exe, "wlanapi.dll", "WlanQueryInterface",
                                       (void*)HookQuery)
                         : NULL;
    if (previous) {
        shimlog::Line("wlan", "WlanQueryInterface hooked (real=%p)", (void*)g_query);
    } else {
        shimlog::Line("wlan", "WlanQueryInterface is not imported: nothing patched");
    }
}
