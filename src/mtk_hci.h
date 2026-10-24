#pragma once
// MediaTek MT7922 vendor HCI channel.
//
// The MediaTek Bluetooth filter driver exposes \\.\MTKBTFilter and turns
// IOCTL 0x223418 into "send these bytes to the controller as a USB vendor
// request, then wait for one vendor event and copy it back".  That is the only
// user-mode path that both issues an HCI command and returns its Command
// Complete; IOCTL 0x223404 only submits (used here to wake a controller that
// answered ERROR_NOT_READY).
//
// Only the parts needed to drive one advertising set are implemented on top:
// see src/hci_adv.cpp.
namespace mtk {

// Is this a MediaTek-class adapter?  True when the filter driver's device
// object exists.  Nothing in this DLL is patched when it returns false.
bool Present();

// Open the vendor channel.  The driver pends the IRP, so the handle is
// overlapped; INVALID_HANDLE_VALUE means the device is not usable right now.
HANDLE Open();

// Send one HCI command and return the Command Complete status byte (0x00 OK),
// or 0xFFFF when the controller did not answer.  Retries the ERROR_NOT_READY
// case that a controller parked in D3 produces.  `tag` labels diagnostics only.
unsigned short Command(HANDLE channel, unsigned short opcode,
                       const unsigned char* params, int paramLen, const char* tag);

}  // namespace mtk
