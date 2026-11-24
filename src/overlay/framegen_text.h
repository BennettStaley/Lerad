// SPDX-License-Identifier: MIT
// The menu's words for frame generation's numbers (framegen_status.h): the
// rates, the screen, why fewer frames are added than asked for, and what the
// screen does to their pacing. Text only, so the tests can read it
// (tests/test_fg_pacing.cpp).
//
// The words follow the launcher's: one name for a thing ("variable refresh"
// in sentences; the readout's short "120 Hz VRR" is a figure), no names of
// files or keys, and a message says what happens, then why, then what to do.
#pragma once

#include <cmath>
#include <cstdio>
#include <string>

#include "app_version.h"
#include "framegen_status.h"

namespace app::overlay {

#define APP_ARROW "\xE2\x86\x92"  // a right arrow, UTF-8

// The frames shown per second: real and generated.
inline double shown_fps(const FramegenStatus &s) { return s.real_fps + s.generated_fps; }

// "9.4 ms": the delay from the card's frame to its present on the PC; and,
// when it is known, half the spread of the card's frame times: "9.4 ms
// \xC2\xB1" "0.1". "" while the delay is not known.
inline std::string delay_text(const FramegenStatus &s) {
    if (s.latency_ms <= 0) return {};
    char text[48];
    if (s.arrival_spread_ms >= 0)
        std::snprintf(text, sizeof(text), "%.1f ms \xC2\xB1%.1f", s.latency_ms, s.arrival_spread_ms / 2);
    else
        std::snprintf(text, sizeof(text), "%.1f ms", s.latency_ms);
    return text;
}

// The same after the rates: ", 9.4 ms".
inline std::string latency_text(const FramegenStatus &s) {
    const std::string delay = delay_text(s);
    return delay.empty() ? delay : ", " + delay;
}

// "30 -> 60 fps": what the card delivers and what is shown. The source alone
// when nothing is added; "45-58 -> 97 fps" for a source whose rate moves. ""
// while nothing is known.
inline std::string rates_text(const FramegenStatus &s) {
    char source[32] = "", text[80] = "";
    if (s.source_variable)
        std::snprintf(source, sizeof(source), "%.0f-%.0f", s.source_low, s.source_high);
    else if (s.source_fps >= 1)
        std::snprintf(source, sizeof(source), "%.0f", s.source_fps);
    if (!source[0]) {
        // No link from the app's capture (the card in basic mode): only what is shown is known.
        if (s.real_fps >= 1) std::snprintf(text, sizeof(text), "%.0f fps", s.real_fps);
        return text;
    }
    // While frame generation is off mpv gets the repeats too: what is shown
    // then is the same pictures, so only the game's rate is given.
    if (s.enabled && s.generated_fps >= 1)
        std::snprintf(text, sizeof(text), "%s " APP_ARROW " %.0f fps", source, shown_fps(s));
    else
        std::snprintf(text, sizeof(text), "%s fps", source);
    return text;
}

// With the delay after it: "30 -> 120 fps, 14.2 ms +-0.1".
inline std::string rates_and_latency_text(const FramegenStatus &s) {
    const std::string rates = rates_text(s);
    return rates.empty() ? rates : rates + latency_text(s);
}

// "120 Hz VRR", "144 Hz fixed", "240 Hz" (variable refresh not known). ""
// when unknown.
inline std::string screen_text(const FramegenStatus &s) {
    if (s.display_hz <= 0) return {};
    char text[48];
    std::snprintf(text, sizeof(text), "%.0f Hz%s", s.display_hz,
                  s.vrr == FramegenStatus::Vrr::On ? " VRR" : s.vrr == FramegenStatus::Vrr::Off ? " fixed" : "");
    return text;
}

// "HDR", "SDR", or "HDR to SDR" for an HDR signal that is tone mapped to an
// SDR output: what the menu's readout says about the picture.
inline const char *picture_text(bool source_hdr, bool output_hdr) {
    return !source_hdr ? "SDR" : output_hdr ? "HDR" : "HDR to SDR";
}

// The screen the window is on, in a line: "1 \xC2\xB7 PA279CDV \xC2\xB7 120 Hz".
// name: the exe's name for it ("1 (PA279CDV)", "2") or Windows' for a screen
// it did not see ("\\.\DISPLAY3"); hz: its refresh rate, 0 unknown. "" when
// neither is known.
inline std::string screen_line(const std::string &name, double hz) {
    std::string text = name;
    // "1 (PA279CDV)" is written as the launcher writes it: "1 \xC2\xB7 PA279CDV".
    const size_t open = text.find(" (");
    if (open != std::string::npos && !text.empty() && text.back() == ')')
        text = text.substr(0, open) + " \xC2\xB7 " + text.substr(open + 2, text.size() - open - 3);
    if (hz > 0) {
        char rate[32];
        std::snprintf(rate, sizeof(rate), "%s%.0f Hz", text.empty() ? "" : " \xC2\xB7 ", hz);
        text += rate;
    }
    return text;
}

// What frame generation takes that screen's variable refresh to be, which
// is what it paces by, and where to change it when that is wrong. told: the
// exe described this screen at start (false: one plugged in or switched on
// since); set: the state is what the user chose in the launcher, not what
// was found.
inline std::string vrr_line(FramegenStatus::Vrr vrr, bool told, bool set) {
    if (!told) return "Variable refresh not known: this screen was not there when " APP_NAME " started.";
    if (vrr == FramegenStatus::Vrr::Unknown)
        return "Variable refresh not known. Say on the Screen tab of " APP_NAME "'s window whether it is on.";
    const std::string state = vrr == FramegenStatus::Vrr::On ? "Variable refresh on" : "Variable refresh off";
    return set ? state + ", as set on the Screen tab of " APP_NAME "'s window."
               : state + ", as found. Change it on the Screen tab of " APP_NAME "'s window if that is wrong.";
}

// The exe's word for a screen's variable refresh (env DISPLAY_VRR): "on",
// "off", anything else unknown.
inline FramegenStatus::Vrr vrr_from_word(const std::string &word) {
    return word == "on" ? FramegenStatus::Vrr::On : word == "off" ? FramegenStatus::Vrr::Off : FramegenStatus::Vrr::Unknown;
}

// Something the menu says about what frame generation is doing.
struct Line {
    // What happens, in a few words, for a warning's first line ("90 of 120
    // fps"). "" when the text says it all.
    std::string title;
    std::string text;      // why, and what to do. "" with no title: nothing to say
    bool matters = false;  // a warning (shown as one, with its sign and word); else a quiet line
    // What the menu's button can do about it.
    enum class Fix { None, FullScreen, FastQuality };
    Fix fix = Fix::None;
    bool empty() const { return title.empty() && text.empty(); }
};

// Why fewer frames are added than asked for.
inline Line reason_text(const FramegenStatus &s) {
    using Reason = FramegenStatus::Reason;
    char title[96] = "", text[320] = "";
    Line line;
    const double source = s.source_fps, shown = shown_fps(s), meant = s.multiplier * s.source_fps;
    switch (s.reason) {
    case Reason::None: break;
    case Reason::Starting: std::snprintf(text, sizeof(text), "Measuring the game's frame rate."); break;
    case Reason::TooFast:
        if (s.cap_hz > 0)
            std::snprintf(text, sizeof(text), "No frames added: %.0f fps doubled is over the limit of %d fps.", source,
                          s.cap_hz);
        else
            std::snprintf(text, sizeof(text), "No frames added: %.0f fps doubled is more than this %.0f Hz screen shows.",
                          source, s.display_hz);
        break;
    case Reason::FewerFit:
        if (s.cap_hz > 0)
            std::snprintf(text, sizeof(text), "x%d, not x%d: more would be over the limit of %d fps.", s.multiplier,
                          s.wanted, s.cap_hz);
        else
            std::snprintf(text, sizeof(text), "x%d, not x%d: more would be over what this %.0f Hz screen shows.",
                          s.multiplier, s.wanted, s.display_hz);
        break;
    case Reason::SteppedDown: {
        // "x3, not x4": what would fit could not be held there, so one that
        // can. When x3 was tried too and failed, both are named: "at x3
        // and x4".
        static const char *const kCount[] = {"no", "one", "two", "three"};
        const int in_use = s.multiplier < 2 ? 2 : s.multiplier > 3 ? 3 : s.multiplier;
        const int from = s.step_from <= in_use ? in_use + 1 : s.step_from > 4 ? 4 : s.step_from;
        const int failed = s.step_failed > in_use && s.step_failed < from ? s.step_failed : from;
        char at[24];
        if (failed < from)
            std::snprintf(at, sizeof(at), "x%d and x%d", failed, from);
        else
            std::snprintf(at, sizeof(at), "x%d", from);
        const bool gpu = s.step_cause == FramegenStatus::StepCause::Gpu;
        std::snprintf(title, sizeof(title), "x%d, not x%d", in_use, from);
        std::snprintf(text, sizeof(text), "%s at %s, so every game frame gets %s added frame%s instead of %s. %s",
                      gpu ? "The GPU cannot keep up" : "The screen takes frames late", at, kCount[in_use - 1],
                      in_use == 2 ? "" : "s", kCount[from - 1],
                      gpu ? "Fast quality or DLSS 5 off leaves it more room."
                          : "Windows may be composing the window.");
        line.matters = true;
        line.fix = gpu ? Line::Fix::FastQuality : Line::Fix::FullScreen;
        break;
    }
    case Reason::SourcePaused: std::snprintf(text, sizeof(text), "The picture is still: nothing to add."); break;
    case Reason::BackingOff:
        std::snprintf(title, sizeof(title), "Paused, back in %.0f s", std::ceil(s.seconds_left));
        std::snprintf(text, sizeof(text),
                      "The GPU could not keep up. Fast quality, a smaller Up to or DLSS 5 off leaves it more room.");
        line.matters = true;
        line.fix = Line::Fix::FastQuality;
        break;
    case Reason::Settling: std::snprintf(text, sizeof(text), "Waiting a moment: frames were dropped."); break;
    case Reason::EffectsMix:
        std::snprintf(text, sizeof(text), "DLSS 5 is starting or switching: frames are added again in a moment.");
        break;
    case Reason::QueueFull:
        std::snprintf(title, sizeof(title), "%.0f of %.0f fps", shown, meant);
        std::snprintf(text, sizeof(text),
                      "The screen takes frames late: %d left out in the last second. Windows may be composing the "
                      "window.",
                      s.reason_count);
        line.matters = true;
        line.fix = Line::Fix::FullScreen;
        break;
    case Reason::Drops:
        std::snprintf(title, sizeof(title), "%.0f of %.0f fps", shown, meant);
        std::snprintf(text, sizeof(text), "%d of the game's frames were dropped in the last second. The GPU is at its limit.",
                      s.reason_count);
        line.matters = true;
        line.fix = Line::Fix::FastQuality;
        break;
    case Reason::Late:
        std::snprintf(text, sizeof(text), "%.0f of %.0f fps: the game's frames came late %d times in the last second.",
                      shown, meant, s.reason_count);
        break;
    }
    line.title = title;
    line.text = text;
    return line;
}

// What this screen does to the pacing of the frames shown, when it is not
// simply even.
inline Line cadence_text(const FramegenStatus &s) {
    using Cadence = FramegenStatus::Cadence;
    using Seen = FramegenStatus::Seen;
    if (s.multiplier < 2 || s.cadence == Cadence::Unknown) return {};
    char text[320] = "";
    const double output = s.multiplier * s.source_fps;
    Cadence cadence = s.cadence;
    // What the screen was seen doing settles what the app could not find out.
    if (cadence == Cadence::MaybeEven && s.seen != Seen::Following) return {};
    if (cadence == Cadence::MaybeUneven && s.seen == Seen::Following) return {};
    if (cadence == Cadence::MaybeUneven && s.seen == Seen::Fixed) cadence = Cadence::FixedUneven;
    switch (cadence) {
    case Cadence::VrrEven:
        if (s.seen != Seen::Fixed) return {};
        std::snprintf(text, sizeof(text),
                      "The screen keeps refreshing at %.0f Hz. Click into the picture, and allow variable refresh for "
                      "windows in the GPU's control panel.",
                      s.seen_hz);
        return {"Variable refresh is not engaging", text, true};
    case Cadence::VrrCeiling:
        std::snprintf(text, sizeof(text),
                      "%.0f fps fills this %.0f Hz screen: variable refresh has no room left, so frames follow its "
                      "refreshes.",
                      output, s.display_hz);
        return {"", text, false};
    case Cadence::FixedUneven:
        if (s.better_hz > 0)
            std::snprintf(text, sizeof(text),
                          "%.0f fps does not divide %.0f Hz, so some frames stay longer than others. Set the screen "
                          "to %d Hz, or turn variable refresh on.",
                          output, s.display_hz, s.better_hz);
        else
            std::snprintf(text, sizeof(text),
                          "%.0f fps does not divide %.0f Hz, so some frames stay longer than others. Variable refresh "
                          "would fix it.",
                          output, s.display_hz);
        return {"Uneven on this screen", text, true};
    case Cadence::MaybeUneven:
        if (s.better_hz > 0)
            std::snprintf(text, sizeof(text),
                          "%.0f fps does not divide this %.0f Hz screen: even only with variable refresh on. Without "
                          "it, set the screen to %d Hz.",
                          output, s.display_hz, s.better_hz);
        else
            std::snprintf(text, sizeof(text),
                          "%.0f fps does not divide this %.0f Hz screen: even only with variable refresh on.", output,
                          s.display_hz);
        return {"", text, false};
    default: return {};
    }
}

}  // namespace app::overlay
