// SPDX-License-Identifier: MIT
// The app's half of the stream window (src/common/stream_link.h): an
// ordinary SDR window on a screen other than the one the picture is on,
// showing the copy of the picture the add-on draws. On its own thread.
#pragma once
#include <windows.h>

#include <atomic>
#include <functional>
#include <thread>

namespace app {

class StreamWindow {
public:
    StreamWindow() = default;
    StreamWindow(const StreamWindow &) = delete;
    StreamWindow &operator=(const StreamWindow &) = delete;
    ~StreamWindow() { stop(); }

    // Before mpv starts: makes the switch the add-on looks for, on or off.
    // False if it cannot be made (no stream window then).
    bool offer(bool on);
    // Once mpv runs (process mpv_pid, to tell which screen the picture is
    // on). closed() is called, on the window's thread, when the window's
    // close box switched the stream window off.
    void start(DWORD mpv_pid, std::function<void()> closed);
    void stop();

private:
    void run();

    std::thread thread_;
    std::atomic<bool> stopping_{false};
    HANDLE on_ = nullptr, frame_ = nullptr;
    DWORD pid_ = 0;
    std::function<void()> closed_;
};

}  // namespace app
