// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2026 Abhito
// zz-framegen.addon64 - frame generation inside mpv for the app.
//
// A ReShade add-on that runs last, after the DLSS 5 pass has drawn into mpv's
// back buffer. For every real frame B it keeps a copy and draws the frame in
// between the previous real frame A and B over it, which mpv's own present
// then shows; half a source frame after the GPU finished that frame it puts B
// back and presents it through the original swap chain (FrameGen below has the
// details). A 30 fps source
// reaches the screen at 60, a 60 fps source at 120 - when the display can show
// it.
//
// The frame in between is motion-compensated (fg_interp.h, flow.hlsl; the
// design and measurements are in README.md); Method=blend mixes A and B 50/50.
//
// It only makes sense when mpv gets real frames, not the pairs of repeats a 30
// fps game captured at 60 arrives as. The app's dedup sends only new
// frames and opens a link to this add-on (framegen_link.h) with when each
// one arrived and how many mpv had to drop; without that link the add-on stays
// off unless RequireDedupLauncher=0.
//
// Settings live in ReShade.ini, in the section kSection names; F8 toggles it.

#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <wincodec.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <reshade.hpp>

#include "app_env.h"
#include "fg_anr.h"
#include "fg_dlss.h"
#include "fg_vfg.h"
#include "keep_regions.h"
#include "fg_hud.h"
#include "fg_interp.h"
#include "fg_pacing.h"
#include "fg_screen.h"
#include "fg_vblank.h"
#include "framegen_link.h"
#include "overlay.h"
#include "reshade_ui.h"

using Microsoft::WRL::ComPtr;

extern "C" __declspec(dllexport) const char *NAME = APP_NAME " Frame Generation";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Raises the frame rate of " APP_NAME " by presenting generated frames between each pair of real frames.";

namespace {

constexpr const char *kSection = APP_ENV_PREFIX "FRAMEGEN";

void log_line(reshade::log::level level, const char *fmt, ...) {
    char text[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    reshade::log::message(level, text);
}

std::string utf8(const std::wstring &text) {
    std::string out;
    if (text.empty()) return out;
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return out;
    out.resize(size_t(n));
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// ---- Settings ----------------------------------------------------------------

struct Settings {
    bool enabled = true;
    bool require_dedup = true;  // only run with the link from the app's dedup
    fg::Method method = fg::Method::motion;  // motion = motion-compensated, blend = 50/50 mix
    bool fast = false;          // Quality=fast: fewer candidates per pixel, about half the drawing cost
    // Backend=dlss: the frames in between by NVIDIA's DLSS Frame Generation
    // (fg_dlss.h) where the GPU has it; the app's own (fg_interp.h) where not,
    // and whenever it fails.
    bool dlss = false;
    // Backend=anr: by Apex Neural Rendering's frame generation (fg_anr.h),
    // given the own search's motion; never together with dlss or video.
    bool anr = false;
    bool video = false;           // NVIDIA Video Frame Generation makes the frames (fg_vfg.h); not together with dlss
    int video_quality = 1;        // its model: 0 low, 1 medium, 2 high
    // Keep: areas the frames in between leave alone (keep_regions.h): as a
    // whole, or (find_hud) only where a HUD is found in them.
    std::vector<app::KeepRegion> keep;
    // Hud=1: "find the HUD everywhere" - the frames in between show the
    // nearer real frame wherever a HUD is found (fg_hud.h), with no area drawn.
    bool hud = false;
    int multiplier = 2;         // frames shown per source frame, at most: 2, 3 or 4
    bool marker = false;        // magenta square on generated frames
    bool log_stats = true;      // a line in ReShade.log every 5 s
    int max_output_hz = 0;      // 0 = the display's refresh rate
    int reset_ms = 100;         // a longer gap between real frames starts over
    int toggle_key = VK_F8;
    int capture_key = VK_F9;    // saves the frames around this moment (Capture below)
    bool dedup_when_off = false;  // 1: mpv gets new frames only while frame generation is off too
    // The switches (the panel has them; switch_setting below for how they are kept).
    bool picture_only = true;     // with black bars around the picture, the generator works on the picture alone
    bool edge_rules = true;       // the strip along the frame's edges that only one frame shows in a camera turn
    bool still_hud = true;        // a HUD over fast scenery is held where it is
    bool history_search = false;  // a test: the search also tries what each block did over the last pair
    bool hud_clean = true;        // with the HUD mask: the app's own generator draws from the frames without their HUD
    bool step_down = true;        // x4 or x3 that cannot be held: one fewer for a while (fg_pacing.h, StepDown)
    bool trim = true;             // the screen behind by a frame: that game frame gets one frame fewer, not none
    std::string mpv_pipe = APP_ID;
    // What the exe found out about the PC's screens at start (its
    // environment variable SCREENS, fg_screen.h), not from
    // ReShade.ini. Which of them counts is decided while running: the one
    // the window is on (FrameGen::watch_screen).
    std::vector<fg::ScreenFacts> screens;
    // Without that list, the older pair about one screen that is not named
    // (env DISPLAY_VRR and env DISPLAY_HZ): taken as the window's.
    bool one_told = false;
    fg::pacing::Vrr vrr = fg::pacing::Vrr::Unknown;
    double launcher_hz = 0;  // 0: not told
};

Settings g_settings;
const Settings kDefaults;

// Every value is read as text and parsed here: ReShade's own bool and int
// readers take neither "true" nor "0x77". A missing key gets the default
// written back so it shows up in ReShade.ini; a value that does not parse is
// left alone and logged.
bool read_text(const char *key, std::string &out) {
    char text[256] = {};
    size_t size = sizeof(text);
    if (!reshade::get_config_value(nullptr, kSection, key, text, &size)) return false;
    out = text;
    while (!out.empty() && isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
    while (!out.empty() && isspace(static_cast<unsigned char>(out.front()))) out.erase(out.begin());
    return true;
}

void bad_setting(const char *key, const std::string &text) {
    log_line(reshade::log::level::warning, "[%s] %s=%s is not a value this add-on understands; using the default",
             kSection, key, text.c_str());
}

bool parse_bool(std::string text, bool &value) {
    for (auto &c : text) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (text == "1" || text == "true" || text == "yes" || text == "on")
        value = true;
    else if (text == "0" || text == "false" || text == "no" || text == "off")
        value = false;
    else
        return false;
    return true;
}

void setting(const char *key, bool &value) {
    std::string text;
    if (!read_text(key, text))
        reshade::set_config_value(nullptr, kSection, key, value ? "1" : "0");
    else if (!parse_bool(text, value))
        bad_setting(key, text);
}

// A switch (PictureOnly, EdgeRules, StillHud, HistorySearch, HudClean, StepDown) is in ReShade.ini
// only while it differs from its default: a missing key stays missing, and one
// that says what the default says is taken out - here, and by the panel when a
// switch is put back. So a later version can change a default for everyone
// who did not choose otherwise. (A null value removes the key: ReShade 6.4 on,
// and this add-on needs a newer one to load at all.)
void forget_setting(const char *key) {
    reshade::set_config_value(nullptr, kSection, key, static_cast<const char *>(nullptr));
}

void switch_setting(const char *key, bool &value) {
    const bool standard = value;
    std::string text;
    if (!read_text(key, text)) return;
    if (!parse_bool(text, value))
        bad_setting(key, text);
    else if (value == standard)
        forget_setting(key);
}

void setting(const char *key, int &value) {
    std::string text;
    if (!read_text(key, text)) {
        reshade::set_config_value(nullptr, kSection, key, std::to_string(value).c_str());
        return;
    }
    char *end = nullptr;
    const long parsed = strtol(text.c_str(), &end, 0);  // base 0: "119" and "0x77" alike
    if (text.empty() || *end != '\0')
        bad_setting(key, text);
    else
        value = static_cast<int>(parsed);
}

void setting(const char *key, std::string &value) {
    std::string text;
    if (read_text(key, text))
        value = text;
    else
        reshade::set_config_value(nullptr, kSection, key, value.c_str());
}

void load_settings() {
    Settings &s = g_settings;
    setting("Enabled", s.enabled);
    setting("RequireDedupLauncher", s.require_dedup);
    setting("DebugMarker", s.marker);
    setting("LogStats", s.log_stats);
    setting("MaxOutputHz", s.max_output_hz);
    setting("ResetMs", s.reset_ms);
    setting("ToggleKey", s.toggle_key);
    setting("MpvPipe", s.mpv_pipe);
    setting("CaptureKey", s.capture_key);
    setting("DedupWhenOff", s.dedup_when_off);
    switch_setting("PictureOnly", s.picture_only);
    switch_setting("EdgeRules", s.edge_rules);
    switch_setting("StillHud", s.still_hud);
    switch_setting("HistorySearch", s.history_search);
    switch_setting("HudClean", s.hud_clean);
    switch_setting("StepDown", s.step_down);
    switch_setting("Trim", s.trim);
    log_line(reshade::log::level::info,
             "switches: PictureOnly=%d EdgeRules=%d StillHud=%d HistorySearch=%d HudClean=%d StepDown=%d Trim=%d",
             s.picture_only, s.edge_rules, s.still_hud, s.history_search, s.hud_clean, s.step_down, s.trim);
    std::string method = s.method == fg::Method::blend ? "blend" : "motion";
    setting("Method", method);
    for (auto &c : method) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (method == "blend")
        s.method = fg::Method::blend;
    else if (method == "motion")
        s.method = fg::Method::motion;
    else
        bad_setting("Method", method);
    std::string quality = s.fast ? "fast" : "high";
    setting("Quality", quality);
    for (auto &c : quality) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (quality == "high" || quality == "fast")
        s.fast = quality == "fast";
    else
        bad_setting("Quality", quality);
    // "own": the app's own generator. It went by the app's name before:
    // read as the same, and written the new way. (An older name still is
    // put right by the exe, which writes this key at every start.)
    std::string backend = s.anr ? "anr" : s.dlss ? "dlss" : "own";
    setting("Backend", backend);
    for (auto &c : backend) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (backend == APP_ID) {
        backend = "own";
        reshade::set_config_value(nullptr, kSection, "Backend", "own");
    }
    if (backend == "own" || backend == "dlss" || backend == "video" || backend == "anr") {
        s.dlss = backend == "dlss";
        s.anr = backend == "anr";
        s.video = backend == "video";
    } else {
        bad_setting("Backend", backend);
    }
    // NVIDIA Video Frame Generation's model: a larger one is cleaner and takes more GPU time.
    std::string video_quality = s.video_quality == 0 ? "low" : s.video_quality == 2 ? "high" : "medium";
    setting("VideoQuality", video_quality);
    for (auto &c : video_quality) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (video_quality == "low" || video_quality == "medium" || video_quality == "high")
        s.video_quality = video_quality == "low" ? 0 : video_quality == "high" ? 2 : 1;
    else
        bad_setting("VideoQuality", video_quality);
    std::string keep = app::reshade_keep_text(s.keep);
    setting("Keep", keep);
    if (!app::parse_keep_regions(keep, s.keep)) bad_setting("Keep", keep);
    setting("Hud", s.hud);
    s.reset_ms = std::clamp(s.reset_ms, 20, 1000);
    setting("Multiplier", s.multiplier);
    if (s.multiplier < 2 || s.multiplier > fg::pacing::kMaxMultiplier) {
        bad_setting("Multiplier", std::to_string(s.multiplier));
        s.multiplier = 2;
    }
    if (const std::string vrr = app::env("DISPLAY_VRR"); !vrr.empty()) {
        s.one_told = true;
        s.vrr = _stricmp(vrr.c_str(), "on") == 0    ? fg::pacing::Vrr::On
                : _stricmp(vrr.c_str(), "off") == 0 ? fg::pacing::Vrr::Off
                                                    : fg::pacing::Vrr::Unknown;
    }
    if (const std::string hz = app::env("DISPLAY_HZ"); !hz.empty()) {
        s.one_told = true;
        s.launcher_hz = std::max(0.0, atof(hz.c_str()));
    }
    // Every screen, as the exe found them at start (monitor names may
    // hold any letters: read wide, kept as UTF-8).
    if (const std::wstring screens = app::env(L"SCREENS"); !screens.empty())
        s.screens = fg::parse_screens(utf8(screens.c_str()));
    // The launcher passes the full \\.\pipe\name form to mpv; take that too.
    const std::string prefix = "\\\\.\\pipe\\";
    if (s.mpv_pipe.compare(0, prefix.size(), prefix) == 0) s.mpv_pipe.erase(0, prefix.size());
}

// ---- Time ----------------------------------------------------------------------

double qpc_period() {
    static const double period = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / double(f.QuadPart);
    }();
    return period;
}

double qpc_seconds() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart) * qpc_period();
}

// Sleeps on a high-resolution waitable timer until shortly before the target,
// then spins the rest, so the real frame goes out within a few microseconds.
class Sleeper {
public:
    Sleeper() {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    ~Sleeper() {
        if (timer_) CloseHandle(timer_);
    }

    void until(double target) {
        constexpr double kSpin = 0.0007;
        const double wait = target - qpc_seconds() - kSpin;
        if (timer_ && wait > 0) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(wait * 1e7);  // relative, 100 ns units
            if (SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0))
                WaitForSingleObject(timer_, 1000);
        }
        while (qpc_seconds() < target) YieldProcessor();
    }

private:
    HANDLE timer_ = nullptr;
};

// ---- The link from the app --------------------------------------------

class Link {
public:
    // Looks for the block once a second until the app has made it
    // (it starts mpv first, then creates the block under mpv's process id).
    void poll(double now) {
        if (view_ || now < next_try_) return;
        next_try_ = now + 1.0;
        wchar_t name[64];
        swprintf(name, 64, APP_FRAMEGEN_LINK_NAME, GetCurrentProcessId());
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
        if (!mapping) return;
        auto *view = static_cast<FrameGenLink *>(
            MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(FrameGenLink)));
        if (!view || view->magic != APP_FRAMEGEN_LINK_MAGIC || view->version != APP_FRAMEGEN_LINK_VERSION) {
            if (view) UnmapViewOfFile(view);
            CloseHandle(mapping);
            return;
        }
        mapping_ = mapping;
        view_ = view;
        log_line(reshade::log::level::info, "linked to " APP_NAME ": new frames only, with their arrival times");
    }

    void close() {
        if (view_) UnmapViewOfFile(view_);
        if (mapping_) CloseHandle(mapping_);
        view_ = nullptr;
        mapping_ = nullptr;
    }

    bool linked() const { return view_ != nullptr; }

    // Asks the app for the repeats too (frame generation off) or for
    // new frames only.
    void send_repeats(bool on) {
        const LONG want = on ? 1 : 0;
        if (view_ && view_->send_repeats != want) InterlockedExchange(&view_->send_repeats, want);
    }
    LONG64 dropped() const { return view_ ? view_->dropped : 0; }

    // The mean time between the last new frames from the source, leaving out
    // pauses longer than `gap`. 0 when there are too few to tell. A mean, not a
    // median: a source that delivers in uneven steps (31, 31, 31, 46 ms) has
    // its median well below its real rate, and the real rate is what decides
    // whether twice it fits the display.
    double source_interval(double gap) const {
        double sum = 0;
        int n = 0;
        each_gap(gap, kAll, [&](double d) {
            sum += d;
            ++n;
        });
        return n >= 3 ? sum / n : 0;
    }

    // The shortest of the last few gaps: how soon the next new frame may
    // come, also while the rate is going up (the mean lags) or uneven. 0
    // when there are too few to tell.
    double shortest_recent_gap(double gap) const {
        double least = 1e9;
        int n = 0;
        each_gap(gap, 4, [&](double d) {
            least = std::min(least, d);
            ++n;
        });
        return n >= 2 ? least : 0;
    }

    // How unevenly new frames arrive: the longest of the last gaps minus the
    // shortest, pauses longer than `gap` left out. Under 0 with too few.
    double gap_spread(double gap) const {
        double least = 1e9, most = 0;
        int n = 0;
        each_gap(gap, kAll, [&](double d) {
            least = std::min(least, d);
            most = std::max(most, d);
            ++n;
        });
        return n >= 4 ? most - least : -1;
    }

    // New frames so far: changes when the source delivered one.
    LONG64 published() const { return view_ ? view_->published : 0; }

    // When new frame number n (1 = the first) arrived, in seconds; only the
    // last kKept are there (the writer may be filling the oldest slot).
    static constexpr int kKept = FrameGenLink::kStamps - 1;
    double arrived(LONG64 n) const {
        return view_ ? double(view_->stamps[(n - 1) % FrameGenLink::kStamps]) * qpc_period() : 0;
    }

    // Seconds since the source last delivered a new frame.
    double since_last(double now) const {
        if (!view_) return 1e9;
        const LONG64 p = view_->published;
        if (p == 0) return 1e9;
        return now - double(view_->stamps[(p - 1) % FrameGenLink::kStamps]) * qpc_period();
    }

private:
    static constexpr int kAll = FrameGenLink::kStamps;

    // Calls f with the gaps between the last `count` pairs of new frames,
    // newest first, in seconds, leaving out pauses longer than `gap`.
    template <class F>
    void each_gap(double gap, int count, F &&f) const {
        if (!view_) return;
        const LONG64 p = view_->published;
        constexpr int kStamps = FrameGenLink::kStamps;
        // The slot the writer fills next is the oldest; leave it out.
        const int pairs = std::min(count, static_cast<int>(std::min<LONG64>(p, kStamps - 1)) - 1);
        for (int k = 1; k <= pairs; ++k) {
            const LONG64 newer = view_->stamps[(p - k) % kStamps];
            const LONG64 older = view_->stamps[(p - k - 1) % kStamps];
            const double d = double(newer - older) * qpc_period();
            if (d > 0.002 && d <= gap) f(d);
        }
    }

    HANDLE mapping_ = nullptr;
    FrameGenLink *view_ = nullptr;
    double next_try_ = 0;
};

Link g_link;

// ---- mpv IPC ------------------------------------------------------------------

// Sends one IPC line to mpv, on a thread of its own so the render thread never
// waits on the pipe. The thread holds a reference to this module and drops it
// on exit, so mpv closing meanwhile cannot unload the code under it.
void mpv_send(const std::string &line) {
    struct Job {
        std::wstring pipe;
        std::string line;
    };
    auto *job = new Job;
    job->pipe = L"\\\\.\\pipe\\" + std::wstring(g_settings.mpv_pipe.begin(), g_settings.mpv_pipe.end());
    job->line = line;

    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&mpv_send), &self)) {
        delete job;
        return;
    }
    auto run = [](LPVOID param) -> DWORD {
        auto *job = static_cast<Job *>(param);
        for (int attempt = 0; attempt < 2; ++attempt) {
            HANDLE pipe = CreateFileW(job->pipe.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(pipe, job->line.data(), static_cast<DWORD>(job->line.size()), &written, nullptr);
                CloseHandle(pipe);
                break;
            }
            if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(job->pipe.c_str(), 250)) break;
        }
        delete job;
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&mpv_send), &module);
        FreeLibraryAndExitThread(module, 0);
    };
    HANDLE thread = CreateThread(nullptr, 0, run, job, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        delete job;
        FreeLibrary(self);
    }
}

// Asks the exe (an IPC client of mpv) to save a bug report, with the
// frames in folder; mpv passes script messages on to its clients. Without
// frames, why says why not: "off" (frame generation is off), "unavailable"
// (it cannot run here) or "failed" (they could not be saved).
void mpv_bug_report(const std::wstring &folder, const char *why = "") {
    std::string utf8;
    if (!folder.empty()) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, folder.data(), int(folder.size()), nullptr, 0, nullptr, nullptr);
        utf8.resize(size_t(n > 0 ? n : 0));
        WideCharToMultiByte(CP_UTF8, 0, folder.data(), int(folder.size()), utf8.data(), n, nullptr, nullptr);
    }
    std::string quoted;
    for (char c : utf8) {
        if (c == '\\' || c == '"') quoted += '\\';
        quoted += c;
    }
    mpv_send("{\"command\":[\"script-message\",\"app-bug-report\",\"" + quoted + "\",\"" + why + "\"]}\n");
}

// ---- Capture ------------------------------------------------------------------

// The capture key saves what frame generation works with at that moment: five
// real frames in a row as the DLSS 5 pass left them (f01.png - f05.png) and
// the frames the add-on drew between them (between_f01_f02.png, ...), into
// Captures\<date-time>\ in the app folder. eval\fg_eval.exe reads such
// a folder like one of its clips: it makes the frame between f01 and f03 and
// compares it with f02, and so on - a score on the game's own pictures.
//
// The copies go to the GPU's staging memory as the frames pass; two frames
// after the last one they are read back (one hitch of a few tens of
// milliseconds) and a thread of its own writes the files. 10-bit HDR (PQ)
// frames are written as 16-bit PNG, everything else as 8-bit: an SDR picture
// in a 10-bit back buffer (mpv picks one on a 10-bit desktop) too, so a 16-bit
// capture always means PQ to the viewer and the eval tools.
class Capture {
public:
    static constexpr int kReals = 5;

    bool busy() const { return active_ || wait_ >= 0; }
    bool active() const { return active_; }

    // Nothing is shown on mpv's OSD here: mpv draws it into the back buffer,
    // and it would be in the frames being captured.
    // pq: the back buffer holds HDR10 (PQ) values, not SDR ones.
    // report: a bug report waits for the frames (the key, not a timed test).
    bool start(ID3D11Device *device, const D3D11_TEXTURE2D_DESC &bb, bool pq, bool report) {
        reset();
        report_ = report;
        if (bb.Format != DXGI_FORMAT_R10G10B10A2_UNORM && bb.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
            bb.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            log_line(reshade::log::level::warning, "capture: back buffer format %d is not one it can save", bb.Format);
            return false;
        }
        SYSTEMTIME t;
        GetLocalTime(&t);
        swprintf(stamp_, 32, L"%04u%02u%02u-%02u%02u%02u-%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                 t.wSecond, t.wMilliseconds);
        device_ = device;
        pq_ = pq;
        desc_ = bb;
        desc_.BindFlags = 0;
        desc_.MiscFlags = 0;
        desc_.Usage = D3D11_USAGE_STAGING;
        desc_.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        active_ = true;
        log_line(reshade::log::level::info, "capture: saving the next %d frames", kReals);
        return true;
    }

    // A frame drawn between the last two real frames: the first of them
    // (part 1) comes before its later real frame is captured, the others (x3,
    // x4) after it. With one frame in between the name is as it always was
    // (the eval tools read it); with more, _1, _2, _3 are added.
    void between(ID3D11DeviceContext *ctx, ID3D11Texture2D *drawn, int part = 1, int parts = 1) {
        if (!active_ && wait_ < 0) return;
        const int earlier = part == 1 ? reals_ : reals_ - 1;
        if (earlier < 1 || (part == 1 && !active_)) return;
        // After the last real frame only its own pair's later frames are
        // taken, and only if that pair had frames in between at all: the
        // pairs that follow (while the copies are read back) would be saved
        // under the same names, over them or in their place.
        if (!active_) {
            if (!tail_) return;
            if (part == parts) tail_ = false;
        } else if (part == 1) {
            first_before_ = reals_ + 1;
        }
        char name[48];
        if (parts == 1)
            snprintf(name, sizeof(name), "between_f%02d_f%02d", earlier, earlier + 1);
        else
            snprintf(name, sizeof(name), "between_f%02d_f%02d_%d", earlier, earlier + 1, part);
        add(ctx, drawn, name);
    }

    void real(ID3D11DeviceContext *ctx, ID3D11Texture2D *frame) {
        if (!active_) return;
        char name[16];
        snprintf(name, sizeof(name), "f%02d", ++reals_);
        add(ctx, frame, name);
        if (reals_ == kReals) {
            active_ = false;
            tail_ = first_before_ == kReals;
            wait_ = 2;
        }
    }

    // Every frame: reads the copies back once the GPU is done with them.
    void tick(ID3D11DeviceContext *ctx) {
        if (wait_ < 0 || active_) return;
        if (wait_ > 0) {
            --wait_;
            return;
        }
        wait_ = -1;
        auto *job = new Job;
        job->expected = shots_.size();
        job->stamp = stamp_;
        job->width = desc_.Width;
        job->height = desc_.Height;
        job->format = desc_.Format;
        job->deep = desc_.Format == DXGI_FORMAT_R10G10B10A2_UNORM && pq_;
        job->report = report_;
        report_ = false;  // the write thread sends it now
        for (auto &shot : shots_) {
            D3D11_MAPPED_SUBRESOURCE map{};
            if (FAILED(ctx->Map(shot.staging.Get(), 0, D3D11_MAP_READ, 0, &map))) continue;
            Image image;
            image.name = shot.name;
            image.pixels.resize(size_t(desc_.Width) * desc_.Height * 4);
            for (UINT y = 0; y < desc_.Height; ++y)
                memcpy(image.pixels.data() + size_t(y) * desc_.Width * 4,
                       static_cast<const BYTE *>(map.pData) + size_t(y) * map.RowPitch, size_t(desc_.Width) * 4);
            ctx->Unmap(shot.staging.Get(), 0);
            job->images.push_back(std::move(image));
        }
        reset();
        write(job);
    }

    // Stops a capture under way; a bug report waiting for it is made without
    // frames (tell = false: mpv is closing, no one to tell).
    void reset(bool tell = true) {
        if (tell && report_ && (active_ || wait_ >= 0)) mpv_bug_report(L"", "failed");
        shots_.clear();
        device_.Reset();
        report_ = false;
        active_ = false;
        reals_ = 0;
        tail_ = false;
        first_before_ = 0;
        wait_ = -1;
    }

private:
    struct Shot {
        std::string name;
        ComPtr<ID3D11Texture2D> staging;
    };
    struct Image {
        std::string name;
        std::vector<BYTE> pixels;  // 4 bytes a pixel, tightly packed
    };
    struct Job {
        size_t expected = 0;  // images copied; fewer read back is a failure
        std::wstring stamp;
        UINT width = 0, height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        bool deep = false;  // 16-bit PNG (10-bit PQ); otherwise 8-bit
        bool report = false;  // a bug report waits for the frames
        std::vector<Image> images;
    };

    void add(ID3D11DeviceContext *ctx, ID3D11Texture2D *source, const char *name) {
        Shot shot;
        shot.name = name;
        if (FAILED(device_->CreateTexture2D(&desc_, nullptr, &shot.staging))) {
            log_line(reshade::log::level::warning, "capture: no memory for %s", name);
            return;
        }
        ctx->CopyResource(shot.staging.Get(), source);
        shots_.push_back(std::move(shot));
    }

    // Writes the files on a thread of its own, which holds a reference to this
    // module so mpv closing meanwhile cannot unload the code under it.
    static void write(Job *job) {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&Capture::write), &self)) {
            if (job->report) mpv_bug_report(L"", "failed");
            delete job;
            return;
        }
        auto run = [](LPVOID param) -> DWORD {
            auto *job = static_cast<Job *>(param);
            const std::wstring folder = save(*job);
            const bool report = job->report;
            delete job;
            if (report) {
                if (folder.empty()) app::overlay::toast("Frame generation: could not save the frames, see ReShade.log");
                mpv_bug_report(folder, folder.empty() ? "failed" : "");  // the exe says when it is saved
            } else {
                app::overlay::toast(folder.empty() ? "Frame generation: capture failed, see ReShade.log"
                                             : "Frame generation: frames saved to Captures");
            }
            HMODULE module = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&Capture::write), &module);
            FreeLibraryAndExitThread(module, 0);
        };
        HANDLE thread = CreateThread(nullptr, 0, run, job, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        } else {
            if (job->report) mpv_bug_report(L"", "failed");
            delete job;
            FreeLibrary(self);
        }
    }

    static std::wstring save(const Job &job) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring folder(exe);
        folder.erase(folder.find_last_of(L"\\/") + 1);
        folder += L"..\\Captures";  // in the app folder (mpv.exe is in engine)
        CreateDirectoryW(folder.c_str(), nullptr);
        // Its own folder, named when the key was pressed; never one in use.
        const std::wstring base = folder + L"\\" + job.stamp;
        folder = base;
        for (int n = 2; !CreateDirectoryW(folder.c_str(), nullptr); ++n) {
            if (GetLastError() != ERROR_ALREADY_EXISTS || n > 99) {
                log_line(reshade::log::level::error, "capture: could not create its folder");
                return {};
            }
            folder = base + L"-" + std::to_wstring(n);
        }
        if (job.images.empty() || job.images.size() < job.expected) {
            log_line(reshade::log::level::error, "capture: only %zu of %zu frames could be read back", job.images.size(),
                     job.expected);
            return {};
        }

        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        ComPtr<IWICImagingFactory> wic;
        bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
        for (const auto &image : job.images) {
            if (!ok) break;
            const std::wstring path = folder + L"\\" + std::wstring(image.name.begin(), image.name.end()) + L".png";
            ok = png(wic.Get(), path, job, image.pixels);
        }
        wic.Reset();
        if (com) CoUninitialize();
        if (!ok) {
            log_line(reshade::log::level::error, "capture: writing the PNG files failed");
            return {};
        }
        log_line(reshade::log::level::info, "capture: %zu frames saved (%ux%u, format %d, %s)", job.images.size(),
                 job.width, job.height, job.format, job.deep ? "16-bit PQ" : "8-bit SDR");
        return folder;
    }

    static bool png(IWICImagingFactory *wic, const std::wstring &path, const Job &job, const std::vector<BYTE> &pixels) {
        // 10-bit PQ as 16-bit RGBA, the rest as BGRA (the order WIC's PNG
        // encoder takes); 10-bit SDR is rounded to 8 bits.
        const bool deep = job.deep;
        const size_t n = size_t(job.width) * job.height;
        std::vector<BYTE> out(n * (deep ? 8 : 4));
        for (size_t i = 0; i < n; ++i) {
            const BYTE *px = pixels.data() + i * 4;
            if (job.format == DXGI_FORMAT_R10G10B10A2_UNORM) {
                UINT32 v;
                memcpy(&v, px, 4);
                UINT16 c[4] = {UINT16(v & 1023), UINT16((v >> 10) & 1023), UINT16((v >> 20) & 1023), UINT16(v >> 30)};
                if (deep) {
                    for (int k = 0; k < 3; ++k) c[k] = UINT16((c[k] << 6) | (c[k] >> 4));
                    c[3] = UINT16(c[3] * 21845);
                    memcpy(out.data() + i * 8, c, 8);
                } else {
                    BYTE *o = out.data() + i * 4;
                    for (int k = 0; k < 3; ++k) o[2 - k] = BYTE((c[k] * 255u + 511u) / 1023u);
                    o[3] = BYTE(c[3] * 85u);
                }
            } else if (job.format == DXGI_FORMAT_R8G8B8A8_UNORM) {
                out[i * 4 + 0] = px[2], out[i * 4 + 1] = px[1], out[i * 4 + 2] = px[0], out[i * 4 + 3] = px[3];
            } else {
                memcpy(out.data() + i * 4, px, 4);
            }
        }
        WICPixelFormatGUID format = deep ? GUID_WICPixelFormat64bppRGBA : GUID_WICPixelFormat32bppBGRA;
        const WICPixelFormatGUID wanted = format;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        return SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
               SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
               SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
               SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
               SUCCEEDED(frame->SetSize(job.width, job.height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
               IsEqualGUID(format, wanted) &&
               SUCCEEDED(frame->WritePixels(job.height, job.width * (deep ? 8 : 4), UINT(out.size()), out.data())) &&
               SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }

    ComPtr<ID3D11Device> device_;
    D3D11_TEXTURE2D_DESC desc_{};
    bool pq_ = false;
    bool report_ = false;
    wchar_t stamp_[32] = {};
    std::vector<Shot> shots_;
    bool active_ = false;
    bool tail_ = false;  // the last real frame is in: its pair's later frames in between (x3, x4) may still come
    int first_before_ = 0;  // the real frame the latest first frame in between was drawn before, by its number
    int reals_ = 0;
    int wait_ = -1;  // frames until the copies are read back; -1 = nothing to read
};

// ---- Display ------------------------------------------------------------------

const char *color_space_name(reshade::api::color_space cs) {
    switch (cs) {
    case reshade::api::color_space::srgb: return "sRGB (SDR)";
    case reshade::api::color_space::scrgb: return "scRGB";
    case reshade::api::color_space::hdr10_pq: return "HDR10 (PQ)";
    case reshade::api::color_space::hdr10_hlg: return "HDR10 (HLG)";
    default: return "unknown";
    }
}

// The window the swap chain draws into (mpv's), null when it has none.
HWND swapchain_window(IDXGISwapChain *swapchain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    return SUCCEEDED(swapchain->GetDesc(&desc)) ? desc.OutputWindow : nullptr;
}

// The refresh rate of the screen the swap chain is on as DXGI has it, 0 when
// unknown. Only for when the window's own screen cannot be found
// (fg::window_screen): DXGI's outputs are those of the factory mpv made at
// start and go stale when the set of screens changes.
int dxgi_display_hz(IDXGISwapChain *swapchain) {
    ComPtr<IDXGIOutput> output;
    if (FAILED(swapchain->GetContainingOutput(&output))) return 0;
    DXGI_OUTPUT_DESC desc{};
    if (FAILED(output->GetDesc(&desc))) return 0;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) return 0;
    return mode.dmDisplayFrequency > 1 ? static_cast<int>(mode.dmDisplayFrequency) : 0;
}

// Presents still queued for the screen, or -1 when the swap chain cannot say:
// the call fails, or succeeds with the statistics left at zero (mpv's own
// d3d11 code has seen that), or gives a count no queue could hold.
int queued_presents(IDXGISwapChain *swapchain) {
    UINT last = 0;
    DXGI_FRAME_STATISTICS stats{};
    if (FAILED(swapchain->GetLastPresentCount(&last)) || FAILED(swapchain->GetFrameStatistics(&stats))) return -1;
    if (stats.PresentCount == 0) return -1;
    const UINT queued = last - stats.PresentCount;
    return queued <= 8 ? static_cast<int>(queued) : -1;
}

// ---- Trace --------------------------------------------------------------------

// env FRAMEGEN_TRACE=<file>: one CSV line per real frame, for the whole run.
FILE *g_trace = nullptr;

void trace_open() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    const std::string path = app::env("FRAMEGEN_TRACE");
    if (!path.empty() && fopen_s(&g_trace, path.c_str(), "w") == 0)
        fprintf(g_trace,
                "time_s,dt_ms,interval_ms,source_interval_ms,queued,event,source_age_ms,effects_ms,drawn_ms,real_ms,"
                "frame,of\n");
}

// One line. The hold thread writes the lines of held frames (once they went
// out), mpv's thread the rest; the C runtime keeps each line whole.
struct TraceLine {
    double now = 0, dt = 0, interval = 0, source_interval = 0;
    int queued = -1;
    const char *event = "";
    double source_age = -1;  // since the source delivered its latest frame, -1 without the link
    double effects = -1;     // from mpv's present to reshade_present
    double drawn = -1;       // when the frame in between was done on the GPU, after now
    double real = -1;        // when this frame went out, after now
    int frame = 2, of = 2;   // which of the frames shown for this source frame it is (2 of 2 = the real one)
};

void write_trace(const TraceLine &l) {
    const auto ms = [](double s) { return s < 0 ? -1.0 : s * 1000.0; };
    if (g_trace)
        fprintf(g_trace, "%.6f,%.3f,%.3f,%.3f,%d,%s,%.3f,%.3f,%.3f,%.3f,%d,%d\n", l.now, l.dt * 1000.0,
                l.interval * 1000.0, l.source_interval * 1000.0, l.queued, l.event, ms(l.source_age),
                ms(l.effects), ms(l.drawn), ms(l.real), l.frame, l.of);
}

// With the trace on: which refresh of the screen each present was shown on,
// as the swap chain's frame statistics report it. A thread of its own reads
// them every half millisecond (they only ever give the latest present) and
// writes a line when another present has reached the screen:
//   #screen,<time_s>,<presents shown>,<the refresh the latest was shown on>,
//           <refreshes so far>,<when the latest refresh was, s>
// From these a test can tell how many refreshes each frame stayed up without
// PresentMon (which needs administrator rights). The refresh numbers are the
// swap chain's: for a window Windows composes they follow its compositor (the
// main screen's rate), not the screen the window is on - unlike the add-on's
// own count (fg_vblank.h), which the pacing and the menu go by.
class ScreenTrace {
public:
    void start(IDXGISwapChain *swapchain) {
        if (thread_ || !g_trace) return;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&ScreenTrace::run),
                                &module_))
            return;
        swapchain_ = swapchain;
        stopping_ = false;
        thread_ = CreateThread(nullptr, 0, run, this, 0, nullptr);
        if (!thread_) {
            FreeLibrary(module_);
            swapchain_.Reset();
        }
    }
    void stop() {
        if (!thread_) return;
        stopping_ = true;
        WaitForSingleObject(thread_, 2000);
        CloseHandle(thread_);
        thread_ = nullptr;
        swapchain_.Reset();
    }

private:
    static DWORD WINAPI run(void *self) {
        auto *trace = static_cast<ScreenTrace *>(self);
        const HMODULE module = trace->module_;
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        UINT last = 0;
        while (!trace->stopping_) {
            DXGI_FRAME_STATISTICS stats{};
            if (SUCCEEDED(trace->swapchain_->GetFrameStatistics(&stats)) && stats.PresentCount != last) {
                last = stats.PresentCount;
                fprintf(g_trace, "#screen,%.6f,%u,%u,%u,%.6f\n", qpc_seconds(), stats.PresentCount,
                        stats.PresentRefreshCount, stats.SyncRefreshCount,
                        double(stats.SyncQPCTime.QuadPart) * qpc_period());
            }
            LARGE_INTEGER due;
            due.QuadPart = -5000;  // half a millisecond
            if (timer && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
                WaitForSingleObject(timer, 10);
            else
                Sleep(1);
        }
        if (timer) CloseHandle(timer);
        FreeLibraryAndExitThread(module, 0);
    }

    ComPtr<IDXGISwapChain> swapchain_;
    HANDLE thread_ = nullptr;
    HMODULE module_ = nullptr;
    std::atomic<bool> stopping_{false};
};

// ---- Holding the real frame on the GPU ----------------------------------------

// A held frame: its fence value, when to let it go, and its trace line. A
// source frame shown as x2 has one (the real frame); as x3 or x4 two or three
// (the later frames in between, then the real frame), let go one step apart.
struct HoldJob {
    uint64_t n = 0;
    int index = 1, count = 1;  // which of the source frame's held frames this is, of how many
    double step = 0;    // index steps after the first frame in between is done
    double lead = 0;    // less this (env FRAMEGEN_LEAD_MS, for experiments)
    double latest = 0;  // but all of them out no later than this
    TraceLine line;
};

// Lets the real frame go out at its time without mpv's render thread waiting
// for it. After mpv's present of the frame in between, mpv's thread queues on
// the GPU a signal that the frame in between is done (fence drawn_), the copy
// of the real frame into the back buffer, a wait for fence release_ and the
// present of the real frame, and goes on. This class's thread sees drawn_
// reach the frame, sleeps until half an interval later and moves release_ on;
// the GPU continues behind it and the real frame is shown.
//
// With more than one frame in between (x3, x4) the later ones are queued the
// same way before the real frame, each behind its own wait: drawn (the GPU
// does that while the frame before is held), wait, present. The thread times
// them all from when the first frame in between was done, a step apart.
//
// D3D11 cannot move a fence from the CPU, so release_ is a Direct3D 12 fence
// on the same GPU, opened in D3D11. Every wait queued must be released, or
// mpv's GPU work stops behind it: the thread lets a frame go after 200 ms even
// if drawn_ never gets there, and stop() lets all of them go.
class Hold {
public:
    bool start(ID3D11Device *device, ID3D11DeviceContext *context, std::string &error);
    void stop();
    // False once a release failed: new frames are then timed from mpv's present.
    bool active() const { return thread_ != nullptr && !signal_failed_; }
    // Lets every frame held so far go now (mpv's thread: a resize is coming,
    // which waits for the held present).
    void release_all() {
        if (thread_) release_up_to(issued_);
    }

    // On mpv's thread, in this order around the real frame's copy and present.
    uint64_t mark_drawn() {
        ctx_->Signal(drawn_.Get(), ++issued_);
        return issued_;
    }
    void wait_release(uint64_t n) { ctx_->Wait(release11_.Get(), n); }
    void let_go(const HoldJob &job);

    // Each frame counts once: drawn (the frame in between was seen done, and
    // the thread timed it: held_sum is from then to the release; early = let
    // go at `latest`, before its half; overdue = `latest` had passed when it
    // was done), not_drawn (no done signal: let go at once), at_once (let go
    // with a later frame: the queue was full, or a resize). timed: frames
    // whose release the thread timed, for late_sum.
    struct Stats {
        unsigned frames = 0, drawn = 0, early = 0, overdue = 0, not_drawn = 0, at_once = 0, timed = 0;
        unsigned firsts = 0;  // of drawn: the first held frame of its source frame (drawn_sum is theirs)
        double drawn_sum = 0, held_sum = 0, late_sum = 0, late_max = 0;
    };
    Stats take_stats() {
        std::lock_guard<std::mutex> lock(mutex_);
        const Stats s = stats_;
        stats_ = Stats{};
        return s;
    }
    // Source frames so far whose held frames got a tenth or more less than
    // their step each (early or overdue above): the first frame in between
    // was done too late for the others. Never set back; for StepDown, which
    // asks every frame.
    unsigned squeezed() const { return squeezed_.load(std::memory_order_relaxed); }

private:
    static DWORD WINAPI run(void *self);
    void loop();
    bool release_up_to(uint64_t n);
    bool release_now(uint64_t n);
    bool released(uint64_t n) {
        std::lock_guard<std::mutex> lock(signal_mutex_);
        return released_ >= n;
    }
    void reset();

    ComPtr<ID3D11DeviceContext4> ctx_;
    ComPtr<ID3D11Fence> drawn_, release11_;
    ComPtr<ID3D12Device> device12_;
    ComPtr<ID3D12Fence> release_;
    HANDLE thread_ = nullptr, drawn_event_ = nullptr;
    HMODULE module_ = nullptr;  // held by the thread while it runs
    HMODULE d3d12_ = nullptr;
    bool stuck_ = false;        // a thread that did not end: its objects stay as they are
    uint64_t issued_ = 0;       // mpv's thread only

    std::mutex mutex_;         // the jobs and the stats
    std::mutex signal_mutex_;  // released_, and the fence moving in order
    std::condition_variable wake_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> signal_failed_{false};
    std::atomic<unsigned> squeezed_{0};
    static constexpr int kJobs = 8;
    HoldJob jobs_[kJobs];
    int first_ = 0, count_ = 0;
    uint64_t released_ = 0;
    Stats stats_;
    // The thread's own: the source frame whose held frames it is timing.
    double base_ = 0, step_ = 0;  // when its first frame in between was done; the step in use
    bool base_known_ = false;
};

bool Hold::start(ID3D11Device *device, ID3D11DeviceContext *context, std::string &error) {
    if (thread_ || stuck_) return thread_ != nullptr;
    ComPtr<ID3D11Device5> device5;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5))) || FAILED(context->QueryInterface(IID_PPV_ARGS(&ctx_)))) {
        error = "no Direct3D 11.4 fences";
        return reset(), false;
    }
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter))) {
        error = "no DXGI adapter";
        return reset(), false;
    }
    d3d12_ = LoadLibraryW(L"d3d12.dll");
    const auto create =
        d3d12_ ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12_, "D3D12CreateDevice")) : nullptr;
    if (!create || FAILED(create(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12_)))) {
        error = "no Direct3D 12 device on this GPU";
        return reset(), false;
    }
    HANDLE shared = nullptr;
    HRESULT hr = device12_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&release_));
    if (SUCCEEDED(hr)) hr = device12_->CreateSharedHandle(release_.Get(), nullptr, GENERIC_ALL, nullptr, &shared);
    if (SUCCEEDED(hr)) hr = device5->OpenSharedFence(shared, IID_PPV_ARGS(&release11_));
    if (shared) CloseHandle(shared);
    if (SUCCEEDED(hr)) hr = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&drawn_));
    if (FAILED(hr)) {
        char text[48];
        snprintf(text, sizeof(text), "fences: 0x%08lX", static_cast<unsigned long>(hr));
        error = text;
        return reset(), false;
    }
    drawn_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!drawn_event_ || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                             reinterpret_cast<LPCWSTR>(&Hold::run), &module_)) {
        error = "no event";
        return reset(), false;
    }
    stopping_ = false;
    thread_ = CreateThread(nullptr, 0, run, this, 0, nullptr);
    if (!thread_) {
        FreeLibrary(module_);
        error = "no thread";
        return reset(), false;
    }
    // Its wake-ups are the real frames' times on screen.
    SetThreadPriority(thread_, THREAD_PRIORITY_TIME_CRITICAL);
    return true;
}

void Hold::reset() {
    ctx_.Reset();
    drawn_.Reset();
    release11_.Reset();
    release_.Reset();
    device12_.Reset();
    if (d3d12_) FreeLibrary(d3d12_);
    d3d12_ = nullptr;
    if (drawn_event_) CloseHandle(drawn_event_);
    drawn_event_ = nullptr;
    module_ = nullptr;
    signal_failed_ = false;
    issued_ = released_ = 0;
    first_ = count_ = 0;
    stats_ = Stats{};
}

void Hold::stop() {
    if (!thread_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        count_ = 0;
    }
    wake_.notify_all();
    release_now(issued_);  // nothing stays held
    SetEvent(drawn_event_);
    const bool ended = WaitForSingleObject(thread_, 2000) == WAIT_OBJECT_0;
    CloseHandle(thread_);
    thread_ = nullptr;
    if (!ended) {
        stuck_ = true;
        log_line(reshade::log::level::error, "the hold thread did not end; the real frame is now timed from mpv's present");
        return;
    }
    reset();
}

void Hold::let_go(const HoldJob &job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ < kJobs) {
            jobs_[(first_ + count_++) % kJobs] = job;
            wake_.notify_one();
            return;
        }
        ++stats_.frames;
        ++stats_.at_once;
    }
    // The thread is that far behind: let this one go now, and those queued before it.
    const bool out = release_now(job.n);
    TraceLine line = job.line;
    line.event = "generated-at-once";
    line.real = out ? qpc_seconds() - line.now : -1;
    write_trace(line);
}

// Moves the release fence on to n (never back). False if it would not move:
// the value stays unreleased, so the next call tries again, and new frames
// stop being held. Its own lock, not the jobs' and stats', so a Signal that
// takes long holds up only other releases.
bool Hold::release_up_to(uint64_t n) {
    std::lock_guard<std::mutex> lock(signal_mutex_);
    if (n <= released_) return true;
    const HRESULT hr = release_->Signal(n);
    if (FAILED(hr)) {
        if (!signal_failed_.exchange(true))
            log_line(reshade::log::level::error,
                     "letting a held real frame go failed (0x%08lX); the real frame is now timed from mpv's present",
                     static_cast<unsigned long>(hr));
        return false;
    }
    released_ = n;
    return true;
}

// release_up_to, tried again for up to 200 ms: the GPU waits behind the value.
bool Hold::release_now(uint64_t n) {
    for (int tries = 0;; ++tries) {
        if (release_up_to(n)) return true;
        if (tries == 200) return false;
        Sleep(1);
    }
}

DWORD WINAPI Hold::run(void *self) {
    auto *hold = static_cast<Hold *>(self);
    const HMODULE module = hold->module_;
    hold->loop();
    FreeLibraryAndExitThread(module, 0);
}

void Hold::loop() {
    Sleeper sleeper;
    for (;;) {
        HoldJob job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || count_ > 0; });
            if (stopping_) return;
            job = jobs_[first_];
            first_ = (first_ + 1) % kJobs;
            --count_;
        }
        const double give_up = qpc_seconds() + 0.2;
        while (drawn_->GetCompletedValue() < job.n && !stopping_ && !released(job.n)) {
            const double left = give_up - qpc_seconds();
            if (left <= 0) break;
            if (FAILED(drawn_->SetEventOnCompletion(job.n, drawn_event_))) break;
            WaitForSingleObject(drawn_event_, static_cast<DWORD>(left * 1000.0) + 1);
        }
        // UINT64_MAX: the device was removed. Nothing was drawn; let it go now.
        const UINT64 completed = drawn_->GetCompletedValue();
        const bool drawn = completed >= job.n && completed != UINT64_MAX;
        const double drawn_at = qpc_seconds();
        // The first held frame of a source frame sets the times of all of
        // them: a step apart from when the first frame in between was done,
        // or less when the next source frame is due before that. For the
        // later ones "drawn" only means the one before went out.
        if (job.index == 1) {
            base_known_ = drawn;
            base_ = drawn_at;
            step_ = fg::pacing::squeezed_step(job.step, drawn_at, job.latest, job.count);
            if (drawn && step_ < 0.9 * job.step) squeezed_.fetch_add(1, std::memory_order_relaxed);
        }
        const bool timed = drawn && base_known_ && !stopping_;
        const double target = timed ? std::max(base_ + step_ * job.index - job.lead, drawn_at) : drawn_at;
        if (!released(job.n)) sleeper.until(target);
        // Let go meanwhile with a later frame (queue full, a resize): not timed
        // here. Otherwise let go now - also when stopping, in case stop()'s
        // own release failed.
        const bool gone = released(job.n);
        const bool out = gone || release_now(job.n);
        const double out_at = qpc_seconds();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.frames;
            if (gone) {
                ++stats_.at_once;
            } else if (!drawn) {
                ++stats_.not_drawn;
            } else {
                ++stats_.drawn;
                if (job.index == 1) {
                    ++stats_.firsts;
                    stats_.drawn_sum += drawn_at - job.line.now;
                }
                stats_.held_sum += out_at - drawn_at;
                if (step_ < job.step) ++(step_ > 0 ? stats_.early : stats_.overdue);
            }
            if (!gone && out) {
                ++stats_.timed;
                stats_.late_sum += out_at - target;
                stats_.late_max = std::max(stats_.late_max, out_at - target);
            }
        }
        job.line.drawn = drawn ? drawn_at - job.line.now : -1;
        job.line.real = !gone && out ? out_at - job.line.now : -1;
        write_trace(job.line);
        if (stopping_) return;
    }
}

// ---- The generator ------------------------------------------------------------

// The pipeline state fg::Interpolator touches, saved before and put back
// after, so mpv and the other add-ons find everything as they left it. ReShade
// does the same rather than SwapDeviceContextState, which exposes an
// ID3D10Device and breaks overlays such as RTSS. The slot counts cover what
// fg_interp.h binds (it stays within them).
class StateBackup {
public:
    static constexpr UINT kSrvs = 8, kUavs = 8, kSamplers = 4, kBuffers = 2;

    explicit StateBackup(ID3D11DeviceContext *ctx) : ctx_(ctx) {
        ctx->IAGetPrimitiveTopology(&topology_);
        ctx->IAGetInputLayout(&layout_);
        ctx->VSGetShader(&vs_, nullptr, nullptr);
        ctx->HSGetShader(&hs_, nullptr, nullptr);
        ctx->DSGetShader(&ds_, nullptr, nullptr);
        ctx->GSGetShader(&gs_, nullptr, nullptr);
        ctx->PSGetShader(&ps_, nullptr, nullptr);
        ctx->PSGetShaderResources(0, kSrvs, ps_srvs_);
        ctx->PSGetSamplers(0, kSamplers, ps_samplers_);
        ctx->PSGetConstantBuffers(0, kBuffers, ps_cbs_);
        ctx->CSGetShader(&cs_, nullptr, nullptr);
        ctx->CSGetShaderResources(0, kSrvs, cs_srvs_);
        ctx->CSGetUnorderedAccessViews(0, kUavs, cs_uavs_);
        ctx->CSGetSamplers(0, kSamplers, cs_samplers_);
        ctx->CSGetConstantBuffers(0, kBuffers, cs_cbs_);
        ctx->RSGetState(&rs_);
        viewport_count_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetViewports(&viewport_count_, viewports_);
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs_, &dsv_);
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            if (rtvs_[i]) rtv_count_ = i + 1;
        ctx->OMGetBlendState(&blend_, blend_factor_, &sample_mask_);
        ctx->OMGetDepthStencilState(&depth_, &stencil_ref_);
    }

    ~StateBackup() {
        ID3D11DeviceContext *ctx = ctx_;
        // Outputs first, so a restored shader resource that is also one of
        // our outputs is not bound twice for a moment. Only as many render
        // target slots as were in use, so unordered access views bound past
        // them stay.
        ctx->OMSetRenderTargets(rtv_count_, rtvs_, dsv_);
        ctx->CSSetUnorderedAccessViews(0, kUavs, cs_uavs_, nullptr);
        ctx->OMSetBlendState(blend_, blend_factor_, sample_mask_);
        ctx->OMSetDepthStencilState(depth_, stencil_ref_);
        ctx->RSSetState(rs_);
        ctx->RSSetViewports(viewport_count_, viewports_);
        ctx->CSSetConstantBuffers(0, kBuffers, cs_cbs_);
        ctx->CSSetSamplers(0, kSamplers, cs_samplers_);
        ctx->CSSetShaderResources(0, kSrvs, cs_srvs_);
        ctx->CSSetShader(cs_, nullptr, 0);
        ctx->PSSetConstantBuffers(0, kBuffers, ps_cbs_);
        ctx->PSSetSamplers(0, kSamplers, ps_samplers_);
        ctx->PSSetShaderResources(0, kSrvs, ps_srvs_);
        ctx->PSSetShader(ps_, nullptr, 0);
        ctx->GSSetShader(gs_, nullptr, 0);
        ctx->DSSetShader(ds_, nullptr, 0);
        ctx->HSSetShader(hs_, nullptr, 0);
        ctx->VSSetShader(vs_, nullptr, 0);
        ctx->IASetInputLayout(layout_);
        ctx->IASetPrimitiveTopology(topology_);
        // The Get* calls above added a reference to everything they returned.
        auto drop = [](IUnknown *p) {
            if (p) p->Release();
        };
        drop(layout_), drop(vs_), drop(hs_), drop(ds_), drop(gs_), drop(ps_), drop(cs_), drop(rs_);
        drop(dsv_), drop(blend_), drop(depth_);
        for (auto *p : ps_srvs_) drop(p);
        for (auto *p : ps_samplers_) drop(p);
        for (auto *p : ps_cbs_) drop(p);
        for (auto *p : cs_srvs_) drop(p);
        for (auto *p : cs_uavs_) drop(p);
        for (auto *p : cs_samplers_) drop(p);
        for (auto *p : cs_cbs_) drop(p);
        for (auto *p : rtvs_) drop(p);
    }

    StateBackup(const StateBackup &) = delete;
    StateBackup &operator=(const StateBackup &) = delete;

private:
    ID3D11DeviceContext *ctx_;
    D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11InputLayout *layout_ = nullptr;
    ID3D11VertexShader *vs_ = nullptr;
    ID3D11HullShader *hs_ = nullptr;
    ID3D11DomainShader *ds_ = nullptr;
    ID3D11GeometryShader *gs_ = nullptr;
    ID3D11PixelShader *ps_ = nullptr;
    ID3D11ShaderResourceView *ps_srvs_[kSrvs] = {};
    ID3D11SamplerState *ps_samplers_[kSamplers] = {};
    ID3D11Buffer *ps_cbs_[kBuffers] = {};
    ID3D11ComputeShader *cs_ = nullptr;
    ID3D11ShaderResourceView *cs_srvs_[kSrvs] = {};
    ID3D11UnorderedAccessView *cs_uavs_[kUavs] = {};
    ID3D11SamplerState *cs_samplers_[kSamplers] = {};
    ID3D11Buffer *cs_cbs_[kBuffers] = {};
    ID3D11RasterizerState *rs_ = nullptr;
    UINT viewport_count_ = 0;
    D3D11_VIEWPORT viewports_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    ID3D11RenderTargetView *rtvs_[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    UINT rtv_count_ = 0;
    ID3D11DepthStencilView *dsv_ = nullptr;
    ID3D11BlendState *blend_ = nullptr;
    FLOAT blend_factor_[4] = {};
    UINT sample_mask_ = 0;
    ID3D11DepthStencilState *depth_ = nullptr;
    UINT stencil_ref_ = 0;
};

// How a real frame B reaches the screen with a frame in between:
//
//   reshade_present  B is finished in the back buffer (the DLSS 5 pass ran
//                    during ReShade's effects, before this). Copy B out, draw
//                    the frame between A and B over it, return: mpv's own
//                    present shows the frame in between right away.
//   finish_present   That present has returned. Copy B back into the back
//                    buffer and present it through the original swap chain,
//                    held on the GPU until half an interval after the frame
//                    in between was done (Hold), and return.
//
// Why from when the frame in between is done: with VRR the screen shows a
// frame as soon as the GPU has drawn it, and the frame in between waits for
// the DLSS 5 pass and its own drawing - about 7 ms, nearly half an interval at
// 60 fps. Timed from mpv's present, B came right behind it: 5 then 11.5 ms on
// screen instead of 8.3 and 8.3. Held on the GPU rather than by sleeping here,
// because mpv's thread must be free for the next frame: from a frame's arrival
// to here takes mpv about 3 ms, so at 60 fps the next one arrives before B is
// due. B is let go before the next frame's DLSS 5 pass reaches the GPU (behind
// the wait it would be held too).
//
// Without the hold (no Direct3D 12, or env FRAMEGEN_PACING=present for
// comparisons) finish_present sleeps until half an interval after B arrived
// and presents B then. The sleep happens there on purpose: during
// reshade_present ReShade holds its own lock and the device's multithread
// lock, so a sleep there would stall every other thread using the device.
//
// More than one frame in between (Multiplier=3 or 4, never the default): the
// first is drawn in reshade_present at a third or a quarter of the way from A
// to B and shown by mpv's present; finish_present draws each later one and
// presents it, then B, a third or a quarter of the source interval apart.
//
// How they are spaced depends on the screen (fg_pacing.h has the arithmetic):
//   - VRR on, or not known: the hold above. Inside the VRR range the screen
//     shows each frame when it is let go. At the top of the range (60 -> 120
//     on a 120 Hz screen) the step equals the refresh time, so a frame let go
//     a step after the one before is still shown at once or within a
//     fraction of a millisecond; over it, frames queue up and the queue rule
//     below leaves one out now and then.
//   - A screen refreshing at a fixed rate (VRR off, not engaging, or the
//     window composed by Windows): the same hold; each frame is then shown
//     on the first refresh after it was let go. "Done" comes just after a
//     refresh, so a step of whole refreshes later is just after the one
//     meant, and the frame waits one more: measured in a composed window on
//     240 Hz, 30 -> 60 stayed up 5 and 3 refreshes, not 4 and 4. status()
//     reports a screen seen refreshing at its fixed rate, and the menu says
//     so. env FRAMEGEN_LEAD_MS=<ms> lets held frames go that much earlier,
//     for experiments on such screens (docs/FRAMEGEN-LEARNINGS.md).
//
// When it generates. The interval is the source's: with the link, the mean
// time between new frames from the card, pauses left out; without it, the
// median between mpv's frames (which, while generating, follow this add-on's
// own pace - so only the link can tell that the display is too slow for twice
// the source rate). A frame in between is only made when
//   - twice the source rate fits the display (and of x3 and x4 the most that
//     fits is used: MultiplierGate),
//   - fewer than two presents are still queued (else it would only add delay).
//     At x3 and x4 a game frame left without its frames that way shows: four
//     frames, then one that stays as long as the four. When that keeps
//     happening, or mpv drops frames, one fewer is shown for every game frame
//     for a while (StepDown=1; fg_pacing.h has the rule, step_changed() logs
//     each change),
//   - the source is not paused (a still picture: only keepalive repeats),
//   - mpv is keeping up (watch_drops): drops caused while generating pause
//     generation for a while, longer each time; drops of mpv's own make it
//     wait until mpv has settled.
class FrameGen {
public:
    void on_reshade_present(reshade::api::effect_runtime *runtime);
    void on_finish_present(reshade::api::swapchain *swapchain);
    // For the in-game menu (src/overlay), on mpv's render thread like the rest.
    bool ready() const { return !broken_ && source_ok(); }
    void set_enabled(bool on);
    void set_fast(bool fast);
    void set_blend(bool blend);
    void set_multiplier(int multiplier);
    void set_dlss(bool dlss);
    // Apex Neural Rendering's frame generation: whether it can be chosen, and the choice.
    bool anr_offered() const;
    void set_anr(bool anr);
    // NVIDIA Video Frame Generation: whether it can be chosen, and the choice.
    bool video_offered() const;
    void set_video(bool video);
    void set_video_model(int model);
    void set_keep(const std::vector<app::KeepRegion> &regions);
    void set_hud(bool on);
    bool dlss_offered() const;
    app::FramegenStatus status() const;
    void rates(double &source, double &real, double &generated) const {
        const app::FramegenStatus s = status();
        source = s.source_fps;
        real = s.real_fps;
        generated = s.generated_fps;
    }
    // For the panel on ReShade's Add-ons page (draw_panel() below), on mpv's
    // render thread like the rest: the switches, changed while playing.
    void set_history_search(bool on);
    void set_picture_only(bool on);
    void set_edge_rules(bool on);
    void set_still_hud(bool on);
    void set_hud_clean(bool on);
    void set_step_down(bool on);
    bool broken() const { return broken_; }
    // What the generator works on: the back buffer's size (0: none yet) and
    // the picture's place in it (all 0: the whole of it).
    void working_on(UINT &width, UINT &height, fg::Picture &picture) const {
        width = frames_[0] ? width_ : 0;
        height = frames_[0] ? height_ : 0;
        picture = picture_made_;
    }
    void on_effects_finished(reshade::api::effect_runtime *runtime) {
        if (mpv_swapchain_ && runtime->get_native() != mpv_swapchain_) return;  // another runtime's effects
        effects_ran_ = true;
    }
    // ReShade's present event, before its effects: the time of mpv's present.
    // Other swap chains (the DLSS 5 add-ons', a third-party tool's) are not
    // mpv's frame and must not move it.
    void on_mpv_present(reshade::api::swapchain *swapchain) {
        if (mpv_swapchain_ && swapchain->get_native() != mpv_swapchain_) return;
        mpv_presented_ = qpc_seconds();
        presented_color_space_ = swapchain->get_color_space();
    }

    bool owns(reshade::api::device *device) const {
        return device_ && device->get_api() == reshade::api::device_api::d3d11 &&
               reinterpret_cast<ID3D11Device *>(device->get_native()) == device_.Get();
    }

    void release_held() { hold_.release_all(); }

    void release() {
        capture_.reset(false);
        screen_trace_.stop();
        // The next swap chain's window is looked up anew, and said in the log.
        vblank_.stop();
        screen_ = {};
        screen_seen_ = false;
        refresh_.reset();
        seen_logged_ = false;
        next_hz_try_ = 0;
        step_.again();
        step_fit_ = 1;
        shape_seen_ = false;
        for (auto &t : frames_) t.Reset();
        for (auto &v : views_) v.Reset();
        interp_.release();
        hud_.release();
        hud_failed_ = hud_live_ = false;
        dlss_.stop();
        dlss_failed_ = false;
        anr_.stop();
        anr_failed_ = false;
        motion_target_.Reset();
        motion_.Reset();
        vfg_.release();
        video_failed_ = false;
        hold_.stop();
        context_.Reset();
        device_.Reset();
        reset_history();
        pending_ = false;
        width_ = height_ = 0;
        failed_w_ = failed_h_ = 0;
        broken_ = false;
        // The next swap chain's first present is taken as mpv's, as at start,
        // so its colour space is read from that very present.
        mpv_swapchain_ = 0;
        color_space_ = presented_color_space_ = reshade::api::color_space::unknown;
    }

private:
    bool init(ID3D11Device *device);
    bool ensure_frames(const D3D11_TEXTURE2D_DESC &bb);
    void follow_picture(const D3D11_TEXTURE2D_DESC &bb);
    bool draw_between(ID3D11Texture2D *back_buffer, int a, int b, int index, int count, bool follows, bool keep);
    void dlss_setup();
    // The generator of another maker that is chosen (DLSS, or ANR), and
    // whether it gave up this session.
    fg::VendorGenerator &vendor() { return g_settings.anr ? static_cast<fg::VendorGenerator &>(anr_) : dlss_; }
    bool &vendor_failed() { return g_settings.anr ? anr_failed_ : dlss_failed_; }
    bool vendor_wanted() const {
        return (g_settings.dlss && !dlss_failed_) || (g_settings.anr && !anr_failed_);
    }
    const char *backend_name() const {
        return g_settings.video ? "video" : g_settings.anr ? "anr" : g_settings.dlss ? "dlss" : "own";
    }
    void keep_areas(ID3D11Texture2D *back_buffer, int slot);
    bool hud_wanted() const;
    void hud_pair(int a, int b, bool drawn);
    void hud_apply(ID3D11RenderTargetView *target, int slot);
    const fg::HudMask::Area *hud_area_list(size_t &count);
    void hud_stats();
    bool menu_repeat(double now);
    void note_source();
    void watch_screen(IDXGISwapChain *swapchain, double now);
    void watch_refresh(double now);
    // The most frames a second that may be shown: the user's limit when it
    // is below the screen's rate, else the screen's (60 when unknown).
    fg::pacing::Limit output_limit() const {
        return fg::pacing::output_limit(display_hz_, g_settings.max_output_hz);
    }
    void reset_history() {
        prev_ = -1;
        dt_interval_ = 0;
        intervals_seen_ = 0;
    }
    void push_interval(double dt) {
        recent_[intervals_seen_ % kRecent] = dt;
        ++intervals_seen_;
        const int n = std::min(intervals_seen_, kRecent);
        double sorted[kRecent];
        std::copy(recent_, recent_ + n, sorted);
        std::nth_element(sorted, sorted + n / 2, sorted + n);
        dt_interval_ = sorted[n / 2];
    }
    bool source_ok() const { return !g_settings.require_dedup || g_link.linked(); }
    // Whether the back buffer holds HDR10 (PQ) values: 10-bit with the PQ
    // colour space. The HDR launcher has mpv write PQ; the SDR one gets a
    // 10-bit back buffer too on a 10-bit desktop, with SDR values. Anything
    // else (gamma 2.2 BT.2020, which ReShade calls hdr10_hlg, or unknown) is
    // not PQ either.
    bool pq() const {
        return format_ == DXGI_FORMAT_R10G10B10A2_UNORM && color_space_ == reshade::api::color_space::hdr10_pq;
    }
    void toggle(reshade::api::effect_runtime *runtime);
    void use_edge_rules() {
        interp_.edge_cost = g_settings.edge_rules ? 0.01f : -1.0f;
        interp_.edge_explains = g_settings.edge_rules ? 0.0f : 1.0f;
    }
    void use_still_hud() {
        interp_.hud_detail = g_settings.still_hud ? 0.04f : 0.0f;
        if (g_settings.still_hud && !still_hud_logged_)
            log_line(reshade::log::level::info, "StillHud=1: cells that hold a HUD over moving scenery stand still");
        still_hud_logged_ = g_settings.still_hud;
    }
    void watch_drops(double now, bool stalled);
    void step_changed(const fg::pacing::StepDown::Change &change);
    void step_retry(double now, const char *why);
    void step_again(const char *why);
    void watch_step(HWND window, double now);
    void stats(double now);
    TraceLine trace_line(double now, double dt, int queued, const char *what) const {
        TraceLine l;
        l.now = now;
        l.dt = dt;
        l.interval = interval_;
        l.source_interval = source_interval_;
        l.queued = queued;
        l.event = what;
        l.source_age = source_age_;
        l.effects = effects_;
        return l;
    }
    void trace(double now, double dt, int queued, const char *what, double real = -1) {
        if (!g_trace) return;
        TraceLine l = trace_line(now, dt, queued, what);
        l.real = real;
        write_trace(l);
    }
    void fail(const char *what, HRESULT hr) {
        log_line(reshade::log::level::error, "%s failed (0x%08lX); frame generation stays off", what, hr);
        broken_ = true;
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    fg::Interpolator interp_;  // makes the frame in between (fg_interp.h)
    // Backend=dlss: makes them instead, while it works (fg_dlss.h).
    fg::DlssFg dlss_;
    bool dlss_failed_ = false;  // did not start, or failed: the app's own from then on
    // Backend=anr: likewise (fg_anr.h), with the motion from the later frame
    // back to the earlier one drawn into motion_ before each pair.
    fg::AnrFg anr_;
    bool anr_failed_ = false;
    ComPtr<ID3D11Texture2D> motion_;
    ComPtr<ID3D11RenderTargetView> motion_target_;
    uint64_t pair_flow_ = 0;    // the real frame (serial_) whose pair interp_ holds find_flow()'s motion of
    fg::Vfg vfg_;               // NVIDIA Video Frame Generation, made for a size when first asked for
    bool video_failed_ = false; // did not start, or failed: the app's own from then on
    bool video_ten_bit_ = false;
    uint64_t video_pair_ = 0;   // the real frame (serial_) whose pair it holds
    bool dlss_made_ = false;    // the frames between the pending pair are in its textures
    uint64_t serial_ = 0;       // counts real frames
    uint64_t dlss_fed_ = 0;     // the last real frame it was given, by serial_
    int dlss_count_ = 0;        // ... and how many frames it made before it
    std::vector<app::PixelBox> keep_boxes_;  // keep_areas' own, kept for its memory
    // The HUD found by itself (fg_hud.h): made when something asks for it
    // (Hud=1, or an area with find_hud) and let go when nothing does.
    fg::HudMask hud_;
    bool hud_failed_ = false;    // it could not be made at this size: areas with find_hud are kept whole
    bool hud_live_ = false;      // its mask is the pending pair's: the pair's frames in between get it
    bool hud_count_due_ = false; // the next pair counts the mask's pixels for the log
    double hud_updated_ = 0;     // when it last saw a pair
    std::string hud_said_;       // what the log last said it works on
    std::vector<fg::HudMask::Area> hud_areas_;  // hud_apply's own, kept for its memory

    // The last two real frames, as copied out of the back buffer, and whether
    // ReShade's effects ran for each. Frames are only mixed with frames of the
    // same kind, so a frame from while the effects were reloading or switched
    // off is not mixed with a neural one. (That the effects ran does not prove
    // the DLSS 5 pass delivered: the feeder can skip a frame and say nothing.)
    ComPtr<ID3D11Texture2D> frames_[2];
    ComPtr<ID3D11ShaderResourceView> views_[2];
    bool neural_[2] = {};
    UINT width_ = 0, height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    // The swap chain's colour space as ReShade tracks it (mpv sets it), read
    // at mpv's present (the effect runtime does not have it).
    reshade::api::color_space color_space_ = reshade::api::color_space::unknown;
    reshade::api::color_space presented_color_space_ = reshade::api::color_space::unknown;
    UINT failed_w_ = 0, failed_h_ = 0;  // the size that last failed, not retried until it changes
    DXGI_FORMAT failed_format_ = DXGI_FORMAT_UNKNOWN;
    int prev_ = -1;  // which of frames_ holds the previous real frame
    bool effects_ran_ = false;

    static constexpr int kRecent = 15;
    double last_real_ = 0;
    double dt_interval_ = 0;      // median time between mpv's recent frames
    double source_interval_ = 0;  // mean time between the source's new frames (link), pauses left out
    double interval_ = 0;         // the one in use
    double recent_[kRecent] = {};
    int intervals_seen_ = 0;

    // A real frame still to present after the frame in between (and, with
    // x3 or x4, the later frames in between before it).
    bool pending_ = false;
    // HistorySearch: interp_ holds the motion of the pair that ended on the last real frame, found at the
    // source's pace - what the next pair's search may start from. Any present without such a pair drops it.
    bool history_fresh_ = false;
    bool history_logged_ = false;
    bool still_hud_logged_ = false;
    // PictureOnly: where mpv has the picture in the back buffer (all 0: it fills it) - as last seen, for how
    // many presents in a row, what interp_ was made for, and one it could not be made for.
    fg::Picture picture_seen_, picture_made_, picture_failed_;
    int picture_steady_ = 0;
    int pending_slot_ = 0, pending_prev_slot_ = 0;
    int pending_count_ = 2;  // frames shown for this source frame, the real one included
    bool pending_follows_ = false, pending_keep_ = false;  // HistorySearch, for the pair's later frames
    double pending_since_ = 0;
    double pending_now_ = 0, pending_dt_ = 0, pending_step_ = 0, pending_latest_ = 0;
    int pending_queued_ = -1;
    IDXGISwapChain *pending_swapchain_ = nullptr;
    bool generated_last_ = false;

    Hold hold_;                    // holds the real frame on the GPU until its time
    bool hold_wanted_ = false;     // start it at the next finish_present (outside ReShade's locks)
    double source_age_ = -1;       // since the source delivered its latest frame, -1 without the link
    uint64_t mpv_swapchain_ = 0;   // native handle of mpv's swap chain, once seen
    double mpv_presented_ = 0;     // when mpv called Present, before ReShade's effects
    double effects_ = -1;          // from then to reshade_present (the DLSS 5 pass and the rest)
    LONG64 last_published_ = -1;   // the link's new-frame count at the last reshade_present
    double arrival_to_present_ = -1;  // from a new frame's arrival to mpv's present, the latest seen

    // Backing off when mpv cannot keep up.
    LONG64 last_dropped_ = -1;
    double drop_times_[2] = {-1e9, -1e9};
    double paused_until_ = 0, backoff_ = 5.0, settle_until_ = 0;
    bool broken_ = false;
    // The screen mpv's window is on now, looked up every second
    // (watch_screen): its rate, and what the exe found about its variable
    // refresh at start (fg_screen.h). Everything below that says "the screen"
    // means this one.
    fg::WindowScreen screen_;
    bool screen_seen_ = false;  // looked up at least once (and said in the log)
    fg::ScreenBelief belief_;
    fg::pacing::Vrr vrr_ = fg::pacing::Vrr::Unknown;  // belief_.vrr
    double display_hz_ = 0;  // belief_.hz: with its fraction (119.88) when known, 0 unknown
    double next_hz_try_ = 0;
    double lead_ = 0;  // env FRAMEGEN_LEAD_MS: held frames are let go this much earlier

    // What the menu shows (status()): over the last second, not the 5 s log's
    // window.
    fg::pacing::SourceCadence cadence_;  // the source's new frames, by the card's clock
    LONG64 cadence_seen_ = 0;            // the link's new-frame count already in it
    fg::pacing::RateWindow shown_real_, shown_generated_, queue_skips_, mix_skips_, late_, dropped_;
    // For the menu's delay: from each new frame's arrival from the card to
    // its present (the real frame's, after the frames in between).
    fg::pacing::MeanWindow latency_;
    LONG64 latency_published_ = 0;
    fg::pacing::MultiplierGate gate_;
    app::FramegenStatus::Reason reason_ = app::FramegenStatus::Reason::Starting;  // the latest frame's
    // StepDown=1: a multiplier that fits the screen and cannot be held gives
    // way to the one below it for a while (fg_pacing.h).
    fg::pacing::StepDown step_;
    int step_fit_ = 1;             // what the latest frame would have been shown as without it
    const char *step_why_ = "";    // why the one above is tried before its time ("": its time came)
    unsigned step_squeezed_ = 0;   // the hold's count (Hold::squeezed) already handed to it
    bool step_neural_ = false;     // whether ReShade's effects ran for the frame before
    // mpv's window as last looked at (watch_step): its place and its style
    // (a border or none, full screen), which change with the window mode.
    RECT shape_rect_{};
    LONG shape_style_ = 0;
    bool shape_seen_ = false;

    // That screen's own refreshes, counted by a thread (fg_vblank.h) and read
    // once a second: at its fixed rate whatever is presented means VRR is not
    // engaging. Both start over when the window is on another screen.
    fg::VblankCounter vblank_;
    fg::pacing::RefreshWatch refresh_;
    bool seen_logged_ = false;
    int count_failed_ = 0;  // readings in a row that said the count failed
    bool count_failure_logged_ = false;
    // The menu on a slow picture: mpv is sent the repeats so the menu is drawn often enough (menu_repeat).
    bool menu_wanted_ = false;      // the repeats are asked for, for the menu
    double menu_until_ = 0;         // ... or were until a moment ago: some are still on their way
    LONG64 menu_attributed_ = 0;    // the newest new frame (the link's count) a present was taken to carry
    bool capture_key_seen_ = false; // the capture key came with a repeat: for the next real frame
    unsigned menu_repeats_ = 0;     // repeats shown for the menu since the last stats line
    Sleeper sleeper_;
    Capture capture_;
    ScreenTrace screen_trace_;  // env FRAMEGEN_TRACE only
    double capture_after_ = -1;  // env FRAMEGEN_CAPTURE_AFTER: when, 0 = none, -1 = not read yet
    double toggle_after_ = -1;   // env FRAMEGEN_TOGGLE_AFTER: the same

    // Stats since the last line.
    double stats_start_ = 0;
    unsigned trimmed_ = 0;  // game frames that got fewer frames than the others: the screen was behind
    fg::pacing::RateWindow trims_;
    unsigned real_ = 0, generated_ = 0, skipped_fast_ = 0, skipped_mixed_ = 0, skipped_queue_ = 0,
             skipped_paused_ = 0, skipped_backoff_ = 0, skipped_settle_ = 0, late_frames_ = 0, drops_ = 0;
    double late_max_ = 0, late_sum_ = 0, mpv_present_max_ = 0, mpv_present_sum_ = 0;
    double out_max_ = 0;  // the real frame's copy and present on mpv's thread, held
};

FrameGen g_gen;

bool FrameGen::init(ID3D11Device *device) {
    device_ = device;
    device->GetImmediateContext(&context_);
    trace_open();
    hold_wanted_ = _stricmp(app::env("FRAMEGEN_PACING").c_str(), "present") != 0;
    if (!hold_wanted_)
        log_line(reshade::log::level::info,
                 APP_ENV("FRAMEGEN_PACING") "=present: the real frame is timed from mpv's present");
    lead_ = std::clamp(atof(app::env("FRAMEGEN_LEAD_MS").c_str()), 0.0, 20.0) / 1000.0;
    if (lead_ > 0)
        log_line(reshade::log::level::info, APP_ENV("FRAMEGEN_LEAD_MS") ": held frames are let go %.2f ms early",
                 lead_ * 1000.0);
    // Which of them the window is on is looked up while running, and said
    // then (watch_screen).
    if (!g_settings.screens.empty()) {
        std::string list;
        for (const fg::ScreenFacts &f : g_settings.screens) {
            char text[200];
            snprintf(text, sizeof(text), "%s%s %.3f Hz VRR %s", list.empty() ? "" : "; ",
                     f.name.empty() ? f.device.c_str() : f.name.c_str(), f.hz, fg::vrr_word(f.vrr));
            list += text;
        }
        if (list.size() > 800) list.resize(800);
        log_line(reshade::log::level::info, "the screens, as " APP_NAME " found them at start: %s", list.c_str());
    } else if (g_settings.one_told) {
        log_line(reshade::log::level::info, "the screen, as " APP_NAME " found it: VRR %s, %.3f Hz",
                 fg::vrr_word(g_settings.vrr), g_settings.launcher_hz);
    } else {
        log_line(reshade::log::level::info, APP_NAME " told nothing about the screens: VRR is not known");
    }
    log_line(reshade::log::level::info, "ready: feature level 0x%X, %s, toggle key %d%s", device->GetFeatureLevel(),
             g_settings.enabled ? "on" : "off", g_settings.toggle_key,
             g_settings.require_dedup ? ", waiting for the link from " APP_NAME "'s dedup" : "");
    return true;
}

// The frame copies and the frame generator for this back buffer size. A
// failure here is tied to the size (allocation during a resize): it is logged
// once and tried again when the size changes, without turning frame
// generation off for good.
bool FrameGen::ensure_frames(const D3D11_TEXTURE2D_DESC &bb) {
    if (frames_[0] && bb.Width == width_ && bb.Height == height_ && bb.Format == format_) return true;
    if (bb.Width == failed_w_ && bb.Height == failed_h_ && bb.Format == failed_format_) return false;
    if (capture_.active()) capture_.reset();  // its copies are of the old size
    dlss_.drop_frames();
    anr_.drop_frames();
    motion_target_.Reset();
    motion_.Reset();
    vfg_.release();  // made anew for the new size at the next frame in between
    hud_.release();  // made anew for the new size when a pair asks for it (hud_pair)
    hud_failed_ = hud_live_ = false;
    for (auto &t : frames_) t.Reset();
    for (auto &v : views_) v.Reset();
    reset_history();
    width_ = height_ = 0;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = bb.Width;
    desc.Height = bb.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = bb.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    // Shared with DLSS Frame Generation's or ANR's Direct3D 12 device (dlss_setup).
    if (vendor_wanted()) desc.MiscFlags = fg::VendorGenerator::kSharedMisc;
    HRESULT hr = S_OK;
    for (int i = 0; i < 2 && SUCCEEDED(hr); ++i) {
        hr = device_->CreateTexture2D(&desc, nullptr, &frames_[i]);
        if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(frames_[i].Get(), nullptr, &views_[i]);
    }
    std::string error;
    if (FAILED(hr)) {
        char text[64];
        snprintf(text, sizeof(text), "frame copies: 0x%08lX", static_cast<unsigned long>(hr));
        error = text;
    } else if (!interp_.create(device_.Get(), bb.Width, bb.Height, error)) {
        hr = E_FAIL;
    }
    if (FAILED(hr)) {
        for (auto &t : frames_) t.Reset();
        for (auto &v : views_) v.Reset();
        failed_w_ = bb.Width;
        failed_h_ = bb.Height;
        failed_format_ = bb.Format;
        log_line(reshade::log::level::error,
                 "the frame generator did not start at %ux%u (%s); trying again at the next size", bb.Width,
                 bb.Height, error.c_str());
        return false;
    }
    if (g_settings.fast) {
        // The 2x2 cells around each pixel, compared on the pixel alone.
        interp_.warp_ring = 2;
        interp_.warp_taps = 1;
        interp_.soft_temp = 0.02f;
    }
    interp_.hist_search = g_settings.history_search ? 1 : 0;
    use_edge_rules();
    use_still_hud();
    picture_made_ = picture_failed_ = fg::Picture{};  // made for the whole frame: follow_picture() takes it from here
    picture_steady_ = 0;
    failed_w_ = failed_h_ = 0;
    width_ = bb.Width;
    height_ = bb.Height;
    format_ = bb.Format;
    log_line(reshade::log::level::info, "back buffer %ux%u, format %d", width_, height_, format_);
    step_retry(qpc_seconds(), "the picture's size changed");
    return true;
}

// Starts DLSS Frame Generation for the frame copies there are now. Called
// outside ReShade's locks (creating the Direct3D 12 device runs other add-ons'
// device events). A failure is said once and leaves the app's own generator.
// The folder this add-on is in (the app's engine folder).
std::wstring engine_folder() {
    wchar_t path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&qpc_seconds), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring engine = path;
    const size_t slash = engine.find_last_of(L'\\');
    engine.erase(slash == std::wstring::npos ? 0 : slash);
    return engine;
}

// Whether DLSS Frame Generation can be chosen in the menu: its model is
// there, the GPU is NVIDIA's, and it has not failed this session.
bool FrameGen::dlss_offered() const {
    static const bool model = GetFileAttributesW((engine_folder() + L"\\nvngx_dlssg.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!model || dlss_failed_ || !device_) return false;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    return SUCCEEDED(device_.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)) &&
           desc.VendorId == 0x10DE;
}

// The menu's choice. The frame copies are made anew for it (shared with
// Direct3D 12 or not), and a failure before is forgiven: it is tried again.
// The folder NVIDIA Video Frame Generation's DLLs are in: where the app's
// setup puts them, or env VFG_RUNTIME.
std::wstring video_runtime() {
    const std::string other = app::env("VFG_RUNTIME");
    if (!other.empty()) return std::wstring(other.begin(), other.end());
    return engine_folder() + L"\\nvidia-vfg";
}

// Whether NVIDIA Video Frame Generation can be chosen in the menu: its DLLs
// are there, the GPU is NVIDIA's, and it has not failed this session.
bool FrameGen::video_offered() const {
    static const bool runtime = fg::Vfg::runtime_present(video_runtime());
    if (!runtime || video_failed_ || !device_) return false;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    return SUCCEEDED(device_.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)) &&
           desc.VendorId == 0x10DE;
}

// The menu's choice; a failure before is forgiven: it is tried again.
void FrameGen::set_video(bool video) {
    g_settings.video = video;
    if (video) {
        g_settings.dlss = false;
        if (g_settings.anr) anr_.drop_frames();
        g_settings.anr = false;
    }
    reshade::set_config_value(nullptr, kSection, "Backend", backend_name());
    video_failed_ = false;
    if (!video) vfg_.release();
    step_again("another generator");
    log_line(reshade::log::level::info, "frames in between by %s", video ? "NVIDIA Video Frame Generation" : APP_NAME);
}

// Its model (0 small, 1 medium, 2 large): made again with the next pair.
void FrameGen::set_video_model(int model) {
    model = model < 0 ? 0 : model > 2 ? 2 : model;
    if (model == g_settings.video_quality) return;
    g_settings.video_quality = model;
    reshade::set_config_value(nullptr, kSection, "VideoQuality", model == 0 ? "low" : model == 2 ? "high" : "medium");
    video_failed_ = false;
    vfg_.release();
    step_again("another model");
}

// Whether ANR Frame Generation can be chosen in the menu: its files are
// there and it has not failed this session. Any GPU.
bool FrameGen::anr_offered() const {
    static const bool files = fg::AnrFg::present(engine_folder());
    return files && !anr_failed_ && device_;
}

void FrameGen::set_anr(bool anr) {
    g_settings.anr = anr;
    if (anr) {
        g_settings.dlss = false;
        dlss_.drop_frames();
        if (g_settings.video) {
            g_settings.video = false;
            vfg_.release();
        }
        width_ = height_ = 0;  // ensure_frames makes them shared
    } else {
        anr_.drop_frames();  // its model's memory back
    }
    reshade::set_config_value(nullptr, kSection, "Backend", backend_name());
    anr_failed_ = false;
    step_again("another generator");
    log_line(reshade::log::level::info, "frames in between by %s", anr ? "ANR Frame Generation" : APP_NAME);
}

void FrameGen::set_dlss(bool dlss) {
    g_settings.dlss = dlss;
    if (g_settings.anr) {
        g_settings.anr = false;
        anr_.drop_frames();
    }
    if (dlss && g_settings.video) {
        g_settings.video = false;
        vfg_.release();
    }
    reshade::set_config_value(nullptr, kSection, "Backend", dlss ? "dlss" : "own");
    dlss_failed_ = false;
    if (dlss) width_ = height_ = 0;  // ensure_frames makes them shared
    step_again("another generator");
    log_line(reshade::log::level::info, "frames in between by %s", dlss ? "DLSS Frame Generation" : APP_NAME);
}

void FrameGen::dlss_setup() {
    const bool anr = g_settings.anr;
    if (!anr && !g_settings.dlss) return;
    fg::VendorGenerator &gen = vendor();
    bool &failed = vendor_failed();
    if (failed || gen.ready() || !frames_[0] || !frames_[1]) return;
    std::string error;
    if (!gen.started()) {
        // The model (nvngx_dlssg.dll) is beside this add-on; NGX's logs go to
        // the app's logs folder.
        const std::wstring engine = engine_folder();
        std::wstring logs = engine;
        const size_t parent = logs.find_last_of(L'\\');
        if (parent != std::wstring::npos) logs = logs.substr(0, parent) + L"\\logs";
        if (GetFileAttributesW(logs.c_str()) == INVALID_FILE_ATTRIBUTES) logs = engine;
        if (anr && !fg::AnrFg::present(engine))
            error = "its files are not in the engine folder (" APP_NAME "'s setup downloads them)";
        else if (!anr && GetFileAttributesW((engine + L"\\nvngx_dlssg.dll").c_str()) == INVALID_FILE_ATTRIBUTES)
            error = "nvngx_dlssg.dll is not in the engine folder (" APP_NAME "'s setup downloads it)";
        else
            gen.start(device_.Get(), logs, engine, error);
        if (!gen.started()) {
            failed = true;
            log_line(reshade::log::level::warning, "%s is not used (%s); the frames in between are " APP_NAME "'s own",
                     gen.name(), error.c_str());
            return;
        }
        log_line(reshade::log::level::info, "%s: started, up to %d frames between two", gen.name(), gen.max_between());
    }
    D3D11_TEXTURE2D_DESC desc{};
    frames_[0]->GetDesc(&desc);
    if (!(desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) return;  // made before the setting: the next size
    bool set = gen.set_frames(frames_[0].Get(), frames_[1].Get(), pq(), error);
    if (set && anr) {
        // Its motion vectors: without them it only mixes the two pictures.
        D3D11_TEXTURE2D_DESC motion = desc;
        motion.Format = DXGI_FORMAT_R16G16_FLOAT;
        motion.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        motion.MiscFlags = fg::VendorGenerator::kSharedMisc;
        motion_target_.Reset();
        motion_.Reset();
        set = SUCCEEDED(device_->CreateTexture2D(&motion, nullptr, &motion_)) &&
              SUCCEEDED(device_->CreateRenderTargetView(motion_.Get(), nullptr, &motion_target_));
        if (!set) error = "the motion texture could not be made";
        if (set) set = gen.set_motion(motion_.Get(), error);
        if (!set) gen.drop_frames();
    }
    if (!set) {
        failed = true;
        log_line(reshade::log::level::warning,
                 "%s is not used at %ux%u, format %d (%s); the frames in between are " APP_NAME "'s own", gen.name(),
                 desc.Width, desc.Height, desc.Format, error.c_str());
        return;
    }
    dlss_fed_ = 0;
    log_line(reshade::log::level::info, "%s makes the frames in between (%ux%u, %s)", gen.name(), desc.Width,
             desc.Height, pq() ? "HDR" : "SDR");
}

// PictureOnly: makes the generator for the picture's part of the back buffer once mpv has had the picture in
// the same place for a quarter of a second of presents (a window being resized moves it with every frame,
// and each change makes the generator's textures anew).
void FrameGen::follow_picture(const D3D11_TEXTURE2D_DESC &bb) {
    constexpr int kSteady = 15;
    fg::Picture now;
    if (g_settings.picture_only) {
        const D3D11_BOX box = app::overlay::picture_box(bb.Width, bb.Height);
        if (box.right - box.left < bb.Width || box.bottom - box.top < bb.Height) now = {box.left, box.top, box.right, box.bottom};
    }
    if (!(now == picture_seen_)) {
        picture_seen_ = now;
        picture_steady_ = 0;
    } else if (picture_steady_ < kSteady) {
        ++picture_steady_;
    }
    // picture_failed_ is all 0 when none failed, which is also "the whole frame": back to that it always goes.
    if (now == picture_made_ || (now.x1 != 0 && now == picture_failed_) || picture_steady_ < kSteady) return;
    std::string error;
    if (interp_.create(device_.Get(), bb.Width, bb.Height, error, now)) {
        picture_made_ = now;
        picture_failed_ = fg::Picture{};
        if (now.x1 != 0)
            log_line(reshade::log::level::info, "PictureOnly=1: the picture is %ux%u at %u,%u of the %ux%u back buffer",
                     now.x1 - now.x0, now.y1 - now.y0, now.x0, now.y0, bb.Width, bb.Height);
        else if (g_settings.picture_only)
            log_line(reshade::log::level::info, "PictureOnly=1: the picture fills the back buffer");
        else
            log_line(reshade::log::level::info, "PictureOnly=0: the whole %ux%u back buffer again", bb.Width, bb.Height);
    } else {
        log_line(reshade::log::level::error, "PictureOnly=1: no generator for the picture alone (%s); the whole frame it is",
                 error.c_str());
        picture_failed_ = now;
        picture_made_ = fg::Picture{};
        if (!interp_.create(device_.Get(), bb.Width, bb.Height, error)) {
            // As a failure in ensure_frames(): tried again at the next size.
            for (auto &t : frames_) t.Reset();
            for (auto &v : views_) v.Reset();
            failed_w_ = bb.Width;
            failed_h_ = bb.Height;
            failed_format_ = bb.Format;
            width_ = height_ = 0;
            log_line(reshade::log::level::error, "the frame generator did not start at %ux%u (%s); trying again at the next size",
                     bb.Width, bb.Height, error.c_str());
        }
    }
    if (capture_.active()) capture_.reset();
    reset_history();
    step_retry(qpc_seconds(), "the picture's place changed");
}

// Draws frame `index` of the `count` shown for this pair (count - 1 in
// between, then the real one) between frames_[a] and frames_[b] into the back
// buffer: at index / count of the way from a to b.
// follows: the last pair drawn ended on frames_[a] and its motion was kept;
// keep: this pair's motion is worth keeping for the next one (HistorySearch).
bool FrameGen::draw_between(ID3D11Texture2D *back_buffer, int a, int b, int index, int count, bool follows,
                            bool keep) {
    if (g_settings.video && !video_failed_) {
        // NVIDIA Video Frame Generation: made for this size and picture when
        // first asked for. Any failure leaves the app's own generator.
        bool ten_bit = false;
        std::string error;
        if (!fg::Vfg::takes(format_, ten_bit)) {
            video_failed_ = true;
            error = "it does not take this picture format";
        } else if (!vfg_.ready() || vfg_.width() != width_ || vfg_.height() != height_ || video_ten_bit_ != ten_bit ||
                   !(vfg_.picture() == picture_made_)) {
            if (vfg_.create(device_.Get(), width_, height_, ten_bit, video_runtime(), fg::VfgMode(g_settings.video_quality),
                            true, error, picture_made_)) {
                video_ten_bit_ = ten_bit;
                video_pair_ = 0;
                log_line(reshade::log::level::info,
                         "NVIDIA Video Frame Generation %s makes the frames in between (%ux%u, %s, its %s model)",
                         vfg_.version().c_str(), width_, height_, ten_bit ? "10-bit" : "8-bit",
                         g_settings.video_quality == 0 ? "low" : g_settings.video_quality == 2 ? "high" : "medium");
            } else {
                video_failed_ = true;
            }
        }
        ComPtr<ID3D11RenderTargetView> target;
        if (!video_failed_ && FAILED(device_->CreateRenderTargetView(back_buffer, nullptr, &target))) {
            video_failed_ = true;
            error = "no view of the back buffer";
        }
        if (!video_failed_) {
            fg::DrawOptions o;
            o.index = index;
            o.multiplier = count;
            o.same_pair = index > 1 && video_pair_ == serial_;
            o.marker = g_settings.marker;
            o.marker_level = pq() ? 0.5f : 0.75f;
            StateBackup backup(context_.Get());
            if (vfg_.draw(context_.Get(), views_[a].Get(), views_[b].Get(), target.Get(), o, error)) {
                video_pair_ = serial_;
                keep_areas(back_buffer, index * 2 < count ? a : b);
                // The HUD mask, with a motion search of its own: the filter hands none out.
                if (hud_wanted() || hud_.ready()) {
                    if (index == 1) hud_pair(a, b, false);
                    if (hud_live_) hud_apply(target.Get(), index * 2 < count ? a : b);
                }
                return true;
            }
            video_failed_ = true;
        }
        vfg_.release();
        log_line(reshade::log::level::warning,
                 "NVIDIA Video Frame Generation stopped (%s); the frames in between are " APP_NAME "'s own from here",
                 error.c_str());
    }
    fg::VendorGenerator &gen = vendor();
    if ((g_settings.dlss || g_settings.anr) && gen.ready() && !vendor_failed() && count - 1 <= gen.max_between()) {
        // All of the pair's frames are made with the first, in one go.
        if (index == 1) {
            if (g_settings.anr && motion_target_) {
                // The motion from b back to a, as its motion vectors (none
                // found, a picture too small for the search: zeros).
                StateBackup backup(context_.Get());
                if (interp_.find_flow(context_.Get(), views_[a].Get(), views_[b].Get(), true) &&
                    interp_.draw_motion_back(context_.Get(), motion_target_.Get())) {
                    pair_flow_ = serial_;
                } else {
                    const float none[4] = {};
                    context_->ClearRenderTargetView(motion_target_.Get(), none);
                }
            }
            const bool continuous = dlss_fed_ + 1 == serial_ && dlss_count_ == count - 1;
            std::string error;
            dlss_made_ = gen.generate(a, b, count - 1, continuous, error);
            if (dlss_made_) {
                dlss_fed_ = serial_;
                dlss_count_ = count - 1;
            } else {
                vendor_failed() = true;
                log_line(reshade::log::level::warning, "%s stopped (%s); the frames in between are " APP_NAME "'s own from here",
                         gen.name(), error.c_str());
            }
        }
        if (dlss_made_) {
            context_->CopyResource(back_buffer, gen.between(index));
            keep_areas(back_buffer, index * 2 < count ? a : b);
            // The HUD mask, with a motion search of its own: DLSS hands none out.
            if (hud_wanted() || hud_.ready()) {
                StateBackup backup(context_.Get());
                if (index == 1) hud_pair(a, b, false);
                ComPtr<ID3D11RenderTargetView> target;
                if (hud_live_ && SUCCEEDED(device_->CreateRenderTargetView(back_buffer, nullptr, &target)))
                    hud_apply(target.Get(), index * 2 < count ? a : b);
            }
            return true;
        }
    }
    const float t = float(index) / float(count);
    ComPtr<ID3D11RenderTargetView> rtv;
    HRESULT hr = device_->CreateRenderTargetView(back_buffer, nullptr, &rtv);
    if (FAILED(hr)) return fail("CreateRenderTargetView", hr), false;

    fg::DrawOptions o;
    o.method = g_settings.method;
    o.t = t;
    o.follows = follows;
    o.marker = g_settings.marker;
    o.marker_level = 0.75f;                                                // sRGB-encoded
    if (pq()) o.marker_level = 0.5f;                                       // PQ, about 90 nits
    if (format_ == DXGI_FORMAT_R16G16B16A16_FLOAT) o.marker_level = 1.0f;  // scRGB, 80 nits

    {
        StateBackup backup(context_.Get());
        // The HUD mask goes one pair on as soon as this draw has found the pair's motion, so that the frame
        // can be drawn from the real frames without their HUD (HudClean): a HUD pixel is not scenery to be
        // carried along. Only while the mask is asked for: without it the draw is what it always was.
        bool paired = false;
        if (hud_wanted() || hud_.ready())
            interp_.after_motion = [&](ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *&fa, ID3D11ShaderResourceView *&fb) {
                if (index == 1) hud_pair(a, b, true);
                paired = true;
                if (!hud_live_ || !g_settings.hud_clean) return;
                size_t areas = 0;
                const fg::HudMask::Area *list = hud_area_list(areas);
                if (!g_settings.hud && areas == 0) return;
                hud_.clean_timer.poll(ctx);
                hud_.clean_timer.begin(ctx);
                hud_.clean_frames(ctx, fa, fb, list, areas);
                hud_.clean_timer.end(ctx);
            };
        interp_.draw(context_.Get(), views_[a].Get(), views_[b].Get(), rtv.Get(), o);
        interp_.after_motion = nullptr;
        // The pair's motion is kept once, after its last frame in between.
        if (g_settings.history_search && keep && index == count - 1 && o.method == fg::Method::motion) {
            interp_.keep_history(context_.Get());
            history_fresh_ = true;
            if (follows && !history_logged_) {
                history_logged_ = true;
                log_line(reshade::log::level::info, "HistorySearch=1: searching around the last pair's motion as well");
            }
        }
        // (A draw that found no motion - Method=blend, a sliver of a window - did not get that far: the mask
        // is told all the same, to let go of what it holds.) Then the mask over the frame that was made.
        if (index == 1 && !paired) hud_pair(a, b, true);
        hud_apply(rtv.Get(), index * 2 < count ? a : b);
    }
    keep_areas(back_buffer, index * 2 < count ? a : b);
    return true;
}

// The areas left alone: the real frame in `slot` (the nearer of the two in
// time) as it is, over what was made there. An area that only says where to
// look for a HUD (find_hud) is hud_apply's - unless the HUD cannot be looked
// for on this picture: then it is kept whole, as it was before there was a
// choice.
void FrameGen::keep_areas(ID3D11Texture2D *back_buffer, int slot) {
    if (g_settings.keep.empty()) return;
    keep_boxes_.clear();
    for (const app::KeepRegion &r : g_settings.keep)
        if (!r.find_hud || hud_failed_) app::region_boxes(r, width_, height_, keep_boxes_);
    for (const app::PixelBox &b : keep_boxes_) {
        const D3D11_BOX box{b.left, b.top, 0, b.right, b.bottom, 1};
        context_->CopySubresourceRegion(back_buffer, 0, b.left, b.top, 0, frames_[slot].Get(), 0, &box);
    }
}

void FrameGen::set_keep(const std::vector<app::KeepRegion> &regions) {
    g_settings.keep = regions;
    reshade::set_config_value(nullptr, kSection, "Keep", app::reshade_keep_text(regions).c_str());
}

void FrameGen::set_hud(bool on) {
    g_settings.hud = on;
    reshade::set_config_value(nullptr, kSection, "Hud", on ? "1" : "0");
    step_again("find the HUD everywhere switched");
    log_line(reshade::log::level::info, "find the HUD everywhere: %s", on ? "on" : "off");
}

// Something asks for the HUD to be found: the switch, or an area that says
// where to look.
bool FrameGen::hud_wanted() const {
    if (g_settings.hud) return true;
    for (const app::KeepRegion &r : g_settings.keep)
        if (r.find_hud) return true;
    return false;
}

// The HUD mask, one pair of real frames on (frames_[a], then frames_[b]):
// once per pair, after its first frame in between was made and before
// hud_apply. drawn: the app's own generator just drew that frame, and its
// motion is the pair's; else (DLSS Frame Generation made it) the motion is
// searched for here, for the mask alone. Changes pipeline state: inside a
// StateBackup.
void FrameGen::hud_pair(int a, int b, bool drawn) {
    hud_live_ = false;
    // Not on an scRGB back buffer (linear values: no levels are known for
    // those), and not while the app's own generator mixes the frames
    // (Method=blend, or a picture too small to follow): a mix moves nothing,
    // so a HUD comes out as it is.
    const bool can = format_ != DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!hud_wanted() || !can) {
        if (hud_.ready()) {
            hud_.release();
            hud_said_.clear();
            log_line(reshade::log::level::info, "HUD mask: off");
        }
        hud_failed_ = hud_wanted() && !can;
        return;
    }
    fg::BlockMotion motion = interp_.block_motion();
    if (drawn && !motion.field) return;
    if (!hud_.ready()) {
        if (hud_failed_) return;
        std::string error;
        if (!hud_.create(device_.Get(), width_, height_, error)) {
            hud_failed_ = true;
            log_line(reshade::log::level::error,
                     "HUD mask: could not be made at %ux%u (%s); areas that look for a HUD are kept whole", width_, height_,
                     error.c_str());
            return;
        }
        hud_updated_ = 0;
    }
    hud_.set_pq(pq());
    // After a break (frame generation paused or off, the source stopped)
    // what was counted says nothing of the picture now.
    const double now = qpc_seconds();
    if (now - hud_updated_ > 1.0) hud_.reset();
    hud_updated_ = now;

    hud_.update_timer.poll(context_.Get());
    hud_.apply_timer.poll(context_.Get());
    hud_.update_timer.begin(context_.Get());
    if (!drawn) {
        // No motion (a picture too small for the search): the plain rule.
        // (ANR's pair has it already.)
        const bool found = pair_flow_ == serial_ && serial_ != 0 && motion.field;
        if (!found) motion = fg::BlockMotion{};
        if (!found && interp_.find_flow(context_.Get(), views_[a].Get(), views_[b].Get())) motion = interp_.block_motion();
    }
    hud_.update(context_.Get(), views_[a].Get(), views_[b].Get(), &motion, picture_made_);
    hud_.update_timer.end(context_.Get());
    if (hud_count_due_) {
        hud_.count_pixels(context_.Get());
        hud_count_due_ = false;
    }
    hud_live_ = true;

    // Said once, and again when it changes.
    int looking = 0;
    for (const app::KeepRegion &r : g_settings.keep) looking += r.find_hud ? 1 : 0;
    const fg::Picture &p = hud_.picture();
    char text[300];
    char where[64] = "everywhere";
    if (!g_settings.hud) snprintf(where, sizeof(where), "in %d area%s", looking, looking == 1 ? "" : "s");
    char picture[96] = "";
    if (p.x1 != 0) snprintf(picture, sizeof(picture), ", the picture %ux%u at %u,%u", p.x1 - p.x0, p.y1 - p.y0, p.x0, p.y0);
    snprintf(text, sizeof(text), "%s, %ux%u%s, %s levels, %s", where, width_, height_, picture, pq() ? "HDR (PQ)" : "SDR",
             hud_.rule() == fg::HudMask::Rule::plain ? "without motion (the picture is too small for a search)"
             : drawn                                 ? "by the generator's motion"
                                                     : "by a motion search of its own (DLSS Frame Generation hands none out)");
    if (hud_said_ != text) {
        hud_said_ = text;
        log_line(reshade::log::level::info, "HUD mask: on, %s", text);
    }
}

// The real frame in `slot` (the nearer of the two in time) over the frame in
// between in target, where the mask is set: over the whole picture with
// Hud=1, else inside the areas that look for a HUD. Changes pipeline state.
void FrameGen::hud_apply(ID3D11RenderTargetView *target, int slot) {
    if (!hud_live_) return;
    size_t count = 0;
    const fg::HudMask::Area *areas = hud_area_list(count);
    if (!g_settings.hud && count == 0) return;
    hud_.apply_timer.begin(context_.Get());
    hud_.apply(context_.Get(), views_[slot].Get(), target, areas, count);
    hud_.apply_timer.end(context_.Get());
}

// Where the mask counts: null with Hud=1 (everywhere), else the areas that
// look for a HUD, in pixels (count 0: none does).
const fg::HudMask::Area *FrameGen::hud_area_list(size_t &count) {
    count = 0;
    if (g_settings.hud) return nullptr;
    hud_areas_.clear();
    for (const app::KeepRegion &r : g_settings.keep) {
        app::PixelBox box;
        if (r.find_hud && app::region_box(r, width_, height_, box))
            hud_areas_.push_back({box.left, box.top, box.right, box.bottom, r.ellipse});
    }
    count = hud_areas_.size();
    return hud_areas_.data();
}

// The log's line about the mask, with the 5 s line.
void FrameGen::hud_stats() {
    if (!hud_.ready() || !g_settings.log_stats) return;
    UINT set = 0;
    const bool counted = hud_.read_count(context_.Get(), set);
    hud_count_due_ = true;
    const double update_ms = hud_.update_timer.take_ms(), apply_ms = hud_.apply_timer.take_ms();
    const double clean_ms = hud_.clean_timer.take_ms();
    const fg::Picture &p = hud_.picture();
    const double pixels = p.x1 != 0 ? double(p.x1 - p.x0) * (p.y1 - p.y0) : double(width_) * height_;
    char share[48] = "not counted yet", cost[200] = "no GPU times yet";
    if (counted) snprintf(share, sizeof(share), "%u px, %.2f %% of the picture", set, 100.0 * set / std::max(1.0, pixels));
    if (update_ms >= 0)
        snprintf(cost, sizeof(cost), "the detector %.2f ms a real frame, the mask's pass %.3f ms a frame in between", update_ms,
                 std::max(0.0, apply_ms));
    if (update_ms >= 0 && clean_ms >= 0)
        snprintf(cost + strlen(cost), sizeof(cost) - strlen(cost), ", the frames without their HUD %.3f ms a frame in between",
                 clean_ms);
    log_line(reshade::log::level::info, "HUD mask: %s | holds %s | GPU: %s", hud_said_.c_str(), share, cost);
}

void FrameGen::toggle(reshade::api::effect_runtime *runtime) {
    // env FRAMEGEN_TOGGLE_AFTER=<seconds>: one toggle that many seconds after
    // the first frame, as if the key had been pressed (for tests).
    const double now = qpc_seconds();
    if (toggle_after_ < 0) {
        const std::string text = app::env("FRAMEGEN_TOGGLE_AFTER");
        toggle_after_ = text.empty() ? 0 : now + atof(text.c_str());
    }
    const bool timed = toggle_after_ > 0 && now >= toggle_after_;
    if (timed) toggle_after_ = 0;
    if (!timed &&
        (g_settings.toggle_key <= 0 || !runtime->is_key_pressed(static_cast<uint32_t>(g_settings.toggle_key))))
        return;
    if (broken_) {
        app::overlay::toast("Frame generation: failed, see ReShade.log");
        return;
    }
    if (!source_ok()) {
        // Not saved: it cannot run either way while the capture keeps the repeated frames (or mpv reads the
        // card itself). The words are the window's: its About tab, "Advanced", "Repeated frames".
        app::overlay::toast("Frame generation needs repeated frames dropped (the window's About tab, Advanced)", 3.0);
        return;
    }
    set_enabled(!g_settings.enabled);
    app::overlay::toast(g_settings.enabled ? "Frame generation: on" : "Frame generation: off");
}

// StepDown's changes (fg_pacing.h), each said in the log with its reason.
void FrameGen::step_changed(const fg::pacing::StepDown::Change &change) {
    using Kind = fg::pacing::StepDown::Change::Kind;
    if (change.kind == Kind::Down) {
        char why[240];
        if (change.left_out <= 0)
            snprintf(why, sizeof(why), "the GPU cannot keep up: mpv dropped new frames while generating");
        else
            snprintf(why, sizeof(why),
                     "%s: %d game frames in the last %.0f s went without their added frames, presents were still queued",
                     change.cause == fg::pacing::StepCause::Gpu
                         ? "the GPU cannot keep up, the first added frame is done too late for the others"
                         : "the screen takes frames late",
                     change.left_out, fg::pacing::StepDown::kWindow);
        log_line(reshade::log::level::warning, "x%d cannot be held (%s); x%d for the next %.0f s", change.from, why,
                 change.to, change.wait);
    } else if (change.kind == Kind::Try) {
        log_line(reshade::log::level::info, "x%d is tried again (%s)", change.to, step_why_[0] ? step_why_ : "its time came");
        step_why_ = "";
    }
}

// Something changed that may be why a multiplier could not be held: the one
// above is tried now, not when its time comes (StepDown::retry).
void FrameGen::step_retry(double now, const char *why) {
    if (!g_settings.step_down || !step_.stepped()) return;
    step_.retry(now);
    step_why_ = why;
}

// A setting of frame generation was changed: what could not be held may hold
// now, so the multiplier is chosen as at start.
void FrameGen::step_again(const char *why) {
    if (step_.stepped())
        log_line(reshade::log::level::info, "%s: no longer stepped down to x%d, the multiplier is chosen anew", why,
                 step_.most(fg::pacing::kMaxMultiplier));
    step_.again();
    step_why_ = "";
}

// Once a second, for StepDown: whether mpv's window changed (its place, its
// size, its style: the window mode) or the source's rate did.
void FrameGen::watch_step(HWND window, double now) {
    RECT rect{};
    LONG style = 0;
    if (window && GetWindowRect(window, &rect)) style = GetWindowLongW(window, GWL_STYLE);
    if (shape_seen_ && (!EqualRect(&rect, &shape_rect_) || style != shape_style_)) step_retry(now, "the window changed");
    shape_seen_ = true;
    shape_rect_ = rect;
    shape_style_ = style;
    if (!g_settings.step_down || !step_.stepped()) return;
    const fg::pacing::SourceCadence::Summary c = cadence_.summary(now);
    if (step_.source(now, c.fps, !c.variable)) step_why_ = "the game's frame rate changed";
}

void FrameGen::set_enabled(bool on) {
    if (on == g_settings.enabled) return;
    g_settings.enabled = on;
    reshade::set_config_value(nullptr, kSection, "Enabled", g_settings.enabled);
    if (capture_.active()) capture_.reset();
    reset_history();
    hud_.reset();
    hud_live_ = false;
    paused_until_ = settle_until_ = 0;
    backoff_ = 5.0;
    drop_times_[0] = drop_times_[1] = -1e9;  // a drop from before does not pair with one after
    gate_.reset();
    step_again("frame generation switched");
    reason_ = app::FramegenStatus::Reason::Starting;
    log_line(reshade::log::level::info, "toggled %s", g_settings.enabled ? "on" : "off");
}

// Method=blend: a 50/50 mix (by how far between, with x3 and x4) instead of
// following the motion. Read at every draw, so it changes at once.
void FrameGen::set_blend(bool blend) {
    g_settings.method = blend ? fg::Method::blend : fg::Method::motion;
    reshade::set_config_value(nullptr, kSection, "Method", blend ? "blend" : "motion");
    step_again("method changed");
    log_line(reshade::log::level::info, "method %s", blend ? "blend" : "motion");
}

// Multiplier: frames shown per source frame, at most. Used from the next
// frame on.
void FrameGen::set_multiplier(int multiplier) {
    g_settings.multiplier = std::clamp(multiplier, 2, fg::pacing::kMaxMultiplier);
    reshade::set_config_value(nullptr, kSection, "Multiplier", g_settings.multiplier);
    gate_.reset();
    step_again("multiplier changed");
    log_line(reshade::log::level::info, "multiplier up to x%d", g_settings.multiplier);
}

// The source's new frames since the last look, into cadence_ (all of them:
// several may have come since mpv's last present, or been dropped before it).
// The menu on a slow picture. The menu (and an area being drawn or dragged,
// the menu faded) is drawn when a frame is presented, and with dedup mpv
// presents only new frames and the frames made between them: 5 to 20 new
// frames a second - a console's home screen, a game's menu - leave short
// clicks unseen and a drag in jerks. So while the menu is open and fewer than
// about 60 frames a second are shown, the app is asked for the repeats too
// (as it is while frame generation is off): mpv then presents every captured
// frame, and the menu is drawn on each.
//
// Those presents are not frames of the game. This says whether the present
// under way is one of them - it carries no new frame - so that it is left out
// of everything: not a real frame, no frame in between made from it, not
// late, not in the rates. A present carries a new frame when one arrived from
// the card before mpv presented (the link's stamps) that no earlier present
// was taken to carry; mpv presents a frame a few milliseconds after it
// arrives and the next comes a capture interval later, so the two are told
// apart unless mpv is held up for longer than that with a repeat in hand.
// Then one new frame goes unused for the frames in between (it is shown).
//
// Called once per present, generating or not: it also keeps the request to
// the app up to date.
bool FrameGen::menu_repeat(double now) {
    const bool linked = g_link.linked();
    const bool running = linked && !broken_ && g_settings.enabled && source_ok();
    bool want = false;
    if (running && app::overlay::menu_open()) {
        // A still picture (a quarter of a second without a new frame), or one
        // shown at under about 60 a second: on below 55, off again from 58.
        const double shown = shown_real_.rate(now) + shown_generated_.rate(now);
        want = g_link.since_last(now) > 0.25 || shown < (menu_wanted_ ? 58.0 : 55.0);
    }
    if (want != menu_wanted_) {
        menu_wanted_ = want;
        if (!want) menu_until_ = now + 0.25;
    }
    // While frame generation is off, mpv gets every frame as without --dedup:
    // nothing gains from new frames only then (DedupWhenOff=1 keeps them).
    if (linked) g_link.send_repeats(want || (!g_settings.dedup_when_off && (broken_ || !g_settings.enabled)));
    if (!linked) return false;
    const LONG64 published = g_link.published();
    if (published < menu_attributed_) menu_attributed_ = published;  // another source
    bool fresh = false;
    for (LONG64 n = published; n > menu_attributed_ && n > published - Link::kKept; --n) {
        if (g_link.arrived(n) + 0.001 <= mpv_presented_) {
            menu_attributed_ = n;
            fresh = true;
            break;
        }
    }
    return running && (want || now < menu_until_) && !fresh;
}

void FrameGen::note_source() {
    const LONG64 published = g_link.published();
    for (LONG64 n = std::max(cadence_seen_, published - Link::kKept) + 1; n <= published; ++n)
        cadence_.add(g_link.arrived(n));
    cadence_seen_ = published;
}

// Once a second: which screen mpv's window is on, and what is known about
// it. The window can be moved to another screen, and screens are switched
// on and off or change their mode while mpv runs. When the screen is another
// one (or the same in another mode), its rate is read from Windows, its VRR
// state is what the exe found for that very screen at start - unknown for
// a screen it did not see - and the count of refreshes and what was made of
// it start over on the new screen.
void FrameGen::watch_screen(IDXGISwapChain *swapchain, double now) {
    watch_step(swapchain_window(swapchain), now);
    const fg::WindowScreen screen = fg::window_screen(swapchain_window(swapchain));
    if (screen_seen_ && screen.same(screen_)) {
        watch_refresh(now);
        return;
    }
    step_retry(now, "another screen, or another mode of it");
    const bool first = !screen_seen_;
    const bool other = first || screen.found != screen_.found || screen.device != screen_.device;
    const std::string before = belief_.name;
    screen_seen_ = true;
    screen_ = screen;

    fg::ScreenNow s;
    s.found = screen.found;
    s.device = utf8(screen.device);
    s.hz_whole = screen.hz_whole;
    if (screen.found) {
        const fg::ScreenConfig config = fg::screen_config(screen.device);
        s.hz_exact = config.hz;
        s.monitor = utf8(config.monitor);
    }
    const double last_hz = display_hz_;
    belief_ = fg::believe(s, g_settings.screens, g_settings.one_told, g_settings.vrr, g_settings.launcher_hz);
    if (!screen.found) {
        // No window, or Windows names no screen for it: DXGI's word for the
        // rate, else the last one known.
        const int hz = dxgi_display_hz(swapchain);
        belief_.hz = hz > 0 ? hz : last_hz;
    }
    vrr_ = belief_.vrr;
    display_hz_ = belief_.hz;

    // What was counted and judged was another screen's (or another mode's):
    // nothing of it is carried over, and nothing is judged this second.
    refresh_.reset();
    seen_logged_ = false;
    count_failure_logged_ = false;
    count_failed_ = 0;
    if (screen.found)
        vblank_.start(screen.device);
    else
        vblank_.stop();

    using Source = fg::ScreenBelief::Source;
    char rate[48] = "refresh rate not known";
    if (display_hz_ > 0) snprintf(rate, sizeof(rate), "%.3f Hz", display_hz_);
    char vrr[160];
    switch (belief_.source) {
    case Source::Told:
        snprintf(vrr, sizeof(vrr), "VRR %s, as " APP_NAME " found it at start", fg::vrr_word(vrr_));
        break;
    case Source::NotTold:
        snprintf(vrr, sizeof(vrr),
                 "VRR not known: " APP_NAME " did not see this screen at start, so nothing is assumed about it");
        break;
    case Source::OneOnly:
        snprintf(vrr, sizeof(vrr), "VRR %s, as " APP_NAME " said of the one screen it described", fg::vrr_word(vrr_));
        break;
    default: snprintf(vrr, sizeof(vrr), "VRR not known"); break;
    }
    if (!screen.found) {
        log_line(reshade::log::level::warning,
                 "the screen the window is on cannot be found: %s (DXGI's), VRR not known, its refreshes are not "
                 "counted",
                 rate);
    } else if (first) {
        log_line(reshade::log::level::info, "the window is on screen %s: %s, %s; its refreshes are counted",
                 belief_.name.c_str(), rate, vrr);
    } else if (other) {
        log_line(belief_.source == Source::NotTold ? reshade::log::level::warning : reshade::log::level::info,
                 "the window is now on screen %s (it was on %s): %s, %s; the count of refreshes starts over",
                 belief_.name.c_str(), before.empty() ? "none" : before.c_str(), rate, vrr);
    } else {
        log_line(reshade::log::level::info,
                 "screen %s changed its mode or place: %ux%u, %s, %s (not read again); the count of refreshes starts "
                 "over",
                 belief_.name.c_str(), unsigned(screen.width), unsigned(screen.height), rate, vrr);
    }
}

// Once a second, on the same screen as the second before: how many times a
// second it was seen refreshing.
void FrameGen::watch_refresh(double now) {
    const fg::VblankCounter::Reading r = vblank_.read();
    const double shown = shown_real_.rate(now) + shown_generated_.rate(now);
    refresh_.sample(r.epoch, r.count, r.counting ? r.at : 0, now, display_hz_, shown);
    // Two seconds in a row: the thread tries again by itself after a failure.
    count_failed_ = !r.counting && r.failure != 0 ? count_failed_ + 1 : 0;
    if (count_failed_ == 2 && !count_failure_logged_) {
        count_failure_logged_ = true;
        log_line(reshade::log::level::warning,
                 "the refreshes of screen %s cannot be counted (0x%08lX): whether VRR engages there is not known",
                 belief_.name.c_str(), static_cast<unsigned long>(r.failure));
    }
    const fg::pacing::Seen seen = refresh_.seen();
    if (seen == fg::pacing::Seen::Fixed && vrr_ == fg::pacing::Vrr::On && !seen_logged_) {
        seen_logged_ = true;
        log_line(reshade::log::level::warning,
                 "VRR is on for this screen, but it keeps refreshing at %.1f Hz while %.1f frames a second are "
                 "shown: VRR is not engaging (not on for windows in the driver, the window not focused, or "
                 "composed by Windows)",
                 refresh_.seen_hz(), shown);
    }
    if (seen == fg::pacing::Seen::Following) seen_logged_ = false;
}

app::FramegenStatus FrameGen::status() const {
    using Status = app::FramegenStatus;
    Status s;
    const double now = qpc_seconds();
    s.enabled = g_settings.enabled && !broken_ && source_ok();
    s.dlss = g_settings.dlss && dlss_.ready() && !dlss_failed_;
    s.anr = g_settings.anr && anr_.ready() && !anr_failed_;
    s.video = g_settings.video && vfg_.ready() && !video_failed_;
    s.hud = s.enabled && hud_live_;
    const fg::pacing::SourceCadence::Summary c = cadence_.summary(now);
    s.source_fps = c.fps;
    s.source_low = c.low;
    s.source_high = c.high;
    s.source_variable = c.variable;
    s.source_uneven = c.uneven;
    s.real_fps = shown_real_.rate(now);
    s.generated_fps = shown_generated_.rate(now);
    s.latency_ms = latency_.mean(now) * 1000.0;
    if (g_link.linked()) {
        const double spread = g_link.gap_spread(g_settings.reset_ms / 1000.0);
        s.arrival_spread_ms = spread >= 0 ? spread * 1000.0 : -1;
    }
    // All of the screen the window is on now (watch_screen).
    snprintf(s.screen, sizeof(s.screen), "%s", belief_.name.c_str());
    s.screen_told = belief_.source == fg::ScreenBelief::Source::Told || belief_.source == fg::ScreenBelief::Source::OneOnly;
    s.display_hz = display_hz_;
    s.cap_hz = output_limit().exact ? g_settings.max_output_hz : 0;
    s.vrr = vrr_;
    s.seen = refresh_.seen();
    s.seen_hz = refresh_.seen_hz();
    s.wanted = g_settings.multiplier;
    if (!s.enabled) return s;

    // What the latest frame decided, when that is a state that lasts; else
    // what got in the way now and then over the last second - if the frames
    // shown really fall short of what was meant.
    s.reason = reason_;
    const bool stepped = reason_ == Status::Reason::SteppedDown;
    const bool none = reason_ != Status::Reason::None && reason_ != Status::Reason::FewerFit && !stepped;
    s.multiplier = none ? 1 : gate_.current();
    if (reason_ == Status::Reason::BackingOff) s.seconds_left = std::max(0.0, paused_until_ - now);
    if (stepped) {
        s.step_from = step_fit_;
        s.step_failed = std::min(step_fit_, step_.most(fg::pacing::kMaxMultiplier) + 1);
        s.step_cause = step_.cause();
        s.seconds_left = step_.seconds_left(now);
    }
    // (Right after a step down the last second's frames are still the
    // higher multiplier's, left out and all: not this one falling short.)
    const bool just_stepped = stepped && step_.since_change(now) < 1.5;
    if (!none && !just_stepped && s.source_fps > 0 && s.real_fps + s.generated_fps < 0.95 * s.multiplier * s.source_fps) {
        const int queue = queue_skips_.count(now), dropped = dropped_.count(now), late = late_.count(now);
        if (mix_skips_.count(now) > 0) {
            s.reason = Status::Reason::EffectsMix;
        } else if (queue > 0 && queue >= dropped) {
            s.reason = Status::Reason::QueueFull;
            s.reason_count = queue;
        } else if (dropped > 0) {
            s.reason = Status::Reason::Drops;
            s.reason_count = dropped;
        } else if (late > 0) {
            s.reason = Status::Reason::Late;
            s.reason_count = late;
        }
    }
    if (s.multiplier > 1 && s.source_fps > 0) {
        const fg::pacing::Verdict v = fg::pacing::judge_cadence(s.multiplier * s.source_fps, display_hz_, s.vrr);
        s.cadence = v.cadence;
        s.better_hz = v.better_hz;
    }
    return s;
}

// Quality=fast: the 2x2 cells around each pixel, compared on the pixel alone.
// Read at every draw, so it changes at once.
void FrameGen::set_fast(bool fast) {
    g_settings.fast = fast;
    interp_.warp_ring = fast ? 2 : 3;
    interp_.warp_taps = fast ? 1 : 5;
    interp_.soft_temp = fast ? 0.02f : 0.04f;
    reshade::set_config_value(nullptr, kSection, "Quality", fast ? "fast" : "high");
    step_again("quality changed");
    log_line(reshade::log::level::info, "quality %s", fast ? "fast" : "high");
}

// From the panel. A check (DebugMarker, LogStats) is kept in ReShade.ini as
// the add-on reads it at start; a switch only while it differs from its
// default (switch_setting).
void save_switch(const char *key, bool on) {
    reshade::set_config_value(nullptr, kSection, key, on ? "1" : "0");
    log_line(reshade::log::level::info, "%s=%d (from the panel)", key, on ? 1 : 0);
}

void save_switch(const char *key, bool on, bool standard) {
    if (on != standard) return save_switch(key, on);
    forget_setting(key);
    log_line(reshade::log::level::info, "%s=%d (from the panel; the default)", key, on ? 1 : 0);
}

// Read at every draw. What was kept for the search is from before the switch:
// the next pair starts without it.
void FrameGen::set_history_search(bool on) {
    g_settings.history_search = on;
    interp_.hist_search = on ? 1 : 0;
    interp_.forget_history();
    history_fresh_ = history_logged_ = false;
    save_switch("HistorySearch", on, kDefaults.history_search);
}

// follow_picture() makes the generator anew, for the picture or for the whole
// frame, a quarter of a second of presents later.
void FrameGen::set_picture_only(bool on) {
    g_settings.picture_only = on;
    save_switch("PictureOnly", on, kDefaults.picture_only);
}

// Off: the multiplier is the most that fits the screen again, at once, and
// nothing is remembered for when it is switched back on.
void FrameGen::set_step_down(bool on) {
    g_settings.step_down = on;
    step_again(on ? "StepDown switched on" : "StepDown switched off");
    save_switch("StepDown", on, kDefaults.step_down);
}

void FrameGen::set_edge_rules(bool on) {
    g_settings.edge_rules = on;
    use_edge_rules();
    save_switch("EdgeRules", on, kDefaults.edge_rules);
}

// Read at every draw.
void FrameGen::set_hud_clean(bool on) {
    g_settings.hud_clean = on;
    save_switch("HudClean", on, kDefaults.hud_clean);
}

void FrameGen::set_still_hud(bool on) {
    g_settings.still_hud = on;
    use_still_hud();
    save_switch("StillHud", on, kDefaults.still_hud);
}

// New frames dropped upstream: mpv is not keeping up with the source.
//
// Drops seen now happened since the previous frame. When that frame had a
// frame in between and mpv came back at its usual pace, the extra present is
// part of why: two such within two seconds pause generation - 5 s the first
// time, twice as long each time it happens again, at most a minute; 30 s after
// the last pause and the last such drop forgive it. With StepDown=1, x4 and
// x3 go one down instead of pausing; the pause is for x2.
//
// Drops across a stall of mpv's own (far longer than the half-interval hold:
// DLSS 5 starting, shaders recompiling) or while not generating are not
// counted against frame generation; it only waits until mpv has gone a second
// without dropping.
void FrameGen::watch_drops(double now, bool stalled) {
    const LONG64 dropped = g_link.dropped();
    const LONG64 fresh = last_dropped_ < 0 ? 0 : dropped - last_dropped_;
    last_dropped_ = dropped;
    for (LONG64 i = 0; i < std::min<LONG64>(fresh, 64); ++i) dropped_.add(now);
    if (fresh <= 0) {
        if (generated_last_ && now - std::max(paused_until_, drop_times_[1]) > 30.0) backoff_ = 5.0;
        return;
    }
    if (stalled || !generated_last_) {
        settle_until_ = now + 1.0;
        return;
    }
    drops_ += static_cast<unsigned>(fresh);
    drop_times_[0] = drop_times_[1];
    drop_times_[1] = now;
    if (fresh < 2 && now - drop_times_[0] > 2.0) return;
    // At x3 and x4, one fewer first (StepDown): x2 may well hold where they
    // do not. mpv gets half a second without added frames to catch up.
    if (g_settings.step_down) {
        if (const fg::pacing::StepDown::Change change = step_.behind(now, pending_count_)) {
            step_changed(change);
            drop_times_[0] = drop_times_[1] = -1e9;  // a drop from before does not pair with one after
            settle_until_ = now + 0.5;
            return;
        }
    }
    paused_until_ = now + backoff_;
    log_line(reshade::log::level::warning,
             "mpv fell behind the source (new frames dropped while generating); frame generation paused for %.0f s",
             backoff_);
    backoff_ = std::min(backoff_ * 2.0, 60.0);
    reset_history();
}

void FrameGen::on_reshade_present(reshade::api::effect_runtime *runtime) {
    reshade::api::device *api_device = runtime->get_device();
    if (api_device->get_api() != reshade::api::device_api::d3d11) return;
    // Both are the original objects under ReShade's proxies, so nothing done
    // through them fires add-on events or runs ReShade's present path again.
    auto *swapchain = reinterpret_cast<IDXGISwapChain *>(runtime->get_native());
    auto *device = reinterpret_cast<ID3D11Device *>(api_device->get_native());
    if (!swapchain || !device) return;
    if (!device_ && !init(device)) return;
    if (device != device_.Get()) return;  // not the device we set up on: not mpv's frame
    mpv_swapchain_ = runtime->get_native();
    screen_trace_.start(swapchain);

    const double now = qpc_seconds();
    if (!g_link.linked()) {
        g_link.poll(now);
        // mpv has only just started: let it settle before the first frame in
        // between, so its start-up drops are not blamed on generating.
        if (g_link.linked()) settle_until_ = now + 2.0;
    }
    // A repeat of the picture, sent for the open menu (menu_repeat): the menu
    // is drawn on it after this returns, and that is all it is for. The keys
    // are still looked at, and the capture's copies read back.
    if (menu_repeat(now)) {
        effects_ran_ = false;
        if (pending_) trace(pending_now_, pending_dt_, pending_queued_, "generated-real-lost");  // as below
        pending_ = false;
        capture_.tick(context_.Get());
        toggle(runtime);
        note_source();
        if (g_settings.capture_key > 0 && runtime->is_key_pressed(static_cast<uint32_t>(g_settings.capture_key)))
            capture_key_seen_ = true;
        if (now >= next_hz_try_) {
            watch_screen(swapchain, now);
            next_hz_try_ = now + 1.0;
        }
        // A still picture: what the menu says of it, as a keepalive frame would have it said.
        if (g_link.since_last(now) > std::max(g_settings.reset_ms / 1000.0, 3.0 * interval_))
            reason_ = app::FramegenStatus::Reason::SourcePaused;
        ++menu_repeats_;
        stats(now);
        return;
    }

    // Whatever the effects did, it was for this frame only.
    const bool neural = effects_ran_;
    effects_ran_ = false;
    const bool history = history_fresh_;
    history_fresh_ = false;  // until this present draws a pair
    // A real frame left over from a present whose finish_present never came
    // (it failed, or was a "still drawing" try) is dropped.
    if (pending_) trace(pending_now_, pending_dt_, pending_queued_, "generated-real-lost");
    pending_ = false;
    capture_.tick(context_.Get());  // a finished capture is read back whatever happens below

    toggle(runtime);
    // For the menu's numbers, whether or not frames are being added.
    note_source();
    shown_real_.add(now);
    // For the menu's delay: how old the card's frame is, when this present
    // carries a new one.
    double new_frame_age = -1;
    if (g_link.linked()) {
        const LONG64 published_now = g_link.published();
        if (published_now != latency_published_) {
            latency_published_ = published_now;
            const double age = g_link.since_last(now);
            if (age >= 0 && age < 0.5) new_frame_age = age;
        }
    }
    if (now >= next_hz_try_) {
        // Every second: the window may move to another screen or the mode
        // change.
        watch_screen(swapchain, now);
        next_hz_try_ = now + 1.0;
    }
    // (Whether mpv gets the repeats too - frame generation off, or the menu
    // open on a slow picture - was decided in menu_repeat.)
    // Also with the in-game menu open (it is drawn after the frames saved).
    const bool capture_key =
        capture_key_seen_ ||
        (g_settings.capture_key > 0 && runtime->is_key_pressed(static_cast<uint32_t>(g_settings.capture_key)));
    capture_key_seen_ = false;
    if (broken_ || !g_settings.enabled || !source_ok()) {
        if (new_frame_age >= 0) latency_.add(now, new_frame_age);
        last_real_ = now;
        generated_last_ = false;
        if (g_link.linked()) last_dropped_ = g_link.dropped();
        if (capture_.active()) capture_.reset();  // its frames would not be in a row
        // No frames to save: the bug report comes without them.
        if (capture_key && !capture_.busy()) mpv_bug_report(L"", g_settings.enabled ? "unavailable" : "off");
        return;
    }

    // The time since mpv's previous frame. Every present counts as a real
    // frame, including mpv drawing the same video frame again for its OSD: the
    // frame in between is then made from that redraw and the next frame, which
    // is the picture on screen anyway. Telling redraws apart by timing alone
    // fails on a jittery source, and a real frame wrongly left out would make
    // the next frame in between step back in time.
    const double dt = last_real_ > 0 ? now - last_real_ : 0;
    last_real_ = now;
    const double reset = g_settings.reset_ms / 1000.0;
    // A gap - longer than ResetMs, or than 2.5 frames once the pace is known
    // - starts the timing over, so pauses never stretch the interval.
    const bool gap = dt <= 0 || dt > reset || (intervals_seen_ >= 3 && dt > 2.5 * dt_interval_);
    if (gap)
        reset_history();
    else
        push_interval(dt);
    source_interval_ = g_link.source_interval(reset);
    interval_ = g_link.linked() ? source_interval_ : (intervals_seen_ >= 3 ? dt_interval_ : 0);
    if (g_link.linked()) watch_drops(now, gap || (interval_ > 0 && dt > 2.0 * interval_));
    source_age_ = g_link.linked() ? g_link.since_last(now) : -1;
    effects_ = mpv_presented_ > 0 && now >= mpv_presented_ ? now - mpv_presented_ : -1;
    // This present carries a new frame if the source delivered one since the
    // last: then it tells how long new frames take from arrival to here.
    const LONG64 published = g_link.published();
    if (published != last_published_ && source_age_ >= 0 && source_age_ < reset && effects_ >= 0 &&
        source_age_ >= effects_)
        arrival_to_present_ = source_age_ - effects_;
    last_published_ = published;

    ComPtr<ID3D11Texture2D> back_buffer;
    D3D11_TEXTURE2D_DESC bb{};
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
        if (capture_.active()) capture_.reset();
        return;
    }
    back_buffer->GetDesc(&bb);
    if (presented_color_space_ != color_space_) {
        color_space_ = presented_color_space_;
        log_line(reshade::log::level::info, "colour space %s", color_space_name(color_space_));
        dlss_.drop_frames();  // it was told SDR or HDR: set up again (dlss_setup)
        anr_.drop_frames();
        if (capture_.active()) capture_.reset();  // its frames would not all be one encoding
    }
    if (bb.SampleDesc.Count != 1 || !ensure_frames(bb)) {
        if (capture_.active()) capture_.reset();
        return;
    }
    follow_picture(bb);
    if (!frames_[0]) return;

    const int cur = prev_ == 0 ? 1 : 0;
    context_->CopyResource(frames_[cur].Get(), back_buffer.Get());
    neural_[cur] = neural;
    ++real_;
    ++serial_;
    if (prev_ >= 0 && interval_ > 0 && dt > 1.5 * interval_) {
        ++late_frames_;  // still generated
        late_.add(now);
    }

    using Reason = app::FramegenStatus::Reason;
    int queued = -1;
    bool generate = prev_ >= 0 && interval_ > 0;
    Reason why = generate ? Reason::None : Reason::Starting;
    // How many frames to show for this source frame: the most that fits the
    // screen, up to the setting - or, while a multiplier that fits cannot be
    // held (StepDown), up to the one below it. That one above is tried again
    // when its time has come, and at once when ReShade's effects start or
    // stop running (DLSS 5 switched), which changes what the GPU has to do -
    // on a frame that will get its frames, not while the picture stands
    // still or frame generation waits: a try nothing is seen of tells nothing.
    const int asked = g_settings.multiplier;
    const bool source_paused = g_link.linked() && g_link.since_last(now) > std::max(reset, 3.0 * interval_);
    int most = asked;
    if (g_settings.step_down) {
        if (neural != step_neural_) step_retry(now, "ReShade's effects started or stopped");
        if (const unsigned squeezed = hold_.squeezed(); squeezed != step_squeezed_) {
            step_.squeezed(now, squeezed - step_squeezed_);
            step_squeezed_ = squeezed;
        }
        if (generate && !source_paused && now >= paused_until_ && now >= settle_until_)
            step_changed(step_.due(now, asked));
        most = step_.most(asked);
    }
    step_neural_ = neural;
    const double source_fps = interval_ > 0 ? 1.0 / interval_ : 0;
    int count = interval_ > 0 ? gate_.choose(now, source_fps, most, output_limit(), vrr_, display_hz_) : 1;
    // What it would be without the step down, for the menu.
    step_fit_ = most < asked ? std::max(count, fg::pacing::fitting(source_fps, asked, output_limit(), vrr_, display_hz_, count))
                             : count;
    if (generate && count < 2) {
        generate = false;  // the display cannot show even twice this rate
        ++skipped_fast_;
        why = Reason::TooFast;
    }
    if (generate && source_paused) {
        generate = false;  // the source is paused: this is a keepalive repeat
        ++skipped_paused_;
        why = Reason::SourcePaused;
    }
    if (generate && now < paused_until_) {
        generate = false;
        ++skipped_backoff_;
        why = Reason::BackingOff;
    }
    if (generate && now < settle_until_) {
        generate = false;  // mpv dropped frames on its own a moment ago
        ++skipped_settle_;
        why = Reason::Settling;
    }
    if (generate && count < g_settings.multiplier) why = Reason::FewerFit;
    if (generate && count < step_fit_) why = Reason::SteppedDown;
    reason_ = why;
    // These two come and go frame by frame: counted, for the menu to name
    // when the frames shown fall short.
    if (generate && neural_[prev_] != neural) {
        generate = false;  // the effects ran for one frame and not the other
        ++skipped_mixed_;
        mix_skips_.add(now);
    }
    bool left_out = false, trimmed = false;
    const int whole = count;  // before any are taken off for a screen that is behind
    if (generate && (queued = queued_presents(swapchain)) >= 2) {
        // The screen is behind: every present more would only wait. At x3
        // and x4 this game frame gets as many frames fewer as the screen is
        // behind by (Trim=1), which is what catches up: at a rate right at
        // the screen's own (29.97 x4 on 120 Hz) nothing else can, and
        // leaving all of them out showed as one frame held four times as
        // long. Only when that would leave none is the game frame shown alone.
        const int fewer = count - (queued - 1);
        if (g_settings.trim && fewer >= 2) {
            count = fewer;
            trimmed = true;
            ++trimmed_;
            trims_.add(now);
        } else {
            generate = false;
            left_out = true;
            ++skipped_queue_;
            queue_skips_.add(now);
        }
    }
    // StepDown counts the game frames that get their frames and those left
    // out this way; too many of the latter at x3 or x4, and the next game
    // frame on gets one fewer. A game frame with fewer frames counts as left
    // out once that happens to one in six: then one fewer for all is evener.
    constexpr int kManyTrims = 10;  // in fg::pacing::StepDown::kWindow seconds, at 30 game frames a second
    if (g_settings.step_down && (generate || left_out))
        step_changed(step_.frame(now, whole,
                                 left_out || (trimmed && trims_.count(now, fg::pacing::StepDown::kWindow) >= kManyTrims)));

    // The last pair's motion says where things go on to only at an even pace:
    // a late frame, or mpv drawing the same frame again for its OSD, is a pair
    // of another length.
    const bool steady = interval_ > 0 && dt > 0.5 * interval_ && dt < 1.5 * interval_;
    pending_follows_ = history && steady;
    pending_keep_ = steady;
    if (generate && draw_between(back_buffer.Get(), prev_, cur, 1, count, pending_follows_, pending_keep_)) {
        pending_ = true;
        pending_slot_ = cur;
        pending_prev_slot_ = prev_;
        pending_count_ = count;
        // The real frame goes out half an interval after the frame in between
        // (never more than 40 ms: below 12.5 fps there is little left to
        // smooth): held, half an interval after it was done; else half an
        // interval after now. Held, it goes at the latest half a millisecond
        // before the next frame's effects could start - even if that is
        // sooner than half an interval: holding longer would hold up the next
        // frame's DLSS 5 pass, queued behind it. The next new frame may arrive
        // as soon as the shortest recent gap after the latest one, and takes
        // about as long from there to mpv's present as new frames have lately
        // (this present may be an OSD redraw that carries none). When that
        // cannot be told, no later than the old time.
        // With x3 and x4 the same holds for a third or a quarter: the
        // frames go out that step apart, the real one last.
        constexpr double kNextEffectsMargin = 0.0005;
        pending_step_ = std::min(interval_, 0.08) / count;
        pending_latest_ = now + pending_step_ * (count - 1);
        const double soonest = g_link.shortest_recent_gap(reset);
        if (soonest > 0 && source_age_ >= 0 && arrival_to_present_ >= 0)
            pending_latest_ = now - source_age_ + soonest + arrival_to_present_ - kNextEffectsMargin;
        pending_now_ = now;
        pending_dt_ = dt;
        pending_swapchain_ = swapchain;
        pending_since_ = qpc_seconds();
        generated_ += count - 1;
        // Each at about the time it is shown, so the menu's rate over the
        // last second comes out even.
        for (int i = 1; i < count; ++i) shown_generated_.add(now + pending_step_ * (i - 1));
    }
    generated_last_ = pending_;
    if (new_frame_age >= 0) {
        // With frames in between, the real frame goes out last: a step after
        // each of them, or sooner when the next frame is due (pending_latest_).
        const double wait =
            pending_ ? std::max(0.0, std::min(pending_latest_, now + pending_step_ * (pending_count_ - 1)) - now) : 0.0;
        latency_.add(now, new_frame_age + wait);
    }
    // env FRAMEGEN_CAPTURE_AFTER=<seconds>: a capture that many seconds after
    // frame generation starts, as if the key had been pressed (for tests).
    if (capture_after_ < 0) {
        const std::string text = app::env("FRAMEGEN_CAPTURE_AFTER");
        capture_after_ = text.empty() ? 0 : now + atof(text.c_str());
    }
    const bool timed = capture_after_ > 0 && now >= capture_after_;
    if (timed) capture_after_ = 0;
    if (!capture_.busy() && (timed || capture_key)) {
        if (!capture_.start(device_.Get(), bb, pq(), !timed)) {
            app::overlay::toast("Frame generation: cannot capture this picture format");
            if (!timed) mpv_bug_report(L"", "failed");
        }
    }
    if (pending_) capture_.between(context_.Get(), back_buffer.Get(), 1, pending_count_ - 1);  // it holds the first frame in between now
    capture_.real(context_.Get(), frames_[cur].Get());
    if (pending_)
        pending_queued_ = queued;  // traced once the real frame went out
    else
        trace(now, dt, queued, prev_ < 0 ? "first" : "real-only");
    prev_ = cur;
    if (!pending_) stats(now);  // else after the real frame went out
}

void FrameGen::on_finish_present(reshade::api::swapchain *api_swapchain) {
    if (!pending_) return;
    auto *swapchain = reinterpret_cast<IDXGISwapChain *>(api_swapchain->get_native());
    if (swapchain != pending_swapchain_) return;  // another swap chain's present: B still waits for mpv's
    pending_ = false;
    if (broken_) {
        trace(pending_now_, pending_dt_, pending_queued_, "generated-real-lost");
        return;
    }

    const double returned = qpc_seconds();
    mpv_present_max_ = std::max(mpv_present_max_, returned - pending_since_);
    mpv_present_sum_ += returned - pending_since_;

    // After the present, buffer 0 is the next buffer in the chain (D3D11 keeps
    // that name for whichever is current) and holds nothing useful.
    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
        trace(pending_now_, pending_dt_, pending_queued_, "generated-real-lost");
        return;
    }

    dlss_setup();
    if (hold_wanted_) {
        // Here, not in init: creating the D3D12 device runs other add-ons'
        // device events, which should not happen under ReShade's locks.
        hold_wanted_ = false;
        std::string error;
        if (hold_.start(device_.Get(), context_.Get(), error))
            log_line(reshade::log::level::info, "the real frame is held on the GPU until its time");
        else
            log_line(reshade::log::level::warning,
                     "the real frame cannot be held on the GPU (%s); it is timed from mpv's present", error.c_str());
    }

    // Frame 1 of pending_count_ is on its way (mpv's present). Now the later
    // frames in between, if any, and last the real frame: each put into the
    // back buffer (after a present, buffer 0 is the next one) and presented
    // through the original swap chain, spaced as the screen needs.
    const int count = pending_count_;
    HRESULT hr = S_OK;
    for (int frame = 2; frame <= count && SUCCEEDED(hr); ++frame) {
        const bool real = frame == count;
        uint64_t held = 0;
        // The signal that the frame before is done comes before this one's
        // drawing: the GPU draws it while that one is on screen.
        const bool hold = hold_.active();
        if (hold) held = hold_.mark_drawn();
        // Frame 1 was drawn at 1 / count of the way from A to B; this one is at frame / count.
        if (real || !draw_between(back_buffer.Get(), pending_prev_slot_, pending_slot_, frame, count, pending_follows_,
                                  pending_keep_))
            context_->CopyResource(back_buffer.Get(), frames_[pending_slot_].Get());
        else
            capture_.between(context_.Get(), back_buffer.Get(), frame, count - 1);
        app::overlay::draw_again(context_.Get(), back_buffer.Get());

        TraceLine line = trace_line(pending_now_, pending_dt_, pending_queued_, real ? "generated" : "between");
        line.frame = frame;
        line.of = count;
        if (hold) {
            // Sync interval 1, like mpv: a 0 could drop a frame in between
            // still queued, and more than 1 would hold the frame longer than
            // its step.
            hold_.wait_release(held);
            hr = swapchain->Present(1, 0);
            // Let go even if the present failed: the wait is queued either way.
            HoldJob job;
            job.n = held;
            job.index = frame - 1;
            job.count = count - 1;
            job.step = pending_step_;
            job.lead = lead_;
            job.latest = pending_latest_;
            job.line = line;
            hold_.let_go(job);
        } else {
            const double target = pending_now_ + pending_step_ * (frame - 1);
            sleeper_.until(target);
            const double out = qpc_seconds();
            late_max_ = std::max(late_max_, out - target);
            late_sum_ += out - target;
            hr = swapchain->Present(1, 0);
            line.real = out - pending_now_;
            write_trace(line);
        }
    }
    out_max_ = std::max(out_max_, qpc_seconds() - returned);
    if (FAILED(hr)) fail("Present of the real frame", hr);
    stats(qpc_seconds());  // no ReShade lock held here, all frames queued
}

void FrameGen::stats(double now) {
    if (stats_start_ == 0) {
        stats_start_ = now;
        return;
    }
    const double span = now - stats_start_;
    if (span < 5.0) return;
    const Hold::Stats held = hold_.take_stats();
    if (g_settings.log_stats) {
        const double n = generated_ ? double(generated_) : 1.0;
        char out[512];
        if (hold_.active() && held.frames == 0) {
            snprintf(out, sizeof(out), "real frame held on the GPU: none in this window");
        } else if (hold_.active()) {
            const double d = held.drawn ? double(held.drawn) : 1.0;
            const double f = held.firsts ? double(held.firsts) : 1.0;
            snprintf(out, sizeof(out),
                     "real frame held on the GPU %.2f ms avg after the frame in between was done (done %.2f ms "
                     "avg after the add-on drew it), let go %.3f ms late avg, %.3f max; of %u: %u timed (%u of "
                     "them let go early for the next frame, %u already due when done), %u without the done "
                     "signal, %u with a later frame (queue full, resize); queued in %.2f ms max",
                     held.held_sum / d * 1000.0, held.drawn_sum / f * 1000.0,
                     held.late_sum / (held.timed ? double(held.timed) : 1.0) * 1000.0, held.late_max * 1000.0,
                     held.frames, held.drawn, held.early, held.overdue, held.not_drawn, held.at_once,
                     out_max_ * 1000.0);
        } else {
            snprintf(out, sizeof(out), "real frame out late by %.3f ms avg, %.3f max", late_sum_ / n * 1000.0,
                     late_max_ * 1000.0);
        }
        const fg::pacing::SourceCadence::Summary c = cadence_.summary(now);
        char source[64] = "";
        if (c.variable)
            snprintf(source, sizeof(source), ", %.0f-%.0f fps, variable", c.low, c.high);
        else if (c.uneven)
            snprintf(source, sizeof(source), ", uneven steps");
        const fg::pacing::Limit limit = output_limit();
        // StepDown: "(x3 of up to x4, stepped down from x4 for the screen, x4 tried again in 12 s)".
        char step[96] = "";
        if (g_settings.step_down && step_.stepped() && gate_.current() < step_fit_)
            snprintf(step, sizeof(step), ", stepped down from x%d for the %s, x%d tried again in %.0f s", step_fit_,
                     step_.cause() == fg::pacing::StepCause::Gpu ? "GPU" : "screen",
                     step_.most(fg::pacing::kMaxMultiplier) + 1, step_.seconds_left(now));
        log_line(reshade::log::level::info,
                 "%.1f real + %.1f generated fps (x%d of up to x%d%s) | interval %.2f ms (%s%s) | display %.2f Hz "
                 "(seen refreshing at %.1f), VRR %s, cap %.0f Hz | %u late frames, %u dropped upstream after a "
                 "generated frame | not generated: %u too fast, %u queue full, %u source paused, %u backing off, "
                 "%u mpv settling, %u effects mix | %u game frames with fewer frames (the screen was behind) | mpv "
                 "present %.2f ms avg, %.2f max | %s",
                 real_ / span, generated_ / span, gate_.current(), g_settings.multiplier, step, interval_ * 1000.0,
                 g_link.linked() ? "source" : "mpv", source, display_hz_, refresh_.seen_hz(), fg::vrr_word(vrr_),
                 limit.hz, late_frames_, drops_, skipped_fast_, skipped_queue_, skipped_paused_, skipped_backoff_,
                 skipped_settle_, skipped_mixed_, trimmed_, mpv_present_sum_ / n * 1000.0, mpv_present_max_ * 1000.0,
                 out);
        hud_stats();
        if (menu_repeats_ > 0)
            log_line(reshade::log::level::info,
                     "the menu was open on a slow picture: %.1f repeats of it a second shown as well, for the menu to be "
                     "drawn on (%.1f presents a second in all); they are not in the numbers above",
                     menu_repeats_ / span, (real_ + generated_ + menu_repeats_) / span);
    }
    if (g_trace) fflush(g_trace);
    stats_start_ = now;
    real_ = generated_ = skipped_fast_ = skipped_mixed_ = skipped_queue_ = skipped_paused_ = skipped_backoff_ = 0;
    skipped_settle_ = trimmed_ = 0;
    late_frames_ = drops_ = 0;
    menu_repeats_ = 0;
    late_max_ = late_sum_ = mpv_present_max_ = mpv_present_sum_ = 0;
    out_max_ = 0;
}

// ---- ReShade events -----------------------------------------------------------

void on_reshade_present(reshade::api::effect_runtime *runtime) {
    // A screenshot first: the back buffer still holds mpv's real frame.
    app::overlay::before_frame_generation(runtime);
    g_gen.on_reshade_present(runtime);
    // The menu last: over the frame in between (or the frame) mpv presents now.
    app::overlay::on_reshade_present(runtime);
}

void on_finish_present(reshade::api::command_queue *, reshade::api::swapchain *swapchain) {
    g_gen.on_finish_present(swapchain);
}

void on_finish_effects(reshade::api::effect_runtime *runtime, reshade::api::command_list *, reshade::api::resource_view,
                       reshade::api::resource_view) {
    g_gen.on_effects_finished(runtime);
}

void on_present(reshade::api::command_queue *, reshade::api::swapchain *swapchain, const reshade::api::rect *,
                const reshade::api::rect *, uint32_t, const reshade::api::rect *) {
    g_gen.on_mpv_present(swapchain);
    app::overlay::note_color_space(swapchain->get_color_space());
}

void register_present_events() {
    reshade::register_event<reshade::addon_event::present>(on_present);
    reshade::register_event<reshade::addon_event::reshade_finish_effects>(on_finish_effects);
    reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
    reshade::register_event<reshade::addon_event::finish_present>(on_finish_present);
}

void unregister_present_events() {
    reshade::unregister_event<reshade::addon_event::finish_present>(on_finish_present);
    reshade::unregister_event<reshade::addon_event::reshade_present>(on_reshade_present);
    reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(on_finish_effects);
    reshade::unregister_event<reshade::addon_event::present>(on_present);
}

// ---- The panel on ReShade's Add-ons page --------------------------------------
//
// Under this add-on's name on the Add-ons page of ReShade's own overlay (Home):
// what the generator is doing, and its switches, so one can be flipped while
// looking at the scene it is about. ReShade draws its overlay in mpv's
// present, on the same thread as the events above, into the frame the
// generator then takes. Frame generation itself and its quality are not here:
// those are the app's settings (the settings file, its menu), written into
// ReShade.ini at every start.
void draw_panel(reshade::api::effect_runtime *) {
    namespace ui = fg::reshade_ui;
    char line[200];
    double source = 0, real = 0, generated = 0;
    g_gen.rates(source, real, generated);
    if (g_gen.broken())
        ui::text("Failed: see ReShade.log.");
    else if (!g_gen.ready())
        ui::text("Not running: it needs " APP_NAME "'s own capture with dedup.");
    else if (!g_settings.enabled)
        ui::text("Off (F8, or " APP_NAME "'s menu: Tab).");
    else if (real <= 0)
        ui::text("On.");
    else {
        snprintf(line, sizeof(line), "On: %.1f real + %.1f generated frames a second.", real, generated);
        ui::text(line);
    }
    if (const app::FramegenStatus st = g_gen.status(); st.reason == app::FramegenStatus::Reason::SteppedDown) {
        snprintf(line, sizeof(line), "Stepped down to x%d: x%d could not be held (%s). Tried again in %.0f s.", st.multiplier,
                 st.step_from, st.step_cause == app::FramegenStatus::StepCause::Gpu ? "the GPU" : "the screen takes frames late",
                 std::ceil(st.seconds_left));
        ui::text(line);
    }
    UINT width = 0, height = 0;
    fg::Picture picture;
    g_gen.working_on(width, height, picture);
    if (width != 0 && picture.x1 != 0) {
        snprintf(line, sizeof(line), "Works on the picture alone: %ux%u at %u,%u of the %ux%u frame.",
                 picture.x1 - picture.x0, picture.y1 - picture.y0, picture.x0, picture.y0, width, height);
        ui::text(line);
    } else if (width != 0) {
        snprintf(line, sizeof(line), "Works on the whole %ux%u frame%s.", width, height,
                 g_settings.picture_only ? " (no black bars in it)" : "");
        ui::text(line);
    }

    ui::heading("Switches");
    bool on = g_settings.picture_only;
    if (ui::checkbox("Picture only", &on)) g_gen.set_picture_only(on);
    ui::tooltip("With black bars around the picture (a 16:9 game on a 21:9 screen), frames are\n"
                "generated from the picture alone. Better scenery at the picture's edges and less\n"
                "GPU time; a HUD near those edges loses the bars' protection (Still HUD is for that).");
    on = g_settings.edge_rules;
    if (ui::checkbox("Edge rules", &on)) g_gen.set_edge_rules(on);
    ui::tooltip("In a camera turn, the strip along the edges that only one frame shows is drawn\n"
                "along the turn's motion. Meant to go with Picture only where there are bars.");
    on = g_settings.still_hud;
    if (ui::checkbox("Still HUD", &on)) g_gen.set_still_hud(on);
    ui::tooltip("Icons and digits drawn over fast-moving scenery are held where they are instead of\n"
                "fading or breaking up; the scenery right beside them (16 px) stands still with them.\n"
                "Gives a HUD near the picture's edges back what Picture only takes from it; also acts\n"
                "on a frame without bars.");
    on = g_settings.hud_clean;
    if (ui::checkbox("HUD not carried", &on)) g_gen.set_hud_clean(on);
    ui::tooltip("With the HUD mask (" APP_NAME "'s own frames): the frame in between is drawn from the two real\n"
                "frames without the HUD the mask holds, so no faint copy of an icon is carried along with\n"
                "the scenery. Does nothing while the HUD is not looked for.");
    on = g_settings.history_search;
    if (ui::checkbox("History search", &on)) g_gen.set_history_search(on);
    ui::tooltip("A test: the motion search also starts from what each block did over the pair before.");
    on = g_settings.step_down;
    if (ui::checkbox("Step down", &on)) g_gen.set_step_down(on);
    ui::tooltip("Where x4 or x3 fits the screen but cannot be held (game frames go without their added\n"
                "frames because the screen takes them late, or the GPU cannot keep up), one fewer is shown\n"
                "for every game frame, so that each frame stays the same time. The higher one is tried again\n"
                "after 20 s, then after twice as long each time, and at once when a setting, the window or\n"
                "the screen changes. Off: always the most that fits the screen.");

    ui::heading("Checks");
    on = g_settings.marker;
    if (ui::checkbox("Marker on generated frames", &on)) {
        g_settings.marker = on;
        save_switch("DebugMarker", on);
    }
    ui::tooltip("A magenta square in the corner of every generated frame.");
    on = g_settings.log_stats;
    if (ui::checkbox("Log the rates", &on)) {
        g_settings.log_stats = on;
        save_switch("LogStats", on);
    }
    ui::tooltip("A line in ReShade.log every 5 s: frame rates, timings, why frames were not generated.");

    ui::space();
    ui::note("Each works at once and is remembered (ReShade.ini, [" APP_ENV_PREFIX "FRAMEGEN]).");
    ui::note("As they come: all on but History search.");
    ui::note("What each gains and loses: docs/FRAMEGEN-LEARNINGS.md in " APP_NAME "'s source.");
}

void set_menu_host();

void on_init_effect_runtime(reshade::api::effect_runtime *) {
    static bool loaded = false;
    if (!loaded) {
        load_settings();
        set_menu_host();
        loaded = true;
    }
    // ReShade calls add-ons in the order they registered, and loads them in
    // whatever order the folder lists them. Registering again now, after all
    // of them loaded, puts us last regardless of the file name.
    unregister_present_events();
    register_present_events();
}

// Everything is let go with mpv's swap chain or device - not with the effect
// runtime, which ReShade rebuilds on every resize, and not with devices other
// add-ons make for themselves.
void on_destroy_swapchain(reshade::api::swapchain *swapchain, bool resize) {
    if (!g_gen.owns(swapchain->get_device())) return;
    if (resize)
        g_gen.release_held();  // ResizeBuffers would wait for the held present
    else
        g_gen.release();
}

void on_destroy_device(reshade::api::device *device) {
    if (!g_gen.owns(device)) return;
    app::overlay::release();
    g_gen.release();
}

void set_menu_host() {
    app::overlay::Host host;
    host.framegen_ready = [] { return g_gen.ready(); };
    host.framegen_enabled = [] { return g_settings.enabled; };
    host.set_framegen_enabled = [](bool on) { g_gen.set_enabled(on); };
    host.framegen_fast = [] { return g_settings.fast; };
    host.set_framegen_fast = [](bool fast) { g_gen.set_fast(fast); };
    host.framegen_blend = [] { return g_settings.method == fg::Method::blend; };
    host.set_framegen_blend = [](bool blend) { g_gen.set_blend(blend); };
    host.framegen_dlss_offered = [] { return g_gen.dlss_offered(); };
    host.framegen_dlss = [] { return g_settings.dlss; };
    host.set_framegen_dlss = [](bool dlss) { g_gen.set_dlss(dlss); };
    host.framegen_anr_offered = [] { return g_gen.anr_offered(); };
    host.framegen_anr = [] { return g_settings.anr; };
    host.set_framegen_anr = [](bool anr) { g_gen.set_anr(anr); };
    host.framegen_video_offered = [] { return g_gen.video_offered(); };
    host.framegen_video = [] { return g_settings.video; };
    host.set_framegen_video = [](bool video) { g_gen.set_video(video); };
    host.framegen_video_model = [] { return g_settings.video_quality; };
    host.set_framegen_video_model = [](int model) { g_gen.set_video_model(model); };
    host.framegen_keep = [] { return g_settings.keep; };
    host.set_framegen_keep = [](const std::vector<app::KeepRegion> &regions) { g_gen.set_keep(regions); };
    host.framegen_hud = [] { return g_settings.hud; };
    host.set_framegen_hud = [](bool on) { g_gen.set_hud(on); };
    host.framegen_multiplier = [] { return g_settings.multiplier; };
    host.set_framegen_multiplier = [](int multiplier) { g_gen.set_multiplier(multiplier); };
    host.framegen_status = [] { return g_gen.status(); };
    host.mpv_pipe = g_settings.mpv_pipe;
    app::overlay::set_host(host);
}

bool g_panel = false;

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(module)) return FALSE;
        // A ReShade without the overlay (or with another Dear ImGui): no
        // panel, the switches stay in ReShade.ini.
        g_panel = fg::reshade_ui::init(reshade::internal::get_reshade_module_handle());
        if (g_panel) reshade::register_overlay(nullptr, draw_panel);
        reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
        reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
        reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
        register_present_events();
        break;
    case DLL_PROCESS_DETACH:
        if (g_panel) reshade::unregister_overlay(nullptr, draw_panel);
        unregister_present_events();
        reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
        reshade::unregister_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
        reshade::unregister_addon(module);
        g_link.close();
        if (g_trace) fclose(g_trace);
        break;
    }
    return TRUE;
}
