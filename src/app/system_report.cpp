// SPDX-License-Identifier: MIT
#include "system_report.h"

#include <cmath>
#include <format>
#include <optional>

#include "reshade_config.h"

namespace glasir {
namespace {

std::wstring rate(double fps) {
    return std::fabs(fps - std::round(fps)) < 0.05 ? std::format(L"{:.0f}", fps) : std::format(L"{:.2f}", fps);
}

std::wstring mode_text(const VideoMode &m) {
    return std::format(L"{}x{} at {} fps", m.width, m.height, rate(m.fps));
}

// The largest mode that passes and, of that size, the fastest.
template <class Pred>
std::optional<VideoMode> largest(const std::vector<VideoMode> &modes, Pred pass) {
    std::optional<VideoMode> best;
    for (const VideoMode &m : modes) {
        if (!pass(m)) continue;
        const UINT64 a = UINT64(m.width) * m.height, b = best ? UINT64(best->width) * best->height : 0;
        if (!best || a > b || (a == b && m.fps > best->fps)) best = m;
    }
    return best;
}

}  // namespace

SystemInfo survey_system(const Settings &s) {
    SystemInfo info;
    info.displays = list_displays();
    if (const DisplayInfo *d = choose_display(info.displays, s.monitor, &info.monitor_matched)) info.display = *d;
    info.gpus = list_gpus();
    info.gpu = choose_gpu(info.gpus, info.display.device, s.gpu);
    return info;
}

AdviceInput advice_input(const Settings &s, const SystemInfo &system, const std::vector<CaptureDevice> &cards,
                         const std::wstring &engine_dir) {
    AdviceInput in;
    in.settings = s;
    in.display = system.display;
    in.monitor_matched = system.monitor_matched;
    in.gpus = system.gpus;
    in.gpu = system.gpu;
    in.reshade_installed = reshade_installed(engine_dir);
    in.dlss5_installed = dlss5_installed(engine_dir);

    // The card as Glasir.exe finds it: one that has the mode asked for, else
    // (so the advice can say what it lacks) the one named, or any card.
    ModeRequest request;
    request.hdr = s.hdr;
    request.width = s.width;
    request.height = s.height;
    request.fps = s.fps;
    request.max_auto_height = system.display.found ? UINT32(system.display.height) : 0;
    const CaptureDevice *card = choose_device(cards, s.device, request);
    if (!card) {
        ModeRequest loose;
        loose.supported_only = false;
        card = choose_device(cards, s.device, loose);
    }
    if (card) {
        in.card_found = true;
        in.card = card->name;
        in.modes = card->modes;
    }
    return in;
}

std::wstring card_summary(const CaptureDevice &card) {
    if (!card.opened) return card.name + L": could not be opened (in use by another app?)";
    auto sdr = [](const VideoMode &m) { return m.supported && m.format != PixelFormat::P010; };
    auto hdr = [](const VideoMode &m) { return m.supported && m.format == PixelFormat::P010; };
    auto hdr60 = [](const VideoMode &m) { return m.supported && m.format == PixelFormat::P010 && m.fps >= 59; };
    std::wstring t = card.name + L": ";
    const auto best_sdr = largest(card.modes, sdr), best_hdr = largest(card.modes, hdr),
               best_hdr60 = largest(card.modes, hdr60);
    t += best_sdr ? L"SDR up to " + mode_text(*best_sdr) + L" (" + best_sdr->subtype + L")" : L"no SDR mode Glasir reads";
    if (best_hdr60) {
        t += L"; HDR up to " + mode_text(*best_hdr60);
        if (best_hdr->width * best_hdr->height > best_hdr60->width * best_hdr60->height)
            t += L", " + mode_text(*best_hdr);
    } else if (best_hdr) {
        t += L"; HDR up to " + mode_text(*best_hdr);
    } else {
        t += L"; no HDR (no 10-bit mode)";
    }
    size_t other = 0;
    for (const VideoMode &m : card.modes) other += !m.supported;
    if (other) t += std::format(L"; {} more mode{} through DirectShow only", other, other == 1 ? L"" : L"s");
    return t;
}

std::wstring system_report(const SystemInfo &system, const std::vector<CaptureDevice> *cards, const Advice *advice) {
    std::wstring t = L"GPUs:\n";
    for (const GpuInfo &g : system.gpus) {
        t += L"  " + gpu_text(g);
        if (system.gpu.gpu.found && g.name == system.gpu.gpu.name && g.device_id == system.gpu.gpu.device_id)
            t += system.gpu.by_setting ? L"  <- mpv draws on it (display.gpu)" : L"  <- mpv draws on it";
        t += L"\n";
    }
    if (system.gpus.empty()) t += L"  none found\n";
    t += L"Screens:\n";
    for (const DisplayInfo &d : system.displays) {
        t += std::format(L"  {}{}: {}", d.number, d.primary ? L" (main)" : L"", display_text(d));
        if (d.device == system.display.device) t += L"  <- Glasir plays here";
        t += L"\n";
    }
    if (system.displays.empty()) t += L"  none found\n";
    if (cards) {
        t += L"Capture cards:\n";
        for (const CaptureDevice &c : *cards) t += L"  " + card_summary(c) + L"\n";
        if (cards->empty()) t += L"  none found\n";
    }
    if (advice) t += advice_text(*advice);
    return t;
}

}  // namespace glasir
