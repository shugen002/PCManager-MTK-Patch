// hci_adv.cpp: keep Huawei's advertisement on the air through the MT7922.
//
// Only the extended advertising commands work on this controller; the legacy
// 0x2006 / 0x2008 / 0x200A set answers 0x0C (Command Disallowed).  The verified
// sequence, one advertising set:
//
//   0x2036 LE Set Extended Advertising Parameters
//          handle 0xE0, properties 0x10 (legacy PDU, so BLE 4.2 scanners hear it),
//          interval 0xA0 (100 ms), all three channels, public address, 1M phy
//   0x2037 LE Set Extended Advertising Data
//          operation 0x03 (complete data), fragment preference 0x01, 31 bytes max
//   0x2039 LE Set Extended Advertising Enable
//          enable, one set, handle, duration in 10 ms units
//
// The vendor channel cannot be used while an advertising set is running, so the
// set is armed with an explicit duration and then re-armed: Huawei hands us a
// payload only when its scene changes, so the worker keeps the last one alive.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>

#include "hci_adv.h"
#include "log.h"
#include "mtk_hci.h"

#define ADV_HANDLE     0xE0
#define ADV_WINDOW_MS  1500       // advertising lifetime per arm
#define ADV_REARM_MS   200        // re-arm this soon after the window expires
#define MAX_AD_LEN     31

// Arm one window; true when the controller accepted the whole sequence.
static bool ArmWindow(HANDLE ch, const unsigned char* ad, int len) {
    // 0x2036: handle, properties(2), interval min(3), interval max(3),
    //         channel map, own address type, peer address type, peer address(6),
    //         filter policy, tx power, primary phy, max skip, secondary phy,
    //         sid, scan-request notification.
    const unsigned char par36[25] = {
        ADV_HANDLE, 0x10, 0x00,             // legacy PDU, not connectable
        0xA0, 0x00, 0x00, 0xA0, 0x00, 0x00, // 100 ms .. 100 ms
        0x07,                               // channels 37, 38, 39
        0x00,                               // own address: public
        0x00, 0, 0, 0, 0, 0, 0,             // peer type + address (unused)
        0x00,                               // filter policy
        0x7F,                               // tx power: no preference
        0x01,                               // primary phy: 1M
        0x00,                               // secondary max skip
        0x01,                               // secondary phy: 1M
        0x00,                               // advertising sid
        0x00                                // scan request notification: off
    };
    if (mtk::Command(ch, 0x2036, par36, sizeof(par36), "0x2036 params") != 0) return false;

    unsigned char par37[4 + MAX_AD_LEN];
    par37[0] = ADV_HANDLE;
    par37[1] = 0x03;                        // complete data, no fragment
    par37[2] = 0x01;                        // controller may fragment
    par37[3] = (unsigned char)len;
    CopyMemory(par37 + 4, ad, len);
    if (mtk::Command(ch, 0x2037, par37, 4 + len, "0x2037 data") != 0) return false;

    unsigned short duration10ms = ADV_WINDOW_MS / 10;
    unsigned char par39[6] = {
        0x01,                               // enable
        0x01,                               // number of sets
        ADV_HANDLE,
        (unsigned char)(duration10ms & 0xFF),
        (unsigned char)(duration10ms >> 8),
        0x00                                // no per-set max events
    };
    return mtk::Command(ch, 0x2039, par39, sizeof(par39), "0x2039 enable") == 0;
}

// ------------------------------------------------------------------ payload
static volatile LONG  g_init = 0;       // 0 new, 1 initialising, 2 ready
static CRITICAL_SECTION g_cs;
static HANDLE         g_wake = NULL;    // auto-reset; interrupts the worker's waits
static HANDLE         g_thread = NULL;
static unsigned char  g_ad[MAX_AD_LEN];
static int            g_adLen = 0;

static unsigned __stdcall Worker(void*) {
    shimlog::Line("adv", "worker up");
    HANDLE ch = INVALID_HANDLE_VALUE;
    DWORD until = 0;
    bool announced = false;

    for (;;) {
        unsigned char ad[MAX_AD_LEN];
        int len;
        EnterCriticalSection(&g_cs);
        len = g_adLen;
        if (len > 0) CopyMemory(ad, g_ad, len);
        LeaveCriticalSection(&g_cs);

        if (len <= 0) {
            WaitForSingleObject(g_wake, 200);
            continue;
        }

        DWORD now = GetTickCount();
        if (until && (int)(now - until) < 0) {   // window still running
            WaitForSingleObject(g_wake, 50);
            continue;
        }

        if (ch == INVALID_HANDLE_VALUE) {
            ch = mtk::Open();
            if (ch == INVALID_HANDLE_VALUE) {
                static int logged = 0;
                if (logged++ < 2) shimlog::Line("adv", "cannot open \\\\.\\MTKBTFilter");
                WaitForSingleObject(g_wake, 2000);
                continue;
            }
        }

        if (ArmWindow(ch, ad, len)) {
            until = GetTickCount() + ADV_WINDOW_MS + ADV_REARM_MS;
            if (!announced) {
                announced = true;
                shimlog::Line("adv", "advertising natively (extended commands, legacy PDU)");
            }
        } else {
            // The channel often sticks right after a previous window; drop the
            // handle and start over after a pause.
            CloseHandle(ch);
            ch = INVALID_HANDLE_VALUE;
            until = 0;
            WaitForSingleObject(g_wake, 500);
        }
    }
    // The worker only ends with the process.
    if (ch != INVALID_HANDLE_VALUE) CloseHandle(ch);
    return 0;
}

namespace adv {

void Publish(const unsigned char* ad, int len) {
    if (!ad || len <= 0) return;
    if (len > MAX_AD_LEN) len = MAX_AD_LEN;

    if (g_init != 2) {
        if (InterlockedCompareExchange(&g_init, 1, 0) == 0) {
            InitializeCriticalSection(&g_cs);
            g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
            InterlockedExchange(&g_init, 2);
        } else {
            while (g_init != 2) Sleep(1);
        }
    }

    bool startWorker = false;
    EnterCriticalSection(&g_cs);
    CopyMemory(g_ad, ad, len);
    g_adLen = len;
    if (!g_thread) {
        g_thread = (HANDLE)_beginthreadex(NULL, 0, Worker, NULL, 0, NULL);
        startWorker = true;
    }
    LeaveCriticalSection(&g_cs);

    if (startWorker) shimlog::Line("adv", "worker=%p", (void*)g_thread);
    SetEvent(g_wake);
}

}  // namespace adv
