# DolphinBar (Mode 4) test protocol

Manual verification for the Mayflash DolphinBar support, for a tester with the
bar and at least one Wii Remote (ideally with a Nunchuk and a Classic
Controller). The runtime cannot exercise any of this without the hardware, so
these scenarios gate the feature.

## Setup

1. Build and deploy as usual, or use the diagnostic build you were handed.
2. In `Config.toml`, under `[controller]`, add:

   ```toml
   wii_hid_trace = true
   ```

3. Keep `console.log` from every run; each scenario below names the lines to
   look for. Attach the logs (and the answers) to the PR.

## Scenarios

### 1. Mode 4, no remotes synced, launch

Bar plugged in, LED 4 lit, no remote on. Launch the game and wait 30 s.

- Expected: `Mayflash DolphinBar detected (Mode 4, 4 slot(s))` at startup,
  `Wii HID trace: 4 Wii Remote HID interface(s) present`, **no** controller
  connect ("port N -> Wii Remote...") lines, no `Wii Remote rescan #N` lines.
- Broken would be: four "Wii Remote with Unknown Extension" ghosts connecting
  and dropping, or a rescan line every 2 seconds.

### 2. Remote synced before launch

Sync a remote to the bar (SYNC on the bar, then SYNC/1+2 on the remote), then
launch.

- Expected: the remote shows up on port 1 within a few seconds; buttons,
  accelerometer readout (F10 > Controller settings > Wii Remotes) and, with a
  Nunchuk/Classic plugged in, the extension all work. Note how long from launch
  to the `port 1 -> ...` trace line.

### 3. Remote synced after launch

Launch with the bar empty, then sync the remote.

- Expected: `HIDAPI Wii: Reconnected joystick as ...` and the port trace line
  within ~2 s of the remote's LEDs settling. Report the actual delay.

### 4. Extension hot-swap mid-game

With the bar's remote in a race/menu, plug the Nunchuk in and out, then the
Classic Controller.

- Expected: same behavior as over Bluetooth — the control scheme switches, no
  "communications interrupted" prompt.

### 5. Remote switched off and back on

Turn the remote off (hold the power button), wait 10 s, press a button to wake
it.

- Expected: clean disconnect after ~3 s, clean reconnect after the wake-up.

### 6. Bar in Mode 1-3

Press MODE until LED 1/2/3, launch.

- Expected: the Wii Remotes menu shows the yellow "appears to be in Mode 1-3"
  warning and console.log says
  `Mayflash DolphinBar appears to be in Mode 1-3; press its MODE button ...`.

### 7. Regression: Bluetooth only

Unplug the bar, pair a remote over Windows Bluetooth as before.

- Expected: identical behavior to the previous build (scanning lines, pairing,
  extensions, calibration).

## IR pointer (needs the bar powered, any mode lights its IR LEDs)

With a remote connected (scenario 2) and the bar on top of or below the screen:

- Point at the screen: the hand cursor should follow the remote, and the menu's
  Pointer section should read `IR camera: tracking (raw ...)`.
- **Check the directions**: pointing right must move the cursor right, pointing
  up must move it up. If either axis is reversed, say which one and attach the
  raw x/y values shown while pointing at the four screen corners.
- Point away: within a moment the cursor should stop tracking and (player 1)
  fall back to the mouse.
- Tune "IR scale" / "IR vertical offset" until the cursor matches where you
  point; report values that felt right and where the bar was mounted.
- Repeat a quick point-at-screen check over plain Bluetooth with the bar only
  powered (not synced), to confirm the camera path also works there.
