// SPDX-License-Identifier: MIT
#include "settings.h"

#include <cwchar>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <vector>

#include "../common/ini_edit.h"
#include "log.h"

namespace glasir {
namespace {

std::wstring trim(const std::wstring &s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::wstring lower(std::wstring s) {
    for (auto &c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

bool parse_bool(const std::wstring &v, bool &out) {
    const std::wstring l = lower(v);
    if (l == L"1" || l == L"yes" || l == L"true" || l == L"on") return out = true, true;
    if (l == L"0" || l == L"no" || l == L"false" || l == L"off") return out = false, true;
    return false;
}

bool parse_int(const std::wstring &v, long lo, long hi, long &out) {
    if (v.empty()) return false;
    wchar_t *end = nullptr;
    const long n = wcstol(v.c_str(), &end, 10);
    if (*end || n < lo || n > hi) return false;
    out = n;
    return true;
}

bool parse_double(const std::wstring &v, double lo, double hi, double &out) {
    if (v.empty()) return false;
    wchar_t *end = nullptr;
    const double n = wcstod(v.c_str(), &end);
    if (*end || n < lo || n > hi) return false;
    out = n;
    return true;
}

// false: bad value (error set). Unknown keys are logged and accepted.
bool apply(Settings &s, const std::wstring &section, const std::wstring &key, const std::wstring &value,
           std::wstring &error) {
    const std::wstring name = section + L"." + key;
    auto bad = [&](const wchar_t *expected) {
        error = L"glasir.ini: " + name + L" = " + value + L" (expected " + expected + L")";
        return false;
    };
    long n = 0;
    double d = 0;
    bool b = false;
    if (name == L"capture.device") {
        s.device = value;
    } else if (name == L"capture.width") {
        if (!parse_int(value, 0, 16384, n) || n % 2) return bad(L"0 or an even number of pixels");
        s.width = UINT32(n);
    } else if (name == L"capture.height") {
        if (!parse_int(value, 0, 16384, n) || n % 2) return bad(L"0 or an even number of pixels");
        s.height = UINT32(n);
    } else if (name == L"capture.fps") {
        if (!parse_double(value, 0, 1000, d)) return bad(L"0 or frames per second");
        s.fps = d;
    } else if (name == L"capture.path") {
        const std::wstring l = lower(value);
        if (l == L"auto") s.path = Settings::CapturePath::Auto;
        else if (l == L"mediafoundation") s.path = Settings::CapturePath::MediaFoundation;
        else if (l == L"directshow") s.path = Settings::CapturePath::DirectShow;
        else return bad(L"auto, mediafoundation or directshow");
    } else if (name == L"video.mode") {
        const std::wstring l = lower(value);
        if (l == L"sdr") s.hdr = false;
        else if (l == L"hdr") s.hdr = true;
        else return bad(L"sdr or hdr");
    } else if (name == L"video.sdr_range") {
        const std::wstring l = lower(value);
        if (l == L"limited") s.sdr_full_range = false;
        else if (l == L"full") s.sdr_full_range = true;
        else return bad(L"limited or full");
    } else if (name == L"video.scaling") {
        const std::wstring l = lower(value);
        if (l == L"mpv") s.scaling = Settings::Scaling::Mpv;
        else if (l == L"fsr") s.scaling = Settings::Scaling::Fsr;
        else if (l == L"vsr") s.scaling = Settings::Scaling::Vsr;
        else if (l == L"cnn") s.scaling = Settings::Scaling::Cnn;
        else if (l == L"anime4k") s.scaling = Settings::Scaling::Anime4k;
        else return bad(L"mpv, fsr, vsr, cnn or anime4k");
    } else if (name == L"video.hdr_peak_nits") {
        if (!parse_int(value, 80, 10000, n)) return bad(L"80 to 10000");
        s.hdr_peak_nits = int(n);
    } else if (name == L"hdr.output") {
        const std::wstring l = lower(value);
        if (l == L"auto") s.hdr_output = Settings::HdrOutput::Auto;
        else if (l == L"hdr") s.hdr_output = Settings::HdrOutput::Hdr;
        else if (l == L"sdr") s.hdr_output = Settings::HdrOutput::Sdr;
        else return bad(L"auto, hdr or sdr");
    } else if (name == L"hdr.display_peak_nits") {
        if (!parse_int(value, 0, 10000, n) || (n && n < 80)) return bad(L"0 or 80 to 10000");
        s.display_peak_nits = int(n);
    } else if (name == L"hdr.tone_mapping") {
        const std::wstring l = lower(value);
        bool known = false;
        for (const auto &t : tone_mapping_names()) known = known || t == l;
        if (!known) return bad(L"auto, clip, spline, bt.2390, bt.2446a, st2094-40, st2094-10, mobius, reinhard, "
                               L"hable, gamma or linear");
        s.tone_mapping = l;
    } else if (name == L"hdr.dynamic_peak") {
        if (!parse_bool(value, b)) return bad(L"1 or 0");
        s.hdr_dynamic_peak = b;
    } else if (name == L"hdr.paper_white_nits") {
        if (!parse_int(value, 0, 1000, n) || (n && n < 80)) return bad(L"0 or 80 to 1000");
        s.paper_white_nits = int(n);
    } else if (name == L"display.vrr") {
        const std::wstring l = lower(value);
        if (l == L"auto") s.vrr = Settings::Vrr::Auto;
        else if (l == L"on" || l == L"1") s.vrr = Settings::Vrr::On;
        else if (l == L"off" || l == L"0") s.vrr = Settings::Vrr::Off;
        else return bad(L"auto, on or off");
    } else if (name == L"display.gpu") {
        s.gpu = value;
    } else if (name == L"display.monitor") {
        s.monitor = value;
    } else if (name == L"framegen.method") {
        const std::wstring l = lower(value);
        if (l == L"motion") s.framegen_blend = false;
        else if (l == L"blend") s.framegen_blend = true;
        else return bad(L"motion or blend");
    } else if (name == L"framegen.max_output_hz") {
        if (!parse_int(value, 0, 1000, n) || (n && n < 24)) return bad(L"0 or 24 to 1000");
        s.framegen_max_hz = int(n);
    } else if (name == L"framegen.multiplier") {
        if (!parse_int(value, 2, 4, n)) return bad(L"2, 3 or 4");
        s.framegen_multiplier = int(n);
    } else if (name == L"framegen.show_rates") {
        if (!parse_bool(value, b)) return bad(L"1 or 0");
        s.framegen_show_rates = b;
    } else if (name == L"frames.dedup") {
        if (!parse_bool(value, b)) return bad(L"1 or 0");
        s.dedup = b;
    } else if (name == L"frames.dedup_threshold") {
        if (!parse_int(value, 0, 1023, n)) return bad(L"0 to 1023");
        s.dedup_threshold = int(n);
    } else if (name == L"frames.dedup_keepalive_ms") {
        if (!parse_int(value, 0, 60000, n)) return bad(L"0 to 60000");
        s.dedup_keepalive_ms = DWORD(n);
    } else if (name == L"framegen.enabled") {
        if (!parse_bool(value, b)) return bad(L"1 or 0");
        s.framegen = b;
    } else if (name == L"framegen.quality") {
        const std::wstring l = lower(value);
        if (l == L"high") s.framegen_fast = false;
        else if (l == L"fast") s.framegen_fast = true;
        else return bad(L"high or fast");
    } else if (name == L"framegen.all_frames_when_off") {
        if (!parse_bool(value, b)) return bad(L"1 or 0");
        s.all_frames_when_off = b;
    } else if (name == L"dlss5.enabled") {
        const std::wstring l = lower(value);
        if (l == L"auto") s.dlss5 = Settings::Dlss5::Auto;
        else if (l == L"on" || l == L"1") s.dlss5 = Settings::Dlss5::On;
        else if (l == L"off" || l == L"0") s.dlss5 = Settings::Dlss5::Off;
        else return bad(L"auto, on or off");
    } else if (name == L"audio.enabled") {
        if (!parse_bool(value, b)) return bad(L"1 or 0");
        s.audio = b;
    } else if (name == L"audio.device") {
        s.audio_device = value;
    } else if (name == L"audio.volume") {
        if (!parse_int(value, 0, 100, n)) return bad(L"0 to 100");
        s.volume = int(n);
    } else if (name == L"audio.boost") {
        if (!parse_int(value, 100, 400, n)) return bad(L"100 to 400");
        s.boost = int(n);
    } else if (name == L"audio.delay_ms") {
        if (!parse_int(value, 0, 1000, n)) return bad(L"0 to 1000");
        s.audio_delay_ms = int(n);
    } else if (name == L"audio.period") {
        const std::wstring l = lower(value);
        if (l == L"minimum") s.audio_min_period = true;
        else if (l == L"default") s.audio_min_period = false;
        else return bad(L"minimum or default");
    } else if (name == L"window.mode") {
        const std::wstring l = lower(value);
        if (l == L"fullscreen") s.borderless = s.fullscreen = true;
        else if (l == L"borderless") s.borderless = true, s.fullscreen = false;
        else if (l == L"windowed") s.borderless = s.fullscreen = false;
        else return bad(L"fullscreen, borderless or windowed");
    } else if (name == L"window.skip_black_bars") {
        if (!parse_bool(value, b)) return bad(L"1 or 0");
        s.skip_black_bars = b;
    } else if (name == L"launcher.show") {
        if (!parse_bool(value, b)) return bad(L"1 or 0");
        s.show_launcher = b;
    } else if (name == L"update.check") {
        if (!parse_bool(value, b)) return bad(L"1 or 0");
        s.update_check = b;
    } else if (name == L"debug.mpv_log") {
        if (!parse_bool(value, b)) return bad(L"1 or 0");
        s.mpv_log = b;
    } else {
        log(L"glasir.ini: unknown setting {}, skipped", name);
    }
    return true;
}

}  // namespace

const wchar_t *scaling_name(Settings::Scaling scaling) {
    switch (scaling) {
    case Settings::Scaling::Fsr: return L"fsr";
    case Settings::Scaling::Vsr: return L"vsr";
    case Settings::Scaling::Cnn: return L"cnn";
    case Settings::Scaling::Anime4k: return L"anime4k";
    default: return L"mpv";
    }
}

const std::vector<std::wstring> &tone_mapping_names() {
    static const std::vector<std::wstring> names = {L"auto",      L"clip",      L"spline", L"bt.2390",
                                                    L"bt.2446a",  L"st2094-40", L"st2094-10", L"mobius",
                                                    L"reinhard",  L"hable",     L"gamma",  L"linear"};
    return names;
}

const wchar_t *hdr_output_name(Settings::HdrOutput output) {
    switch (output) {
    case Settings::HdrOutput::Hdr: return L"hdr";
    case Settings::HdrOutput::Sdr: return L"sdr";
    default: return L"auto";
    }
}

const wchar_t *vrr_setting_name(Settings::Vrr vrr) {
    switch (vrr) {
    case Settings::Vrr::On: return L"on";
    case Settings::Vrr::Off: return L"off";
    default: return L"auto";
    }
}

const char *default_settings_text() {
    return R"(; Glasir settings. Lines starting with ; are comments.
; A missing file is recreated with these defaults.

[capture]
; The card's name as "Glasir.exe --list" shows it. Empty: pick automatically.
device =
; Capture size and rate. 0: automatic (60 fps if offered, the largest size
; that fits the display).
width = 0
height = 0
fps = 0
; auto: Media Foundation if the card offers a usable mode, else DirectShow.
; DirectShow works with more cards but has no dedup or frame generation.
path = auto

[video]
; sdr or hdr. HDR needs a card with a 10-bit (P010) mode and HDR on the console.
mode = sdr
; The SDR signal's range: limited (most consoles) or full.
sdr_range = limited
; Peak brightness written into the HDR10 metadata.
hdr_peak_nits = 1000
; How a capture smaller than the window is scaled up (before DLSS 5):
; mpv (its own Lanczos), fsr (AMD FSR 1: sharper, any GPU), vsr (the GPU's
; video super resolution: NVIDIA RTX or Intel; FSR 1 where it cannot run),
; cnn (ArtCNN, a small neural network: the most detail, any GPU, more GPU
; time) or anime4k (Anime4K: for cel-shaded and drawn styles, any GPU, more
; GPU time).
scaling = mpv

[hdr]
; What is done with an HDR capture (video.mode = hdr). auto: sent to the
; screen as HDR when Windows has HDR on for it, else tone mapped to SDR.
; hdr: always sent as HDR. sdr: always tone mapped to SDR.
output = auto
; The screen's peak brightness (nits) the picture is fitted to. 0: what the
; screen reports to Windows.
display_peak_nits = 0
; How highlights brighter than the screen are brought down: auto, clip,
; spline, bt.2390, bt.2446a, st2094-40, st2094-10, mobius, reinhard, hable,
; gamma or linear (mpv's tone mapping curves).
tone_mapping = auto
; 1: measure each frame's brightest point instead of trusting hdr_peak_nits
; (a better fit for games that never reach their peak; some GPU time).
dynamic_peak = 0
; How bright the menu's white is in HDR (nits). 0: Windows' "SDR content
; brightness" for the screen.
paper_white_nits = 0

[display]
; The screen Glasir plays on: its number as Windows' display settings show it
; (1, 2) or a part of its name. Empty: the main screen.
monitor =
; Variable refresh rate (G-SYNC, FreeSync, HDMI VRR) on the PC's screen.
; auto: as Glasir finds it. on / off: if it finds it wrong.
vrr = auto
; The GPU mpv draws on, by a part of its name ("RTX", "Radeon"). Empty: the
; one the screen is plugged into.
gpu =

[frames]
; 1: send mpv only new frames (a 30 fps game captured at 60 arrives at 30).
; Frame generation needs it.
dedup = 1
; How much a 16x16 block's average brightness must change (10-bit steps) to
; count as a new frame. Raise it for noisy sources.
dedup_threshold = 3
; A still picture is still sent this often (ms), so menus keep drawing.
dedup_keepalive_ms = 200

[framegen]
; 1: frame generation adds frames between the game's (30 -> 60, 60 -> 120)
; when the screen can show them. Needs dedup = 1 and ReShade. F8 toggles it
; while playing; the menu (Tab) switches it and keeps the choices here, and
; says what is shown and why when it is less than asked for.
enabled = 1
; high, or fast (about half the GPU time, a little less clean).
quality = high
; motion: the frame in between follows what moves (the default). blend: a
; plain mix of the two frames (the lightest; moving things show double).
method = motion
; Frames shown per game frame, at most: 2 (one frame in between), 3 or 4
; (two or three: 30 -> 90 or 120 on a fast screen). More costs GPU time for
; each added frame and shows the game's own frame later (half a game frame
; at 2, two thirds at 3, three quarters at 4). Glasir uses the most that
; fits the screen: with 4 on a 120 Hz screen, a 30 fps game is shown at 120
; and a 60 fps game at 120 (x2).
multiplier = 2
; Frames are only added while the result stays at or under this many a
; second. 0: the screen's refresh rate. With VRR, a value under the screen's
; rate keeps the result inside the VRR range: 100 on a 120 Hz screen shows a
; 30 fps game at 90 (x3) instead of 120 (x4) with multiplier = 4. It cannot
; make 60 -> 120 fit under 120: that game is then shown at 60.
max_output_hz = 0
; 1: a small readout in the top left corner while playing: the game's rate,
; the rate shown, the screen's rate. The menu (Tab) switches it.
show_rates = 0
; While frame generation is off (F8): 1 = send mpv every frame (lowest
; latency), 0 = new frames only.
all_frames_when_off = 1

[dlss5]
; DLSS 5 neural rendering (NVIDIA RTX 30, 40 and 50 series). auto: on with
; one of those GPUs, off otherwise. on: always try (e.g. an RTX 20). off:
; starts off; on an RTX 30, 40 or 50 the menu (Tab) can switch it on. F6
; toggles it while playing.
enabled = auto

[audio]
; 1: play the card's sound (low latency, through mpv). 0: no sound.
enabled = 1
; The audio input to use. Empty: the card's own (found automatically).
device =
; Volume 0-100 and boost 100-400 (%). Keys: Up/Down volume, M mute,
; Ctrl+Up/Down boost. What you change while playing (keys or the menu) is
; kept here.
volume = 100
boost = 100
; Holds the sound back (ms) so it matches the picture; DLSS 5 and frame
; generation make the picture a little later. Keys: [ and ].
delay_ms = 0
; minimum: lowest latency. default: steadier if the sound crackles.
period = minimum

[window]
; fullscreen (mpv's own full screen, as f or a double-click switches it),
; borderless (a window without a frame that fills the screen) or windowed.
; The menu (Tab) switches it and keeps the choice here.
mode = borderless
; Full screen on a screen of another shape than the picture (16:9 on an
; ultrawide). 0: mpv covers the screen, black bars included. 1: mpv's window
; takes the picture only, black around it, so DLSS 5 and frame generation skip
; the bars. Some PCs then compose the window (no overlay plane for it) and
; frame generation falls short of double: check the menu's fps.
skip_black_bars = 0

[launcher]
; 1: Glasir.exe opens its window first (card, picture, enhancements, sound).
; 0: it plays at once; hold Shift while starting it to see the window.
show = 1

[update]
; 1: the window asks GitHub (github.com/Abhito/Glasir) whether a newer Glasir
; is out, and offers to install it. Nothing is installed unasked.
check = 1

[debug]
; 1: mpv writes a detailed log to logs\mpv.log.
mpv_log = 0
)";
}

bool load_settings(const std::wstring &path, Settings &s, std::wstring &error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::ofstream out(path, std::ios::binary);
        if (out) {
            const std::string text = default_settings_text();
            out << text;
            log(L"wrote default settings to {}", path);
        } else {
            log(L"could not write {}; using defaults", path);
        }
        return true;
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) bytes.erase(0, 3);
    std::wistringstream lines(widen(bytes));
    std::wstring line, section;
    for (int number = 1; std::getline(lines, line); ++number) {
        line = trim(line);
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line.front() == L'[') {
            if (line.back() != L']') {
                error = L"glasir.ini line " + std::to_wstring(number) + L": " + line;
                return false;
            }
            section = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) {
            error = L"glasir.ini line " + std::to_wstring(number) + L": " + line + L" (expected key = value)";
            return false;
        }
        std::wstring value = line.substr(eq + 1);
        // A comment after a value needs a space before it: "fps = 60 ; note".
        for (size_t i = 1; i < value.size(); ++i)
            if ((value[i] == L';' || value[i] == L'#') && iswspace(value[i - 1])) {
                value.resize(i);
                break;
            }
        if (!apply(s, section, lower(trim(line.substr(0, eq))), trim(value), error)) return false;
    }
    return true;
}

namespace {
std::vector<IniValue> to_ini(const std::vector<SettingValue> &values) {
    std::vector<IniValue> out;
    for (const auto &v : values) out.push_back({narrow(v.section), narrow(v.key), narrow(v.value)});
    return out;
}
}  // namespace

std::string update_settings_text(const std::string &text, const std::vector<SettingValue> &values) {
    return ini_set_values(text, to_ini(values));
}

bool save_settings(const std::wstring &path, const std::vector<SettingValue> &values) {
    if (ini_save_values(path, to_ini(values))) return true;
    log(L"could not write {}", path);
    return false;
}

bool apply_override(Settings &s, const std::wstring &assignment, std::wstring &error) {
    const size_t dot = assignment.find(L'.');
    const size_t eq = assignment.find(L'=');
    if (dot == std::wstring::npos || eq == std::wstring::npos || dot > eq) {
        error = L"--set " + assignment + L" (expected section.key=value)";
        return false;
    }
    return apply(s, lower(trim(assignment.substr(0, dot))), lower(trim(assignment.substr(dot + 1, eq - dot - 1))),
                 trim(assignment.substr(eq + 1)), error);
}

}  // namespace glasir
