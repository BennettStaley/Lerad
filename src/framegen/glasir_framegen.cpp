// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2026 Abhito
// zz-glasir-framegen.addon64 - frame generation inside mpv for Glasir.
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
// fps game captured at 60 arrives as. Glasir's dedup sends only new
// frames and opens a link to this add-on (framegen_link.h) with when each
// one arrived and how many mpv had to drop; without that link the add-on stays
// off unless RequireDedupLauncher=0.
//
// Settings live in ReShade.ini, section [GLASIR_FRAMEGEN]; F8 toggles it.

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

#include "fg_interp.h"
#include "fg_pacing.h"
#include "framegen_link.h"
#include "overlay.h"

using Microsoft::WRL::ComPtr;

extern "C" __declspec(dllexport) const char *NAME = "Glasir Frame Generation";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Raises the frame rate of Glasir by presenting generated frames between each pair of real frames.";

namespace {

constexpr const char *kSection = "GLASIR_FRAMEGEN";

void log_line(reshade::log::level level, const char *fmt, ...) {
    char text[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    reshade::log::message(level, text);
}

// ---- Settings ----------------------------------------------------------------

struct Settings {
    bool enabled = true;
    bool require_dedup = true;  // only run with the link from Glasir's dedup
    fg::Method method = fg::Method::motion;  // motion = motion-compensated, blend = 50/50 mix
    bool fast = false;          // Quality=fast: fewer candidates per pixel, about half the drawing cost
    int multiplier = 2;         // frames shown per source frame, at most: 2, 3 or 4
    bool marker = false;        // magenta square on generated frames
    bool log_stats = true;      // a line in ReShade.log every 5 s
    int max_output_hz = 0;      // 0 = the display's refresh rate
    int reset_ms = 100;         // a longer gap between real frames starts over
    int toggle_key = VK_F8;
    int capture_key = VK_F9;    // saves the frames around this moment (Capture below)
    bool dedup_when_off = false;  // 1: mpv gets new frames only while frame generation is off too
    std::string mpv_pipe = "glasir";
    // What Glasir.exe found out about the screen (its environment variables
    // GLASIR_DISPLAY_VRR and GLASIR_DISPLAY_HZ), not from ReShade.ini.
    fg::pacing::Vrr vrr = fg::pacing::Vrr::Unknown;
    double launcher_hz = 0;  // 0: not told
};

Settings g_settings;

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

void setting(const char *key, bool &value) {
    std::string text;
    if (!read_text(key, text)) {
        reshade::set_config_value(nullptr, kSection, key, value ? "1" : "0");
        return;
    }
    for (auto &c : text) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (text == "1" || text == "true" || text == "yes" || text == "on")
        value = true;
    else if (text == "0" || text == "false" || text == "no" || text == "off")
        value = false;
    else
        bad_setting(key, text);
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
    s.reset_ms = std::clamp(s.reset_ms, 20, 1000);
    setting("Multiplier", s.multiplier);
    if (s.multiplier < 2 || s.multiplier > fg::pacing::kMaxMultiplier) {
        bad_setting("Multiplier", std::to_string(s.multiplier));
        s.multiplier = 2;
    }
    char env[32] = {};
    DWORD len = GetEnvironmentVariableA("GLASIR_DISPLAY_VRR", env, sizeof(env));
    if (len > 0 && len < sizeof(env))
        s.vrr = _stricmp(env, "on") == 0    ? fg::pacing::Vrr::On
                : _stricmp(env, "off") == 0 ? fg::pacing::Vrr::Off
                                            : fg::pacing::Vrr::Unknown;
    len = GetEnvironmentVariableA("GLASIR_DISPLAY_HZ", env, sizeof(env));
    if (len > 0 && len < sizeof(env)) s.launcher_hz = std::max(0.0, atof(env));
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

// ---- The link from Glasir --------------------------------------------

class Link {
public:
    // Looks for the block once a second until Glasir has made it
    // (it starts mpv first, then creates the block under mpv's process id).
    void poll(double now) {
        if (view_ || now < next_try_) return;
        next_try_ = now + 1.0;
        wchar_t name[64];
        swprintf(name, 64, GLASIR_FRAMEGEN_LINK_NAME, GetCurrentProcessId());
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
        if (!mapping) return;
        auto *view = static_cast<GlasirFrameGenLink *>(
            MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(GlasirFrameGenLink)));
        if (!view || view->magic != GLASIR_FRAMEGEN_LINK_MAGIC || view->version != GLASIR_FRAMEGEN_LINK_VERSION) {
            if (view) UnmapViewOfFile(view);
            CloseHandle(mapping);
            return;
        }
        mapping_ = mapping;
        view_ = view;
        log_line(reshade::log::level::info, "linked to Glasir: new frames only, with their arrival times");
    }

    void close() {
        if (view_) UnmapViewOfFile(view_);
        if (mapping_) CloseHandle(mapping_);
        view_ = nullptr;
        mapping_ = nullptr;
    }

    bool linked() const { return view_ != nullptr; }

    // Asks Glasir for the repeats too (frame generation off) or for
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

    // New frames so far: changes when the source delivered one.
    LONG64 published() const { return view_ ? view_->published : 0; }

    // When new frame number n (1 = the first) arrived, in seconds; only the
    // last kKept are there (the writer may be filling the oldest slot).
    static constexpr int kKept = GlasirFrameGenLink::kStamps - 1;
    double arrived(LONG64 n) const {
        return view_ ? double(view_->stamps[(n - 1) % GlasirFrameGenLink::kStamps]) * qpc_period() : 0;
    }

    // Seconds since the source last delivered a new frame.
    double since_last(double now) const {
        if (!view_) return 1e9;
        const LONG64 p = view_->published;
        if (p == 0) return 1e9;
        return now - double(view_->stamps[(p - 1) % GlasirFrameGenLink::kStamps]) * qpc_period();
    }

private:
    static constexpr int kAll = GlasirFrameGenLink::kStamps;

    // Calls f with the gaps between the last `count` pairs of new frames,
    // newest first, in seconds, leaving out pauses longer than `gap`.
    template <class F>
    void each_gap(double gap, int count, F &&f) const {
        if (!view_) return;
        const LONG64 p = view_->published;
        constexpr int kStamps = GlasirFrameGenLink::kStamps;
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
    GlasirFrameGenLink *view_ = nullptr;
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

// Asks Glasir.exe (an IPC client of mpv) to save a bug report, with the
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
    mpv_send("{\"command\":[\"script-message\",\"glasir-bug-report\",\"" + quoted + "\",\"" + why + "\"]}\n");
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
                if (folder.empty()) glasir::overlay::toast("Frame generation: could not save the frames, see ReShade.log");
                mpv_bug_report(folder, folder.empty() ? "failed" : "");  // Glasir.exe says when it is saved
            } else {
                glasir::overlay::toast(folder.empty() ? "Frame generation: capture failed, see ReShade.log"
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

// The refresh rate of the screen the swap chain is on, 0 when unknown.
int display_hz(IDXGISwapChain *swapchain) {
    ComPtr<IDXGIOutput> output;
    if (FAILED(swapchain->GetContainingOutput(&output))) return 0;
    DXGI_OUTPUT_DESC desc{};
    if (FAILED(output->GetDesc(&desc))) return 0;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) return 0;
    return mode.dmDisplayFrequency > 1 ? static_cast<int>(mode.dmDisplayFrequency) : 0;
}

// The screen's refreshes so far and when the latest was, from the swap
// chain's frame statistics. False when it cannot say.
bool refresh_counter(IDXGISwapChain *swapchain, UINT &count, double &at) {
    DXGI_FRAME_STATISTICS stats{};
    if (FAILED(swapchain->GetFrameStatistics(&stats)) || stats.SyncQPCTime.QuadPart == 0) return false;
    count = stats.SyncRefreshCount;
    at = double(stats.SyncQPCTime.QuadPart) * qpc_period();
    return true;
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

// GLASIR_FRAMEGEN_TRACE=<file>: one CSV line per real frame, for the whole run.
FILE *g_trace = nullptr;

void trace_open() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    char path[MAX_PATH] = {};
    const DWORD len = GetEnvironmentVariableA("GLASIR_FRAMEGEN_TRACE", path, sizeof(path));
    if (len > 0 && len < sizeof(path) && fopen_s(&g_trace, path, "w") == 0)
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
// PresentMon (which needs administrator rights).
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
    double lead = 0;    // less this (GLASIR_FRAMEGEN_LEAD_MS, for experiments)
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
// Without the hold (no Direct3D 12, or GLASIR_FRAMEGEN_PACING=present for
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
//     so. GLASIR_FRAMEGEN_LEAD_MS=<ms> lets held frames go that much earlier,
//     for experiments on such screens (docs/FRAMEGEN-LEARNINGS.md).
//
// When it generates. The interval is the source's: with the link, the mean
// time between new frames from the card, pauses left out; without it, the
// median between mpv's frames (which, while generating, follow this add-on's
// own pace - so only the link can tell that the display is too slow for twice
// the source rate). A frame in between is only made when
//   - twice the source rate fits the display (and of x3 and x4 the most that
//     fits is used: MultiplierGate),
//   - fewer than two presents are still queued (else it would only add delay),
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
    glasir::FramegenStatus status() const;
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
        for (auto &t : frames_) t.Reset();
        for (auto &v : views_) v.Reset();
        interp_.release();
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
    bool draw_between(ID3D11Texture2D *back_buffer, int a, int b, float t);
    void note_source();
    void watch_refresh(IDXGISwapChain *swapchain, double now);
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
    void watch_drops(double now, bool stalled);
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
    int pending_slot_ = 0, pending_prev_slot_ = 0;
    int pending_count_ = 2;  // frames shown for this source frame, the real one included
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
    int hz_ = 0;             // the screen's refresh rate as Windows gives it (whole)
    double display_hz_ = 0;  // with its fraction when Glasir.exe told it (119.88), else hz_
    double next_hz_try_ = 0;
    double lead_ = 0;  // GLASIR_FRAMEGEN_LEAD_MS: held frames are let go this much earlier

    // What the menu shows (status()): over the last second, not the 5 s log's
    // window.
    fg::pacing::SourceCadence cadence_;  // the source's new frames, by the card's clock
    LONG64 cadence_seen_ = 0;            // the link's new-frame count already in it
    fg::pacing::RateWindow shown_real_, shown_generated_, queue_skips_, mix_skips_, late_, dropped_;
    fg::pacing::MultiplierGate gate_;
    glasir::FramegenStatus::Reason reason_ = glasir::FramegenStatus::Reason::Starting;  // the latest frame's

    // The screen's refreshes as the swap chain counts them, sampled once a
    // second: at its fixed rate whatever is presented means VRR is not
    // engaging.
    UINT refresh_count_ = 0;
    double refresh_at_ = 0, seen_hz_ = 0;
    fg::pacing::Seen seen_ = fg::pacing::Seen::Unknown;
    int seen_fixed_ = 0;  // samples in a row that read "fixed"
    bool seen_logged_ = false;
    Sleeper sleeper_;
    Capture capture_;
    ScreenTrace screen_trace_;  // GLASIR_FRAMEGEN_TRACE only
    double capture_after_ = -1;  // GLASIR_FRAMEGEN_CAPTURE_AFTER: when, 0 = none, -1 = not read yet
    double toggle_after_ = -1;   // GLASIR_FRAMEGEN_TOGGLE_AFTER: the same

    // Stats since the last line.
    double stats_start_ = 0;
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
    char pacing[16] = {};
    const DWORD len = GetEnvironmentVariableA("GLASIR_FRAMEGEN_PACING", pacing, sizeof(pacing));
    hold_wanted_ = !(len > 0 && len < sizeof(pacing) && _stricmp(pacing, "present") == 0);
    if (!hold_wanted_)
        log_line(reshade::log::level::info, "GLASIR_FRAMEGEN_PACING=present: the real frame is timed from mpv's present");
    char lead[16] = {};
    const DWORD lead_len = GetEnvironmentVariableA("GLASIR_FRAMEGEN_LEAD_MS", lead, sizeof(lead));
    if (lead_len > 0 && lead_len < sizeof(lead)) lead_ = std::clamp(atof(lead), 0.0, 20.0) / 1000.0;
    if (lead_ > 0)
        log_line(reshade::log::level::info, "GLASIR_FRAMEGEN_LEAD_MS: held frames are let go %.2f ms early",
                 lead_ * 1000.0);
    log_line(reshade::log::level::info, "the screen, as Glasir found it: VRR %s, %.3f Hz",
             g_settings.vrr == fg::pacing::Vrr::On ? "on" : g_settings.vrr == fg::pacing::Vrr::Off ? "off" : "unknown",
             g_settings.launcher_hz);
    log_line(reshade::log::level::info, "ready: feature level 0x%X, %s, toggle key %d%s", device->GetFeatureLevel(),
             g_settings.enabled ? "on" : "off", g_settings.toggle_key,
             g_settings.require_dedup ? ", waiting for the link from Glasir's dedup" : "");
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
    failed_w_ = failed_h_ = 0;
    width_ = bb.Width;
    height_ = bb.Height;
    format_ = bb.Format;
    log_line(reshade::log::level::info, "back buffer %ux%u, format %d", width_, height_, format_);
    return true;
}

// Draws the frame at t (0 = a, 1 = b) between frames_[a] and frames_[b] into
// the back buffer.
bool FrameGen::draw_between(ID3D11Texture2D *back_buffer, int a, int b, float t) {
    ComPtr<ID3D11RenderTargetView> rtv;
    HRESULT hr = device_->CreateRenderTargetView(back_buffer, nullptr, &rtv);
    if (FAILED(hr)) return fail("CreateRenderTargetView", hr), false;

    fg::DrawOptions o;
    o.method = g_settings.method;
    o.t = t;
    o.marker = g_settings.marker;
    o.marker_level = 0.75f;                                                // sRGB-encoded
    if (pq()) o.marker_level = 0.5f;                                       // PQ, about 90 nits
    if (format_ == DXGI_FORMAT_R16G16B16A16_FLOAT) o.marker_level = 1.0f;  // scRGB, 80 nits

    StateBackup backup(context_.Get());
    interp_.draw(context_.Get(), views_[a].Get(), views_[b].Get(), rtv.Get(), o);
    return true;
}

void FrameGen::toggle(reshade::api::effect_runtime *runtime) {
    // GLASIR_FRAMEGEN_TOGGLE_AFTER=<seconds>: one toggle that many seconds after
    // the first frame, as if the key had been pressed (for tests).
    const double now = qpc_seconds();
    if (toggle_after_ < 0) {
        char text[16] = {};
        const DWORD len = GetEnvironmentVariableA("GLASIR_FRAMEGEN_TOGGLE_AFTER", text, sizeof(text));
        toggle_after_ = (len > 0 && len < sizeof(text)) ? now + atof(text) : 0;
    }
    const bool timed = toggle_after_ > 0 && now >= toggle_after_;
    if (timed) toggle_after_ = 0;
    if (!timed &&
        (g_settings.toggle_key <= 0 || !runtime->is_key_pressed(static_cast<uint32_t>(g_settings.toggle_key))))
        return;
    if (broken_) {
        glasir::overlay::toast("Frame generation: failed, see ReShade.log");
        return;
    }
    if (!source_ok()) {
        // Not saved: this launcher cannot run it either way.
        glasir::overlay::toast("Frame generation: needs a launcher with dedup=1");
        return;
    }
    set_enabled(!g_settings.enabled);
    glasir::overlay::toast(g_settings.enabled ? "Frame generation: on" : "Frame generation: off");
}

void FrameGen::set_enabled(bool on) {
    if (on == g_settings.enabled) return;
    g_settings.enabled = on;
    reshade::set_config_value(nullptr, kSection, "Enabled", g_settings.enabled);
    if (capture_.active()) capture_.reset();
    reset_history();
    paused_until_ = settle_until_ = 0;
    backoff_ = 5.0;
    drop_times_[0] = drop_times_[1] = -1e9;  // a drop from before does not pair with one after
    gate_.reset();
    reason_ = glasir::FramegenStatus::Reason::Starting;
    log_line(reshade::log::level::info, "toggled %s", g_settings.enabled ? "on" : "off");
}

// Method=blend: a 50/50 mix (by how far between, with x3 and x4) instead of
// following the motion. Read at every draw, so it changes at once.
void FrameGen::set_blend(bool blend) {
    g_settings.method = blend ? fg::Method::blend : fg::Method::motion;
    reshade::set_config_value(nullptr, kSection, "Method", blend ? "blend" : "motion");
    log_line(reshade::log::level::info, "method %s", blend ? "blend" : "motion");
}

// Multiplier: frames shown per source frame, at most. Used from the next
// frame on.
void FrameGen::set_multiplier(int multiplier) {
    g_settings.multiplier = std::clamp(multiplier, 2, fg::pacing::kMaxMultiplier);
    reshade::set_config_value(nullptr, kSection, "Multiplier", g_settings.multiplier);
    gate_.reset();
    log_line(reshade::log::level::info, "multiplier up to x%d", g_settings.multiplier);
}

// The source's new frames since the last look, into cadence_ (all of them:
// several may have come since mpv's last present, or been dropped before it).
void FrameGen::note_source() {
    const LONG64 published = g_link.published();
    for (LONG64 n = std::max(cadence_seen_, published - Link::kKept) + 1; n <= published; ++n)
        cadence_.add(g_link.arrived(n));
    cadence_seen_ = published;
}

// Once a second: how many times a second the screen was seen refreshing.
void FrameGen::watch_refresh(IDXGISwapChain *swapchain, double now) {
    UINT count = 0;
    double at = 0;
    if (!refresh_counter(swapchain, count, at)) return;
    if (refresh_at_ > 0 && at > refresh_at_ + 0.5 && at < refresh_at_ + 3.0) {
        // The counter can stand still or jump back when the window is flipped
        // straight to a VRR screen: a count that makes no sense is "unknown".
        const double counted = double(static_cast<INT32>(count - refresh_count_)) / (at - refresh_at_);
        seen_hz_ = counted > 0 && counted < 1000 ? counted : 0;
        const double shown = shown_real_.rate(now) + shown_generated_.rate(now);
        const fg::pacing::Seen seen = fg::pacing::judge_refresh(seen_hz_, display_hz_, shown);
        // "Fixed" only after three seconds of it: one sample can straddle a
        // change of rate.
        seen_fixed_ = seen == fg::pacing::Seen::Fixed ? seen_fixed_ + 1 : 0;
        seen_ = seen == fg::pacing::Seen::Fixed && seen_fixed_ < 3 ? fg::pacing::Seen::Unknown : seen;
        if (seen_ == fg::pacing::Seen::Fixed && g_settings.vrr == fg::pacing::Vrr::On && !seen_logged_) {
            seen_logged_ = true;
            log_line(reshade::log::level::warning,
                     "VRR is on for this screen, but it keeps refreshing at %.1f Hz while %.1f frames a second are "
                     "shown: VRR is not engaging (not on for windows in the driver, the window not focused, or "
                     "composed by Windows)",
                     seen_hz_, shown);
        }
        if (seen_ == fg::pacing::Seen::Following) seen_logged_ = false;
    }
    refresh_count_ = count;
    refresh_at_ = at;
}

glasir::FramegenStatus FrameGen::status() const {
    using Status = glasir::FramegenStatus;
    Status s;
    const double now = qpc_seconds();
    s.enabled = g_settings.enabled && !broken_ && source_ok();
    const fg::pacing::SourceCadence::Summary c = cadence_.summary(now);
    s.source_fps = c.fps;
    s.source_low = c.low;
    s.source_high = c.high;
    s.source_variable = c.variable;
    s.source_uneven = c.uneven;
    s.real_fps = shown_real_.rate(now);
    s.generated_fps = shown_generated_.rate(now);
    s.display_hz = display_hz_;
    s.cap_hz = output_limit().exact ? g_settings.max_output_hz : 0;
    s.vrr = g_settings.vrr;
    s.seen = seen_;
    s.seen_hz = seen_hz_;
    s.wanted = g_settings.multiplier;
    if (!s.enabled) return s;

    // What the latest frame decided, when that is a state that lasts; else
    // what got in the way now and then over the last second - if the frames
    // shown really fall short of what was meant.
    s.reason = reason_;
    const bool none = reason_ != Status::Reason::None && reason_ != Status::Reason::FewerFit;
    s.multiplier = none ? 1 : gate_.current();
    if (reason_ == Status::Reason::BackingOff) s.seconds_left = std::max(0.0, paused_until_ - now);
    if (!none && s.source_fps > 0 && s.real_fps + s.generated_fps < 0.95 * s.multiplier * s.source_fps) {
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
    log_line(reshade::log::level::info, "quality %s", fast ? "fast" : "high");
}

// New frames dropped upstream: mpv is not keeping up with the source.
//
// Drops seen now happened since the previous frame. When that frame had a
// frame in between and mpv came back at its usual pace, the extra present is
// part of why: two such within two seconds pause generation - 5 s the first
// time, twice as long each time it happens again, at most a minute; 30 s after
// the last pause and the last such drop forgive it.
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

    // Whatever the effects did, it was for this frame only.
    const bool neural = effects_ran_;
    effects_ran_ = false;
    // A real frame left over from a present whose finish_present never came
    // (it failed, or was a "still drawing" try) is dropped.
    if (pending_) trace(pending_now_, pending_dt_, pending_queued_, "generated-real-lost");
    pending_ = false;
    capture_.tick(context_.Get());  // a finished capture is read back whatever happens below

    const double now = qpc_seconds();
    if (!g_link.linked()) {
        g_link.poll(now);
        // mpv has only just started: let it settle before the first frame in
        // between, so its start-up drops are not blamed on generating.
        if (g_link.linked()) settle_until_ = now + 2.0;
    }
    toggle(runtime);
    // For the menu's numbers, whether or not frames are being added.
    note_source();
    shown_real_.add(now);
    if (now >= next_hz_try_) {
        // Every second: the window may move to another screen or the mode
        // change. A failed read keeps the last rate. Glasir.exe's figure has
        // the fraction (119.88); it is used while it agrees with Windows'.
        if (const int hz = display_hz(swapchain)) hz_ = hz;
        display_hz_ = hz_ > 0 && std::fabs(g_settings.launcher_hz - hz_) < 1.0 ? g_settings.launcher_hz : hz_;
        watch_refresh(swapchain, now);
        next_hz_try_ = now + 1.0;
    }
    // While frame generation is off, mpv gets every frame as without --dedup:
    // nothing gains from new frames only then (DedupWhenOff=1 keeps them).
    // So too while the in-game menu is open on a still picture (a quarter of
    // a second without a new frame): only keepalive frames would come, and the
    // menu is drawn only when a frame is presented. On a moving picture it is
    // drawn on every frame shown, generated ones included.
    const bool still_menu =
        glasir::overlay::menu_open() && g_link.linked() && g_link.since_last(now) > 0.25;
    if (g_link.linked())
        g_link.send_repeats(still_menu || (!g_settings.dedup_when_off && (broken_ || !g_settings.enabled)));
    // Also with the in-game menu open (it is drawn after the frames saved).
    const bool capture_key =
        g_settings.capture_key > 0 && runtime->is_key_pressed(static_cast<uint32_t>(g_settings.capture_key));
    if (broken_ || !g_settings.enabled || !source_ok()) {
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
        if (capture_.active()) capture_.reset();  // its frames would not all be one encoding
    }
    if (bb.SampleDesc.Count != 1 || !ensure_frames(bb)) {
        if (capture_.active()) capture_.reset();
        return;
    }

    const int cur = prev_ == 0 ? 1 : 0;
    context_->CopyResource(frames_[cur].Get(), back_buffer.Get());
    neural_[cur] = neural;
    ++real_;
    if (prev_ >= 0 && interval_ > 0 && dt > 1.5 * interval_) {
        ++late_frames_;  // still generated
        late_.add(now);
    }

    using Reason = glasir::FramegenStatus::Reason;
    int queued = -1;
    bool generate = prev_ >= 0 && interval_ > 0;
    Reason why = generate ? Reason::None : Reason::Starting;
    // How many frames to show for this source frame: the most that fits the
    // screen, up to the setting.
    const int count = interval_ > 0 ? gate_.choose(now, 1.0 / interval_, g_settings.multiplier, output_limit(),
                                                   g_settings.vrr, display_hz_)
                                    : 1;
    if (generate && count < 2) {
        generate = false;  // the display cannot show even twice this rate
        ++skipped_fast_;
        why = Reason::TooFast;
    }
    if (generate && g_link.linked() && g_link.since_last(now) > std::max(reset, 3.0 * interval_)) {
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
    reason_ = why;
    // These two come and go frame by frame: counted, for the menu to name
    // when the frames shown fall short.
    if (generate && neural_[prev_] != neural) {
        generate = false;  // the effects ran for one frame and not the other
        ++skipped_mixed_;
        mix_skips_.add(now);
    }
    if (generate && (queued = queued_presents(swapchain)) >= 2) {
        generate = false;  // the screen is behind: another present would only wait
        ++skipped_queue_;
        queue_skips_.add(now);
    }

    if (generate && draw_between(back_buffer.Get(), prev_, cur, 1.0f / count)) {
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
    // GLASIR_FRAMEGEN_CAPTURE_AFTER=<seconds>: a capture that many seconds after
    // frame generation starts, as if the key had been pressed (for tests).
    if (capture_after_ < 0) {
        char text[16] = {};
        const DWORD len = GetEnvironmentVariableA("GLASIR_FRAMEGEN_CAPTURE_AFTER", text, sizeof(text));
        capture_after_ = (len > 0 && len < sizeof(text)) ? now + atof(text) : 0;
    }
    const bool timed = capture_after_ > 0 && now >= capture_after_;
    if (timed) capture_after_ = 0;
    if (!capture_.busy() && (timed || capture_key)) {
        if (!capture_.start(device_.Get(), bb, pq(), !timed)) {
            glasir::overlay::toast("Frame generation: cannot capture this picture format");
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
        if (real || !draw_between(back_buffer.Get(), pending_prev_slot_, pending_slot_, float(frame) / count))
            context_->CopyResource(back_buffer.Get(), frames_[pending_slot_].Get());
        else
            capture_.between(context_.Get(), back_buffer.Get(), frame, count - 1);
        glasir::overlay::draw_again(context_.Get(), back_buffer.Get());

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
        log_line(reshade::log::level::info,
                 "%.1f real + %.1f generated fps (x%d of up to x%d) | interval %.2f ms (%s%s) | display %.2f Hz "
                 "(seen refreshing at %.1f), VRR %s, cap %.0f Hz | %u late frames, %u dropped upstream after a "
                 "generated frame | not generated: %u too fast, %u queue full, %u source paused, %u backing off, "
                 "%u mpv settling, %u effects mix | mpv present %.2f ms avg, %.2f max | %s",
                 real_ / span, generated_ / span, gate_.current(), g_settings.multiplier, interval_ * 1000.0,
                 g_link.linked() ? "source" : "mpv", source, display_hz_, seen_hz_,
                 g_settings.vrr == fg::pacing::Vrr::On ? "on" : g_settings.vrr == fg::pacing::Vrr::Off ? "off" : "unknown",
                 limit.hz, late_frames_, drops_, skipped_fast_, skipped_queue_, skipped_paused_, skipped_backoff_,
                 skipped_settle_, skipped_mixed_, mpv_present_sum_ / n * 1000.0, mpv_present_max_ * 1000.0, out);
    }
    if (g_trace) fflush(g_trace);
    stats_start_ = now;
    real_ = generated_ = skipped_fast_ = skipped_mixed_ = skipped_queue_ = skipped_paused_ = skipped_backoff_ = 0;
    skipped_settle_ = 0;
    late_frames_ = drops_ = 0;
    late_max_ = late_sum_ = mpv_present_max_ = mpv_present_sum_ = 0;
    out_max_ = 0;
}

// ---- ReShade events -----------------------------------------------------------

void on_reshade_present(reshade::api::effect_runtime *runtime) {
    // A screenshot first: the back buffer still holds mpv's real frame.
    glasir::overlay::before_frame_generation(runtime);
    g_gen.on_reshade_present(runtime);
    // The menu last: over the frame in between (or the frame) mpv presents now.
    glasir::overlay::on_reshade_present(runtime);
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
    glasir::overlay::note_color_space(swapchain->get_color_space());
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
    glasir::overlay::release();
    g_gen.release();
}

void set_menu_host() {
    glasir::overlay::Host host;
    host.framegen_ready = [] { return g_gen.ready(); };
    host.framegen_enabled = [] { return g_settings.enabled; };
    host.set_framegen_enabled = [](bool on) { g_gen.set_enabled(on); };
    host.framegen_fast = [] { return g_settings.fast; };
    host.set_framegen_fast = [](bool fast) { g_gen.set_fast(fast); };
    host.framegen_blend = [] { return g_settings.method == fg::Method::blend; };
    host.set_framegen_blend = [](bool blend) { g_gen.set_blend(blend); };
    host.framegen_multiplier = [] { return g_settings.multiplier; };
    host.set_framegen_multiplier = [](int multiplier) { g_gen.set_multiplier(multiplier); };
    host.framegen_status = [] { return g_gen.status(); };
    host.mpv_pipe = g_settings.mpv_pipe;
    glasir::overlay::set_host(host);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(module)) return FALSE;
        reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
        reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
        reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
        register_present_events();
        break;
    case DLL_PROCESS_DETACH:
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
