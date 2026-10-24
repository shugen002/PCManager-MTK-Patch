#pragma once
// Everything src/install.cpp switches on once the startup gate has passed.

// Unblock Huawei's Realtek advertising path and intercept the payload it builds.
void InstallRealtekHooks(void);

// Repair the WLAN switch-state query so an already-enabled WLAN is not shown as
// off by PC Manager.
void InstallWlanFix(void);
