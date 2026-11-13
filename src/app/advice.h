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
#include "gpu.h"
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
};

// One game frame rate and what the screen shows for it.
struct ShownRate {
    double game_fps = 0;   // what the console renders: 30, 60, 120 (dedup finds it while playing)
    double shown_fps = 0;  // what the screen shows
    bool doubled = false;  // by frame generation
};

struct Outcome {
    // The capture mode Glasir will open, if the card has one for the settings.
    bool capture_known = false;
    VideoMode capture;
    bool media_foundation = false;  // false: mpv's DirectShow input (no dedup, no frame generation)
    double raw_gb_per_s = 0;        // Media Foundation: raw frames through the pipe to mpv

    // Frame generation will run (it doubles only the rates marked in shown).
    bool framegen = false;
    double max_shown_hz = 0;       // the most the screen shows (the refresh rate, or framegen.max_output_hz)
    std::vector<ShownRate> shown;  // for the game rates this capture rate can carry

    // An HDR capture: sent to the screen as HDR, or tone mapped to SDR.
    enum class Hdr { SdrCapture, Hdr, ToneMapped };
    Hdr hdr = Hdr::SdrCapture;

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
    std::vector<AdviceItem> items;  // problems first, then warnings, limits, recommendations
};

Advice advise(const AdviceInput &in);

// The item with this id, or null.
const AdviceItem *find_advice(const Advice &advice, const std::string &id);

// The whole advice as text lines (--system, the log, bug reports).
std::wstring advice_text(const Advice &advice);

// "section.key=value", as --set takes it.
std::wstring setting_text(const SettingValue &v);

}  // namespace glasir
