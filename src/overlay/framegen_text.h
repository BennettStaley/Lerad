// SPDX-License-Identifier: MIT
// The menu's words for frame generation's numbers (framegen_status.h): the
// rates, the screen, why fewer frames are added than asked for, and what the
// screen does to their pacing. Text only, so the tests can read it
// (tests/test_fg_pacing.cpp).
#pragma once

#include <cmath>
#include <cstdio>
#include <string>

#include "framegen_status.h"

namespace glasir::overlay {

#define GLASIR_ARROW "\xE2\x86\x92"  // a right arrow, UTF-8

// The frames shown per second: real and generated.
inline double shown_fps(const FramegenStatus &s) { return s.real_fps + s.generated_fps; }

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
        // No link from Glasir's capture (the DirectShow fallback): only what is shown is known.
        if (s.real_fps >= 1) std::snprintf(text, sizeof(text), "%.0f fps", s.real_fps);
        return text;
    }
    // While frame generation is off mpv gets the repeats too: what is shown
    // then is the same pictures, so only the game's rate is given.
    if (s.enabled && s.generated_fps >= 1)
        std::snprintf(text, sizeof(text), "%s " GLASIR_ARROW " %.0f fps", source, shown_fps(s));
    else
        std::snprintf(text, sizeof(text), "%s fps", source);
    return text;
}

// "120 Hz VRR", "144 Hz fixed", "240 Hz" (VRR not known). "" when unknown.
inline std::string screen_text(const FramegenStatus &s) {
    if (s.display_hz <= 0) return {};
    char text[48];
    std::snprintf(text, sizeof(text), "%.0f Hz%s", s.display_hz,
                  s.vrr == FramegenStatus::Vrr::On ? " VRR" : s.vrr == FramegenStatus::Vrr::Off ? " fixed" : "");
    return text;
}

struct Line {
    std::string text;      // "" = nothing to say
    bool matters = false;  // shown in the menu's warning colour
};

// Why fewer frames are added than asked for.
inline Line reason_text(const FramegenStatus &s) {
    using Reason = FramegenStatus::Reason;
    char text[320] = "";
    bool matters = false;
    const double source = s.source_fps, shown = shown_fps(s), meant = s.multiplier * s.source_fps;
    switch (s.reason) {
    case Reason::None: break;
    case Reason::Starting: std::snprintf(text, sizeof(text), "Measuring the game's frame rate."); break;
    case Reason::TooFast:
        if (s.cap_hz > 0)
            std::snprintf(text, sizeof(text), "No frames added: %.0f fps doubled is over the limit of %d (max_output_hz).",
                          source, s.cap_hz);
        else
            std::snprintf(text, sizeof(text), "No frames added: %.0f fps doubled is more than this %.0f Hz screen shows.",
                          source, s.display_hz);
        break;
    case Reason::FewerFit:
        if (s.cap_hz > 0)
            std::snprintf(text, sizeof(text), "x%d, not x%d: more would be over the limit of %d (max_output_hz).",
                          s.multiplier, s.wanted, s.cap_hz);
        else
            std::snprintf(text, sizeof(text), "x%d, not x%d: more would be over what this %.0f Hz screen shows.",
                          s.multiplier, s.wanted, s.display_hz);
        break;
    case Reason::SourcePaused: std::snprintf(text, sizeof(text), "The picture is still: nothing to add."); break;
    case Reason::BackingOff:
        std::snprintf(text, sizeof(text),
                      "Paused, back in %.0f s: the GPU could not keep up (mpv dropped frames). Fast quality, a "
                      "smaller multiplier or a lighter DLSS 5 setting leaves it more room.",
                      std::ceil(s.seconds_left));
        matters = true;
        break;
    case Reason::Settling: std::snprintf(text, sizeof(text), "Waiting a moment: mpv dropped frames."); break;
    case Reason::EffectsMix:
        std::snprintf(text, sizeof(text), "DLSS 5 is starting or switching: frames are added again in a moment.");
        break;
    case Reason::QueueFull:
        std::snprintf(text, sizeof(text),
                      "%.0f of %.0f fps: the screen takes frames late (%d left out in the last second). Windows may "
                      "be composing the window: use full screen, with \"Skip the black bars\" off.",
                      shown, meant, s.reason_count);
        matters = true;
        break;
    case Reason::Drops:
        std::snprintf(text, sizeof(text),
                      "%.0f of %.0f fps: %d of the game's frames were dropped in the last second. The GPU is at its "
                      "limit.",
                      shown, meant, s.reason_count);
        matters = true;
        break;
    case Reason::Late:
        std::snprintf(text, sizeof(text), "%.0f of %.0f fps: the game's frames came late %d times in the last second.",
                      shown, meant, s.reason_count);
        break;
    }
    return {text, matters};
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
    // What the screen was seen doing settles what Glasir could not find out.
    if (cadence == Cadence::MaybeEven && s.seen != Seen::Following) return {};
    if (cadence == Cadence::MaybeUneven && s.seen == Seen::Following) return {};
    if (cadence == Cadence::MaybeUneven && s.seen == Seen::Fixed) cadence = Cadence::FixedUneven;
    switch (cadence) {
    case Cadence::VrrEven:
        if (s.seen != Seen::Fixed) return {};
        std::snprintf(text, sizeof(text),
                      "VRR (G-SYNC, FreeSync) is not engaging: the screen keeps refreshing at %.0f Hz. Click into "
                      "Glasir, and turn VRR on for windowed mode in the GPU's control panel.",
                      s.seen_hz);
        return {text, true};
    case Cadence::VrrCeiling:
        std::snprintf(text, sizeof(text),
                      "%.0f fps fills this %.0f Hz screen: VRR has no room left, so frames follow its refreshes.",
                      output, s.display_hz);
        return {text, false};
    case Cadence::FixedUneven:
        if (s.better_hz > 0)
            std::snprintf(text, sizeof(text),
                          "Uneven on this screen: %.0f fps does not divide %.0f Hz. Set the screen to %d Hz, or turn "
                          "VRR on.",
                          output, s.display_hz, s.better_hz);
        else
            std::snprintf(text, sizeof(text), "Uneven on this screen: %.0f fps does not divide %.0f Hz. VRR would fix it.",
                          output, s.display_hz);
        return {text, true};
    case Cadence::MaybeUneven:
        if (s.better_hz > 0)
            std::snprintf(text, sizeof(text),
                          "%.0f fps does not divide this %.0f Hz screen: even only with VRR on. Without VRR, set "
                          "the screen to %d Hz.",
                          output, s.display_hz, s.better_hz);
        else
            std::snprintf(text, sizeof(text), "%.0f fps does not divide this %.0f Hz screen: even only with VRR on.",
                          output, s.display_hz);
        return {text, false};
    default: return {};
    }
}

}  // namespace glasir::overlay
