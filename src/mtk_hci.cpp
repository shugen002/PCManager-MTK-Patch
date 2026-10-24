// mtk_hci.cpp: the MediaTek MT7922 vendor HCI channel.
//
// Nothing here is MediaTek-private beyond the two IOCTL numbers and the buffer
// layout, all of which were recovered from the driver (see docs/):
//
//   IOCTL 0x223418  input: buf[0] is skipped by the driver, buf+1 is the HCI
//                   packet; output: the vendor event that follows it.  This is
//                   the only channel that reports a Command Complete.
//                   It also pends the IRP, hence FILE_FLAG_OVERLAPPED.
//   IOCTL 0x223404  fire-and-forget submit over the bulk path.  Used only to
//                   wake a controller that answered ERROR_NOT_READY, which the
//                   driver's own sender hits as well.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

#include "mtk_hci.h"
#include "log.h"

#define MTK_CTL(fn)           ((DWORD)((0x22u << 16) | ((DWORD)(fn) << 2)))
#define IOCTL_MTK_HCI_VENDOR  MTK_CTL(0xD06)   // 0x223418  command + response
#define IOCTL_MTK_HCI_INJECT  MTK_CTL(0xD01)   // 0x223404  submit only

static const wchar_t* kDevice = L"\\\\.\\MTKBTFilter";

// One overlapped IOCTL.  Returns the Win32 error (ERROR_SUCCESS on success) and
// the byte count actually returned by the driver.
static DWORD IoOnce(HANDLE ch, DWORD code, const void* in, DWORD inLen,
                    void* out, DWORD outLen, DWORD* got, DWORD timeoutMs) {
    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return GetLastError();

    DWORD ret = 0;
    BOOL ok = DeviceIoControl(ch, code, (LPVOID)in, inLen, out, outLen, &ret, &ov);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && err == ERROR_IO_PENDING) {
        DWORD w = WaitForSingleObject(ov.hEvent, timeoutMs);
        if (w == WAIT_OBJECT_0) {
            err = GetOverlappedResult(ch, &ov, &ret, FALSE) ? ERROR_SUCCESS : GetLastError();
        } else {
            CancelIoEx(ch, &ov);
            err = ERROR_TIMEOUT;
        }
    }
    CloseHandle(ov.hEvent);
    if (got) *got = ret;
    return err;
}

// Submit an HCI packet over the fire-and-forget path.  The 10-byte header is the
// driver's BuildUpNewUrb: kind must be 4 (app HCI transfer) and the length must
// equal the packet length or the driver rejects it.
static DWORD Inject(HANDLE ch, const unsigned char* hci, DWORD len) {
    if (len < 3) return ERROR_INVALID_PARAMETER;
    unsigned char in[10 + 64];
    ZeroMemory(in, sizeof(in));
    DWORD kind = 4;
    CopyMemory(in + 0, &kind, 4);
    in[4] = 0x02;
    in[5] = 0x00;                 // u16 type = 2 (HCI command), little endian
    CopyMemory(in + 6, &len, 4);
    CopyMemory(in + 10, hci, len);

    unsigned char out[0x100];
    DWORD got = 0;
    return IoOnce(ch, IOCTL_MTK_HCI_INJECT, in, 10 + len, out, sizeof(out), &got, 3000);
}

// A controller parked in D3 answers the vendor request with ERROR_NOT_READY
// until something touches the bulk path; one Read Local Version is enough.
static void Wake(void) {
    HANDLE h = mtk::Open();
    if (h == INVALID_HANDLE_VALUE) return;
    static const unsigned char kReadLocalVersion[3] = { 0x01, 0x10, 0x00 };
    Inject(h, kReadLocalVersion, sizeof(kReadLocalVersion));
    CloseHandle(h);
}

namespace mtk {

bool Present() {
    static int state = 0;         // 0 = unknown, 1 = yes, -1 = no
    if (state) return state > 0;

    HANDLE h = CreateFileW(kDevice, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        state = -1;
        return false;
    }
    CloseHandle(h);
    state = 1;
    return true;
}

HANDLE Open() {
    return CreateFileW(kDevice, GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
}

unsigned short Command(HANDLE ch, unsigned short opcode,
                       const unsigned char* params, int paramLen, const char* tag) {
    if (ch == INVALID_HANDLE_VALUE || ch == NULL) return 0xFFFF;
    if (paramLen < 0 || paramLen > 64) return 0xFFFF;

    unsigned char pkt[3 + 64];
    pkt[0] = (unsigned char)(opcode & 0xFF);
    pkt[1] = (unsigned char)(opcode >> 8);
    pkt[2] = (unsigned char)paramLen;
    if (paramLen) CopyMemory(pkt + 3, params, paramLen);

    unsigned char in[1 + 3 + 64];
    in[0] = 0x00;                 // leading byte skipped by the driver
    CopyMemory(in + 1, pkt, 3 + paramLen);

    unsigned char out[0x104];
    DWORD got = 0;
    DWORD err = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 6; ++attempt) {
        ZeroMemory(out, sizeof(out));
        err = IoOnce(ch, IOCTL_MTK_HCI_VENDOR, in, 1 + 3 + paramLen,
                     out, sizeof(out), &got, 6000);
        if (err != ERROR_NOT_READY) break;
        Wake();
        Sleep(300);
    }
    if (err != ERROR_SUCCESS) {
        shimlog::Line("mtk", "%s -> ioctl err=%lu", tag, err);
        return 0xFFFF;
    }
    // The response carries one leading byte before the HCI event, so the
    // Command Complete status sits at out[5] (verified against the driver).
    if (got >= 6 && out[0] == 0x0E) {
        unsigned short status = out[5];
        if (status != 0) shimlog::Line("mtk", "%s -> status=0x%02X", tag, status);
        return status;
    }
    shimlog::Line("mtk", "%s -> %lu bytes, not a Command Complete", tag, got);
    return 0xFFFF;
}

}  // namespace mtk
