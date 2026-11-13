// SPDX-License-Identifier: MIT
// What frame generation tells the in-game menu about itself: the rates as
// they are now, the screen, and - when fewer frames are shown than asked for
// - why. Filled by the add-on (src/framegen), shown by the menu
// (src/overlay); plain data, so both and the tests can use it.
#pragma once

namespace glasir {

struct FramegenStatus {
    // Variable refresh rate on the PC's screen, as Glasir.exe found it
    // (GLASIR_DISPLAY_VRR) - not whether it is engaged this moment (it
    // follows the focused window).
    enum class Vrr { Unknown, On, Off };

    // Why fewer frames are added than the setting asks for. The first that
    // applies, in the order the add-on decides them.
    enum class Reason {
        None,          // nothing is short
        Starting,      // the source's rate is not known yet
        TooFast,       // even one frame in between is more than the screen shows
        FewerFit,      // a smaller multiplier than asked for is all that fits
        SourcePaused,  // a still picture: only keepalive repeats come
        BackingOff,    // mpv dropped frames while generating: paused for seconds_left
        Settling,      // mpv dropped frames on its own a moment ago
        EffectsMix,    // DLSS 5 ran for one frame and not the other (starting, switching)
        QueueFull,     // presents are waiting for the screen
        Drops,         // new frames dropped before mpv took them
        Late,          // the source's frames come late or unevenly
    };

    // How evenly the frames can reach this screen.
    enum class Cadence {
        Unknown,        // not generating, or the rates are not known
        VrrEven,        // inside the VRR range: each frame is shown when it is due
        VrrCeiling,     // the output fills the screen's refresh rate: VRR has no room
        FixedEven,      // fixed refresh, each frame a whole number of refreshes
        FixedUneven,    // fixed refresh, the output does not divide it
        MaybeEven,      // VRR unknown; even either way (the output divides the refresh rate)
        MaybeUneven,    // VRR unknown; even only if VRR is on
    };

    bool enabled = false;
    // Frames per second over about the last second.
    double source_fps = 0;     // new frames the card delivered
    double real_fps = 0;       // real frames presented (repeats too while frame generation is off)
    double generated_fps = 0;  // frames in between presented
    // The source's rate over the last few seconds, when it is not steady.
    double source_low = 0, source_high = 0;
    bool source_variable = false;  // the rate itself moves (a VRR or unlocked game)
    bool source_uneven = false;    // a steady rate in uneven steps (45 fps in a 60 Hz signal)

    double display_hz = 0;  // the screen's refresh rate, 0 unknown
    int cap_hz = 0;         // [framegen] max_output_hz when set, else 0
    Vrr vrr = Vrr::Unknown;
    // What the screen was seen doing while frames were added (the swap
    // chain's refresh counter): Fixed = it kept refreshing at display_hz
    // whatever was presented, so VRR is not engaging (off for windows in the
    // driver, the window not focused, or composed by Windows); Following =
    // its refreshes follow the frames.
    enum class Seen { Unknown, Fixed, Following };
    Seen seen = Seen::Unknown;
    double seen_hz = 0;  // refreshes per second seen, 0 unknown

    int wanted = 2;      // the multiplier asked for (2, 3, 4)
    int multiplier = 1;  // the one in use: 1 = no frames added
    Reason reason = Reason::None;
    double seconds_left = 0;  // BackingOff: until it tries again
    int reason_count = 0;     // QueueFull, Drops, Late: how many in the last second

    Cadence cadence = Cadence::Unknown;
    int better_hz = 0;  // FixedUneven / MaybeUneven: a refresh rate the output would divide, 0 none
};

}  // namespace glasir
