# Source-level fixes applied to the vendored SDL3 tree before it is built.
#
# Each patch is an exact string replacement, checked and idempotent: a tree that
# already carries the fix is left alone, a tree where the anchor text is missing
# (a different SDL version) stops the configure with a clear message instead of
# silently building without the fix.
#
# Used two ways:
#   - included by AuroraSDL3Provider.cmake, which calls aurora_sdl3_apply_patches()
#     on a pre-provided source tree (FETCHCONTENT_SOURCE_DIR_SDL);
#   - run as `cmake -DSDL_SOURCE_DIR=<dir> -P AuroraSDL3Patches.cmake` from the
#     FetchContent PATCH_COMMAND for a freshly extracted tarball.

function(_aurora_sdl3_replace file description old new)
  file(READ "${file}" _content)
  string(FIND "${_content}" "${new}" _already)
  if (NOT _already EQUAL -1)
    return()
  endif ()
  string(FIND "${_content}" "${old}" _anchor)
  if (_anchor EQUAL -1)
    message(FATAL_ERROR "aurora: SDL3 patch '${description}' does not apply to ${file}; "
      "the vendored SDL version changed, review AuroraSDL3Patches.cmake")
  endif ()
  string(REPLACE "${old}" "${new}" _content "${_content}")
  file(WRITE "${file}" "${_content}")
  message(STATUS "aurora: SDL3 patch applied: ${description}")
endfunction()

function(aurora_sdl3_apply_patches sdl_source_dir)
  set(_wii "${sdl_source_dir}/src/joystick/hidapi/SDL_hidapi_wii.c")
  if (NOT EXISTS "${_wii}")
    message(FATAL_ERROR "aurora: SDL3 source tree at ${sdl_source_dir} has no SDL_hidapi_wii.c")
  endif ()

  # Wii Remote: rebuild the joystick in place after an extension change.
  #
  # When a Nunchuk or Classic Controller is plugged in or pulled out, the driver
  # flags the joystick as disconnected so it can come back with the new name and
  # capabilities. But the HID device stays open, and HIDAPI only removes a
  # joystick-less device once its handle is closed, so nothing ever re-creates
  # the joystick: the remote is gone for good until the application toggles the
  # driver hint (which closes and re-opens the Bluetooth HID handle, something
  # some Windows Bluetooth stacks answer by dropping the link). Instead, when the
  # device has no joystick, probe the extension again (two bounded attempts, at
  # most once a second) and connect a new joystick on the still-open handle.
  _aurora_sdl3_replace("${_wii}" "Wii Remote in-place reconnect (context field)"
[==[    Uint64 m_ulNextMotionPlusCheck;
    bool m_bDisconnected;
]==]
[==[    Uint64 m_ulNextMotionPlusCheck;
    bool m_bDisconnected;
    Uint64 m_ulNextReconnect; /* WiiCompiled: see HIDAPI_DriverWii_UpdateDevice */
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote in-place reconnect (bounded extension probe)"
[==[static EWiiExtensionControllerType ReadExtensionControllerType(SDL_HIDAPI_Device *device)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;
    EWiiExtensionControllerType eExtensionControllerType = k_eWiiExtensionControllerType_Unknown;
    const int MAX_ATTEMPTS = 20;
    int attempts = 0;
]==]
[==[static EWiiExtensionControllerType ReadExtensionControllerTypeAttempts(SDL_HIDAPI_Device *device, int MAX_ATTEMPTS)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;
    EWiiExtensionControllerType eExtensionControllerType = k_eWiiExtensionControllerType_Unknown;
    int attempts = 0;
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote in-place reconnect (probe wrapper)"
[==[static void UpdateDeviceIdentity(SDL_HIDAPI_Device *device)
{
]==]
[==[static EWiiExtensionControllerType ReadExtensionControllerType(SDL_HIDAPI_Device *device)
{
    return ReadExtensionControllerTypeAttempts(device, 20);
}

static void UpdateDeviceIdentity(SDL_HIDAPI_Device *device)
{
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote in-place reconnect (UpdateDevice)"
[==[    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    } else {
        return false;
    }

    now = SDL_GetTicks();
]==]
[==[    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    } else {
        /* WiiCompiled: the joystick was dropped (extension plugged or unplugged,
         * or a failed read) but the HID device is still open, and the device
         * list only removes a joystick-less device once its handle is closed.
         * Rebuild the joystick in place, so an extension change behaves like it
         * does on the console: the remote never goes away, it just changes
         * type. If the remote does not answer, keep the device and retry. */
        now = SDL_GetTicks();
        if (ctx->m_ulNextReconnect != 0 && now < ctx->m_ulNextReconnect) {
            return false;
        }
        ctx->m_ulNextReconnect = now + 1000;
        {
            EWiiExtensionControllerType type = ReadExtensionControllerTypeAttempts(device, 2);
            if (type == k_eWiiExtensionControllerType_Unknown) {
                return false;
            }
            ctx->m_bDisconnected = false;
            ctx->m_ulLastInput = now;
            ctx->m_ulNextReconnect = 0;
            ctx->m_eExtensionControllerType = type;
            UpdateDeviceIdentity(device);
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: Reconnected joystick as %s", device->name);
            return HIDAPI_JoystickConnected(device, NULL);
        }
    }

    now = SDL_GetTicks();
]==])

  # Wii Remote: factory accelerometer calibration.
  #
  # The driver assumes the nominal 100 counts/g scale with a 0x200 zero point,
  # but real remotes are off by enough that games see a tilted resting pose.
  # Read the factory zero-g / +1g calibration block from EEPROM 0x16 at open,
  # validate it, and scale samples with it; keep the nominal values as the
  # fallback when the read times out (some Bluetooth stacks drop EEPROM reads).
  _aurora_sdl3_replace("${_wii}" "Wii Remote factory accel calibration (context fields)"
[==[    bool m_bReportSensors;
    Uint8 m_rgucReadBuffer[k_unWiiPacketDataLength];
]==]
[==[    bool m_bReportSensors;
    bool m_bAccelCalibrationValid;
    Uint16 m_unAccelZero[3];
    Uint16 m_unAccelOne[3];
    Uint8 m_rgucReadBuffer[k_unWiiPacketDataLength];
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote factory accel calibration (EEPROM read)"
[==[static bool SendExtensionIdentify(SDL_DriverWii_Context *ctx, bool sync)
{
    return ReadRegister(ctx, 0xA400FE, 2, sync);
]==]
[==[static bool ReadEEPROM(SDL_DriverWii_Context *ctx, Uint16 address, int size)
{
    Uint8 readRequest[7];

    readRequest[0] = k_eWiiOutputReportIDs_ReadMemory;
    readRequest[1] = (Uint8)ctx->m_bRumbleActive;
    readRequest[2] = 0;
    readRequest[3] = (address >> 8) & 0xff;
    readRequest[4] = address & 0xff;
    readRequest[5] = (size >> 8) & 0xff;
    readRequest[6] = size & 0xff;

    SDL_assert(size > 0 && size <= 16);
    if (!WriteOutput(ctx, readRequest, sizeof(readRequest), true)) {
        return false;
    }
    return ReadInputSync(ctx, k_eWiiInputReportIDs_ReadMemory, NULL);
}

static bool SendExtensionIdentify(SDL_DriverWii_Context *ctx, bool sync)
{
    return ReadRegister(ctx, 0xA400FE, 2, sync);
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote factory accel calibration (parse and validate)"
[==[static EWiiExtensionControllerType GetExtensionType(Uint16 extension_id)
{
]==]
[==[static bool InitializeAccelerometerCalibration(SDL_DriverWii_Context *ctx)
{
    const Uint8 *calibration;
    int i;

    ctx->m_bAccelCalibrationValid = false;
    if (!ReadEEPROM(ctx, 0x0016, 10)) {
        return false;
    }
    if (ctx->m_rgucReadBuffer[0] != k_eWiiInputReportIDs_ReadMemory ||
        ctx->m_rgucReadBuffer[4] != 0x00 || ctx->m_rgucReadBuffer[5] != 0x16 ||
        (ctx->m_rgucReadBuffer[3] & 0x0f) != 0 ||
        ((ctx->m_rgucReadBuffer[3] >> 4) + 1) < 10) {
        SDL_SetError("Invalid Wii accelerometer calibration response");
        return false;
    }

    // EEPROM 0x16 contains two packed 10-bit points: zero-g and +1g.
    calibration = ctx->m_rgucReadBuffer + 6;
    ctx->m_unAccelZero[0] = (Uint16)((calibration[0] << 2) | ((calibration[3] >> 4) & 0x03));
    ctx->m_unAccelZero[1] = (Uint16)((calibration[1] << 2) | ((calibration[3] >> 2) & 0x03));
    ctx->m_unAccelZero[2] = (Uint16)((calibration[2] << 2) | (calibration[3] & 0x03));
    ctx->m_unAccelOne[0] = (Uint16)((calibration[4] << 2) | ((calibration[7] >> 4) & 0x03));
    ctx->m_unAccelOne[1] = (Uint16)((calibration[5] << 2) | ((calibration[7] >> 2) & 0x03));
    ctx->m_unAccelOne[2] = (Uint16)((calibration[6] << 2) | (calibration[7] & 0x03));

    for (i = 0; i < 3; ++i) {
        const int range = (int)ctx->m_unAccelOne[i] - (int)ctx->m_unAccelZero[i];
        if (ctx->m_unAccelZero[i] < 256 || ctx->m_unAccelZero[i] > 768 ||
            range < 40 || range > 240) {
            SDL_SetError("Invalid Wii accelerometer calibration axis %d: zero=%u one=%u",
                         i, ctx->m_unAccelZero[i], ctx->m_unAccelOne[i]);
            return false;
        }
    }

    ctx->m_bAccelCalibrationValid = true;
    return true;
}

static EWiiExtensionControllerType GetExtensionType(Uint16 extension_id)
{
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote factory accel calibration (calibrated samples)"
[==[    const float ACCEL_RES_PER_G = 100.0f;
    Sint16 x, y, z;
    float values[3];

    if (!ctx->m_bReportSensors) {
        return;
    }

    x = ((data->rgucAccelerometer[0] << 2) | ((data->rgucBaseButtons[0] >> 5) & 0x03)) - 0x200;
    y = ((data->rgucAccelerometer[1] << 2) | ((data->rgucBaseButtons[1] >> 4) & 0x02)) - 0x200;
    z = ((data->rgucAccelerometer[2] << 2) | ((data->rgucBaseButtons[1] >> 5) & 0x02)) - 0x200;

    values[0] = -((float)x / ACCEL_RES_PER_G) * SDL_STANDARD_GRAVITY;
    values[1] = ((float)z / ACCEL_RES_PER_G) * SDL_STANDARD_GRAVITY;
    values[2] = ((float)y / ACCEL_RES_PER_G) * SDL_STANDARD_GRAVITY;
]==]
[==[    Uint16 raw_x, raw_y, raw_z;
    float x, y, z;
    float values[3];

    if (!ctx->m_bReportSensors) {
        return;
    }

    raw_x = (Uint16)((data->rgucAccelerometer[0] << 2) | ((data->rgucBaseButtons[0] >> 5) & 0x03));
    raw_y = (Uint16)((data->rgucAccelerometer[1] << 2) | ((data->rgucBaseButtons[1] >> 4) & 0x02));
    raw_z = (Uint16)((data->rgucAccelerometer[2] << 2) | ((data->rgucBaseButtons[1] >> 5) & 0x02));
    if (ctx->m_bAccelCalibrationValid) {
        x = ((float)raw_x - (float)ctx->m_unAccelZero[0]) /
            ((float)ctx->m_unAccelOne[0] - (float)ctx->m_unAccelZero[0]);
        y = ((float)raw_y - (float)ctx->m_unAccelZero[1]) /
            ((float)ctx->m_unAccelOne[1] - (float)ctx->m_unAccelZero[1]);
        z = ((float)raw_z - (float)ctx->m_unAccelZero[2]) /
            ((float)ctx->m_unAccelOne[2] - (float)ctx->m_unAccelZero[2]);
    } else {
        x = ((float)raw_x - 512.0f) / 100.0f;
        y = ((float)raw_y - 512.0f) / 100.0f;
        z = ((float)raw_z - 512.0f) / 100.0f;
    }

    values[0] = -x * SDL_STANDARD_GRAVITY;
    values[1] = z * SDL_STANDARD_GRAVITY;
    values[2] = y * SDL_STANDARD_GRAVITY;
]==])

  # Wii Remote: IR camera pointer.
  #
  # Upstream leaves the IR camera off ("IR camera data is not supported").
  # Enable it with the console's own paced init sequence, switch the sensor
  # report modes to the IR-carrying ones (0x33 alone, 0x37 with an extension),
  # average the tracked dots and publish the pointer both as SDL touchpad
  # finger 0 and as the AURORA.wii.ir_valid/ir_x/ir_y joystick properties the
  # runtime reads for KPAD. 0x37 leaves 6 extension bytes, enough for the
  # standard Nunchuk/Classic reports the driver already parses.
  _aurora_sdl3_replace("${_wii}" "Wii Remote IR camera (report modes with IR data)"
[==[    switch (ctx->m_eExtensionControllerType) {
    case k_eWiiExtensionControllerType_WiiUPro:
        return k_eWiiInputReportIDs_ButtonDataD;
    case k_eWiiExtensionControllerType_Nunchuk:
    case k_eWiiExtensionControllerType_Gamepad:
        if (ctx->m_bReportSensors) {
            return k_eWiiInputReportIDs_ButtonData5;
        } else {
            return k_eWiiInputReportIDs_ButtonData2;
        }
    default:
        if (ctx->m_bReportSensors) {
            return k_eWiiInputReportIDs_ButtonData5;
        } else {
            return k_eWiiInputReportIDs_ButtonData0;
        }
]==]
[==[    switch (ctx->m_eExtensionControllerType) {
    case k_eWiiExtensionControllerType_WiiUPro:
        return k_eWiiInputReportIDs_ButtonDataD;
    case k_eWiiExtensionControllerType_None:
        if (ctx->m_bReportSensors) {
            return k_eWiiInputReportIDs_ButtonData3;
        } else {
            return k_eWiiInputReportIDs_ButtonData0;
        }
    case k_eWiiExtensionControllerType_Nunchuk:
    case k_eWiiExtensionControllerType_Gamepad:
        if (ctx->m_bReportSensors) {
            return k_eWiiInputReportIDs_ButtonData7;
        } else {
            return k_eWiiInputReportIDs_ButtonData2;
        }
    default:
        if (ctx->m_bReportSensors) {
            return k_eWiiInputReportIDs_ButtonData7;
        } else {
            return k_eWiiInputReportIDs_ButtonData0;
        }
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote IR camera (paced init sequence)"
[==[static void ResetButtonPacketType(SDL_DriverWii_Context *ctx)
{
    RequestButtonPacketType(ctx, GetButtonPacketType(ctx));
]==]
[==[static bool InitializeIRCamera(SDL_DriverWii_Context *ctx)
{
    static const Uint8 sensitivity_block_1[] = { 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x90, 0x00, 0x41 };
    static const Uint8 sensitivity_block_2[] = { 0x40, 0x00 };
    const Uint8 enable[] = { k_eWiiOutputReportIDs_IRCameraEnable, (Uint8)(0x06 | (Uint8)ctx->m_bRumbleActive) };
    const Uint8 enable2[] = { k_eWiiOutputReportIDs_IRCameraEnable2, (Uint8)(0x06 | (Uint8)ctx->m_bRumbleActive) };
    const Uint8 camera_prepare = 0x01;
    const Uint8 camera_enable = 0x08;
    const Uint8 camera_mode =
        ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_None ? 0x03 : 0x01;

    // Real hardware can randomly remain at half sensitivity or fail to start
    // sampling when these commands are sent back-to-back. This is the same
    // paced sequence used by the Wii: prepare at 0x30, configure, select the
    // mode, then commit object tracking with a final 0x08 write.
    if (!WriteOutput(ctx, enable, sizeof(enable), true)) {
        return false;
    }
    SDL_Delay(50);
    if (!WriteOutput(ctx, enable2, sizeof(enable2), true)) {
        return false;
    }
    SDL_Delay(50);
    if (!WriteRegister(ctx, 0xB00030, &camera_prepare, sizeof(camera_prepare), true)) {
        return false;
    }
    SDL_Delay(50);
    if (!WriteRegister(ctx, 0xB00000, sensitivity_block_1, sizeof(sensitivity_block_1), true)) {
        return false;
    }
    SDL_Delay(50);
    if (!WriteRegister(ctx, 0xB0001A, sensitivity_block_2, sizeof(sensitivity_block_2), true)) {
        return false;
    }
    SDL_Delay(50);
    if (!WriteRegister(ctx, 0xB00033, &camera_mode, sizeof(camera_mode), true)) {
        return false;
    }
    SDL_Delay(50);
    return WriteRegister(ctx, 0xB00030, &camera_enable, sizeof(camera_enable), true);
}

static void ResetButtonPacketType(SDL_DriverWii_Context *ctx)
{
    RequestButtonPacketType(ctx, GetButtonPacketType(ctx));
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote IR camera (init at open, with accel calibration)"
[==[    ctx->joystick = joystick;

    InitializeExtension(ctx);

    GetMotionPlusState(ctx, &ctx->m_bMotionPlusPresent, &ctx->m_ucMotionPlusMode);
]==]
[==[    ctx->joystick = joystick;

    InitializeExtension(ctx);
    if (!InitializeAccelerometerCalibration(ctx)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "HIDAPI Wii: Using fallback accelerometer calibration: %s", SDL_GetError());
    }
    if (!InitializeIRCamera(ctx)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: Failed to initialize IR camera: %s", SDL_GetError());
    }

    GetMotionPlusState(ctx, &ctx->m_bMotionPlusPresent, &ctx->m_ucMotionPlusMode);
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote IR camera (accel sensor for Classic Controller)"
[==[    if (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_None ||
        ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_Nunchuk) {
        SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_ACCEL, 100.0f);
]==]
[==[    if (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_None ||
        ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_Nunchuk ||
        ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_Gamepad) {
        SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_ACCEL, 100.0f);
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote IR camera (pointer touchpad)"
[==[    joystick->naxes = SDL_GAMEPAD_AXIS_COUNT;

    ctx->m_ulLastInput = SDL_GetTicks();
]==]
[==[    joystick->naxes = SDL_GAMEPAD_AXIS_COUNT;
    SDL_PrivateJoystickAddTouchpad(joystick, 1);

    ctx->m_ulLastInput = SDL_GetTicks();
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote IR camera (reinit from status report)"
[==[    GetBaseButtons(&data, ctx->m_rgucReadBuffer + 1);
    HandleButtonData(ctx, joystick, &data);

    if (ctx->m_eExtensionControllerType != k_eWiiExtensionControllerType_WiiUPro) {
]==]
[==[    GetBaseButtons(&data, ctx->m_rgucReadBuffer + 1);
    HandleButtonData(ctx, joystick, &data);

    const SDL_PropertiesID properties = SDL_GetJoystickProperties(joystick);
    SDL_SetNumberProperty(properties, "AURORA.wii.status_flags", ctx->m_rgucReadBuffer[3]);

    if ((ctx->m_rgucReadBuffer[3] & 0x08) == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: IR camera disabled; reinitializing");
        if (InitializeIRCamera(ctx)) {
            SDL_SetNumberProperty(properties, "AURORA.wii.status_flags", ctx->m_rgucReadBuffer[3] | 0x08);
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: Failed to reinitialize IR camera: %s", SDL_GetError());
        }
    }
    if (ctx->m_eExtensionControllerType != k_eWiiExtensionControllerType_WiiUPro) {
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote IR camera (parse report 0x33)"
[==[    // IR camera data is not supported
    SDL_zero(data);
    switch (ctx->m_rgucReadBuffer[0]) {
    case k_eWiiInputReportIDs_ButtonData0: // 30 BB BB
        GetBaseButtons(&data, ctx->m_rgucReadBuffer + 1);
        break;
    case k_eWiiInputReportIDs_ButtonData1: // 31 BB BB AA AA AA
    case k_eWiiInputReportIDs_ButtonData3: // 33 BB BB AA AA AA II II II II II II II II II II II II
        GetBaseButtons(&data, ctx->m_rgucReadBuffer + 1);
        GetAccelerometer(&data, ctx->m_rgucReadBuffer + 3);
        break;
]==]
[==[    SDL_zero(data);
    switch (ctx->m_rgucReadBuffer[0]) {
    case k_eWiiInputReportIDs_ButtonData0: // 30 BB BB
        GetBaseButtons(&data, ctx->m_rgucReadBuffer + 1);
        break;
    case k_eWiiInputReportIDs_ButtonData1: // 31 BB BB AA AA AA
        GetBaseButtons(&data, ctx->m_rgucReadBuffer + 1);
        GetAccelerometer(&data, ctx->m_rgucReadBuffer + 3);
        break;
    case k_eWiiInputReportIDs_ButtonData3: // 33 BB BB AA AA AA II II II II II II II II II II II II
        GetBaseButtons(&data, ctx->m_rgucReadBuffer + 1);
        GetAccelerometer(&data, ctx->m_rgucReadBuffer + 3);
        {
            const Uint8 *ir = ctx->m_rgucReadBuffer + 6;
            Uint32 sum_x = 0;
            Uint32 sum_y = 0;
            int valid_points = 0;
            int point;
            for (point = 0; point < 4; ++point) {
                const Uint8 *object = ir + point * 3;
                const Uint16 object_x = (Uint16)(object[0] | ((object[2] & 0x30) << 4));
                const Uint16 object_y = (Uint16)(object[1] | ((object[2] & 0xC0) << 2));
                if (object_x != 0x03ff || object_y != 0x03ff) {
                    sum_x += object_x;
                    sum_y += object_y;
                    ++valid_points;
                }
            }
            if (valid_points > 0) {
                const float x = 1.0f - ((float)sum_x / (float)valid_points) / 1023.0f;
                const float y = ((float)sum_y / (float)valid_points) / 767.0f;
                const SDL_PropertiesID properties = SDL_GetJoystickProperties(joystick);
                SDL_SetBooleanProperty(properties, "AURORA.wii.ir_valid", true);
                SDL_SetFloatProperty(properties, "AURORA.wii.ir_x", x);
                SDL_SetFloatProperty(properties, "AURORA.wii.ir_y", y);
                SDL_SendJoystickTouchpad(ctx->timestamp, joystick, 0, 0, true, x, y, 1.0f);
            } else {
                SDL_SetBooleanProperty(SDL_GetJoystickProperties(joystick), "AURORA.wii.ir_valid", false);
                SDL_SendJoystickTouchpad(ctx->timestamp, joystick, 0, 0, false, 0.0f, 0.0f, 0.0f);
            }
        }
        break;
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote IR camera (parse report 0x37)"
[==[    case k_eWiiInputReportIDs_ButtonData7: // 37 BB BB AA AA AA II II II II II II II II II II EE EE EE EE EE EE
        GetBaseButtons(&data, ctx->m_rgucReadBuffer + 1);
        GetExtensionData(&data, ctx->m_rgucReadBuffer + 16, 6);
        break;
]==]
[==[    case k_eWiiInputReportIDs_ButtonData7: // 37 BB BB AA AA AA II II II II II II II II II II EE EE EE EE EE EE
        GetBaseButtons(&data, ctx->m_rgucReadBuffer + 1);
        GetAccelerometer(&data, ctx->m_rgucReadBuffer + 3);
        {
            const Uint8 *ir = ctx->m_rgucReadBuffer + 6;
            Uint32 sum_x = 0;
            Uint32 sum_y = 0;
            int valid_points = 0;
            int pair;
            for (pair = 0; pair < 2; ++pair) {
                const Uint8 *p = ir + pair * 5;
                const Uint16 x0 = (Uint16)(p[0] | ((p[2] & 0x30) << 4));
                const Uint16 y0 = (Uint16)(p[1] | ((p[2] & 0xC0) << 2));
                const Uint16 x1 = (Uint16)(p[3] | ((p[2] & 0x03) << 8));
                const Uint16 y1 = (Uint16)(p[4] | ((p[2] & 0x0C) << 6));
                if (x0 != 0x03ff || y0 != 0x03ff) {
                    sum_x += x0;
                    sum_y += y0;
                    ++valid_points;
                }
                if (x1 != 0x03ff || y1 != 0x03ff) {
                    sum_x += x1;
                    sum_y += y1;
                    ++valid_points;
                }
            }
            if (valid_points > 0) {
                const float x = 1.0f - ((float)sum_x / (float)valid_points) / 1023.0f;
                const float y = ((float)sum_y / (float)valid_points) / 767.0f;
                const SDL_PropertiesID properties = SDL_GetJoystickProperties(joystick);
                SDL_SetBooleanProperty(properties, "AURORA.wii.ir_valid", true);
                SDL_SetFloatProperty(properties, "AURORA.wii.ir_x", x);
                SDL_SetFloatProperty(properties, "AURORA.wii.ir_y", y);
                SDL_SendJoystickTouchpad(ctx->timestamp, joystick, 0, 0, true, x, y, 1.0f);
            } else {
                SDL_SetBooleanProperty(SDL_GetJoystickProperties(joystick), "AURORA.wii.ir_valid", false);
                SDL_SendJoystickTouchpad(ctx->timestamp, joystick, 0, 0, false, 0.0f, 0.0f, 0.0f);
            }
        }
        GetExtensionData(&data, ctx->m_rgucReadBuffer + 16, 6);
        break;
]==])

  # Wii Remote: raw battery byte for WPADInfo.
  #
  # SDL's power API rounds the status report's battery byte down to four
  # coarse percentages; keep the raw byte in a property so the runtime can
  # report the console's own battery granularity.
  _aurora_sdl3_replace("${_wii}" "Wii Remote raw battery property"
[==[static void UpdatePowerLevelWii(SDL_Joystick *joystick, Uint8 batteryLevelByte)
{
    int percent;
]==]
[==[static void UpdatePowerLevelWii(SDL_Joystick *joystick, Uint8 batteryLevelByte)
{
    // Preserve the physical status-report byte for WPADInfo. SDL's generic
    // power API intentionally reduces this to four coarse percentages.
    SDL_SetNumberProperty(SDL_GetJoystickProperties(joystick), "AURORA.wii.raw_battery", batteryLevelByte);
    int percent;
]==])

  # Wii Remote: no phantom joystick for a Mayflash DolphinBar empty slot.
  #
  # A DolphinBar in mode 4 exposes four HID interfaces with the Wii Remote
  # VID/PID even when no remote is synced to a slot; writes to an empty slot
  # fail instantly, so the identification above comes back Unknown and the
  # unconditional connect would surface up to four "Wii Remote with Unknown
  # Extension" joysticks at startup. Do not surface a joystick for such a
  # device: keep the handle open and let the reconnect probe in
  # HIDAPI_DriverWii_UpdateDevice connect it as soon as a remote answers —
  # which is also how a remote synced to the bar mid-session is picked up,
  # since the always-present interfaces never raise a hotplug event.
  _aurora_sdl3_replace("${_wii}" "Wii Remote DolphinBar empty slot (no phantom joystick)"
[==[    if (device->vendor_id == USB_VENDOR_NINTENDO) {
        ctx->m_eExtensionControllerType = ReadExtensionControllerType(device);

        UpdateDeviceIdentity(device);
    }
    return HIDAPI_JoystickConnected(device, NULL);
]==]
[==[    if (device->vendor_id == USB_VENDOR_NINTENDO) {
        ctx->m_eExtensionControllerType = ReadExtensionControllerType(device);

        UpdateDeviceIdentity(device);
    }
    if (ctx->m_eExtensionControllerType == k_eWiiExtensionControllerType_Unknown) {
        ctx->m_ulNextReconnect = SDL_GetTicks() + 1000;
        return true;
    }
    return HIDAPI_JoystickConnected(device, NULL);
]==])
endfunction()

# Script mode (FetchContent PATCH_COMMAND).
if (CMAKE_SCRIPT_MODE_FILE AND DEFINED SDL_SOURCE_DIR)
  aurora_sdl3_apply_patches("${SDL_SOURCE_DIR}")
endif ()
