#pragma once

#include <cstdint>

// The Wii Remote's IR pointer, from two sources:
//
//  - The remote's own IR camera, when it sees a sensor bar (a Mayflash
//    DolphinBar or any powered bar). SDL's patched Wii driver publishes the
//    averaged camera dots as joystick properties (see AuroraSDL3Patches.cmake);
//    SampleForChannel maps them into KPAD's screen coordinates with the
//    user-tunable scale/offset from Config.toml, smoothed against camera jitter.
//  - The window's mouse, published every presented frame, standing in on
//    channel 0 when no camera data is available (no bar, or pointing away).
//
// The KPAD HLE serves whichever source is live to the game as a fully tracked
// pointer (see kpad.cpp).
namespace IrPointer {

// Publishes the current mouse position from ImGui's input state. Must run on
// the thread that pumps SDL events and draws ImGui (settings_overlay::Draw).
void PublishFromImGui();

// Latest pointer position for the KPAD HLE, normalized to the game window:
// x -1 (left edge) .. 1 (right edge), y -1 (top) .. 1 (bottom), the axes KPAD
// reports. Prefers the channel's remote IR camera and falls back to the mouse
// on channel 0. False when the game should see "pointing away from the
// screen": no camera lock and (on channel 0) the mouse is outside the window
// or the window is unfocused. Safe to call from the guest thread.
bool SampleForChannel(uint32_t chan, float& x, float& y);

// Whether the last SampleForChannel on this channel used the remote's IR
// camera (as opposed to the mouse), for the overlay's status line.
bool UsingCamera(uint32_t chan);

// Raw camera position of the last camera-sourced sample (0..1 axes as the SDL
// driver reports them), for the overlay's diagnostics; false when the channel
// has no camera lock.
bool CameraDebug(uint32_t chan, float& rawX, float& rawY);

} // namespace IrPointer
