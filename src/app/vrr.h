// SPDX-License-Identifier: MIT
// Finding out about variable refresh (G-SYNC, FreeSync, HDMI VRR) for one
// screen, for display.h. Two sources, each saying only what it knows:
//
// - The monitor's EDID, as Windows keeps it in the registry (any GPU): what
//   the screen can do and its range. Read from AMD's FreeSync data block and
//   the HDMI Forum's (HDMI 2.1 VRR), whose layouts are public (the HDMI
//   Forum's in CTA-861; AMD's as edid-decode documents it).
// - NVIDIA's driver, through NVAPI (nvapi64.dll, which the driver installs;
//   loaded at run time, nothing of it is shipped): whether the driver offers
//   VRR on the screen, whether G-SYNC is switched on and for windows too.
//
// Not read yet, so they end at "Supported" from the EDID: AMD's driver (ADL /
// ADLX: FreeSync on or off) and Intel's (IGCL). Windows has no documented
// call that says whether VRR is on for a screen.
#pragma once
#include <string>
#include <vector>

#include "display.h"

namespace app {

// What a monitor's EDID says about variable refresh.
struct EdidVrr {
    bool valid = false;  // an EDID that could be read (its first block, at least)
    bool freesync = false;  // AMD's FreeSync block
    double freesync_min_hz = 0, freesync_max_hz = 0;
    bool hdmi_vrr = false;  // the HDMI Forum block's VRR range
    double hdmi_min_hz = 0, hdmi_max_hz = 0;
    // The first block's range limits, when the monitor says it takes any rate
    // in between ("continuous frequency"). DisplayPort Adaptive-Sync screens
    // have this, but so do plain ones: a range, never proof of VRR.
    bool range = false;
    double range_min_hz = 0, range_max_hz = 0;
};
EdidVrr parse_edid_vrr(const unsigned char *edid, size_t size);

// What NVIDIA's driver says about a screen.
struct NvidiaVrr {
    bool read = false;       // the driver answered for this screen
    bool supported = false;  // it offers VRR on this screen
    bool capable_now = false;  // ... at the display mode set now
    bool true_gsync = false;   // a G-SYNC module, not Adaptive-Sync / HDMI VRR
    double min_hz = 0;         // the lowest rate it holds, 0: not said
    // "Enable G-SYNC" in the NVIDIA Control Panel (the driver's global
    // profile): 0 off, 1 full screen only, 2 windowed and full screen;
    // -1: could not be read.
    int mode = -1;
};

// Fills d's vrr fields from what was read (either may be null: not read).
// hdmi: the screen is connected by HDMI (its EDID's HDMI range applies).
void apply_vrr(DisplayInfo &d, const EdidVrr *edid, const NvidiaVrr *nvidia, bool hdmi);

// The EDID of a monitor by its device path (DISPLAYCONFIG_TARGET_DEVICE_NAME's
// monitorDevicePath); empty if Windows has none.
std::vector<unsigned char> read_edid(const std::wstring &monitor_device_path);

// device: "\\.\DISPLAY1". read stays false without an NVIDIA driver or when
// it does not know the screen.
NvidiaVrr read_nvidia_vrr(const std::wstring &device);

// What NVIDIA's driver has set for one program (the NVIDIA Control Panel's
// "Program Settings"). A program's profile can say "full screen only" while
// the global setting says "windowed and full screen": G-SYNC then never
// engages in mpv's window, which is how the app shows the picture, and the
// screen keeps its fixed rate. The control panel does not show these two
// settings per program.
struct NvidiaAppVrr {
    bool read = false;         // the driver answered
    bool has_profile = false;  // a profile names this program
    bool own_path = false;     // ... by this very path (not every mpv.exe)
    std::wstring profile;      // its name
    // In effect for the program, from its profile or what it inherits:
    // 0 off, 1 full screen only, 2 windowed and full screen; -1: not read.
    int mode = -1;       // "Enable G-SYNC" (VRR_MODE)
    int requested = -1;  // the state asked for (VRRREQUESTSTATE)
    bool mode_own = false, requested_own = false;  // set in the profile itself
    // Windowed G-SYNC is allowed for the program (both are 2).
    bool windowed() const { return mode == 2 && requested == 2; }
};

// exe: the program's full path (engine\mpv.exe).
NvidiaAppVrr read_nvidia_app_vrr(const std::wstring &exe);

// Sets both to "windowed and full screen" for this path only, in the
// program's own profile (made if there is none). What was there before is
// kept in backup_file (never overwritten: the first state is the one to go
// back to). The program must be started again to see it.
bool allow_nvidia_windowed_vrr(const std::wstring &exe, const std::wstring &backup_file, std::wstring &error);

// Puts back what backup_file holds and deletes it; a profile the app made is
// removed when nothing else was added to it.
bool restore_nvidia_app_vrr(const std::wstring &exe, const std::wstring &backup_file, std::wstring &error);

// One or two lines for --system and --gsync-profile.
std::wstring nvidia_app_vrr_text(const NvidiaAppVrr &v);

// All of it for one screen: reads the EDID, asks NVIDIA's driver when the
// screen is on an NVIDIA GPU (d.gpu_vendor), and fills d.
void read_vrr(DisplayInfo &d, const std::wstring &monitor_device_path, bool hdmi);

}  // namespace app
