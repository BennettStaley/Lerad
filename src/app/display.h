// SPDX-License-Identifier: MIT
// What the screens of this PC can do: size and refresh rate, HDR (whether
// the screen has it, whether Windows has it on, how bright it gets) and
// variable refresh (G-SYNC, FreeSync, HDMI VRR). The launcher, mpv's options
// and frame generation all decide from this instead of assuming.
//
// The app plays on one screen: the settings file's display.monitor, or the primary.
// list_displays() reads them all, choose_display() picks.
#pragma once
#include <string>
#include <vector>

namespace app {

struct DisplayInfo {
    bool found = false;
    std::wstring name;    // the monitor's own name ("LG TV SSCR2"), "" if Windows has none
    std::wstring device;  // Windows' name for the output ("\\.\DISPLAY1")
    // Windows' device path of the monitor plugged into it ("\\?\DISPLAY#..."),
    // "" unknown: the output's name can pass to another monitor when screens
    // are plugged in or switched off, this cannot.
    std::wstring monitor_path;
    int number = 0;       // the 1 of \\.\DISPLAY1: what display.monitor = 1 means; 0: unknown
    bool primary = false;
    // Its place in Windows' list of monitors (EnumDisplayMonitors), which is
    // how mpv's --screen counts them; -1: unknown.
    int enum_index = -1;
    int left = 0, top = 0;      // where it is on the desktop
    int width = 0, height = 0;  // the desktop mode, in pixels
    double refresh_hz = 0;      // 0: unknown
    std::wstring gpu;           // the GPU it is plugged into (GpuInfo::name), "" if unknown
    unsigned gpu_vendor = 0;

    // HDR. hdr_on: Windows' "Use HDR" is on for this screen now (what mpv's
    // swap chain can present as HDR10). The brightness values are what the
    // screen reports to Windows; 0: it does not say.
    bool hdr_supported = false;
    bool hdr_on = false;
    double peak_nits = 0;        // brightest small highlight
    double full_frame_nits = 0;  // brightest full white screen
    double min_nits = 0;
    double sdr_white_nits = 0;   // HDR on: Windows' "SDR content brightness"; 0: unknown

    // Variable refresh. Nothing here is guessed: what could not be read
    // stays Unknown.
    //   Unknown      nothing could tell (assume neither way).
    //   Unsupported  the GPU's driver says this screen has none.
    //   Supported    the screen can (its EDID says so), but whether it is
    //                switched on could not be read (AMD and Intel GPUs today).
    //   On           the driver has it switched on for this screen.
    //   Off          the screen can, and the driver has it switched off (or
    //                does not offer it on this screen as it is connected).
    enum class Vrr { Unknown, Unsupported, Supported, On, Off };
    Vrr vrr = Vrr::Unknown;
    double vrr_min_hz = 0, vrr_max_hz = 0;  // the range, 0: unknown
    // What kind, "" if unknown: "G-SYNC", "G-SYNC Compatible", "FreeSync",
    // "HDMI VRR", "Adaptive-Sync".
    std::wstring vrr_kind;
    // How each fact was found, for the log and bug reports, e.g. "NVIDIA
    // driver: on, for windows too; EDID: HDMI VRR 48-240 Hz".
    std::wstring vrr_source;
    enum class Tri { Unknown, No, Yes };
    // VRR also works for a window, not only for full screen (NVIDIA: "Enable
    // for windowed and full screen mode"). The app's full screen and
    // borderless modes cover the screen and get it either way; a window
    // smaller than the screen needs Yes.
    Tri vrr_windowed = Tri::Unknown;
};

// Every screen that is part of the desktop, the primary first, then by number.
std::vector<DisplayInfo> list_displays();

// The screen the settings file's display.monitor asks for: a number (the 2 of
// \\.\DISPLAY2, as the exe's --system lists them; Windows' display
// settings usually show the same numbers) or a part of the monitor's name
// (any case). Empty, or nothing matches: the primary (matched says which;
// it may be null). Null only for an empty list.
const DisplayInfo *choose_display(const std::vector<DisplayInfo> &displays, const std::wstring &wanted,
                                  bool *matched = nullptr);

// The primary screen alone: list_displays() and the first of it.
DisplayInfo primary_display();

const wchar_t *vrr_name(DisplayInfo::Vrr vrr);  // "unknown", "unsupported", "supported", "on", "off"

// What the add-ons inside mpv are told (env DISPLAY_VRR): "on", "off" or
// "unknown". user: the settings file's display.vrr, 1 = on, 0 = off, -1 = auto (as
// found: Supported is "unknown", as nothing says it is switched on).
const wchar_t *vrr_for_addons(DisplayInfo::Vrr found, int user);

// What the frame generation add-on is told about every screen of the PC
// (env SCREENS; src/framegen/fg_screen.h has the format): Windows' name
// for the output, its VRR state as vrr_for_addons gives it, its refresh rate,
// the monitor's device path and a name for people ("1 (PA279CDV)"). mpv's
// window can be moved to any of them while it runs, and the add-on goes by
// the one it is on. chosen: the device of the screen the app plays on, the
// only one the settings file's display.vrr (user, as above) speaks for.
std::wstring screens_for_addons(const std::vector<DisplayInfo> &displays, const std::wstring &chosen, int user);

// One line for the log and bug reports.
std::wstring display_text(const DisplayInfo &d);

}  // namespace app
