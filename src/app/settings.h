// SPDX-License-Identifier: MIT
// glasir.ini: the one settings file. The app writes everything else (mpv's
// options, later ReShade's) from it.
#pragma once
#include <windows.h>

#include <string>
#include <vector>

namespace glasir {

struct Settings {
    enum class CapturePath { Auto, MediaFoundation, DirectShow };

    // [capture]
    std::wstring device;  // empty: pick automatically
    UINT32 width = 0, height = 0;
    double fps = 0;
    CapturePath path = CapturePath::Auto;
    // [video]
    bool hdr = false;
    bool sdr_full_range = false;
    int hdr_peak_nits = 1000;
    // How the capture is scaled up to the window (glasir_scaling.lua).
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
    std::wstring monitor; // the screen Glasir plays on: its number (1, 2: Windows' own) or part of its name; empty: the primary
    // [frames]
    bool dedup = true;
    int dedup_threshold = 3;
    DWORD dedup_keepalive_ms = 200;
    // [framegen]
    bool framegen = true;
    bool framegen_fast = false;       // quality = fast: about half the GPU time
    bool framegen_blend = false;      // method = blend: a plain mix of the two frames, not motion-compensated
    int framegen_max_hz = 0;          // the most frames a second shown; 0: the screen's refresh rate
    bool all_frames_when_off = true;  // frame generation off: every frame (lowest latency)
    // [dlss5]
    enum class Dlss5 { Auto, On, Off };
    Dlss5 dlss5 = Dlss5::Auto;  // auto: on with an RTX 30, 40 or 50 series GPU
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
    // [launcher]
    bool show_launcher = true;  // Glasir.exe opens its window first
    // [update]
    bool update_check = true;  // the launcher looks for a newer Glasir
    // [debug]
    bool mpv_log = false;
};

// Reads the file; a missing file is created with the defaults. Unknown keys
// are logged and skipped. False with a message for a value that makes no sense.
bool load_settings(const std::wstring &path, Settings &s, std::wstring &error);

// "mpv", "fsr", "vsr", "cnn", "anime4k": glasir.ini's and glasir_scaling.lua's names.
const wchar_t *scaling_name(Settings::Scaling scaling);

// glasir.ini's names for [hdr] tone_mapping (mpv's own, "auto" first).
const std::vector<std::wstring> &tone_mapping_names();
const wchar_t *hdr_output_name(Settings::HdrOutput output);  // "auto", "hdr", "sdr"
const wchar_t *vrr_setting_name(Settings::Vrr vrr);          // "auto", "on", "off"

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

}  // namespace glasir
