// SPDX-License-Identifier: MIT
// The launcher's words and small decisions, apart from its drawing so they
// can be tested (tests/test_launcher.cpp): how a screen and a GPU are named
// in its lists, the "what you will get" lines, where each piece of advice
// (advice.h) is shown, and what each frame generation multiplier would come
// to on this screen.
#pragma once
#include <string>
#include <vector>

#include "advice.h"
#include "display.h"
#include "gpu.h"
#include "settings.h"

namespace glasir {

// A screen in the launcher's list, in two lines:
//   "1  ·  PA279CDV  ·  3840 x 2160  ·  120 Hz"   (", main" for the primary)
//   "HDR on, peak 1000 nits  ·  FreeSync 48-120 Hz, switched off"
std::wstring screen_title(const DisplayInfo &d);
std::wstring screen_details(const DisplayInfo &d);
// The two halves of screen_details.
std::wstring screen_hdr_text(const DisplayInfo &d);  // "HDR on, peak 1000 nits", "HDR off in Windows", "no HDR"
// "G-SYNC Compatible on", "FreeSync 48-120 Hz, switched off", "FreeSync 48-144 Hz
// (on or off: not known)", "no variable refresh", "variable refresh not known".
std::wstring screen_vrr_text(const DisplayInfo &d);

// "NVIDIA GeForce RTX 5090  ·  32 GB".
std::wstring gpu_title(const GpuInfo &gpu);

// What display.monitor is set to when this screen is picked: its number,
// or for a screen without one its Windows name ("\\.\DISPLAY3").
std::wstring monitor_setting(const DisplayInfo &d);

struct SummaryLine {
    std::wstring text;
    bool matters = false;  // not what was asked for, or nothing to play: shown in the warning colour
};

// "What you will get", a line each for the capture, the frames shown, HDR,
// the screen with its variable refresh, and DLSS 5 with the scaling.
std::vector<SummaryLine> summary_lines(const AdviceInput &in, const Advice &advice);

// "Set your console to": the settings on a line ("2160p  ·  HDR on  ·  120 Hz
// output off  ·  VRR off"), and the hint that goes with them (the HDR
// calibration), "" if there is none.
std::wstring console_line(const Advice &advice);
std::wstring console_hint(const Advice &advice);

// Where the launcher shows a piece of advice.
enum class AdvicePlace { Capture, Screen, Enhancements, Hdr, Framegen };
AdvicePlace advice_place(const std::string &id);

// How the launcher shows an item:
//   Hidden  the settings already have what it suggests, or it is shown
//           another way (the console's settings: console_line)
//   Notice  problems, warnings and recommendations with something to set:
//           above Play, whatever tab is open
//   Place   the rest: beside the controls they are about. Limits for
//           information, and other ways to set things up that suit some
//           players only (a 120 fps capture mode, a second GPU): offered
//           there with "Use", never pressed on anyone
enum class AdviceShown { Hidden, Notice, Place };
AdviceShown advice_shown(const AdviceItem &item);

// Sets what an item suggests ("Use").
void apply_suggestion(Settings &s, const AdviceItem &item);

// One of the "up to" choices of frame generation on this screen.
struct MultiplierChoice {
    int multiplier = 2;
    // Some game rate is shown with this many frames (false: choosing it
    // changes nothing here, and why says so).
    bool usable = false;
    std::wstring text;  // "30 fps shown at 90", or for several rates "30 fps at 120, 60 fps at 120"
    std::wstring why;   // not usable: the reason, a sentence
    double delay_ms = 0;  // how much later the game's own frame is shown at this multiplier, for a 30 fps game
};
// For x2, x3 and x4 with everything else as in.settings. Empty when frame
// generation cannot run or the screen's rate is not known.
std::vector<MultiplierChoice> multiplier_choices(const AdviceInput &in);

}  // namespace glasir
