#pragma once

// Mayflash DolphinBar presence detection.
//
// The DolphinBar pairs Wii Remotes against its own Bluetooth radio and
// re-exposes them over USB. In mode 4 (LED 4) it is a pure passthrough: four
// always-present HID interfaces carrying Nintendo's own VID:PID 057e:0306,
// one per sync slot, whether or not a remote is synced. That makes the bar
// indistinguishable from real remotes by IDs alone; like Dolphin, we tell
// them apart by the bus-reported device description, which only the bar sets
// to "Mayflash Wiimote PC Adapter". Modes 1-3 instead emulate a mouse or four
// generic joysticks (VID:PID 0079:1802/1803), useless for KPAD, so seeing
// those means the user needs to press the bar's MODE button.
//
// Detection enumerates device interfaces without opening any handles, so it
// never interferes with the HID handles SDL's Wii driver keeps open. Only
// implemented on Windows; elsewhere every query reports "no bar".
namespace DolphinBar {

// What the last enumeration saw.
struct Status {
    bool present = false;  // any interface identified as a DolphinBar
    bool mode4 = false;    // Wii Remote passthrough slots found (the mode we support)
    bool mode123 = false;  // the bar's mouse/joystick emulation IDs found
    int mode4Slots = 0;    // passthrough slots seen; a bar in mode 4 exposes 4
};

// Result of the most recent Refresh() (or an all-false Status before the first).
// By value: Refresh() may run concurrently on another thread.
Status Cached();

// Re-runs Detect() into Cached(), at most once per second no matter how often
// it is called. Safe to call from UI and polling code alike.
void Refresh();

} // namespace DolphinBar
