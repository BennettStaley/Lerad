// SPDX-License-Identifier: MIT
// What the screen the picture goes on can do: its size and refresh rate,
// HDR (whether it has it, whether Windows has it on, how bright it gets) and
// variable refresh (G-SYNC, FreeSync, HDMI VRR). The launcher, mpv's options
// and frame generation all decide from this instead of assuming.
#pragma once
#include <string>

namespace glasir {

struct DisplayInfo {
    bool found = false;
    std::wstring name;    // the monitor's own name ("LG TV SSCR2"), "" if Windows has none
    std::wstring device;  // Windows' name for the output ("\\.\DISPLAY1")
    int width = 0, height = 0;  // the desktop mode, in pixels
    double refresh_hz = 0;      // 0: unknown

    // HDR. hdr_on: Windows' "Use HDR" is on for this screen now (what mpv's
    // swap chain can present as HDR10). The brightness values are what the
    // screen reports to Windows; 0: it does not say.
    bool hdr_supported = false;
    bool hdr_on = false;
    double peak_nits = 0;        // brightest small highlight
    double full_frame_nits = 0;  // brightest full white screen
    double min_nits = 0;
    double sdr_white_nits = 0;   // HDR on: Windows' "SDR content brightness"; 0: unknown

    // Variable refresh. Unknown: nothing could tell (assume neither way).
    // Supported: the screen and GPU can, but whether it is switched on could
    // not be read. On: switched on for this screen.
    enum class Vrr { Unknown, Unsupported, Supported, On };
    Vrr vrr = Vrr::Unknown;
    double vrr_min_hz = 0, vrr_max_hz = 0;  // the range, 0: unknown
    std::wstring vrr_source;                // how it was found, for the log ("NVIDIA driver", "EDID")
};

// The primary screen (mpv's window opens there).
DisplayInfo primary_display();

const wchar_t *vrr_name(DisplayInfo::Vrr vrr);  // "unknown", "unsupported", "supported", "on"

// One line for the log and bug reports.
std::wstring display_text(const DisplayInfo &d);

}  // namespace glasir
