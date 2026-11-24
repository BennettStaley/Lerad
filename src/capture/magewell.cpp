// SPDX-License-Identifier: MIT
#include "magewell.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

namespace app {

namespace {

// {3A45B930-FF5C-42AA-A5A2-43771B93EBEF}: the driver's filter extension.
constexpr GUID kMagewellSet = {0x3A45B930, 0xFF5C, 0x42AA, {0xA5, 0xA2, 0x43, 0x77, 0x1B, 0x93, 0xEB, 0xEF}};
constexpr DWORD kIoctlKsProperty = 0x002F0003;
constexpr ULONG kGet = 1, kSet = 2;
constexpr ULONG kCaptureSettings = 0x2F, kPreviewSettings = 0x35;
constexpr DWORD kSettingsSize = 49;
constexpr size_t kLowLatency = 28;

struct KsProperty {
    GUID set;
    ULONG id;
    ULONG flags;
};

bool property(const std::wstring &link, ULONG id, ULONG flags, BYTE *data, DWORD size) {
    const HANDLE device = CreateFileW(link.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (device == INVALID_HANDLE_VALUE) return false;
    KsProperty request{kMagewellSet, id, flags};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(device, kIoctlKsProperty, &request, sizeof(request), data, size, &returned, nullptr);
    CloseHandle(device);
    return ok && (flags == kSet || returned == size);
}

}  // namespace

bool is_magewell(const std::wstring &symbolic_link) {
    std::wstring lower = symbolic_link;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    return lower.find(L"ven_1cd7") != std::wstring::npos;
}

bool magewell_low_latency(const std::wstring &symbolic_link, bool &on) {
    BYTE settings[kSettingsSize] = {};
    if (!is_magewell(symbolic_link) || !property(symbolic_link, kCaptureSettings, kGet, settings, kSettingsSize))
        return false;
    on = settings[kLowLatency] != 0;
    return true;
}

bool set_magewell_low_latency(const std::wstring &symbolic_link, bool on) {
    if (!is_magewell(symbolic_link)) return false;
    bool all = true;
    for (ULONG id : {kCaptureSettings, kPreviewSettings}) {
        BYTE settings[kSettingsSize] = {};
        if (!property(symbolic_link, id, kGet, settings, kSettingsSize)) {
            all = false;
            continue;
        }
        settings[kLowLatency] = on ? 1 : 0;
        all = property(symbolic_link, id, kSet, settings, kSettingsSize) && all;
    }
    return all;
}

}  // namespace app
