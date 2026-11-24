// SPDX-License-Identifier: MIT
// The launcher's words and small decisions, apart from its drawing so they
// can be tested (tests/test_launcher.cpp): how a card, a screen and a GPU are
// named in its lists, "what you will get" as four steps, the line beside
// Play, where each piece of advice (advice.h) is shown, and what each frame
// generation multiplier would come to on this screen.
//
// Figures are always written the same way: 3840 x 2160, 59.94 fps, 120 Hz,
// x4, 30 -> 120 fps (with a real arrow).
#pragma once
#include <string>
#include <vector>

#include "advice.h"
#include "display.h"
#include "gpu.h"
#include "settings.h"

namespace app {

// A capture card as the launcher names it: the product's name where the
// driver wraps it ("Video (00 Pro Capture HDMI 4K+)" is "Pro Capture HDMI
// 4K+"), with the maker's name before it for a card known to be Magewell's.
std::wstring card_title(const std::wstring &name, bool magewell = false);

// A screen in the launcher's list, in two lines:
//   "1 · PA279CDV · 3840 x 2160 · 120 Hz"   (", main" for the primary)
//   "HDR on, peak 1000 nits · variable refresh off (FreeSync)"
std::wstring screen_title(const DisplayInfo &d);
std::wstring screen_details(const DisplayInfo &d);
// The two halves of screen_details.
std::wstring screen_hdr_text(const DisplayInfo &d);  // "HDR on, peak 1000 nits", "HDR off in Windows", "no HDR"
// "variable refresh on (G-SYNC Compatible)", "variable refresh off
// (FreeSync)", "variable refresh on or off: not known (FreeSync)", "no
// variable refresh", "variable refresh not known". One name for the thing,
// the screen's brand for it in brackets.
std::wstring screen_vrr_text(const DisplayInfo &d);
// What was found, in a word or two, for "As found: on": "on", "off", "none",
// "not known".
std::wstring vrr_found_word(const DisplayInfo &d);
// The brand and the range alone: "G-SYNC, 48-120 Hz", "FreeSync", "" when
// neither is known.
std::wstring vrr_brand_text(const DisplayInfo &d);

// "NVIDIA GeForce RTX 5090 · 32 GB".
std::wstring gpu_title(const GpuInfo &gpu);

// What display.monitor is set to when this screen is picked: its number,
// or for a screen without one its Windows name ("\\.\DISPLAY3").
std::wstring monitor_setting(const DisplayInfo &d);

// "What you will get": the same facts in the order the picture travels, one
// step each for the console, the capture card, the frames and the screen.
struct ResultStep {
    enum class Kind { Console, Card, Frames, Screen };
    Kind kind = Kind::Console;
    std::wstring title;               // "Console, set it to", "Capture card", "Frames", "Screen"
    std::vector<std::wstring> chips;  // the console's settings: "2160p", "HDR on", "120 Hz output off", "VRR off"
    std::wstring big;                 // the step's figure: "3840 x 2160", "30 -> 120 fps", "120 Hz · HDR10"
    std::wstring detail;              // a line or two under it
    bool matters = false;             // not what was asked for: shown as a warning
    bool problem = false;             // nothing can play as it is set (matters too): shown as a problem
    int multiplier = 1;               // Frames: frames shown per game frame, for the strip (1: none are added)
};
// low_latency: the card passes frames on early (a Magewell card with the
// setting on), which the card's step says.
std::vector<ResultStep> result_steps(const AdviceInput &in, const Advice &advice, bool low_latency = false);

// The hint that goes with the console's settings (its HDR calibration), ""
// if there is none.
std::wstring console_hint(const Advice &advice);

// The line beside Play: whether it can play, in a word, and what will be
// seen, in a sentence.
struct PlaySummary {
    enum class State { Ready, Warning, Problem };
    State state = State::Ready;
    std::wstring word;  // "Ready", "1 warning", "2 warnings", "Cannot play"
    std::wstring text;  // "A 30 fps game is shown at 120 fps, in HDR."
};
PlaySummary play_summary(const AdviceInput &in, const Advice &advice);

// Where the launcher shows a piece of advice: with the card (Play tab), the
// extras (Play tab), or on the Picture, Frame generation or Screen tab.
enum class AdvicePlace { Card, Extras, Picture, Frames, Screen };
AdvicePlace advice_place(const std::string &id);

// How the launcher shows an item:
//   Hidden  the settings already have what it suggests, or it is shown
//           another way (the console's settings: the first step)
//   Notice  problems, warnings and recommendations with something to set:
//           under "Worth a look", whatever tab is open
//   Place   the rest: as a note beside the controls they are about. Limits
//           for information, and other ways to set things up that suit some
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
    std::wstring note;  // under the choice: "30 -> 90 fps"; not usable: "does not fit"
    std::wstring why;   // not usable: the reason, a sentence
    double delay_ms = 0;  // how much later the game's own frame is shown at this multiplier, for a 30 fps game
};
// For x2, x3 and x4 with everything else as in.settings. Empty when frame
// generation cannot run or the screen's rate is not known.
std::vector<MultiplierChoice> multiplier_choices(const AdviceInput &in);

}  // namespace app
