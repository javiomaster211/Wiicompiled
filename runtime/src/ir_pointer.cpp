#include "ir_pointer.h"

#include "runtime_config.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_properties.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>

namespace IrPointer {
namespace {

// KPAD serves four channels; matches PAD_MAX_CONTROLLERS without the header.
constexpr uint32_t kChannels = 4;

// Camera samples arrive at the remote's ~100 Hz report rate with visible
// jitter; blend this much of the new sample in per KPAD read.
constexpr float kSmoothing = 0.5f;

// Written on the presenting thread, read on the guest thread inside KPADRead.
// Plain atomics: the axes may tear by one frame against each other, which is
// well under a pixel of pointer motion.
std::atomic<bool> g_mouseValid{false};
std::atomic<float> g_mouseX{0.0f};
std::atomic<float> g_mouseY{0.0f};

// Per-channel camera state. Only touched from the guest thread inside
// SampleForChannel, except the two flags the overlay polls, which are atomic.
struct CameraState {
    bool smoothed = false; // whether smoothX/Y hold a running average
    float smoothX = 0.0f;
    float smoothY = 0.0f;
    std::atomic<bool> usingCamera{false};
    std::atomic<float> rawX{0.0f};
    std::atomic<float> rawY{0.0f};
};
std::array<CameraState, kChannels> g_camera;

// Reads the AURORA.wii.ir_* joystick properties the patched SDL Wii driver
// publishes for this channel's remote. False when the channel has no remote or
// its camera sees no sensor-bar dot right now.
bool ReadCamera(uint32_t chan, float& camX, float& camY) {
    SDL_Gamepad* gamepad = SDL_GetGamepadFromPlayerIndex(static_cast<int>(chan));
    if (gamepad == nullptr) {
        return false;
    }
    SDL_Joystick* joystick = SDL_GetGamepadJoystick(gamepad);
    if (joystick == nullptr) {
        return false;
    }
    const SDL_PropertiesID properties = SDL_GetJoystickProperties(joystick);
    if (!SDL_GetBooleanProperty(properties, "AURORA.wii.ir_valid", false)) {
        return false;
    }
    camX = SDL_GetFloatProperty(properties, "AURORA.wii.ir_x", 0.5f);
    camY = SDL_GetFloatProperty(properties, "AURORA.wii.ir_y", 0.5f);
    return true;
}

} // namespace

void PublishFromImGui() {
    const ImGuiIO& io = ImGui::GetIO();
    // ImGui parks MousePos at -FLT_MAX while the window is unfocused or the OS
    // cursor has left it, which is exactly "pointing away from the screen".
    const bool inside = io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f && io.MousePos.x >= 0.0f &&
                        io.MousePos.y >= 0.0f && io.MousePos.x < io.DisplaySize.x && io.MousePos.y < io.DisplaySize.y;
    if (!inside) {
        g_mouseValid.store(false, std::memory_order_relaxed);
        return;
    }
    g_mouseX.store(io.MousePos.x / io.DisplaySize.x * 2.0f - 1.0f, std::memory_order_relaxed);
    g_mouseY.store(io.MousePos.y / io.DisplaySize.y * 2.0f - 1.0f, std::memory_order_relaxed);
    g_mouseValid.store(true, std::memory_order_release);
}

bool SampleForChannel(uint32_t chan, float& x, float& y) {
    if (chan >= kChannels) {
        return false;
    }
    CameraState& state = g_camera[chan];
    float camX = 0.0f;
    float camY = 0.0f;
    if (ReadCamera(chan, camX, camY)) {
        state.rawX.store(camX, std::memory_order_relaxed);
        state.rawY.store(camY, std::memory_order_relaxed);
        state.usingCamera.store(true, std::memory_order_relaxed);
        // The driver reports 0..1 with x already running left-to-right on the
        // screen. Its y is the camera image's own axis: pointing up moves the
        // bar down in the image, so the screen direction is the inverse.
        const float scale = RuntimeConfigFile::WiiIrScale();
        const float mappedX = (camX - 0.5f) * 2.0f * scale;
        const float mappedY = (camY - 0.5f) * -2.0f * scale + RuntimeConfigFile::WiiIrOffsetY();
        if (state.smoothed) {
            state.smoothX += (mappedX - state.smoothX) * kSmoothing;
            state.smoothY += (mappedY - state.smoothY) * kSmoothing;
        } else {
            state.smoothX = mappedX;
            state.smoothY = mappedY;
            state.smoothed = true;
        }
        x = std::clamp(state.smoothX, -1.0f, 1.0f);
        y = std::clamp(state.smoothY, -1.0f, 1.0f);
        return true;
    }
    state.smoothed = false;
    state.usingCamera.store(false, std::memory_order_relaxed);
    // Without a camera lock, player 1 falls back to the window's mouse.
    if (chan != 0 || !g_mouseValid.load(std::memory_order_acquire)) {
        return false;
    }
    x = g_mouseX.load(std::memory_order_relaxed);
    y = g_mouseY.load(std::memory_order_relaxed);
    return true;
}

bool UsingCamera(uint32_t chan) {
    return chan < kChannels && g_camera[chan].usingCamera.load(std::memory_order_relaxed);
}

bool CameraDebug(uint32_t chan, float& rawX, float& rawY) {
    if (!UsingCamera(chan)) {
        return false;
    }
    rawX = g_camera[chan].rawX.load(std::memory_order_relaxed);
    rawY = g_camera[chan].rawY.load(std::memory_order_relaxed);
    return true;
}

} // namespace IrPointer
