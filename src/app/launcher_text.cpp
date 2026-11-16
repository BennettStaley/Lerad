// SPDX-License-Identifier: MIT
#include "launcher_text.h"

#include <cmath>
#include <format>

namespace glasir {
namespace {

const wchar_t *const kDot = L"  ·  ";

std::wstring number(double v) {
    return std::fabs(v - std::round(v)) < 0.05 ? std::format(L"{:.0f}", v) : std::format(L"{:.2f}", v);
}

std::wstring range_text(const DisplayInfo &d) {
    if (d.vrr_min_hz > 0 && d.vrr_max_hz > 0) return std::format(L" {:.0f}-{:.0f} Hz", d.vrr_min_hz, d.vrr_max_hz);
    return {};
}

const wchar_t *scaling_label(Settings::Scaling s) {
    switch (s) {
    case Settings::Scaling::Fsr: return L"FSR 1";
    case Settings::Scaling::Vsr: return L"video super resolution";
    case Settings::Scaling::Cnn: return L"ArtCNN";
    case Settings::Scaling::Anime4k: return L"Anime4K";
    default: return L"mpv";
    }
}

}  // namespace

std::wstring screen_title(const DisplayInfo &d) {
    std::wstring t = d.number > 0 ? std::to_wstring(d.number) : d.device;
    if (!d.name.empty()) t += kDot + d.name;
    t += std::format(L"{}{} x {}", kDot, d.width, d.height);
    if (d.refresh_hz > 0) t += std::format(L"{}{:.0f} Hz", kDot, d.refresh_hz);
    if (d.primary) t += L", main";
    return t;
}

std::wstring screen_hdr_text(const DisplayInfo &d) {
    if (d.hdr_on)
        return d.peak_nits >= 80 ? std::format(L"HDR on, peak {:.0f} nits", d.peak_nits) : std::wstring(L"HDR on");
    return d.hdr_supported ? L"HDR off in Windows" : L"no HDR";
}

std::wstring screen_vrr_text(const DisplayInfo &d) {
    const std::wstring kind = (d.vrr_kind.empty() ? L"Variable refresh" : d.vrr_kind) + range_text(d);
    switch (d.vrr) {
    case DisplayInfo::Vrr::On: return kind + L", on";
    case DisplayInfo::Vrr::Off: return kind + L", switched off";
    case DisplayInfo::Vrr::Supported: return kind + L" (on or off: not known)";
    case DisplayInfo::Vrr::Unsupported: return L"no variable refresh";
    default: return L"variable refresh not known";
    }
}

std::wstring screen_details(const DisplayInfo &d) { return screen_hdr_text(d) + kDot + screen_vrr_text(d); }

std::wstring gpu_title(const GpuInfo &gpu) {
    if (!gpu.found) return L"not found";
    std::wstring t = gpu.name;
    if (gpu.dedicated_mb >= 1000) t += std::format(L"{}{:.0f} GB", kDot, gpu.dedicated_mb / 1024.0);
    return t;
}

std::wstring monitor_setting(const DisplayInfo &d) { return d.number > 0 ? std::to_wstring(d.number) : d.device; }

std::vector<SummaryLine> summary_lines(const AdviceInput &in, const Advice &advice) {
    const Outcome &o = advice.outcome;
    const Settings &s = in.settings;
    std::vector<SummaryLine> lines;

    // The capture.
    if (!in.card_found) {
        lines.push_back({L"No capture card: nothing to play.", true});
    } else if (!o.capture_known) {
        const bool unreadable = find_advice(advice, "capture-directshow") != nullptr;
        lines.push_back({in.card + (unreadable ? L", through DirectShow (mpv picks the mode)."
                                               : L": no mode for these settings."),
                         !unreadable});
    } else {
        lines.push_back({std::format(L"{}: {} x {} at {} fps, {}{}.", in.card, o.capture.width, o.capture.height,
                                     number(o.capture.fps),
                                     s.hdr_auto && o.video.ten_bit ? (o.video.hdr ? L"HDR or SDR as the console sends it"
                                                                                  : L"SDR or HDR as the console sends it")
                                                                   : o.video.hdr ? L"HDR" : L"SDR",
                                     o.media_foundation ? L"" : L", through DirectShow"),
                         false});
    }

    // The frames shown.
    if (in.card_found) {
        std::wstring text;
        bool added = false;
        for (const ShownRate &r : o.shown) {
            const std::wstring shown =
                r.multiplier > 1 ? std::format(L"{} (x{})", number(r.shown_fps), r.multiplier) : number(r.shown_fps);
            text += text.empty() ? std::format(L"A {} fps game is shown at {}", number(r.game_fps), shown)
                                 : std::format(L", a {} fps game at {}", number(r.game_fps), shown);
            added = added || r.multiplier > 1;
        }
        if (o.framegen && o.max_shown_hz <= 0)
            lines.push_back({L"Frame generation adds what the screen can show (its rate is not known yet).", false});
        else if (o.framegen)
            lines.push_back({text + (added ? L"." : L": frame generation has no room on this screen."), !added});
        else if (s.framegen)
            lines.push_back({text + L": frame generation cannot run.", true});
        else
            lines.push_back({text + L" (frame generation off).", false});
    }

    // HDR.
    if (o.video.hdr) {
        const HdrPlan &p = o.hdr_plan;
        const bool as_asked = p.output == HdrPlan::Output::Hdr ? !p.forced_without_hdr
                                                               : s.hdr_output == Settings::HdrOutput::Sdr;
        lines.push_back({hdr_plan_text(p) + L".", !as_asked});
    }

    // The screen and its variable refresh, as frame generation is told.
    if (in.display.found) {
        const DisplayInfo &d = in.display;
        std::wstring text = L"On screen " + monitor_setting(d) + (d.name.empty() ? L"" : L" (" + d.name + L")");
        if (d.refresh_hz > 0) text += std::format(L", {:.0f} Hz", d.refresh_hz);
        const bool set = s.vrr != Settings::Vrr::Auto;
        text += o.vrr == L"on"    ? L", variable refresh on"
                : o.vrr == L"off" ? (d.vrr == DisplayInfo::Vrr::Unsupported && !set ? L", no variable refresh"
                                                                                     : L", variable refresh off")
                                  : L", variable refresh not known";
        if (set) text += L" (as set here)";
        lines.push_back({text + L".", false});
    }

    // DLSS 5 and the scaling.
    std::wstring last = o.dlss5 ? L"DLSS 5 on" : L"DLSS 5 off";
    if (o.upscaled && o.capture_known)
        last += std::format(L"; scaled up from {} to {} lines with {}", o.capture.height, o.picture_height,
                            scaling_label(s.scaling));
    lines.push_back({last + L".", false});
    return lines;
}

std::wstring console_line(const Advice &advice) {
    std::wstring line;
    for (const AdviceItem &i : advice.items)
        if (i.kind == AdviceKind::Console && !i.brief.empty()) line += (line.empty() ? L"" : kDot) + i.brief;
    return line;
}

std::wstring console_hint(const Advice &advice) {
    for (const AdviceItem &i : advice.items)
        if (i.kind == AdviceKind::Console && i.brief.empty()) return i.text;
    return {};
}

AdvicePlace advice_place(const std::string &id) {
    const auto starts = [&id](const char *prefix) { return id.rfind(prefix, 0) == 0; };
    if (starts("hdr-")) return AdvicePlace::Hdr;
    if (starts("framegen-") || starts("rec-framegen-") || id == "screen-60hz" || id == "refresh-unknown")
        return AdvicePlace::Framegen;
    if (starts("vrr-") || starts("gpu-") || id == "monitor-not-found") return AdvicePlace::Screen;
    if (starts("dlss5-") || id == "rec-scaling" || id == "vsr-unavailable") return AdvicePlace::Enhancements;
    return AdvicePlace::Capture;  // the card, its modes, the data rate
}

AdviceShown advice_shown(const AdviceItem &item) {
    if (item.applied || item.kind == AdviceKind::Console) return AdviceShown::Hidden;
    if (item.kind == AdviceKind::Recommendation) {
        if (item.suggest.empty()) return AdviceShown::Hidden;
        // Not "better", only "also possible": for a console set to 120 Hz,
        // for 30 fps games, for a laptop's second GPU.
        for (const char *choice : {"capture-120", "capture-sharper-30", "gpu-has-rtx"})
            if (item.id == choice) return AdviceShown::Place;
        return AdviceShown::Notice;
    }
    return item.severity == AdviceSeverity::Info ? AdviceShown::Place : AdviceShown::Notice;
}

void apply_suggestion(Settings &s, const AdviceItem &item) {
    std::wstring error;
    for (const SettingValue &v : item.suggest) apply_override(s, setting_text(v), error);
}

std::vector<MultiplierChoice> multiplier_choices(const AdviceInput &in) {
    std::vector<MultiplierChoice> choices;
    AdviceInput with = in;
    for (int k = 2; k <= 4; ++k) {
        with.settings.framegen_multiplier = k;
        const Outcome o = advise(with).outcome;
        if (!o.framegen || o.max_shown_hz <= 0) return {};
        MultiplierChoice c;
        c.multiplier = k;
        c.delay_ms = 1000.0 / 30.0 * (k - 1) / k;
        const ShownRate *slowest = o.shown.empty() ? nullptr : &o.shown.front();
        int rates = 0;
        for (const ShownRate &r : o.shown) rates += r.multiplier > 1;
        for (const ShownRate &r : o.shown) {
            c.usable = c.usable || r.multiplier == k;
            if (r.multiplier < 2) continue;
            c.text += (c.text.empty() ? L"" : L", ") +
                      std::format(L"{} fps {}{}", number(r.game_fps), rates > 1 ? L"at " : L"shown at ",
                                  number(r.shown_fps));
        }
        if (!c.usable && slowest) {
            const std::wstring fps = number(k * slowest->game_fps);
            const std::wstring hz = std::format(L"{:.0f} Hz", in.display.refresh_hz);
            c.why = slowest->kept_even ? fps + L" fps would not fall on whole refreshes of this fixed " + hz + L" screen."
                    : o.capped         ? fps + std::format(L" fps is over the limit of {}.", in.settings.framegen_max_hz)
                                       : fps + L" fps is more than this " + hz + L" screen shows.";
        }
        choices.push_back(std::move(c));
    }
    return choices;
}

}  // namespace glasir
