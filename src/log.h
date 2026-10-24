#pragma once
// Minimal file logger shared by the shim modules.
//
// HiConnectivityService runs as SYSTEM, so ProgramData is the only location that
// is writable without asking for anything and survives across service restarts.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

namespace shimlog {

static const wchar_t* kLogDir  = L"C:\\ProgramData\\MTK-Patch";
static const wchar_t* kLogFile = L"C:\\ProgramData\\MTK-Patch\\mt7922_hci.log";

inline void Line(const char* tag, const char* fmt, ...) {
    char msg[768];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    const char* base = exe;
    for (const char* q = exe; *q; ++q) {
        if (*q == '\\') base = q + 1;
    }

    char line[1024];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[pid=%lu %s] [%s] %s\r\n",
                GetCurrentProcessId(), base, tag, msg);

    HANDLE h = CreateFileW(kLogFile, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        CreateDirectoryW(kLogDir, NULL);
        h = CreateFileW(kLogFile, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return;
    }
    SetFilePointer(h, 0, NULL, FILE_END);
    DWORD written = 0;
    WriteFile(h, line, (DWORD)lstrlenA(line), &written, NULL);
    CloseHandle(h);
}

}  // namespace shimlog
