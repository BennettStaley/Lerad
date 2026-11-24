// SPDX-License-Identifier: MIT
#include "launcher_text.h"

#include <cmath>
#include <cwctype>
#include <format>

#include "app_version.h"

namespace app {
namespace {

const wchar_t *const kDot = L" · ";
const wchar_t *const kArrow = L" → ";

std::wstring number(double v) {
    return std::fabs(v - std::round(v)) < 0.05 ? std::format(L"{:.0f}", v) : std::format(L"{:.2f}", v);
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

// Variable refresh on the screen as frame generation is told it, in words.
std::wstring vrr_words(const AdviceInput &in, const Outcome &o) {
    const bool set = in.settings.vrr != Settings::Vrr::Auto;
    std::wstring text = o.vrr == L"on"    ? L"variable refresh on"
                        : o.vrr == L"off" ? (in.display.vrr == DisplayInfo::Vrr::Unsupported && !set
                                                 ? L"no variable refresh"
                                                 : L"variable refresh off")
                                          : L"variable refresh not known";
    if (set) text += L" (as set here)";
    return text;
}

// What makes the frames in between: NVIDIA's generator where it is chosen
// and can run, else the app's own.
const wchar_t *frames_by(const AdviceInput &in) {
    const bool nvidia = !in.gpu.gpu.found || dlss_fg_gpu(in.gpu.gpu);
    if (in.settings.framegen_video && in.video_installed && nvidia) return L"NVIDIA Video";
    if (in.settings.framegen_anr && in.anr_installed) return L"Apex ANR";
    const bool dlss = in.settings.framegen_dlss && in.dlssfg_installed && nvidia;
    return dlss ? L"NVIDIA DLSS" : APP_NAME_W;
}

}  // namespace

std::wstring card_title(const std::wstring &name, bool magewell) {
    std::wstring t = name;
    // "Video (00 Pro Capture HDMI 4K+)": the driver's wrapping and the
    // board's number say nothing to a player.
    const std::wstring wrap = L"Video (";
    if (t.size() > wrap.size() + 1 && t.compare(0, wrap.size(), wrap) == 0 && t.back() == L')') {
        t = t.substr(wrap.size(), t.size() - wrap.size() - 1);
        if (t.size() > 3 && iswdigit(t[0]) && iswdigit(t[1]) && t[2] == L' ') t = t.substr(3);
    }
    if (magewell && t.find(L"Magewell") == std::wstring::npos) t = L"Magewell " + t;
    return t;
}

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

std::wstring vrr_brand_text(const DisplayInfo &d) {
    std::wstring t = d.vrr_kind;
    if (d.vrr_min_hz > 0 && d.vrr_max_hz > 0)
        t += std::format(L"{}{:.0f}-{:.0f} Hz", t.empty() ? L"" : L", ", d.vrr_min_hz, d.vrr_max_hz);
    return t;
}

std::wstring vrr_found_word(const DisplayInfo &d) {
    switch (d.vrr) {
    case DisplayInfo::Vrr::On: return L"on";
    case DisplayInfo::Vrr::Off: return L"off";
    case DisplayInfo::Vrr::Unsupported: return L"none";
    default: return L"not known";
    }
}

std::wstring screen_vrr_text(const DisplayInfo &d) {
    const std::wstring in_brackets = d.vrr_kind.empty() ? L"" : L" (" + d.vrr_kind + L")";
    switch (d.vrr) {
    case DisplayInfo::Vrr::On: return L"variable refresh on" + in_brackets;
    case DisplayInfo::Vrr::Off: return L"variable refresh off" + in_brackets;
    case DisplayInfo::Vrr::Supported: return L"variable refresh on or off: not known" + in_brackets;
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

std::vector<ResultStep> result_steps(const AdviceInput &in, const Advice &advice, bool low_latency) {
    const Outcome &o = advice.outcome;
    const Settings &s = in.settings;
    std::vector<ResultStep> steps;
    using Kind = ResultStep::Kind;

    // The console: what to set on it for these choices.
    {
        ResultStep step;
        step.kind = Kind::Console;
        step.title = L"Console, set it to";
        for (const AdviceItem &i : advice.items)
            if (i.kind == AdviceKind::Console && !i.brief.empty()) step.chips.push_back(i.brief);
        if (!step.chips.empty()) steps.push_back(std::move(step));
    }

    // The capture card.
    {
        ResultStep step;
        step.kind = Kind::Card;
        step.title = L"Capture card";
        if (!in.card_found) {
            step.big = L"No capture card";
            step.detail = L"Nothing to play.";
            step.matters = step.problem = true;
        } else if (!o.capture_known) {
            const bool basic = find_advice(advice, "capture-directshow") != nullptr;
            step.big = basic ? L"Basic mode" : L"No mode";
            step.detail = basic ? L"mpv picks the capture mode." : L"This card has no mode for what is chosen.";
            step.matters = step.problem = !basic;
        } else {
            step.big = std::format(L"{} x {}", o.capture.width, o.capture.height);
            step.detail = number(o.capture.fps) + L" fps" + kDot +
                          (s.hdr_auto && o.video.ten_bit ? (o.video.hdr ? L"HDR or SDR, as the console sends it"
                                                                        : L"SDR or HDR, as the console sends it")
                           : o.video.hdr                 ? L"HDR (10-bit)"
                                                         : L"SDR");
            if (!o.media_foundation)
                step.detail += std::wstring(kDot) + L"basic mode";
            else if (low_latency)
                step.detail += std::wstring(kDot) + L"low latency";
        }
        steps.push_back(std::move(step));
    }

    // The frames shown.
    if (in.card_found && !o.shown.empty()) {
        ResultStep step;
        step.kind = Kind::Frames;
        step.title = L"Frames";
        const ShownRate &first = o.shown.front();
        bool added = false;
        for (const ShownRate &r : o.shown) added = added || r.multiplier > 1;
        step.multiplier = first.multiplier;
        step.big = first.multiplier > 1 ? number(first.game_fps) + kArrow + number(first.shown_fps) + L" fps"
                                        : number(first.shown_fps) + L" fps";
        const std::wstring dlss5 = o.dlss5 ? L" DLSS 5 on." : L" DLSS 5 off.";
        if (o.framegen && o.max_shown_hz <= 0) {
            step.detail = L"Frame generation adds what the screen can show: its rate is not known yet." + dlss5;
        } else if (o.framegen && added) {
            step.detail = first.multiplier > 1 ? std::format(L"x{} by {}.", first.multiplier, frames_by(in))
                                               : std::wstring(L"No frames added at this rate.");
            for (size_t i = 1; i < o.shown.size(); ++i) {
                const ShownRate &r = o.shown[i];
                step.detail += r.multiplier > 1 ? std::format(L" A {} fps game: {} fps (x{}).", number(r.game_fps),
                                                              number(r.shown_fps), r.multiplier)
                                                : std::format(L" A {} fps game: no frames added.", number(r.game_fps));
            }
            step.detail += dlss5;
        } else if (o.framegen) {
            step.detail = L"Frame generation has no room on this screen." + dlss5;
            step.matters = true;
        } else if (s.framegen) {
            step.detail = L"Frame generation cannot run." + dlss5;
            step.matters = true;
        } else {
            step.detail = L"Frame generation off." + dlss5;
        }
        steps.push_back(std::move(step));
    }

    // The screen: its rate, what the picture goes out as, variable refresh
    // as frame generation is told, and the scaling.
    {
        ResultStep step;
        step.kind = Kind::Screen;
        step.title = L"Screen";
        const DisplayInfo &d = in.display;
        const HdrPlan &p = o.hdr_plan;
        const bool hdr_out = o.video.hdr && p.output == HdrPlan::Output::Hdr;
        const wchar_t *picture = hdr_out ? L"HDR10" : L"SDR";
        step.big = d.found && d.refresh_hz > 0 ? std::format(L"{:.0f} Hz{}{}", d.refresh_hz, kDot, picture) : std::wstring(picture);
        if (d.found)
            step.detail = (d.name.empty() ? L"Screen " + monitor_setting(d) : d.name) + L", " + vrr_words(in, o) + L".";
        else
            step.detail = L"The main screen: it could not be read.";
        if (o.video.hdr) {
            // Tone mapped without being asked to, or HDR sent where Windows has it off: not as asked.
            const bool as_asked = p.output == HdrPlan::Output::Hdr ? !p.forced_without_hdr
                                                                   : s.hdr_output == Settings::HdrOutput::Sdr;
            if (p.output == HdrPlan::Output::Sdr || p.fit || p.forced_without_hdr) step.detail += L" " + hdr_plan_text(p) + L".";
            step.matters = !as_asked;
        }
        if (o.upscaled && o.capture_known)
            step.detail += std::format(L" Scaled up from {} to {} lines with {}.", o.capture.height, o.picture_height,
                                       scaling_label(s.scaling));
        steps.push_back(std::move(step));
    }
    return steps;
}

std::wstring console_hint(const Advice &advice) {
    for (const AdviceItem &i : advice.items)
        if (i.kind == AdviceKind::Console && i.brief.empty()) return i.text;
    return {};
}

PlaySummary play_summary(const AdviceInput &in, const Advice &advice) {
    const Outcome &o = advice.outcome;
    PlaySummary sum;
    int warnings = 0;
    bool problem = false;
    for (const AdviceItem &i : advice.items) {
        if (advice_shown(i) != AdviceShown::Notice) continue;
        problem = problem || i.severity == AdviceSeverity::Problem;
        warnings += i.severity == AdviceSeverity::Warning;
    }
    if (problem) {
        sum.state = PlaySummary::State::Problem;
        sum.word = L"Cannot play";
        sum.text = !in.card_found ? L"No capture card was found." : L"This card cannot capture what is chosen.";
        return sum;
    }
    if (warnings) {
        sum.state = PlaySummary::State::Warning;
        sum.word = warnings == 1 ? std::wstring(L"1 warning") : std::format(L"{} warnings", warnings);
    } else {
        sum.word = L"Ready";
    }
    if (o.shown.empty()) return sum;
    const ShownRate &first = o.shown.front();
    const bool tone_mapped = o.video.hdr && o.hdr_plan.output == HdrPlan::Output::Sdr;
    sum.text = std::format(L"A {} fps game is shown at {} fps, in {}.", number(first.game_fps), number(first.shown_fps),
                           tone_mapped ? L"SDR (tone mapped from HDR)" : o.video.hdr ? L"HDR" : L"SDR");
    if (!in.settings.framegen) sum.text += L" Frame generation is off.";
    return sum;
}

AdvicePlace advice_place(const std::string &id) {
    const auto starts = [&id](const char *prefix) { return id.rfind(prefix, 0) == 0; };
    if (starts("hdr-") || id == "rec-scaling" || id == "vsr-unavailable") return AdvicePlace::Picture;
    if (starts("framegen-") || starts("rec-framegen-") || id == "screen-60hz" || id == "refresh-unknown")
        return AdvicePlace::Frames;
    if (starts("vrr-") || starts("gpu-") || id == "monitor-not-found") return AdvicePlace::Screen;
    if (starts("dlss5-")) return AdvicePlace::Extras;
    return AdvicePlace::Card;  // the card, its modes, the data rate
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
            // Under the choice: the slowest game rate that gets this many.
            if (r.multiplier == k && c.note.empty())
                c.note = number(r.game_fps) + kArrow + number(r.shown_fps) + L" fps";
        }
        if (!c.usable) c.note = L"does not fit";
        if (!c.usable && slowest) {
            const std::wstring fps = number(k * slowest->game_fps);
            const std::wstring hz = std::format(L"{:.0f} Hz", in.display.refresh_hz);
            c.why = slowest->kept_even ? fps + L" fps would not fall on whole refreshes of this fixed " + hz + L" screen."
                    : o.capped         ? fps + std::format(L" fps is over the limit of {} fps.", in.settings.framegen_max_hz)
                                       : fps + L" fps is more than this " + hz + L" screen shows.";
        }
        choices.push_back(std::move(c));
    }
    return choices;
}

}  // namespace app
