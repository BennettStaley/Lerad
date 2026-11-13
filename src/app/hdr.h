// SPDX-License-Identifier: MIT
// What is done with an HDR capture (glasir.ini video.mode = hdr), decided in
// one place from the [hdr] settings and the screen: the launcher, mpv's
// options, the log and the in-game menu all say and do the same thing.
//
// Two questions:
//
// 1. HDR or SDR to the screen. HDR10 when Windows has HDR on for the screen
//    (or glasir.ini says so); else mpv tone maps the picture to SDR, as it
//    must: PQ values on an SDR screen are a grey, washed-out picture.
//
// 2. In HDR, who brings down highlights brighter than the screen. By default
//    nobody in Glasir: the picture goes out as the console sends it, the way
//    it would over a cable, and the console's own HDR calibration (done
//    through Glasir) sets the peak. That cannot make a chain that is set up
//    right look worse, and it does not depend on the peak the screen reports
//    to Windows, which is often wrong (a 1000 nit screen saying 400).
//    [hdr] fit_to_screen = 1 has mpv fit the picture to the screen's peak
//    instead, for a console that cannot be calibrated to this screen.
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "display.h"
#include "settings.h"

namespace glasir {

struct HdrPlan {
    enum class Output { Hdr, Sdr };
    Output output = Output::Sdr;
    // Why that output: the setting asks for it, or what the screen is.
    enum class Reason { Setting, HdrOn, HdrOff, NoHdr, NoScreen };
    Reason reason = Reason::NoScreen;
    // HDR output only: mpv fits the picture to peak_nits (tone mapping).
    // False: the picture goes out as the console sends it.
    bool fit = false;
    // The screen's peak the picture is fitted to, or would be if fit were
    // switched on: glasir.ini's, else what the screen reports. 0: unknown
    // (then nothing can be fitted, and fit is false).
    int peak_nits = 0;
    bool peak_reported = false;  // peak_nits is the screen's own word, not glasir.ini's
    // HDR output although Windows has HDR off for the screen ([hdr] output = hdr).
    bool forced_without_hdr = false;
};

// For these settings on this screen. Only means something when s.hdr.
HdrPlan hdr_plan(const Settings &s, const DisplayInfo &display);

// One plain line: "HDR10 to the screen, as the console sends it",
// "HDR10 to the screen, fitted to its 400 nits", "Tone mapped to SDR: HDR is
// off in Windows".
std::wstring hdr_plan_text(const HdrPlan &plan);
// Only the reason for SDR ("HDR is off in Windows"); "" for HDR output.
std::wstring hdr_sdr_reason(const HdrPlan &plan);

// mpv's name for the tone mapping curve: glasir.ini's, or for "auto" the one
// that suits the output (bt.2390 between HDR peaks, mpv's own for SDR).
const wchar_t *hdr_tone_mapping(const HdrPlan &plan, const Settings &s);

// mpv's output options for an HDR capture.
std::vector<std::wstring> hdr_mpv_options(const HdrPlan &plan, const Settings &s);

// What the in-game menu needs to know (it runs inside mpv): environment
// variables to set before mpv starts. Set for an SDR capture too, so that
// nothing is left over from a start in the other mode.
std::vector<std::pair<std::wstring, std::wstring>> hdr_environment(const Settings &s, const DisplayInfo &display);

// The menu's white in HDR, in nits: glasir.ini's, else Windows' "SDR content
// brightness" for the screen, else BT.2408's 203.
double paper_white_nits(const Settings &s, const DisplayInfo &display);

}  // namespace glasir
