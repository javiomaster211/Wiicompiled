#include "dolphinbar_detect.h"

#include <chrono>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cfgmgr32.h>
#include <devpropdef.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>
#endif

namespace DolphinBar {
namespace {

// Cached() and Refresh() are called from both the guest thread (Poll) and the
// presenting thread (overlay), so the cache is guarded; Refresh() is throttled,
// so the lock is uncontended in practice.
std::mutex g_mutex;
Status g_cached;
std::chrono::steady_clock::time_point g_lastRefresh;

#ifdef _WIN32

// HID device interface class, and the two device properties the detection
// reads. Defined locally instead of pulling in hidclass.h/devpkey.h, whose
// DEFINE_ macros are include-order sensitive across MSVC and MinGW headers.
const GUID kHidInterfaceGuid = {0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
const DEVPROPKEY kDevPkeyDeviceInstanceId = {
    {0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 256};
const DEVPROPKEY kDevPkeyBusReportedDeviceDesc = {
    {0x540b947e, 0x8b40, 0x45bc, {0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2}}, 4};

// Lowercases an ASCII wide string, for the case-insensitive path/desc matches.
std::wstring ToLower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    return text;
}

// The bus-reported description of the device behind an interface, walking up
// to the USB parent when the immediate device node does not carry one, or an
// empty string. This is how Dolphin tells a DolphinBar slot from a Bluetooth
// remote: both carry Nintendo's VID:PID, only the bar's USB descriptor says
// "Mayflash Wiimote PC Adapter".
std::wstring BusReportedDescription(const wchar_t* interfacePath) {
    DEVPROPTYPE type = 0;
    wchar_t instanceId[MAX_DEVICE_ID_LEN] = {};
    ULONG size = sizeof(instanceId);
    if (CM_Get_Device_Interface_PropertyW(interfacePath, &kDevPkeyDeviceInstanceId,
                                          &type, reinterpret_cast<BYTE*>(instanceId), &size,
                                          0) != CR_SUCCESS ||
        type != DEVPROP_TYPE_STRING) {
        return {};
    }
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, instanceId, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        return {};
    }
    for (int level = 0; level < 3; ++level) {
        wchar_t description[512] = {};
        size = sizeof(description);
        type = 0;
        if (CM_Get_DevNode_PropertyW(node, &kDevPkeyBusReportedDeviceDesc, &type,
                                     reinterpret_cast<BYTE*>(description), &size, 0) == CR_SUCCESS &&
            type == DEVPROP_TYPE_STRING && description[0] != L'\0') {
            return description;
        }
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) {
            break;
        }
        node = parent;
    }
    return {};
}

// One full enumeration pass over the present HID interfaces.
Status DetectNow() {
    Status status;
    // The list can grow between the size query and the fetch; retry like the
    // CM_Get_Device_Interface_List documentation prescribes.
    std::vector<wchar_t> list;
    for (int attempt = 0; attempt < 4; ++attempt) {
        ULONG characters = 0;
        if (CM_Get_Device_Interface_List_SizeW(&characters, const_cast<GUID*>(&kHidInterfaceGuid), nullptr,
                                               CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
            characters == 0) {
            return status;
        }
        list.assign(characters, L'\0');
        const CONFIGRET result =
            CM_Get_Device_Interface_ListW(const_cast<GUID*>(&kHidInterfaceGuid), nullptr, list.data(),
                                          characters, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (result == CR_SUCCESS) {
            break;
        }
        if (result != CR_BUFFER_SMALL) {
            return status;
        }
    }
    // The list is a sequence of null-terminated paths ended by an empty one.
    for (const wchar_t* path = list.data(); *path != L'\0'; path += wcslen(path) + 1) {
        const std::wstring lowered = ToLower(path);
        const bool wiiRemoteIds = lowered.find(L"vid_057e&pid_0306") != std::wstring::npos;
        const bool emulationIds = lowered.find(L"vid_0079&pid_1802") != std::wstring::npos ||
                                  lowered.find(L"vid_0079&pid_1803") != std::wstring::npos;
        if (!wiiRemoteIds && !emulationIds) {
            continue;
        }
        if (ToLower(BusReportedDescription(path)).find(L"mayflash") == std::wstring::npos) {
            continue;
        }
        status.present = true;
        if (wiiRemoteIds) {
            status.mode4 = true;
            ++status.mode4Slots;
        } else {
            status.mode123 = true;
        }
    }
    return status;
}

#else

// Only the Windows build can meet a DolphinBar behind the Windows HID stack;
// macOS/Linux report no bar and keep the Bluetooth-only behavior.
Status DetectNow() {
    return {};
}

#endif

} // namespace

Status Detect() {
    return DetectNow();
}

Status Cached() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_cached;
}

void Refresh() {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (g_lastRefresh.time_since_epoch().count() != 0 && now - g_lastRefresh < std::chrono::seconds(1)) {
        return;
    }
    g_lastRefresh = now;
    g_cached = DetectNow();
}

} // namespace DolphinBar
