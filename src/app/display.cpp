// SPDX-License-Identifier: MIT
#include "display.h"

#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <format>
#include <vector>

#include "devices.h"
#include "vrr.h"

using Microsoft::WRL::ComPtr;

namespace glasir {
namespace {

// What DXGI says about every output: where it is, which GPU drives it,
// whether it presents HDR10 now and the brightness the screen reports.
std::vector<DisplayInfo> read_dxgi() {
    std::vector<DisplayInfo> out;
    // Windows' own order of the monitors (DisplayInfo::enum_index).
    std::vector<HMONITOR> order;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM lp) -> BOOL {
            reinterpret_cast<std::vector<HMONITOR> *>(lp)->push_back(monitor);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&order));
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return out;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 gpu{};
        adapter->GetDesc1(&gpu);
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop) continue;
            DisplayInfo d;
            d.found = true;
            d.device = desc.DeviceName;
            const size_t digits = d.device.find_last_not_of(L"0123456789") + 1;
            if (digits < d.device.size()) d.number = _wtoi(d.device.c_str() + digits);
            d.left = desc.DesktopCoordinates.left;
            d.top = desc.DesktopCoordinates.top;
            d.width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            d.height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
            d.primary = d.left == 0 && d.top == 0;  // the primary is the desktop's origin
            const auto at = std::find(order.begin(), order.end(), desc.Monitor);
            if (at != order.end()) d.enum_index = int(at - order.begin());
            d.gpu = gpu.Description;
            d.gpu_vendor = gpu.VendorId;
            ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1 desc1{};
            if (SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&desc1))) {
                d.hdr_on = desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                d.peak_nits = desc1.MaxLuminance;
                d.full_frame_nits = desc1.MaxFullFrameLuminance;
                d.min_nits = desc1.MinLuminance;
            }
            out.push_back(std::move(d));
        }
    }
    return out;
}

bool hdmi_output(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY t) { return t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI; }

// What the display configuration says about the same outputs: the monitor's
// name, the exact refresh rate, whether it has HDR at all, and Windows' SDR
// brightness while HDR is on. Then variable refresh, which needs the
// monitor's device from here.
void read_display_config(std::vector<DisplayInfo> &displays) {
    UINT32 paths_n = 0, modes_n = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths_n, &modes_n) != ERROR_SUCCESS) return;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(paths_n);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modes_n);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths_n, paths.data(), &modes_n, modes.data(), nullptr) !=
        ERROR_SUCCESS)
        return;
    paths.resize(paths_n);
    for (const auto &path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), path.sourceInfo.adapterId,
                         path.sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
        const auto it = std::find_if(displays.begin(), displays.end(),
                                     [&](const DisplayInfo &d) { return d.device == source.viewGdiDeviceName; });
        if (it == displays.end()) continue;
        DisplayInfo &d = *it;

        const auto &rate = path.targetInfo.refreshRate;
        if (rate.Denominator) d.refresh_hz = double(rate.Numerator) / rate.Denominator;

        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target), path.targetInfo.adapterId,
                         path.targetInfo.id};
        const bool named = DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS;
        if (named && target.flags.friendlyNameFromEdid) d.name = target.monitorFriendlyDeviceName;

        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO colour{};
        colour.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO, sizeof(colour),
                         path.targetInfo.adapterId, path.targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&colour.header) == ERROR_SUCCESS) {
            d.hdr_supported = colour.advancedColorSupported;
            // Windows' own switch, where DXGI could not say.
            if (colour.advancedColorEnabled && !colour.wideColorEnforced && d.peak_nits == 0) d.hdr_on = true;
        }

        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof(white), path.targetInfo.adapterId,
                        path.targetInfo.id};
        if (d.hdr_on && DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel)
            d.sdr_white_nits = white.SDRWhiteLevel / 1000.0 * 80.0;

        if (named) read_vrr(d, target.monitorDevicePath, hdmi_output(target.outputTechnology));
    }
}

}  // namespace

std::vector<DisplayInfo> list_displays() {
    std::vector<DisplayInfo> displays = read_dxgi();
    read_display_config(displays);
    for (DisplayInfo &d : displays) {
        if (d.refresh_hz <= 0) {
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsW(d.device.c_str(), ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
                d.refresh_hz = mode.dmDisplayFrequency;
        }
        if (d.hdr_on) d.hdr_supported = true;
    }
    std::stable_sort(displays.begin(), displays.end(), [](const DisplayInfo &a, const DisplayInfo &b) {
        if (a.primary != b.primary) return a.primary;
        return a.number < b.number;
    });
    return displays;
}

const DisplayInfo *choose_display(const std::vector<DisplayInfo> &displays, const std::wstring &wanted,
                                  bool *matched) {
    if (matched) *matched = wanted.empty() && !displays.empty();
    if (displays.empty()) return nullptr;
    const DisplayInfo *primary = &displays.front();
    for (const DisplayInfo &d : displays)
        if (d.primary) primary = &d;
    if (wanted.empty()) return primary;
    const bool number = std::all_of(wanted.begin(), wanted.end(), [](wchar_t c) { return iswdigit(c) != 0; });
    for (const DisplayInfo &d : displays) {
        const bool hit = number ? d.number == _wtoi(wanted.c_str())
                                : (!d.name.empty() && name_contains(d.name, wanted)) || names_match(d.device, wanted);
        if (!hit) continue;
        if (matched) *matched = true;
        return &d;
    }
    return primary;
}

DisplayInfo primary_display() {
    const std::vector<DisplayInfo> displays = list_displays();
    const DisplayInfo *d = choose_display(displays, L"");
    return d ? *d : DisplayInfo{};
}

const wchar_t *vrr_name(DisplayInfo::Vrr vrr) {
    switch (vrr) {
    case DisplayInfo::Vrr::Unsupported: return L"unsupported";
    case DisplayInfo::Vrr::Supported: return L"supported";
    case DisplayInfo::Vrr::On: return L"on";
    case DisplayInfo::Vrr::Off: return L"off";
    default: return L"unknown";
    }
}

const wchar_t *vrr_for_addons(DisplayInfo::Vrr found, int user) {
    if (user >= 0) return user ? L"on" : L"off";
    switch (found) {
    case DisplayInfo::Vrr::On: return L"on";
    case DisplayInfo::Vrr::Off:
    case DisplayInfo::Vrr::Unsupported: return L"off";
    default: return L"unknown";
    }
}

std::wstring display_text(const DisplayInfo &d) {
    if (!d.found) return L"unknown";
    std::wstring t = std::format(L"{}{}x{} @ {:.3f} Hz", d.name.empty() ? L"" : d.name + L", ", d.width, d.height,
                                 d.refresh_hz);
    if (d.hdr_on)
        t += std::format(L", HDR on (peak {:.0f} nits, full frame {:.0f}, SDR white {:.0f})", d.peak_nits,
                         d.full_frame_nits, d.sdr_white_nits);
    else
        t += d.hdr_supported ? L", HDR off in Windows" : L", no HDR";
    t += std::format(L", VRR {}", vrr_name(d.vrr));
    if (!d.vrr_kind.empty()) t += L" (" + d.vrr_kind + L")";
    if (d.vrr_min_hz > 0 && d.vrr_max_hz > 0) t += std::format(L" {:.0f}-{:.0f} Hz", d.vrr_min_hz, d.vrr_max_hz);
    else if (d.vrr_min_hz > 0) t += std::format(L" from {:.0f} Hz", d.vrr_min_hz);
    if (!d.vrr_source.empty()) t += L" [" + d.vrr_source + L"]";
    return t;
}

}  // namespace glasir
