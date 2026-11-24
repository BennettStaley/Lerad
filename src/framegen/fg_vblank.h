// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// The screen a window is on, as Windows has it now, and a count of that
// screen's own refreshes. The Windows side of fg_screen.h.
//
// Which screen: MonitorFromWindow, asked again every second - not the swap
// chain's GetContainingOutput, whose outputs are those of the DXGI factory
// mpv made at start and go stale when a screen is plugged in, switched off
// or moved (IDXGIFactory1::IsCurrent), and which knows nothing of a screen on
// another GPU.
//
// The count: a thread waits for each vertical blank of that screen's own
// video source (D3DKMTWaitForVerticalBlankEvent, opened by Windows' name for
// the screen, "\\.\DISPLAY2"). It wakes with IDXGIOutput's WaitForVBlank
// (the same gaps to the microsecond when both were run), which Chromium
// (ui/gl/vsync_thread_win) and Qt run on a thread to follow a screen's
// refreshes; opened by name it needs no DXGI output that could go stale, and
// it is Windows' own call, the same for every GPU maker. The wait sleeps
// (0.14 s of processor time in 12 s of counting).
//
// The swap chain's own counter (GetFrameStatistics' SyncRefreshCount), used
// before, follows Windows' compositor whenever the window is composed - the
// main screen's rate on a PC with two (docs/FRAMEGEN-LEARNINGS.md 11.3).
//
// Measured on one 120 Hz screen (2026-11-18, with a small probe kept outside the repo): 120.00
// refreshes a second, none missed in 600, at normal thread priority too; and
// 119.86-119.90 while the app showed 59.94 x2 on it with VRR on (the
// refreshes following the frames). Not seen yet: two screens at once, the
// window moved between them, a screen switched off while counted, and a VRR
// screen well below its top rate.

#pragma once

#include <windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fg {

// QueryPerformanceCounter in seconds: the add-on's clock.
inline double vblank_clock() {
    static const double period = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / double(f.QuadPart);
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart) * period;
}

// The screen a window is on (the one most of it is on; the nearest when it
// is on none), with the mode Windows has set there.
struct WindowScreen {
    bool found = false;
    HMONITOR monitor = nullptr;  // changes when the set of screens does
    std::wstring device;         // "\\.\DISPLAY2"
    RECT rect{};                 // its place on the desktop
    int width = 0, height = 0;   // the mode, in pixels
    int hz_whole = 0;            // its rate as Windows rounds it, 0 unknown

    // The same screen in the same mode at the same place.
    bool same(const WindowScreen &o) const {
        return found == o.found && monitor == o.monitor && device == o.device && EqualRect(&rect, &o.rect) &&
               width == o.width && height == o.height && hz_whole == o.hz_whole;
    }
};

inline WindowScreen window_screen(HWND window) {
    WindowScreen s;
    if (!window || !IsWindow(window)) return s;
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return s;
    s.found = true;
    s.monitor = monitor;
    s.device = info.szDevice;
    s.rect = info.rcMonitor;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
        s.width = static_cast<int>(mode.dmPelsWidth);
        s.height = static_cast<int>(mode.dmPelsHeight);
        s.hz_whole = mode.dmDisplayFrequency > 1 ? static_cast<int>(mode.dmDisplayFrequency) : 0;
    }
    return s;
}

// What the display configuration says about a screen: its refresh rate with
// the fraction (119.88; 0 when it does not say) and the device path of the
// monitor plugged into it ("" unknown), which tells one monitor from another
// when Windows gives an output's name to a new one. Under a tenth of a
// millisecond when measured, but it can take longer while screens change:
// ask when the screen or its mode changed, not every second.
struct ScreenConfig {
    double hz = 0;
    std::wstring monitor;
};

inline ScreenConfig screen_config(const std::wstring &device) {
    ScreenConfig config;
    UINT32 paths_n = 0, modes_n = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths_n, &modes_n) != ERROR_SUCCESS) return config;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(paths_n);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modes_n);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths_n, paths.data(), &modes_n, modes.data(), nullptr) !=
        ERROR_SUCCESS)
        return config;
    for (UINT32 i = 0; i < paths_n && i < paths.size(); ++i) {
        const DISPLAYCONFIG_PATH_INFO &path = paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
        if (_wcsicmp(source.viewGdiDeviceName, device.c_str()) != 0) continue;
        const DISPLAYCONFIG_RATIONAL &rate = path.targetInfo.refreshRate;
        if (rate.Denominator) config.hz = double(rate.Numerator) / rate.Denominator;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) config.monitor = target.monitorDevicePath;
        break;
    }
    return config;
}

// Counts one screen's refreshes on a thread of its own, from start() until
// stop() or the next start(). Each start is a thread and a count of its own:
// one that hangs in a wait on a screen that was switched off is left behind
// (it ends by itself at that screen's next refresh, or when the wait fails)
// and never holds up the next. The thread keeps this module loaded while it
// runs, and touches nothing but its own block of memory.
class VblankCounter {
public:
    VblankCounter() = default;
    VblankCounter(const VblankCounter &) = delete;
    VblankCounter &operator=(const VblankCounter &) = delete;
    ~VblankCounter() { stop(); }

    // device: "\\.\DISPLAY2". False when the thread could not be made.
    bool start(const std::wstring &device) {
        stop();
        auto shared = std::make_shared<Shared>();
        shared->device = device;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&VblankCounter::run),
                                &shared->module))
            return false;
        shared->wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        auto *handed = new std::shared_ptr<Shared>(shared);
        const HANDLE thread = CreateThread(nullptr, 0, run, handed, 0, nullptr);
        if (!thread) {
            delete handed;
            FreeLibrary(shared->module);
            return false;
        }
        CloseHandle(thread);  // it ends by itself
        shared_ = std::move(shared);
        return true;
    }

    // Takes no lock: at the end of the process the thread may have been ended
    // by Windows while it held one.
    void stop() {
        if (!shared_) return;
        shared_->stopping = true;
        if (shared_->wake) SetEvent(shared_->wake);
        shared_.reset();
        base_epoch_ = last_epoch_ + 1;  // the next start's epochs are new ones
    }

    struct Reading {
        // Goes up when the count starts over: a new start(), or a hole in
        // the counting (the wait failed, or the screen stopped refreshing).
        // Counts of different epochs must not be subtracted.
        unsigned epoch = 0;
        bool counting = false;  // the screen's refreshes are being waited for
        unsigned count = 0;     // refreshes counted in this epoch
        double at = 0;          // when the latest was (vblank_clock), 0 none yet
        long failure = 0;       // not counting: the NTSTATUS of the call that failed, 0 none
    };

    Reading read() {
        Reading r;
        r.epoch = base_epoch_;
        if (!shared_) return r;
        {
            std::lock_guard<std::mutex> lock(shared_->lock);
            r = shared_->reading;
        }
        r.epoch += base_epoch_;
        last_epoch_ = r.epoch;
        return r;
    }

private:
    struct Shared {
        std::mutex lock;
        Reading reading;
        std::wstring device;
        std::atomic<bool> stopping{false};
        HANDLE wake = nullptr;  // set by stop(): ends the thread's pauses early
        HMODULE module = nullptr;
        ~Shared() {
            if (wake) CloseHandle(wake);
        }
    };

    // d3dkmthk.h's, declared here so the build needs no driver kit header.
    struct OpenAdapter {
        WCHAR DeviceName[32];
        UINT hAdapter;
        LUID AdapterLuid;
        UINT VidPnSourceId;
    };
    struct WaitForVblank {
        UINT hAdapter;
        UINT hDevice;
        UINT VidPnSourceId;
    };
    struct CloseAdapter {
        UINT hAdapter;
    };
    using OpenFn = LONG(APIENTRY *)(OpenAdapter *);
    using WaitFn = LONG(APIENTRY *)(const WaitForVblank *);
    using CloseFn = LONG(APIENTRY *)(const CloseAdapter *);

    static void pause(const Shared &shared, DWORD ms) {
        if (shared.wake)
            WaitForSingleObject(shared.wake, ms);
        else
            Sleep(ms);
    }

    // The count starts over, with nothing counted.
    static void restart(Shared &shared, bool counting, long failure) {
        std::lock_guard<std::mutex> lock(shared.lock);
        ++shared.reading.epoch;
        shared.reading.counting = counting;
        shared.reading.count = 0;
        shared.reading.at = 0;
        shared.reading.failure = failure;
    }

    static DWORD WINAPI run(void *handed) {
        std::shared_ptr<Shared> shared = std::move(*static_cast<std::shared_ptr<Shared> *>(handed));
        delete static_cast<std::shared_ptr<Shared> *>(handed);
        const HMODULE module = shared->module;
        // It only ever notes a time: above mpv's own threads, so a busy
        // moment does not cost a refresh.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

        const HMODULE gdi = LoadLibraryW(L"gdi32.dll");
        const auto open = gdi ? reinterpret_cast<OpenFn>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromGdiDisplayName")) : nullptr;
        const auto wait = gdi ? reinterpret_cast<WaitFn>(GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent")) : nullptr;
        const auto close = gdi ? reinterpret_cast<CloseFn>(GetProcAddress(gdi, "D3DKMTCloseAdapter")) : nullptr;

        UINT adapter = 0, source = 0;
        double last = 0;
        int early = 0;
        while (open && wait && close && !shared->stopping) {
            if (!adapter) {
                OpenAdapter o{};
                wcsncpy_s(o.DeviceName, shared->device.c_str(), _TRUNCATE);
                const LONG status = open(&o);
                if (status < 0 || !o.hAdapter) {
                    // No such screen now (it was switched off): try again in
                    // a while; the add-on moves on when the window does.
                    restart(*shared, false, status < 0 ? status : -1);
                    pause(*shared, 500);
                    continue;
                }
                adapter = o.hAdapter;
                source = o.VidPnSourceId;
                last = 0;
                early = 0;
                restart(*shared, true, 0);
            }
            const WaitForVblank w{adapter, 0, source};
            const LONG status = wait(&w);
            const double now = vblank_clock();
            if (status < 0) {
                const CloseAdapter c{adapter};
                close(&c);
                adapter = 0;
                restart(*shared, false, status);
                pause(*shared, 250);
                continue;
            }
            // A return within a fraction of a millisecond of the last is no
            // refresh: the DXGI call over this one is known to come back at
            // once while the screen sleeps or the desktop is locked
            // (Chromium's vsync thread guards against the same). A run of
            // them: nothing is being counted.
            if (last > 0 && now - last < 0.0008) {
                if (++early >= 8) {
                    early = 0;
                    last = 0;
                    restart(*shared, false, 0);
                    pause(*shared, 250);
                    restart(*shared, true, 0);
                }
                continue;
            }
            early = 0;
            last = now;
            std::lock_guard<std::mutex> lock(shared->lock);
            ++shared->reading.count;
            shared->reading.at = now;
        }
        if (adapter && close) {
            const CloseAdapter c{adapter};
            close(&c);
        }
        if (!(open && wait && close)) restart(*shared, false, -1);
        if (gdi) FreeLibrary(gdi);
        shared.reset();
        FreeLibraryAndExitThread(module, 0);
    }

    std::shared_ptr<Shared> shared_;
    // Epochs never repeat across starts: each start's begin above the last
    // one read.
    unsigned base_epoch_ = 0, last_epoch_ = 0;
};

}  // namespace fg
