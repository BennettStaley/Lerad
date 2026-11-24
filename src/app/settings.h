// SPDX-License-Identifier: MIT
// The settings file (APP_INI, beside the exe): the one settings file. The app
// writes everything else (mpv's options, later ReShade's) from it.
#pragma once
#include <windows.h>

#include <string>
#include <vector>

namespace app {

struct Settings {
    enum class CapturePath { Auto, MediaFoundation, DirectShow };

    // [capture]
    std::wstring device;  // empty: pick automatically
    UINT32 width = 0, height = 0;
    double fps = 0;
    CapturePath path = CapturePath::Auto;
    bool low_latency = true;  // a Magewell Pro Capture card's low latency mode (capture/magewell.h)
    // [video]
    // mode = auto: a 10-bit capture, shown as what the signal turns out to
    // be (signal_kind.h). hdr is then what it is shown as: what was found
    // last time (auto_found) until the picture says otherwise.
    bool hdr_auto = true;
    bool hdr = false;
    bool auto_found_hdr = true;
    bool sdr_full_range = false;
    int hdr_peak_nits = 1000;
    // How the capture is scaled up to the window (scaling.lua).
    enum class Scaling { Mpv, Fsr, Vsr, Cnn, Anime4k };
    Scaling scaling = Scaling::Mpv;
    // [hdr]: what is done with an HDR capture (video.mode = hdr). hdr.h
    // (hdr_plan) says what these come to on a given screen.
    // Auto: HDR10 to the screen when Windows has HDR on for it, else tone
    // mapped to SDR. Hdr / Sdr: always that.
    enum class HdrOutput { Auto, Hdr, Sdr };
    HdrOutput hdr_output = HdrOutput::Auto;
    // HDR output: false, the picture goes out as the console sends it (the
    // console's HDR calibration sets the peak); true, mpv fits it to the
    // screen's peak (display_peak_nits) with tone_mapping.
    bool hdr_fit_to_screen = false;
    int display_peak_nits = 0;           // fitting: the screen's peak; 0: what the screen reports to Windows
    // Fitting and SDR output: mpv's --tone-mapping (tone_mapping_names());
    // auto: bt.2390 when fitting, mpv's own (spline) for SDR (hdr_tone_mapping).
    std::wstring tone_mapping = L"auto";
    bool hdr_dynamic_peak = false;       // fitting and SDR output: measure each frame's peak (mpv's --hdr-compute-peak)
    int paper_white_nits = 0;            // the menu's white in HDR; 0: Windows' SDR brightness, else 203
    // [display]
    enum class Vrr { Auto, On, Off };
    Vrr vrr = Vrr::Auto;  // auto: as detected (display.h); on / off: the user knows better
    std::wstring gpu;     // part of the GPU's name mpv renders on; empty: the screen's own
    std::wstring monitor; // the screen the app plays on: its number (1, 2: Windows' own) or part of its name; empty: the primary
    // [frames]
    bool dedup = true;
    int dedup_threshold = 3;
    DWORD dedup_keepalive_ms = 200;
    // Every captured frame is a game frame, pictures not compared. auto: at a
    // capture rate a 30 fps game fills (repeat_filter.h, capture_rate_is_game_rate).
    enum class EveryFrame { Auto, On, Off };
    EveryFrame every_frame = EveryFrame::Auto;
    // [framegen]
    bool framegen = true;
    bool framegen_fast = false;       // quality = fast: about half the GPU time
    bool framegen_blend = false;      // method = blend: a plain mix of the two frames, not motion-compensated
    // backend = dlss: NVIDIA DLSS Frame Generation makes the frames (RTX 40
    // and newer; the app's own where it cannot run). own: the app's own
    // (what the app is or was called is read as "own" too: older files).
    bool framegen_dlss = false;
    bool framegen_video = false;  // NVIDIA Video Frame Generation makes the frames (never together with framegen_dlss)
    // backend = anr: Apex Neural Rendering's frame generation makes them, on
    // the motion the app's own search finds (never together with the others).
    bool framegen_anr = false;
    int framegen_max_hz = 0;          // the most frames a second shown; 0: the screen's refresh rate
    int framegen_multiplier = 2;      // frames shown per game frame, at most: 2, 3 or 4
    // keep: areas frame generation leaves alone (src/common/keep_regions.h),
    // as the text "r:2,80,20,15;e:85,5,12,20". Empty: none. An area with
    // ",hud" after its numbers is kept only where a HUD is found in it.
    std::wstring framegen_keep;
    // hud = 1: "find the HUD everywhere" - frames in between show the game's
    // own frame wherever frame generation takes the picture for a HUD
    // (src/framegen/fg_hud.h), with no area drawn.
    bool framegen_hud = false;
    bool framegen_show_rates = false; // the frame rate readout in the corner (read by the menu's add-on)
    bool all_frames_when_off = true;  // frame generation off: every frame (lowest latency)
    // [dlss5]
    enum class Dlss5 { Auto, On, Off };
    Dlss5 dlss5 = Dlss5::Off;  // off unless asked for; auto: on with an RTX 30, 40 or 50 series GPU
    // [audio]
    bool audio = true;
    std::wstring audio_device;  // empty: the card's own audio input
    int volume = 100;           // 0-100 %
    int boost = 100;            // 100-400 %
    int audio_delay_ms = 0;     // 0-1000
    bool audio_min_period = true;
    // [window]
    bool borderless = true;
    // window.mode = fullscreen: mpv's own full screen (f, double-click);
    // borderless is then true too, where leaving it goes.
    bool fullscreen = false;
    bool skip_black_bars = false;  // full screen: mpv's window takes the picture only (backdrop.h)
    bool stream_window = false;    // a second SDR window with the picture, for sharing (stream_window.h)
    int stream_brightness = 80;    // an HDR picture in it, percent: 100 puts HDR's reference white at the SDR white
    // [input]
    bool input = false;            // start the input program with the picture (input_program.h)
    std::wstring input_program;    // its file; from the app's folder unless a full path
    int input_sensitivity = 30;        // the mouse: 5-100, how far the view turns for a movement
    int input_aim_sensitivity = 100;   // while aiming down the sights, percent of the above (20-200)
    bool input_forward = true;         // the picture moves with the mouse ahead of the console (view_shift.h)
    int input_lead_ms = 140;           // how long the console takes to show a turn, which the picture is moved ahead by (0-300)
    // [launcher]
    bool show_launcher = true;  // the exe opens its window first
    // [debug]
    bool mpv_log = false;
};

// Reads the file; a missing file is created with the defaults. Unknown keys
// are logged and skipped. False with a message for a value that makes no sense.
bool load_settings(const std::wstring &path, Settings &s, std::wstring &error);

// "mpv", "fsr", "vsr", "cnn", "anime4k": the settings file's and scaling.lua's names.
const wchar_t *scaling_name(Settings::Scaling scaling);

// The settings file's names for [hdr] tone_mapping (mpv's own, "auto" first).
const std::vector<std::wstring> &tone_mapping_names();
const wchar_t *hdr_output_name(Settings::HdrOutput output);  // "auto", "hdr", "sdr"
const wchar_t *vrr_setting_name(Settings::Vrr vrr);          // "auto", "on", "off"
const wchar_t *every_frame_name(Settings::EveryFrame every); // "auto", "on", "off"

// One setting, as "section.key=value" (the command line's --set).
bool apply_override(Settings &s, const std::wstring &assignment, std::wstring &error);

// The file written when there is none: every key at its default, explained.
const char *default_settings_text();

struct SettingValue {
    std::wstring section, key, value;
};
// The text with these values set: an existing key keeps its line (spacing,
// a comment after it), a missing one is added at the end of its section (the
// section at the end of the file if there is none). Everything else stays.
std::string update_settings_text(const std::string &text, const std::vector<SettingValue> &values);
// update_settings_text on the file, which is replaced as a whole.
bool save_settings(const std::wstring &path, const std::vector<SettingValue> &values);

// The values of `to` that differ from `from`, as the settings file writes them: what
// a UI that edits a Settings saves, so that everything the user did not
// touch stays as it is in the file (its own spelling, or not there at all).
// The capture's width, height and fps go together: one changed, all three.
std::vector<SettingValue> changed_settings(const Settings &from, const Settings &to);

}  // namespace app
