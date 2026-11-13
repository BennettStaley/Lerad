// SPDX-License-Identifier: MIT
#include "display.h"

#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <format>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace glasir {
namespace {

// What DXGI says about the output: its mode, whether it presents HDR10 now
// and the brightness the screen reports.
void read_dxgi(HMONITOR monitor, DisplayInfo &d) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || desc.Monitor != monitor) continue;
            d.found = true;
            d.device = desc.DeviceName;
            d.width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            d.height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
            ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1 desc1{};
            if (SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&desc1))) {
                d.hdr_on = desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                d.peak_nits = desc1.MaxLuminance;
                d.full_frame_nits = desc1.MaxFullFrameLuminance;
                d.min_nits = desc1.MinLuminance;
            }
            return;
        }
    }
}

// What the display configuration says about the same output: the monitor's
// name, the exact refresh rate, whether it has HDR at all, and Windows' SDR
// brightness while HDR is on.
void read_display_config(DisplayInfo &d) {
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
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || d.device != source.viewGdiDeviceName)
            continue;

        const auto &rate = path.targetInfo.refreshRate;
        if (rate.Denominator) d.refresh_hz = double(rate.Numerator) / rate.Denominator;

        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target), path.targetInfo.adapterId,
                         path.targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS && target.flags.friendlyNameFromEdid)
            d.name = target.monitorFriendlyDeviceName;

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
        return;
    }
}

}  // namespace

DisplayInfo primary_display() {
    DisplayInfo d;
    const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    read_dxgi(monitor, d);
    if (!d.found) return d;
    read_display_config(d);
    if (d.refresh_hz <= 0) {
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsW(d.device.c_str(), ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
            d.refresh_hz = mode.dmDisplayFrequency;
    }
    if (d.hdr_on) d.hdr_supported = true;
    return d;
}

const wchar_t *vrr_name(DisplayInfo::Vrr vrr) {
    switch (vrr) {
    case DisplayInfo::Vrr::Unsupported: return L"unsupported";
    case DisplayInfo::Vrr::Supported: return L"supported";
    case DisplayInfo::Vrr::On: return L"on";
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
    if (d.vrr_max_hz > 0) t += std::format(L" ({:.0f}-{:.0f} Hz)", d.vrr_min_hz, d.vrr_max_hz);
    if (!d.vrr_source.empty()) t += L" [" + d.vrr_source + L"]";
    return t;
}

}  // namespace glasir
