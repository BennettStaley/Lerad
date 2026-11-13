// SPDX-License-Identifier: MIT
#include "gpu.h"

#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <format>
#include <regex>

#include "devices.h"

using Microsoft::WRL::ComPtr;

namespace glasir {

std::vector<GpuInfo> list_gpus() {
    std::vector<GpuInfo> gpus;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return gpus;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        GpuInfo info;
        info.name = desc.Description;
        info.vendor = desc.VendorId;
        info.found = true;
        info.device_id = desc.DeviceId;
        info.dedicated_mb = unsigned(desc.DedicatedVideoMemory >> 20);
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC out{};
            if (SUCCEEDED(output->GetDesc(&out))) info.screens.push_back(out.DeviceName);
        }
        bool twin = false;
        for (const GpuInfo &g : gpus)
            twin = twin || (info.screens.empty() && g.name == info.name && g.vendor == info.vendor &&
                            g.device_id == info.device_id);
        if (!twin) gpus.push_back(std::move(info));
    }
    return gpus;
}

GpuChoice choose_gpu(const std::vector<GpuInfo> &gpus, const std::wstring &screen_device, const std::wstring &wanted) {
    GpuChoice c;
    for (const GpuInfo &g : gpus)
        for (const std::wstring &screen : g.screens)
            if (!c.screen_gpu.found && !screen_device.empty() && names_match(screen, screen_device)) c.screen_gpu = g;
    if (!wanted.empty()) {
        for (const GpuInfo &g : gpus)
            if (!c.by_setting && name_contains(g.name, wanted)) {
                c.gpu = g;
                c.by_setting = true;
            }
        c.setting_not_found = !c.by_setting;
    }
    if (!c.gpu.found) c.gpu = c.screen_gpu;
    if (!c.gpu.found && !gpus.empty()) c.gpu = gpus.front();
    c.other_than_screen = c.gpu.found && c.screen_gpu.found &&
                          (c.gpu.name != c.screen_gpu.name || c.gpu.device_id != c.screen_gpu.device_id);
    return c;
}

GpuInfo display_gpu() {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    return choose_gpu(list_gpus(), mi.szDevice, L"").gpu;
}

bool dlss5_gpu(const GpuInfo &gpu) {
    if (gpu.vendor != kVendorNvidia) return false;
    // GeForce RTX 3050 ... 5090, desktop and laptop, and the workstation
    // cards of the same generations (RTX A4000, RTX 4000 Ada Generation,
    // RTX PRO 6000 Blackwell).
    static const std::wregex nr(L"RTX\\s*[345]0[5-9]0|RTX\\s*A\\d{3,4}\\b|\\bAda\\b|Blackwell", std::regex::icase);
    return std::regex_search(gpu.name, nr);
}

const wchar_t *vsr_kind(const GpuInfo &gpu) {
    if (gpu.vendor == kVendorNvidia && gpu.name.find(L"RTX") != std::wstring::npos) return L"nvidia";
    if (gpu.vendor == kVendorIntel) return L"intel";
    return L"none";
}

GpuClass gpu_class(const GpuInfo &gpu) {
    if (!gpu.found) return GpuClass::Unknown;
    // Integrated graphics: by name, or by having next to no memory of their own.
    static const std::wregex integrated(L"UHD Graphics|HD Graphics|Iris|Radeon\\(TM\\) Graphics|Radeon Graphics|Vega \\d",
                                        std::regex::icase);
    if (std::regex_search(gpu.name, integrated)) return GpuClass::Low;
    if (gpu.dedicated_mb == 0) return GpuClass::Unknown;  // the memory was not reported: no guess
    if (gpu.dedicated_mb < 3000) return GpuClass::Low;

    GpuClass c = gpu.dedicated_mb >= 11000 ? GpuClass::High : gpu.dedicated_mb >= 5500 ? GpuClass::Mid : GpuClass::Low;
    // GeForce RTX: the model says more than the memory (a 3060 has 12 GB, a
    // 3080 has 10). x070 and up of the RTX 30, 40 and 50 series are High.
    std::wsmatch m;
    static const std::wregex rtx(L"RTX\\s*([2-9])0([5-9])0", std::regex::icase);
    if (gpu.vendor == kVendorNvidia && std::regex_search(gpu.name, m, rtx)) {
        const int series = m[1].str()[0] - L'0', tier = m[2].str()[0] - L'0';
        c = series >= 3 && tier >= 7 ? GpuClass::High : GpuClass::Mid;
    }
    // Laptop GPUs are a step below the desktop card of the same name; Intel's
    // Arc cards have more memory than speed.
    static const std::wregex laptop(L"Laptop|Max-Q|Mobile", std::regex::icase);
    if (c == GpuClass::High && (std::regex_search(gpu.name, laptop) || gpu.vendor == kVendorIntel)) c = GpuClass::Mid;
    return c;
}

const wchar_t *gpu_class_name(GpuClass c) {
    switch (c) {
    case GpuClass::Low: return L"low";
    case GpuClass::Mid: return L"mid";
    case GpuClass::High: return L"high";
    default: return L"unknown";
    }
}

std::wstring gpu_text(const GpuInfo &gpu) {
    if (!gpu.found) return L"unknown";
    std::wstring t = std::format(L"{}, {:.1f} GB, class {} (a guess from its name and memory)", gpu.name,
                                 gpu.dedicated_mb / 1024.0, gpu_class_name(gpu_class(gpu)));
    if (gpu.screens.empty()) return t + L", no screen";
    t += L", drives";
    for (const std::wstring &s : gpu.screens) t += L" " + s;
    return t;
}

}  // namespace glasir
