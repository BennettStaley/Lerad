// SPDX-License-Identifier: MIT
// What this PC will really do with the settings, what holds it back, and
// what would suit it: from the screen, the GPUs, the capture card's modes
// and glasir.ini. No UI and nothing read from the PC here: advise() only
// works on what it is given, so it can be tested and called from anywhere
// (the launcher, the in-game menu, --system, bug reports).
//
// For a UI:
//   AdviceInput in;                      // system_report.h's advice_input() fills one
//   const Advice a = advise(in);
//   a.outcome                            // facts to show: "60 -> 120 fps", "HDR tone mapped"
//   for (const AdviceItem &i : a.items)  // in the order to show them
//       i.text;                          // one or two plain sentences
//       i.suggest;                       // not empty: offer "use recommended", then
//                                        // save_settings(path, i.suggest)
//       i.applied;                       // glasir.ini already has i.suggest: show it as done
// An item's id never changes between versions (a UI may hide, group or
// replace the text of items it knows by id); the text may.
#pragma once
#include <string>
#include <vector>

#include "devices.h"
#include "display.h"
#include "framegen_status.h"
#include "gpu.h"
#include "hdr.h"
#include "settings.h"

namespace glasir {

struct AdviceInput {
    Settings settings;

    // The screen Glasir plays on (choose_display); found = false: unknown.
    DisplayInfo display;
    bool monitor_matched = true;  // display.monitor named a screen that is there

    std::vector<GpuInfo> gpus;  // all of them (list_gpus)
    GpuChoice gpu;              // the one mpv draws on (choose_gpu)

    // The capture card that will be used and its modes (CaptureDevice). No
    // card: card_found = false. A card only DirectShow sees has no modes.
    bool card_found = false;
    std::wstring card;
    std::vector<VideoMode> modes;

    // What is downloaded (components.h).
    bool reshade_installed = true;  // without it: no frame generation, no DLSS 5
    bool dlss5_installed = true;
};

enum class AdviceSeverity {
    Info,     // good to know; nothing is wrong
    Warning,  // works, but worse than it could: worth changing
    Problem,  // will not play, or not what was asked for, as it is set
};

enum class AdviceKind {
    Limit,           // something the hardware or a setting holds back, with the reason
    Recommendation,  // a setting that suits this PC, with a one-line why
    // What to set on the console for these choices. The console only knows
    // what the capture card's HDMI input says it takes, not what Glasir will
    // do with the picture, so Glasir says it in words. Nothing of it can be
    // set from here; the ids start with "console-".
    Console,
};

struct AdviceItem {
    std::string id;  // stable, e.g. "screen-60hz" (the list is in advice.cpp, above each item)
    AdviceKind kind = AdviceKind::Limit;
    AdviceSeverity severity = AdviceSeverity::Info;
    std::wstring text;  // plain English, one or two sentences, with a full stop
    // The glasir.ini values that follow the advice ("use recommended"); empty
    // when there is nothing to set (a limit of the hardware, or a switch in
    // Windows or the GPU's control panel).
    std::vector<SettingValue> suggest;
    bool applied = false;  // suggest is what the settings already say
    // Console items: the setting in two or three words ("2160p", "HDR on",
    // "VRR off"), for a UI that lists them on a line. Empty for a hint that
    // is not a setting (the HDR calibration).
    std::wstring brief;
};

// One game frame rate and what the screen shows for it, as the frame
// generation add-on will decide it (src/framegen/fg_pacing.h: the same
// functions are asked here).
struct ShownRate {
    double game_fps = 0;   // what the console renders: 30, 60, 120 (dedup finds it while playing)
    double shown_fps = 0;  // what the screen shows
    // Frames shown per game frame: 1, none are added; 2, 3 or 4 by frame
    // generation (the most that fits, up to framegen.multiplier).
    int multiplier = 1;
    // multiplier < the setting: a larger one would fit the screen's rate, but
    // not on whole refreshes of a screen known to refresh at a fixed rate.
    bool kept_even = false;
    // How evenly the frames can reach this screen (Unknown when none are
    // added), and for an uneven one a refresh rate that would make it even.
    FramegenStatus::Cadence cadence = FramegenStatus::Cadence::Unknown;
    int better_hz = 0;
};

// What video.mode comes to with a card's modes, as Glasir.exe settles it when
// it plays (main.cpp open_source). sdr and hdr are as said. auto captures in
// the card's 10-bit mode for the size and rate asked, if it has one, and
// starts as what the signal was last time (video.auto_found); without such a
// mode it is SDR.
struct VideoKind {
    bool ten_bit = false;  // the capture is one of the card's 10-bit (P010) modes
    bool hdr = false;      // the picture is shown as HDR (to start with, for auto)
};
VideoKind video_kind(const Settings &s, const std::vector<VideoMode> &modes, UINT32 max_auto_height);

struct Outcome {
    // SDR or HDR, with video.mode = auto settled for this card.
    VideoKind video;
    // The capture mode Glasir will open, if the card has one for the settings.
    bool capture_known = false;
    VideoMode capture;
    bool media_foundation = false;  // false: mpv's DirectShow input (no dedup, no frame generation)
    double raw_gb_per_s = 0;        // Media Foundation: raw frames through the pipe to mpv

    // Frame generation will run (it adds frames only to the rates marked in shown).
    bool framegen = false;
    int multiplier_wanted = 2;  // framegen.multiplier: at most this many frames per game frame
    // The most frames a second that are shown: the screen's refresh rate, or
    // framegen.max_output_hz when that is below it (capped). 0: the screen's
    // rate is not known (frame generation finds it while playing).
    double max_shown_hz = 0;
    bool capped = false;
    std::vector<ShownRate> shown;  // for the game rates this capture rate can carry

    // An HDR capture: sent to the screen as HDR, or tone mapped to SDR.
    enum class Hdr { SdrCapture, Hdr, ToneMapped };
    Hdr hdr = Hdr::SdrCapture;
    // An HDR capture: all of what hdr.h decides (why, fitted to which peak);
    // hdr_plan_text() says it in a line.
    HdrPlan hdr_plan;

    // Variable refresh as the add-ons are told: "on", "off" or "unknown"
    // (vrr_for_addons: glasir.ini's display.vrr, else as found).
    std::wstring vrr = L"unknown";

    bool dlss5 = false;        // DLSS 5 starts on
    bool upscaled = false;     // the capture is smaller than its place on the screen
    int picture_width = 0, picture_height = 0;  // that place (full screen), 0: unknown
    GpuClass gpu_class = GpuClass::Unknown;     // of the GPU mpv draws on: a guess (gpu.h)
};

struct Advice {
    Outcome outcome;
    std::vector<AdviceItem> items;  // problems first, then warnings, limits, recommendations, the console's settings
};

Advice advise(const AdviceInput &in);

// The item with this id, or null.
const AdviceItem *find_advice(const Advice &advice, const std::string &id);

// The whole advice as text lines (--system, the log, bug reports).
std::wstring advice_text(const Advice &advice);

// "section.key=value", as --set takes it.
std::wstring setting_text(const SettingValue &v);

}  // namespace glasir
