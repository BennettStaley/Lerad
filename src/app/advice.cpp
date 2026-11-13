// SPDX-License-Identifier: MIT
#include "advice.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

#include "fg_pacing.h"
#include "log.h"

namespace glasir {
namespace {

using Vrr = DisplayInfo::Vrr;

std::wstring number(double v) {
    return std::fabs(v - std::round(v)) < 0.05 ? std::format(L"{:.0f}", v) : std::format(L"{:.2f}", v);
}

std::wstring size_text(const VideoMode &m) { return std::format(L"{}x{}", m.width, m.height); }
std::wstring mode_text(const VideoMode &m) { return size_text(m) + L" at " + number(m.fps) + L" fps"; }

bool sdr_format(const VideoMode &m) { return m.supported && m.format != PixelFormat::P010; }
bool hdr_format(const VideoMode &m) { return m.supported && m.format == PixelFormat::P010; }
UINT64 area(const VideoMode &m) { return UINT64(m.width) * m.height; }
bool rate60(const VideoMode &m) { return m.fps >= 59.0; }
// Consoles send 16:9; a card's 21:9 mode would stretch the picture.
bool wide16x9(const VideoMode &m) { return std::abs(int(m.width) * 9 - int(m.height) * 16) <= int(m.height) / 4; }

// The largest mode that passes, and of that size the fastest.
template <class Pred>
std::optional<VideoMode> largest(const std::vector<VideoMode> &modes, Pred pass) {
    std::optional<VideoMode> best;
    for (const VideoMode &m : modes)
        if (pass(m) && (!best || area(m) > area(*best) || (area(m) == area(*best) && m.fps > best->fps))) best = m;
    return best;
}

// What glasir.ini has now for a key advise() may suggest.
std::wstring current(const Settings &s, const SettingValue &v) {
    const std::wstring name = v.section + L"." + v.key;
    if (name == L"capture.width") return std::to_wstring(s.width);
    if (name == L"capture.height") return std::to_wstring(s.height);
    if (name == L"capture.fps") return number(s.fps);
    if (name == L"video.mode") return s.hdr ? L"hdr" : L"sdr";
    if (name == L"video.scaling") return scaling_name(s.scaling);
    if (name == L"framegen.quality") return s.framegen_fast ? L"fast" : L"high";
    if (name == L"framegen.method") return s.framegen_blend ? L"blend" : L"motion";
    if (name == L"framegen.multiplier") return std::to_wstring(s.framegen_multiplier);
    if (name == L"framegen.max_output_hz") return std::to_wstring(s.framegen_max_hz);
    if (name == L"frames.dedup") return s.dedup ? L"1" : L"0";
    if (name == L"display.gpu") return s.gpu;
    if (name == L"display.vrr") return vrr_setting_name(s.vrr);
    return L"\x1";  // not a key advise() knows: never "applied"
}

std::vector<SettingValue> capture_settings(const VideoMode &m) {
    return {{L"capture", L"width", std::to_wstring(m.width)},
            {L"capture", L"height", std::to_wstring(m.height)},
            {L"capture", L"fps", number(m.fps)}};
}

int severity_rank(const AdviceItem &i) {
    if (i.kind == AdviceKind::Console) return 4;
    if (i.kind == AdviceKind::Recommendation && i.severity == AdviceSeverity::Info) return 3;
    return i.severity == AdviceSeverity::Problem ? 0 : i.severity == AdviceSeverity::Warning ? 1 : 2;
}

}  // namespace

std::wstring setting_text(const SettingValue &v) { return v.section + L"." + v.key + L"=" + v.value; }

Advice advise(const AdviceInput &in) {
    Advice a;
    Outcome &o = a.outcome;
    const Settings &s = in.settings;
    const DisplayInfo &d = in.display;
    const GpuInfo &gpu = in.gpu.gpu;
    using Sev = AdviceSeverity;
    auto add = [&](const char *id, AdviceKind kind, Sev severity, std::wstring text,
                   std::vector<SettingValue> suggest = {}) {
        AdviceItem item{id, kind, severity, std::move(text), std::move(suggest)};
        item.applied = !item.suggest.empty() &&
                       std::all_of(item.suggest.begin(), item.suggest.end(),
                                   [&](const SettingValue &v) { return current(s, v) == v.value; });
        a.items.push_back(std::move(item));
    };
    auto limit = [&](const char *id, Sev severity, std::wstring text, std::vector<SettingValue> suggest = {}) {
        add(id, AdviceKind::Limit, severity, std::move(text), std::move(suggest));
    };
    auto recommend = [&](const char *id, std::wstring text, std::vector<SettingValue> suggest) {
        add(id, AdviceKind::Recommendation, Sev::Info, std::move(text), std::move(suggest));
    };
    const std::wstring card = in.card.empty() ? L"The capture card" : L"\"" + in.card + L"\"";

    // ------------------------------------------------------------ the screen
    // monitor-not-found: display.monitor names no screen.
    if (!in.monitor_matched && !s.monitor.empty())
        limit("monitor-not-found", Sev::Warning,
              L"display.monitor = " + s.monitor + L" names no screen of this PC: Glasir plays on the main screen. "
              L"\"Glasir.exe --system\" lists the screens.");

    // ------------------------------------------------------------ the capture
    // The mode, picked as Glasir.exe does when it plays (main.cpp open_source).
    ModeRequest request;
    request.hdr = s.hdr;
    request.width = s.width;
    request.height = s.height;
    request.fps = s.fps;
    request.max_auto_height = d.found ? UINT32(d.height) : 0;
    std::optional<VideoMode> mode;
    if (in.card_found) {
        if (s.path != Settings::CapturePath::DirectShow) mode = choose_mode(in.modes, request);
        o.media_foundation = mode.has_value();
        if (!mode && !s.hdr && s.path != Settings::CapturePath::MediaFoundation) {
            ModeRequest any = request;
            any.supported_only = false;
            mode = choose_mode(in.modes, any);
        }
    }
    o.capture_known = mode.has_value();
    if (mode) o.capture = *mode;
    if (mode && o.media_foundation)
        o.raw_gb_per_s = double(frame_size(mode->format, mode->width, mode->height)) * mode->fps / 1e9;

    const auto best_sdr = largest(in.modes, sdr_format);
    const auto best_hdr = largest(in.modes, hdr_format);
    const auto best_hdr60 = largest(in.modes, [](const VideoMode &m) { return hdr_format(m) && rate60(m); });
    const bool asked = s.width || s.height || s.fps > 0;
    const std::wstring asked_text =
        (s.width && s.height ? std::format(L"{}x{}", s.width, s.height) : std::wstring(L"any size")) +
        (s.fps > 0 ? L" at " + number(s.fps) + L" fps" : L"");

    if (!in.card_found) {
        // no-card: nothing to capture from.
        limit("no-card", Sev::Problem,
              s.device.empty() ? std::wstring(L"No capture card was found. Check that it is plugged in and its driver "
                                              L"is installed.")
                               : L"The capture card \"" + s.device + L"\" from glasir.ini was not found.");
    } else if (!mode && s.hdr && !best_hdr) {
        // capture-no-hdr-mode: HDR asked of a card without a 10-bit mode.
        limit("capture-no-hdr-mode", Sev::Problem,
              card + L" has no 10-bit mode (P010), so it cannot capture HDR. Use SDR.",
              {{L"video", L"mode", L"sdr"}});
    } else if (!mode && !in.modes.empty()) {
        // capture-no-mode: the size and rate asked for are not offered.
        limit("capture-no-mode", Sev::Problem,
              card + L" has no " + (s.hdr ? L"HDR" : L"SDR") + L" mode of " + asked_text +
                  L". Automatic (0) picks one it has.",
              {{L"capture", L"width", L"0"}, {L"capture", L"height", L"0"}, {L"capture", L"fps", L"0"}});
    } else if (!o.media_foundation) {
        // capture-directshow: the fallback path and what it loses.
        limit("capture-directshow", Sev::Warning,
              card + L" is opened through DirectShow" +
                  (s.path == Settings::CapturePath::DirectShow ? L" (capture.path = directshow)"
                                                               : L" (it has no mode Glasir can read itself)") +
                  L": no frame generation, and repeated frames are not dropped.");
    }

    // card-hdr-limit / card-no-hdr: what the card's HDR modes reach.
    if (in.card_found && best_sdr && !best_hdr) {
        if (!s.hdr) limit("card-no-hdr", Sev::Info, card + L" has no 10-bit mode: SDR only.");
    } else if (best_sdr && best_hdr) {
        const VideoMode &top = best_hdr60 ? *best_hdr60 : *best_hdr;
        const bool less = area(top) < area(*best_sdr) || top.fps + 1 < best_sdr->fps;
        if (less) {
            std::wstring text = card + L" has HDR only up to " + mode_text(top);
            if (best_hdr60 && area(*best_hdr) > area(*best_hdr60)) text += L" (" + mode_text(*best_hdr) + L" too)";
            limit("card-hdr-limit", Sev::Info, text + L"; SDR goes up to " + mode_text(*best_sdr) + L".");
        }
    }

    // The picture's place on the screen (full screen, the source's shape).
    if (d.found && mode && mode->width && mode->height) {
        const double shape = double(mode->width) / mode->height;
        o.picture_height = std::min(d.height, int(std::lround(d.width / shape)));
        o.picture_width = std::min(d.width, int(std::lround(o.picture_height * shape)));
        o.upscaled = int(mode->height) < o.picture_height;
    }

    if (mode && o.media_foundation) {
        // What automatic picks for this screen: 60 fps first, then the
        // largest 16:9 size that fits.
        ModeRequest automatic;
        automatic.hdr = s.hdr;
        automatic.max_auto_height = request.max_auto_height;
        const auto pick = choose_mode(in.modes, automatic);
        const bool same = pick && pick->width == mode->width && pick->height == mode->height &&
                          std::fabs(pick->fps - mode->fps) < 1.0;
        if (pick && d.found && int(mode->height) > d.height && pick->height < mode->height) {
            // capture-larger-than-screen: more pixels than the screen has.
            limit("capture-larger-than-screen", Sev::Warning,
                  std::format(L"The capture ({}) is larger than the screen ({}x{}): it is scaled down again, and "
                              L"moves {:.1f} GB/s for nothing. {} looks the same.",
                              size_text(*mode), d.width, d.height, o.raw_gb_per_s, mode_text(*pick)),
                  capture_settings(*pick));
        } else if (pick && !same && !rate60(*mode) && rate60(*pick)) {
            // capture-30fps: a mode slower than 60 where the card has 60.
            limit("capture-30fps", Sev::Info,
                  L"The capture runs at " + number(mode->fps) + L" fps: a game faster than that loses frames. " +
                      card + L" has " + mode_text(*pick) + (s.hdr ? L" in HDR." : L"."),
                  capture_settings(*pick));
        } else if (pick) {
            // rec-capture: the mode for this screen.
            recommend("rec-capture",
                      std::format(L"Capture mode for this screen: {} ({} fps first, then the largest 16:9 size "
                                  L"within its {} lines).",
                                  mode_text(*pick), rate60(*pick) ? L"60" : number(pick->fps), d.found ? d.height : 0),
                      asked || !same ? capture_settings(*pick) : std::vector<SettingValue>{});
            if (!asked && !a.items.empty()) a.items.back().applied = true;  // automatic already picks it
        }
        // capture-sharper-30: the screen's own size is offered, but only
        // below 60 fps (HDR on many cards): a choice for 30 fps games.
        if (same && d.found && int(mode->height) < d.height) {
            const auto full = largest(in.modes, [&](const VideoMode &m) {
                return (s.hdr ? hdr_format(m) : sdr_format(m)) && wide16x9(m) && int(m.height) <= d.height &&
                       m.height > mode->height && m.fps >= 29.0;
            });
            if (full && !rate60(*full))
                recommend("capture-sharper-30",
                          card + L" has " + mode_text(*full) + (s.hdr ? L" in HDR" : L"") +
                              L": sharper on this screen for 30 fps games (a 60 fps game would lose half its "
                              L"frames).",
                          capture_settings(*full));
        }
        // capture-120: a 120 fps mode for a 120 Hz screen.
        if (d.found && d.refresh_hz >= 118 && mode->fps < 100) {
            const auto fast = largest(in.modes, [&](const VideoMode &m) {
                return (s.hdr ? hdr_format(m) : sdr_format(m)) && wide16x9(m) && m.fps >= 118 && m.fps <= 122 &&
                       int(m.height) <= d.height;
            });
            if (fast)
                recommend("capture-120",
                          card + L" can capture " + mode_text(*fast) + (s.hdr ? L" in HDR" : L"") +
                              L", for a console set to 120 Hz. Games at 60 fps or less gain nothing from it.",
                          capture_settings(*fast));
        }
        // raw-data-rate: a lot of bytes through the pipe.
        if (o.raw_gb_per_s >= 1.0)
            limit("raw-data-rate", Sev::Info,
                  std::format(L"Raw {} frames are {:.1f} GB/s through the pipe to mpv. If frames are lost "
                              L"(logs\\glasir.log says \"mpv fell behind\"), capture smaller or slower.",
                              mode_text(*mode), o.raw_gb_per_s));
    }

    // ------------------------------------------------------------ frame rates
    const double capture_fps = mode ? mode->fps : 60.0;
    const double refresh = d.found ? d.refresh_hz : 0;
    // Variable refresh as the add-ons are told it (glasir.ini's display.vrr,
    // else as found): frame generation's choices below go by the same word.
    const int vrr_user = s.vrr == Settings::Vrr::On ? 1 : s.vrr == Settings::Vrr::Off ? 0 : -1;
    o.vrr = vrr_for_addons(d.found ? d.vrr : Vrr::Unknown, vrr_user);
    const fg::pacing::Vrr pacing_vrr = o.vrr == L"on"    ? fg::pacing::Vrr::On
                                       : o.vrr == L"off" ? fg::pacing::Vrr::Off
                                                         : fg::pacing::Vrr::Unknown;
    // As the frame generation add-on decides (src/framegen/fg_pacing.h, asked
    // here with the same numbers): the most frames per game frame that fit
    // the screen's rate (with 5 % to spare) or framegen.max_output_hz (kept
    // exactly, when it is below the screen's rate), up to framegen.multiplier;
    // on a screen known to refresh at a fixed rate, a smaller multiplier that
    // falls on whole refreshes before a larger one that does not.
    const fg::pacing::Limit output_limit = fg::pacing::output_limit(refresh, s.framegen_max_hz);
    o.max_shown_hz = refresh > 0 ? output_limit.hz : 0;
    o.capped = refresh > 0 && output_limit.exact;
    o.multiplier_wanted = std::clamp(s.framegen_multiplier, 2, fg::pacing::kMaxMultiplier);
    o.framegen = s.framegen && s.dedup && in.reshade_installed && o.media_foundation;
    for (double game : {30.0, 60.0, 120.0}) {
        if (game > capture_fps + 1 && game != 30.0) continue;
        ShownRate r;
        r.game_fps = std::min(game, capture_fps);
        if (o.framegen && o.max_shown_hz > 0) {
            fg::pacing::MultiplierGate gate;  // a fresh one: no memory of another rate
            r.multiplier = gate.choose(0, r.game_fps, o.multiplier_wanted, output_limit, pacing_vrr, refresh);
            for (int k = r.multiplier + 1; k <= o.multiplier_wanted; ++k)
                r.kept_even = r.kept_even || fg::pacing::fits(k * r.game_fps, output_limit, false);
        }
        if (r.multiplier > 1) {
            r.shown_fps = r.multiplier * r.game_fps;
            const fg::pacing::Verdict v = fg::pacing::judge_cadence(r.shown_fps, refresh, pacing_vrr);
            r.cadence = v.cadence;
            r.better_hz = v.better_hz;
        } else {
            r.shown_fps = refresh > 0 ? std::min(r.game_fps, refresh) : r.game_fps;
        }
        if (o.shown.empty() || o.shown.back().game_fps != r.game_fps) o.shown.push_back(r);
    }
    if (s.framegen && !s.dedup)
        // framegen-needs-dedup
        limit("framegen-needs-dedup", Sev::Warning,
              L"Frame generation is on but frames.dedup is off: it stays off, as it needs to know which frames are "
              L"new.",
              {{L"frames", L"dedup", L"1"}});
    if (s.framegen && !in.reshade_installed)
        // framegen-no-reshade
        limit("framegen-no-reshade", Sev::Warning,
              L"ReShade is not downloaded: no frame generation and no DLSS 5. \"Glasir.exe --setup\" gets it.");
    if (o.framegen && d.found && refresh <= 0)
        // refresh-unknown
        limit("refresh-unknown", Sev::Info,
              L"The screen's refresh rate could not be read: frame generation finds it while playing.");
    if (s.framegen && s.framegen_max_hz > 0 && refresh > 0 && !o.capped)
        // framegen-cap-no-effect: a limit that is not below the screen's rate.
        limit("framegen-cap-no-effect", Sev::Info,
              std::format(L"framegen.max_output_hz = {} is not below this screen's {:.0f} Hz, so it changes nothing. "
                          L"0 means the same.",
                          s.framegen_max_hz, refresh),
              {{L"framegen", L"max_output_hz", L"0"}});
    if (o.framegen && o.max_shown_hz > 0) {
        const std::wstring hz = std::format(L"{:.0f} Hz", refresh);
        const std::wstring why = o.capped ? std::format(L"framegen.max_output_hz is {}", s.framegen_max_hz)
                                          : L"The screen is " + hz;
        const ShownRate *thirty = nullptr;
        bool none = true, sixty = true;  // sixty: a 60 fps game gets frames added (or cannot be captured)
        for (const ShownRate &r : o.shown) {
            if (r.multiplier > 1) none = false;
            else if (std::fabs(r.game_fps - 60) < 1.5) sixty = false;
            if (r.game_fps < 31) thirty = &r;
        }
        if (none)
            // framegen-nothing-doubled: not even 30 fps fits twice.
            limit("framegen-nothing-doubled", Sev::Warning,
                  why + L": frame generation has no room to add a frame to anything (a 30 fps game needs 60 Hz).");
        else if (!sixty)
            // screen-60hz: 60 fps games get no frames added.
            limit("screen-60hz", Sev::Info,
                  why + L": 60 fps games get no frames added (that needs 120 Hz); 30 fps games are shown at " +
                      number(thirty ? thirty->shown_fps : 60.0) + L".");
        std::wstring fewer, uneven, ceiling;
        int better_hz = 0;
        for (const ShownRate &r : o.shown) {
            if (r.multiplier < 2) continue;
            const std::wstring rate = L"a " + number(r.game_fps) + L" fps game";
            if (r.multiplier < o.multiplier_wanted)
                fewer += (fewer.empty() ? L"" : L"; ") + rate +
                         std::format(L" gets x{} ({} fps), as ", r.multiplier, number(r.shown_fps)) +
                         (r.kept_even ? L"more would not fall on whole refreshes of this fixed " + hz + L" screen"
                          : o.capped  ? std::format(L"more is over framegen.max_output_hz = {}", s.framegen_max_hz)
                                      : L"more is over what this " + hz + L" screen shows");
            if (r.cadence == FramegenStatus::Cadence::FixedUneven) {
                uneven += (uneven.empty() ? L"" : L", ") + rate + L" at " + number(r.shown_fps) + L" fps";
                if (r.better_hz > 0 && (better_hz == 0 || r.better_hz == better_hz)) better_hz = r.better_hz;
                else better_hz = -1;  // no one rate suits them all
            }
            if (r.cadence == FramegenStatus::Cadence::VrrCeiling)
                ceiling += (ceiling.empty() ? L"" : L", ") + rate + L" at " + number(r.shown_fps) + L" fps";
        }
        if (!fewer.empty()) {
            // framegen-fewer-fit: more than x2 was asked for, and less is used.
            fewer[0] = L'A';
            limit("framegen-fewer-fit", Sev::Info,
                  std::format(L"Up to x{} is asked for (framegen.multiplier). ", o.multiplier_wanted) + fewer + L".");
        }
        if (!uneven.empty()) {
            // framegen-uneven: a fixed refresh rate the output does not divide.
            uneven[0] = L'A';
            limit("framegen-uneven", Sev::Info,
                  uneven + L" does not divide this screen's fixed " + hz +
                      L": some frames stay a refresh longer than others. " +
                      (better_hz > 0 ? std::format(L"Setting the screen to {} Hz, or variable refresh, makes it even.",
                                                   better_hz)
                                     : std::wstring(L"Variable refresh makes it even.")));
        }
        if (!ceiling.empty()) {
            // framegen-vrr-ceiling: the output fills the screen's rate.
            ceiling[0] = L'A';
            limit("framegen-vrr-ceiling", Sev::Info,
                  ceiling + L" fills this " + hz +
                      L" screen: variable refresh has no room left there, so frames follow the screen's refreshes.");
        }
    }

    // ------------------------------------------------------------ variable refresh
    const bool nvidia_screen = d.gpu_vendor == kVendorNvidia;
    const std::wstring kind = d.vrr_kind.empty() ? L"variable refresh" : d.vrr_kind;
    const std::wstring range = d.vrr_min_hz > 0 && d.vrr_max_hz > 0
                                   ? L" (" + number(d.vrr_min_hz) + L"-" + number(d.vrr_max_hz) + L" Hz)"
                                   : L"";
    const std::wstring fixed = refresh > 0 ? L" Frames then wait for the screen's fixed " + number(refresh) +
                                                 L" Hz beat, so they are paced less evenly."
                                           : L" Frames are then paced less evenly.";
    if (vrr_user >= 0) {
        // vrr-override: the user said so; say when Glasir found otherwise.
        const bool differs = d.found && ((vrr_user == 1 && (d.vrr == Vrr::Off || d.vrr == Vrr::Unsupported)) ||
                                         (vrr_user == 0 && d.vrr == Vrr::On));
        if (differs)
            limit("vrr-override", Sev::Info,
                  std::wstring(L"display.vrr = ") + vrr_setting_name(s.vrr) + L" in glasir.ini; Glasir found it " +
                      vrr_name(d.vrr) + L" for this screen. \"auto\" follows what it finds.",
                  {{L"display", L"vrr", L"auto"}});
    } else if (d.found && d.vrr == Vrr::Off) {
        // vrr-off: the screen can, but it is switched off.
        limit("vrr-off", Sev::Warning,
              L"This screen has " + kind + range + L", but it is not switched on." + fixed +
                  (nvidia_screen ? L" Turn it on in the NVIDIA Control Panel (\"Set up G-SYNC\": pick this screen, "
                                   L"enable it for windowed and full screen mode), and in the monitor's own menu."
                                 : L" Turn it on in the GPU's control panel, and in the monitor's own menu."));
    } else if (d.found && d.vrr == Vrr::Unsupported) {
        // vrr-none: no VRR on this screen.
        limit("vrr-none", Sev::Info, L"No variable refresh (G-SYNC, FreeSync) was found on this screen." + fixed);
    } else if (d.found && d.vrr == Vrr::Supported) {
        // vrr-not-confirmed: the screen can; nothing says whether it is on.
        limit("vrr-not-confirmed", Sev::Info,
              L"This screen has " + kind + range + L", but Glasir could not read whether it is switched on with this "
              L"GPU. If it is, set display.vrr = on.");
    } else if (d.found && d.vrr == Vrr::Unknown) {
        // vrr-unknown
        limit("vrr-unknown", Sev::Info,
              L"Glasir could not tell whether this screen has variable refresh (G-SYNC, FreeSync). If it has and it "
              L"is on, set display.vrr = on; if not, display.vrr = off.");
    }
    if (d.found && d.vrr == Vrr::On && d.vrr_windowed == DisplayInfo::Tri::No && vrr_user != 0)
        // vrr-fullscreen-only: Glasir's full screen is a window.
        limit("vrr-fullscreen-only", Sev::Warning,
              L"G-SYNC is on for full screen mode only. Glasir's full screen is a window that covers the screen, so "
              L"it may not get it: in the NVIDIA Control Panel, \"Set up G-SYNC\", choose \"Enable for windowed and "
              L"full screen mode\".");

    // ------------------------------------------------------------ HDR
    if (s.hdr) {
        // What is done with it is hdr.h's to decide (mpv's options come from
        // the same plan).
        const HdrPlan &plan = o.hdr_plan = hdr_plan(s, d);
        o.hdr = plan.output == HdrPlan::Output::Hdr ? Outcome::Hdr::Hdr : Outcome::Hdr::ToneMapped;
        if (plan.forced_without_hdr && d.found)
            // hdr-forced: HDR sent to a screen that is not in HDR.
            limit("hdr-forced", Sev::Warning,
                  std::wstring(L"hdr.output = hdr, but ") +
                      (d.hdr_supported ? L"HDR is off in Windows for this screen" : L"this screen has no HDR") +
                      L": the picture will look washed out. \"auto\" tone maps it to SDR instead.");
        else if (plan.reason == HdrPlan::Reason::HdrOff)
            // hdr-off-in-windows
            limit("hdr-off-in-windows", Sev::Warning,
                  L"HDR is off in Windows for this screen: the HDR capture is tone mapped to SDR. Turn on \"Use "
                  L"HDR\" in Windows' display settings (Win+Alt+B) to see it in HDR.");
        else if (plan.reason == HdrPlan::Reason::NoHdr)
            // hdr-no-screen
            limit("hdr-no-screen", Sev::Info,
                  L"This screen has no HDR: the HDR capture is tone mapped to SDR.");
        if (plan.output == HdrPlan::Output::Hdr && s.hdr_fit_to_screen && !plan.fit)
            // hdr-fit-no-peak: nothing to fit to.
            limit("hdr-fit-no-peak", Sev::Warning,
                  L"hdr.fit_to_screen = 1, but this screen reports no peak brightness to Windows and "
                  L"hdr.display_peak_nits is 0: the picture goes out as the console sends it. Set "
                  L"hdr.display_peak_nits to the screen's peak.");
        else if (plan.fit && plan.peak_reported)
            // hdr-fit-reported-peak: fitted to the screen's own word.
            limit("hdr-fit-reported-peak", Sev::Info,
                  std::format(L"The picture is fitted to the {} nits this screen reports to Windows, which is often "
                              L"too low. If you know its real peak, set hdr.display_peak_nits.",
                              plan.peak_nits));
    }

    // ------------------------------------------------------------ the GPU
    o.gpu_class = gpu_class(gpu);
    if (in.gpu.setting_not_found)
        // gpu-setting-not-found
        limit("gpu-setting-not-found", Sev::Warning,
              L"display.gpu = " + s.gpu + L" names no GPU of this PC: mpv draws on " +
                  (gpu.found ? gpu.name : std::wstring(L"its own choice")) + L".");
    if (in.gpu.other_than_screen)
        // gpu-other-than-screen: frames cross from one GPU to the other.
        limit("gpu-other-than-screen", Sev::Warning,
              L"mpv draws on " + gpu.name + L", but the screen is plugged into " + in.gpu.screen_gpu.name +
                  L": every frame is copied across, which adds latency, and variable refresh may not hold. Plug the "
                  L"screen into the " + gpu.name + L" if you can.");

    const bool dlss5_possible = in.reshade_installed && in.dlss5_installed;
    o.dlss5 = dlss5_possible && s.dlss5 != Settings::Dlss5::Off && (s.dlss5 == Settings::Dlss5::On || dlss5_gpu(gpu));
    if (gpu.found && !dlss5_gpu(gpu) && s.dlss5 != Settings::Dlss5::Off) {
        // dlss5-gpu: not an RTX 30, 40 or 50.
        limit("dlss5-gpu", s.dlss5 == Settings::Dlss5::On ? Sev::Warning : Sev::Info,
              L"DLSS 5 needs an NVIDIA RTX 30, 40 or 50 series GPU; mpv draws on " + gpu.name +
                  (s.dlss5 == Settings::Dlss5::On ? L". It is tried anyway (dlss5.enabled = on)." : L"."));
        // gpu-has-rtx: another GPU of this PC could run it (a hybrid laptop).
        for (const GpuInfo &other : in.gpus)
            if (dlss5_gpu(other) && !in.gpu.by_setting) {
                recommend("gpu-has-rtx",
                          L"This PC also has " + other.name + L". display.gpu = " + other.name +
                              L" draws on it and gets DLSS 5, but every frame is then copied to the screen's GPU: "
                              L"some latency, and variable refresh may not hold. Glasir does not switch by itself.",
                          {{L"display", L"gpu", other.name}});
                break;
            }
    } else if (dlss5_gpu(gpu) && in.reshade_installed && !in.dlss5_installed && s.dlss5 != Settings::Dlss5::Off) {
        // dlss5-not-downloaded
        limit("dlss5-not-downloaded", Sev::Info,
              L"This GPU can run DLSS 5, but it is not downloaded: \"Glasir.exe --setup\" offers it.");
    }

    // ------------------------------------------------------------ recommendations
    // All three go by the GPU's class, which is a guess from its name and
    // memory (gpu.h): a starting point, not a measurement.
    const std::wstring guess = std::wstring(L" (") + gpu_class_name(o.gpu_class) + L"-class GPU, judged by its name "
                               L"and memory)";
    const std::wstring vsr = vsr_kind(gpu);
    {
        // rec-scaling
        const wchar_t *pick = L"mpv";
        std::wstring why;
        if (mode && !o.upscaled && o.picture_height > 0) {
            why = L"The capture already fills its place on the screen, so nothing is scaled up";
        } else if (o.gpu_class == GpuClass::High) {
            pick = L"cnn";
            why = L"ArtCNN rebuilds the most detail, and this GPU has room for it" + guess;
        } else if (o.gpu_class == GpuClass::Mid && vsr != L"none") {
            pick = L"vsr";
            why = L"The GPU's video super resolution runs in its video engine, beside DLSS 5 and frame generation" +
                  guess;
        } else {
            pick = L"fsr";
            why = L"FSR 1 sharpens edges for very little GPU time" + guess;
        }
        recommend("rec-scaling", std::wstring(L"Scaling: ") + pick + L". " + why + L".",
                  {{L"video", L"scaling", pick}});
        if (s.scaling == Settings::Scaling::Vsr && gpu.found && vsr == L"none")
            // vsr-unavailable
            limit("vsr-unavailable", Sev::Warning,
                  L"video.scaling = vsr needs an NVIDIA RTX or Intel GPU; on " + gpu.name + L" FSR 1 is used instead.",
                  {{L"video", L"scaling", pick}});
    }
    {
        // rec-framegen-quality: fast where the GPU is small, or shared with
        // DLSS 5 on a mid-class one.
        const bool fast = o.gpu_class == GpuClass::Low || o.gpu_class == GpuClass::Unknown ||
                          (o.gpu_class == GpuClass::Mid && o.dlss5);
        recommend("rec-framegen-quality",
                  std::wstring(L"Frame generation quality: ") + (fast ? L"fast" : L"high") + L". " +
                      (o.gpu_class == GpuClass::Unknown
                           ? L"The GPU could not be judged; fast takes about half the GPU time."
                           : fast ? (o.gpu_class == GpuClass::Mid
                                         ? L"DLSS 5 and frame generation share the GPU, and fast takes about half "
                                           L"the time" + guess + L"."
                                         : L"Fast takes about half the GPU time" + guess + L".")
                                  : L"This GPU has room for the cleaner one" + guess + L"."),
                  {{L"framegen", L"quality", fast ? L"fast" : L"high"}});
        // rec-framegen-multiplier: only said when more than x2 is asked of a
        // GPU that has little to spare; x3 and x4 stay the user's choice.
        if (s.framegen && o.multiplier_wanted > 2 &&
            (o.gpu_class == GpuClass::Low || (o.gpu_class == GpuClass::Mid && o.dlss5)))
            recommend("rec-framegen-multiplier",
                      std::format(L"Frame generation: up to x2. Every frame in between is a whole pass on the GPU, and "
                                  L"x{} makes {} of them per game frame",
                                  o.multiplier_wanted, o.multiplier_wanted - 1) +
                          guess + L".",
                      {{L"framegen", L"multiplier", L"2"}});
        // rec-framegen-method: blend only for a small GPU drawing more than 1440p.
        const bool blend = o.gpu_class == GpuClass::Low && d.found && INT64(d.width) * d.height > 2560 * 1440 + 1 &&
                           o.picture_height > 1440;
        recommend("rec-framegen-method",
                  std::wstring(L"Frame generation method: ") + (blend ? L"blend" : L"motion") + L". " +
                      (blend ? L"Following motion at this size is a lot for this GPU" + guess +
                                   L"; a plain mix is the lightest. Try motion with fast quality first."
                             : std::wstring(L"The frame in between follows what moves; blend (a plain mix) is only "
                                            L"for a GPU that cannot keep up.")),
                  {{L"framegen", L"method", blend ? L"blend" : L"motion"}});
    }

    // ------------------------------------------------------------ the console
    // What the console should be set to for the capture chosen above. Named
    // generically; a console's own name for a setting only where it is known.
    {
        auto console = [&](const char *id, std::wstring brief, std::wstring text) {
            add(id, AdviceKind::Console, Sev::Info, std::move(text));
            a.items.back().brief = std::move(brief);
        };
        if (mode) {
            // console-resolution: the size the card is asked for.
            const std::wstring lines = std::format(L"{}p", mode->height);
            console("console-resolution", lines,
                    L"Video output resolution: " + lines + L" (" + size_text(*mode) +
                        L"), the size Glasir captures. Another size is scaled by the console or the card first.");
        }
        // console-hdr: as video.mode.
        if (s.hdr)
            console("console-hdr", L"HDR on",
                    L"HDR: on (PS5: \"On When Supported\" or \"Always On\"; Xbox: \"Allow HDR10\"). Glasir captures "
                    L"in HDR (video.mode = hdr); an SDR signal would look wrong.");
        else
            console("console-hdr", L"HDR off",
                    L"HDR: off. Glasir captures in SDR (video.mode = sdr); an HDR signal would look grey and washed "
                    L"out.");
        if (mode) {
            // console-120hz: only for a capture mode of 120 fps.
            const bool fast = mode->fps >= 100;
            console("console-120hz", fast ? L"120 Hz output on" : L"120 Hz output off",
                    fast ? L"120 Hz output: on. The capture runs at " + number(mode->fps) + L" fps."
                         : L"120 Hz output: off. The capture runs at " + number(mode->fps) +
                               L" fps, so a faster signal would lose frames; frame generation adds frames on the PC.");
        }
        // console-vrr: the card needs a steady rate.
        console("console-vrr", L"VRR off",
                L"VRR: off. The capture card needs a steady rate; variable refresh belongs on the PC's screen, where "
                L"frame generation uses it.");
        // console-hdr-calibration: HDR passed through as the console sends it.
        if (s.hdr && o.hdr == Outcome::Hdr::Hdr && !o.hdr_plan.fit)
            console("console-hdr-calibration", L"",
                    L"Run the console's HDR calibration once while playing through Glasir (PS5: \"Adjust HDR\"; "
                    L"Xbox: \"Calibrate HDR for games\"): the picture goes to this screen as the console sends it, "
                    L"so that is what fits it to the screen.");
    }

    std::stable_sort(a.items.begin(), a.items.end(),
                     [](const AdviceItem &x, const AdviceItem &y) { return severity_rank(x) < severity_rank(y); });
    return a;
}

const AdviceItem *find_advice(const Advice &advice, const std::string &id) {
    for (const AdviceItem &i : advice.items)
        if (i.id == id) return &i;
    return nullptr;
}

std::wstring advice_text(const Advice &advice) {
    const Outcome &o = advice.outcome;
    std::wstring t = L"What will happen:\n";
    if (o.capture_known)
        t += std::format(L"  Capture: {} {}x{} at {} fps, {}\n", o.capture.subtype, o.capture.width, o.capture.height,
                         number(o.capture.fps),
                         o.media_foundation ? std::format(L"Media Foundation, {:.2f} GB/s raw", o.raw_gb_per_s)
                                            : std::wstring(L"DirectShow (no dedup or frame generation)"));
    else
        t += L"  Capture: no mode for these settings\n";
    t += L"  Shown:";
    for (size_t i = 0; i < o.shown.size(); ++i)
        t += std::format(L"{} a {} fps game at {} fps{}", i ? L"," : L"", number(o.shown[i].game_fps),
                         number(o.shown[i].shown_fps),
                         o.shown[i].multiplier > 1 ? std::format(L" (x{})", o.shown[i].multiplier) : std::wstring());
    t += o.framegen ? std::format(L"; frame generation up to x{}, at most {} frames a second{}\n",
                                  o.multiplier_wanted, number(o.max_shown_hz),
                                  o.capped ? L" (framegen.max_output_hz)" : L" (the screen's rate)")
                    : std::wstring(L"; frame generation off\n");
    t += L"  HDR: " + (o.hdr == Outcome::Hdr::SdrCapture ? std::wstring(L"an SDR capture") : hdr_plan_text(o.hdr_plan)) +
         L"\n";
    t += L"  Variable refresh, as frame generation is told: " + o.vrr + L"\n";
    t += std::wstring(L"  DLSS 5: ") + (o.dlss5 ? L"on" : L"off") + L"\n";
    if (o.picture_height > 0)
        t += std::format(L"  Picture: {}x{} on the screen{}\n", o.picture_width, o.picture_height,
                         o.upscaled ? L", scaled up from the capture" : L"");
    std::wstring limits, recommended, console;
    for (const AdviceItem &i : advice.items) {
        if (i.kind == AdviceKind::Console) {
            console += L"  " + i.text + L" (" + widen(i.id) + L")\n";
            continue;
        }
        std::wstring line = L"  ";
        if (i.kind == AdviceKind::Limit)
            line += i.severity == AdviceSeverity::Problem   ? L"[problem] "
                    : i.severity == AdviceSeverity::Warning ? L"[warning] "
                                                            : L"[info] ";
        line += i.text + L" (" + widen(i.id) + L")";
        if (!i.suggest.empty()) {
            line += L"\n      ";
            for (size_t k = 0; k < i.suggest.size(); ++k) line += (k ? L", " : L"") + setting_text(i.suggest[k]);
            if (i.applied) line += L" (already set)";
        }
        (i.kind == AdviceKind::Limit ? limits : recommended) += line + L"\n";
    }
    if (!limits.empty()) t += L"Limits:\n" + limits;
    if (!recommended.empty()) t += L"Recommended:\n" + recommended;
    if (!console.empty()) t += L"Set your console to:\n" + console;
    return t;
}

}  // namespace glasir
