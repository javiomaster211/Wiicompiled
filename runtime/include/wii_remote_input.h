#pragma once

#include <cstdint>

struct PADStatus;

// Real Wii Remotes paired over Bluetooth.
//
// SDL 3 ships a HIDAPI driver for them (Wii Remote alone, with Nunchuk, with a
// Classic Controller, and the Wii U Pro Controller) that exposes each as a
// regular SDL gamepad; it is off by default on SDL's side and ConfigureSdlHints
// turns it on. A bare remote or remote + Nunchuk is handed to the game as a real
// Wii Remote: the KPAD/WPAD HLE builds a KPADStatus from it every frame (see
// ReadKpadSample) and the game's own motion code does wheelies, tricks and Wii
// Wheel steering. Classic Controllers and Wii U Pro Controllers keep going
// through aurora's PAD layer as GameCube pads.
namespace WiiRemoteInput {

enum class Kind : uint8_t {
    NotWii,
    Remote,             // Wii Remote with no extension
    RemoteWithNunchuk,
    RemoteWithClassic,
    WiiUPro,
};

// Must run before SDL's joystick subsystem is initialized (aurora does that
// inside aurora_initialize); SDL only consults the hint on its first device scan.
void ConfigureSdlHints(bool enabled);

// Classifies a gamepad by the name SDL's Wii driver reports for it.
Kind KindForName(const char* gamepadName);
// Classification of the SDL gamepad currently assigned to a game port.
Kind KindForPort(uint32_t port);
const char* KindLabel(Kind kind);

// One frame of a Wii Remote in the units KPAD uses.
struct KpadSample {
    uint32_t hold = 0;      // WPAD button bits (WPAD_BUTTON_*), Nunchuk C/Z included
    float acc[3] = {};      // remote accelerometer in g, KPAD frame (rest: y = -1)
    bool hasNunchuk = false;
    float stick[2] = {};    // Nunchuk stick, -1..1, +y up
    float nunchukAcc[3] = {};
};

// True when the gamepad on `chan` is a bare Wii Remote or a remote + Nunchuk,
// i.e. one the game reads through KPAD.
bool IsRemoteChannel(uint32_t chan);
// Reads the current state of the remote on `chan`; false when IsRemoteChannel
// is false.
bool ReadKpadSample(uint32_t chan, KpadSample& sample);

// Reports "no controller" on the GameCube side for every port served through
// KPAD, so the game never sees the same remote twice. Runs after aurora's PADRead.
void HideRemotesFromPad(PADStatus* statuses, uint32_t count);

// Dolphin-style continuous scanning. SDL's HIDAPI Wii driver drops a remote on
// a failed Bluetooth read or an extension change and never re-adds it on its
// own. Poll() runs once per PADRead and, while no Wii controller is present,
// periodically forces SDL to re-enumerate HIDAPI by toggling the driver hint.
void Poll();
// Forces one re-enumeration right now (settings overlay "Rescan now").
void RescanNow();
// True while Poll() is actively rescanning (no Wii controller connected).
bool IsScanning();
// Rescans issued since a Wii controller was last seen.
uint32_t ScanCount();

} // namespace WiiRemoteInput
