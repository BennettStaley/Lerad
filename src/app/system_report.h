// SPDX-License-Identifier: MIT
// Looking at this PC once (screens, GPUs) and saying what was found: the
// input for advice.h, and the text of the exe's --system, the log and bug
// reports.
#pragma once
#include <string>
#include <vector>

#include "advice.h"
#include "devices.h"
#include "display.h"
#include "gpu.h"
#include "settings.h"

namespace app {

struct SystemInfo {
    std::vector<DisplayInfo> displays;  // all screens (list_displays)
    DisplayInfo display;                // the one the app plays on (display.monitor, else the primary)
    bool monitor_matched = true;        // display.monitor named a screen that is there
    std::vector<GpuInfo> gpus;          // all GPUs (list_gpus)
    GpuChoice gpu;                      // the one mpv draws on (display.gpu, else the screen's)
};

// Reads the screens and GPUs and applies the settings file's [display] choices.
// Takes some tens of milliseconds (it asks the GPU driver about VRR): call
// it off the UI thread, and again when the user picks another screen or GPU.
SystemInfo survey_system(const Settings &s);

// The input for advise(): the survey, plus the capture card the app would
// open for these settings (picked from cards as it does when it plays; no
// card that fits: card_found = false) and what is downloaded in engine_dir.
AdviceInput advice_input(const Settings &s, const SystemInfo &system, const std::vector<CaptureDevice> &cards,
                         const std::wstring &engine_dir);

// advice_input() keeps what the NVIDIA driver said about mpv.exe for ten
// seconds; this makes it ask again (after --gsync-profile's fix).
void forget_mpv_profile();

// One line: "Elgato 4K X: SDR up to 3840x2160 at 144 fps; HDR up to ...".
std::wstring card_summary(const CaptureDevice &card);

// Everything as text: the GPUs, every screen with HDR and VRR, the cards
// (null: left out, e.g. while one is in use) and the advice (null: left out).
std::wstring system_report(const SystemInfo &system, const std::vector<CaptureDevice> *cards, const Advice *advice);

}  // namespace app
