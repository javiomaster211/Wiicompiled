#include "wii_remote_input.h"

#include "runtime_config.h"
#include "runtime_log.h"

#include <dolphin/pad.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_sensor.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace WiiRemoteInput {
namespace {

// Dolphin's continuous scanning polls Bluetooth about once a second; SDL's
// enumeration walks every HID device on the main thread, so stay a bit lazier.
constexpr uint64_t kScanIntervalMs = 2000;
// Right after a remote drops it is almost certainly still there, but the first
// re-open attempts tend to fail on timed-out reads, so retry quickly for a while.
constexpr uint64_t kFastScanIntervalMs = 500;
constexpr uint64_t kFastScanWindowMs = 15000;
// How long the Wii driver hint stays at "0" during a rescan; SDL applies hint
// changes on its next joystick update, once per pumped frame.
constexpr uint64_t kRescanDriverOffMs = 100;

constexpr float kStandardGravity = 9.80665f;

// SDL's Wii driver posts the remote's own buttons as raw joystick buttons
// starting at SDL_GAMEPAD_BUTTON_MISC1, in this order (SDL_hidapi_wii.c,
// EWiiButtons), whatever the extension.
enum RawWiiButton : int {
    kRawA = SDL_GAMEPAD_BUTTON_MISC1,
    kRawB,
    kRawOne,
    kRawTwo,
    kRawPlus,
    kRawMinus,
    kRawHome,
    kRawDpadUp,
    kRawDpadDown,
    kRawDpadLeft,
    kRawDpadRight,
};

// WPAD_BUTTON_* bits as the game reads them from KPADStatus.hold.
constexpr uint32_t kWpadLeft = 0x0001, kWpadRight = 0x0002, kWpadDown = 0x0004, kWpadUp = 0x0008,
                   kWpadPlus = 0x0010, kWpadTwo = 0x0100, kWpadOne = 0x0200, kWpadB = 0x0400, kWpadA = 0x0800,
                   kWpadMinus = 0x1000, kWpadZ = 0x2000, kWpadC = 0x4000, kWpadHome = 0x8000;

bool g_wiiDriverEnabled = false;
uint64_t g_lastScanMs = 0;
uint32_t g_scanCount = 0;
bool g_scanning = false;
// Non-zero while a rescan has the Wii driver hint switched off (see RescanNow).
uint64_t g_driverOffSinceMs = 0;
// When the current scan started (last Wii controller seen).
uint64_t g_lostAtMs = 0;

// Instance ids whose accelerometers have been switched on. SDL keeps sensors
// off until asked and forgets that when the gamepad is closed, so a re-paired
// remote gets a fresh id and is enabled again.
std::array<SDL_JoystickID, PAD_MAX_CONTROLLERS> g_sensorsEnabledFor{};

// Case-sensitive substring test that tolerates a null name.
bool NameContains(const char* name, const char* needle) {
    return name != nullptr && std::strstr(name, needle) != nullptr;
}

// Turns on the remote (and Nunchuk) accelerometers once per gamepad instance.
void EnsureSensors(SDL_Gamepad* gamepad, uint32_t port) {
    const SDL_JoystickID id = SDL_GetGamepadID(gamepad);
    if (g_sensorsEnabledFor[port] == id) {
        return;
    }
    for (SDL_SensorType sensor : {SDL_SENSOR_ACCEL, SDL_SENSOR_ACCEL_L}) {
        if (SDL_GamepadHasSensor(gamepad, sensor) && !SDL_SetGamepadSensorEnabled(gamepad, sensor, true)) {
            RT_LOG(RT_TAG_CONFIG) << "Wii Remote on port " << (port + 1)
                                  << ": could not enable an accelerometer: " << SDL_GetError() << std::endl;
        }
    }
    g_sensorsEnabledFor[port] = id;
}

// SDL reports (-wiiX, wiiZ, wiiY) in m/s^2: right across the face, out of the
// face (+9.8 at rest face up), along the length towards the tip. KPAD's acc is
// in g with the face normal on y resting at -1, x across the face and z along
// the length: the WPAD reading negated with y/z swapped.
bool ReadAccelAsKpad(SDL_Gamepad* gamepad, SDL_SensorType sensor, float* kpad) {
    float sdl[3] = {};
    if (!SDL_GamepadSensorEnabled(gamepad, sensor) || !SDL_GetGamepadSensorData(gamepad, sensor, sdl, 3) ||
        !std::isfinite(sdl[0]) || !std::isfinite(sdl[1]) || !std::isfinite(sdl[2])) {
        return false;
    }
    kpad[0] = sdl[0] / kStandardGravity;
    kpad[1] = -sdl[1] / kStandardGravity;
    kpad[2] = -sdl[2] / kStandardGravity;
    return true;
}

// True when any controller aurora knows about is a Wii device.
bool AnyWiiControllerConnected() {
    const uint32_t count = PADCount();
    for (uint32_t index = 0; index < count; ++index) {
        if (KindForName(PADGetNameForControllerIndex(index)) != Kind::NotWii) {
            return true;
        }
    }
    return false;
}

// Route SDL's input diagnostics (HIDAPI open failures, the Wii driver's
// extension/status messages) into console.log, minus the periodic chatter.
void SDLCALL LogSdlMessage(void*, int category, SDL_LogPriority priority, const char* message) {
    if (message == nullptr) {
        return;
    }
    if (category == SDL_LOG_CATEGORY_INPUT && priority < SDL_LOG_PRIORITY_WARN &&
        (std::strstr(message, "Motion Plus") != nullptr || std::strstr(message, "Resetting report mode") != nullptr)) {
        return;
    }
    if (category == SDL_LOG_CATEGORY_INPUT || priority >= SDL_LOG_PRIORITY_WARN) {
        RT_LOG("sdl") << message << std::endl;
    }
}

// Second half of a rescan: re-enables the Wii driver once SDL has seen it off.
void FinishRescan(uint64_t now) {
    if (g_driverOffSinceMs == 0 || now - g_driverOffSinceMs < kRescanDriverOffMs) {
        return;
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_WII, "1");
    g_driverOffSinceMs = 0;
    g_lastScanMs = now;
    ++g_scanCount;
}

} // namespace

// Enables SDL's HIDAPI Wii driver and player LEDs, and routes SDL's input log.
void ConfigureSdlHints(bool enabled) {
    if (!SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_WII, enabled ? "1" : "0")) {
        RT_LOG(RT_TAG_CONFIG) << "Failed to set " << SDL_HINT_JOYSTICK_HIDAPI_WII << ": " << SDL_GetError()
                              << std::endl;
    }
    // Light the player LED that matches the SDL player index, like the console does.
    if (!SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_WII_PLAYER_LED, "1")) {
        RT_LOG(RT_TAG_CONFIG) << "Failed to set " << SDL_HINT_JOYSTICK_HIDAPI_WII_PLAYER_LED << ": "
                              << SDL_GetError() << std::endl;
    }
    RT_LOG(RT_TAG_CONFIG) << "Bluetooth Wii Remote support " << (enabled ? "enabled" : "disabled") << std::endl;
    g_wiiDriverEnabled = enabled;
    if (enabled) {
        SDL_SetLogPriority(SDL_LOG_CATEGORY_INPUT, SDL_LOG_PRIORITY_DEBUG);
        SDL_SetLogOutputFunction(LogSdlMessage, nullptr);
    }
}

// Starts a rescan by disabling the Wii driver hint; Poll() finishes it.
void RescanNow() {
    if (!g_wiiDriverEnabled || g_driverOffSinceMs != 0) {
        return;
    }
    // SDL only closes the HID handle of a remote it dropped while the Wii driver
    // is disabled, and only re-opens it when the driver is enabled again; both
    // must happen on separate joystick updates, so the hint stays at "0" until
    // FinishRescan() a few frames later. Flipping 1->0->1 within one frame does
    // nothing: SDL only ever sees the final "1".
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_WII, "0");
    g_driverOffSinceMs = SDL_GetTicks();
}

// Per-frame scanning state machine: rescans while no Wii controller is present.
void Poll() {
    if (!g_wiiDriverEnabled) {
        return;
    }
    // Always complete a rescan in progress so the driver is never left disabled.
    FinishRescan(SDL_GetTicks());
    if (g_driverOffSinceMs != 0) {
        return;
    }
    if (AnyWiiControllerConnected()) {
        if (g_scanning) {
            RT_LOG(RT_TAG_CONFIG) << "Wii Remote found after " << g_scanCount << " rescan(s)" << std::endl;
        }
        g_scanning = false;
        g_scanCount = 0;
        g_lastScanMs = SDL_GetTicks();
        return;
    }
    if (!RuntimeConfigFile::WiiContinuousScanEnabled(true)) {
        g_scanning = false;
        return;
    }
    const uint64_t now = SDL_GetTicks();
    if (!g_scanning) {
        RT_LOG(RT_TAG_CONFIG) << "No Wii Remote connected; scanning for one (press 1+2 on the remote)"
                              << std::endl;
        g_scanning = true;
        g_lostAtMs = now;
    }
    const uint64_t interval = now - g_lostAtMs < kFastScanWindowMs ? kFastScanIntervalMs : kScanIntervalMs;
    if (now - g_lastScanMs < interval) {
        return;
    }
    RescanNow();
}

// True while Poll() is looking for a remote.
bool IsScanning() {
    return g_scanning;
}

// Number of rescans since a Wii controller was last seen.
uint32_t ScanCount() {
    return g_scanCount;
}

// Maps the gamepad name SDL's Wii driver reports to a Kind.
Kind KindForName(const char* name) {
    // Names come from SDL's hidapi Wii driver: "Nintendo Wii Remote",
    // "Nintendo Wii Remote with Nunchuk", "Nintendo Wii Remote with Classic
    // Controller" and "Nintendo Wii U Pro Controller".
    if (NameContains(name, "Wii U Pro Controller")) return Kind::WiiUPro;
    if (!NameContains(name, "Wii Remote")) return Kind::NotWii;
    if (NameContains(name, "Nunchuk")) return Kind::RemoteWithNunchuk;
    if (NameContains(name, "Classic Controller")) return Kind::RemoteWithClassic;
    return Kind::Remote;
}

// Kind of the SDL gamepad assigned to a game port, NotWii when empty.
Kind KindForPort(uint32_t port) {
    if (port >= PAD_MAX_CONTROLLERS) return Kind::NotWii;
    SDL_Gamepad* gamepad = SDL_GetGamepadFromPlayerIndex(static_cast<int>(port));
    if (gamepad == nullptr) return Kind::NotWii;
    return KindForName(SDL_GetGamepadName(gamepad));
}

// Human-readable name of a Kind for the settings overlay.
const char* KindLabel(Kind kind) {
    switch (kind) {
    case Kind::Remote: return "Wii Remote";
    case Kind::RemoteWithNunchuk: return "Wii Remote + Nunchuk";
    case Kind::RemoteWithClassic: return "Wii Remote + Classic Controller";
    case Kind::WiiUPro: return "Wii U Pro Controller";
    default: return "Not a Wii controller";
    }
}

// True when the port carries a bare Wii Remote or a remote + Nunchuk.
bool IsRemoteChannel(uint32_t chan) {
    const Kind kind = KindForPort(chan);
    return kind == Kind::Remote || kind == Kind::RemoteWithNunchuk;
}

// Marks KPAD-served ports as "no controller" in the GameCube pad statuses.
void HideRemotesFromPad(PADStatus* statuses, uint32_t count) {
    for (uint32_t port = 0; port < count && port < PAD_MAX_CONTROLLERS; ++port) {
        if (IsRemoteChannel(port)) {
            statuses[port] = {};
            statuses[port].err = PAD_ERR_NO_CONTROLLER;
        }
    }
}

// Samples buttons, accelerometers and the Nunchuk stick of the remote on a port.
bool ReadKpadSample(uint32_t chan, KpadSample& sample) {
    if (chan >= PAD_MAX_CONTROLLERS) {
        return false;
    }
    SDL_Gamepad* gamepad = SDL_GetGamepadFromPlayerIndex(static_cast<int>(chan));
    if (gamepad == nullptr) {
        return false;
    }
    const Kind kind = KindForName(SDL_GetGamepadName(gamepad));
    if (kind != Kind::Remote && kind != Kind::RemoteWithNunchuk) {
        return false;
    }
    SDL_Joystick* joystick = SDL_GetGamepadJoystick(gamepad);
    if (joystick == nullptr) {
        return false;
    }
    EnsureSensors(gamepad, chan);

    sample = {};
    const auto raw = [&](int index, uint32_t bit) {
        if (SDL_GetJoystickButton(joystick, index)) sample.hold |= bit;
    };
    raw(kRawA, kWpadA);
    raw(kRawB, kWpadB);
    raw(kRawOne, kWpadOne);
    raw(kRawTwo, kWpadTwo);
    raw(kRawPlus, kWpadPlus);
    raw(kRawMinus, kWpadMinus);
    raw(kRawHome, kWpadHome);
    raw(kRawDpadUp, kWpadUp);
    raw(kRawDpadDown, kWpadDown);
    raw(kRawDpadLeft, kWpadLeft);
    raw(kRawDpadRight, kWpadRight);

    if (!ReadAccelAsKpad(gamepad, SDL_SENSOR_ACCEL, sample.acc)) {
        sample.acc[1] = -1.0f; // at rest
    }

    if (kind == Kind::RemoteWithNunchuk) {
        sample.hasNunchuk = true;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) sample.hold |= kWpadC;
        if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0) sample.hold |= kWpadZ;
        sample.stick[0] = static_cast<float>(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX)) / 32767.0f;
        // SDL's y grows downwards; KPAD's stick y is up-positive.
        sample.stick[1] = -static_cast<float>(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY)) / 32767.0f;
        for (float& v : sample.stick) v = std::clamp(v, -1.0f, 1.0f);
        ReadAccelAsKpad(gamepad, SDL_SENSOR_ACCEL_L, sample.nunchukAcc);
    }
    return true;
}

} // namespace WiiRemoteInput
