// SPDX-License-Identifier: MIT
// The menu's line to mpv (its IPC pipe), on a thread of its own: mpv's render
// thread, where the menu is drawn, never waits on the pipe. It follows the
// properties the menu shows and sends the commands the menu gives.
#pragma once
#include <windows.h>

#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace app::overlay {

struct MpvState {
    bool connected = false;
    double volume = -1;  // -1: not known yet
    bool mute = false;
    bool fullscreen = false;
    int boost = -1;   // the audio plugin's (user-data/app/audio-boost), -1: no plugin
    int delay = -1;   // ms, likewise
    int width = 0, height = 0;  // the video's size
    std::string scaling;        // scaling.lua's mode (user-data/app/scaling): mpv, fsr, vsr, cnn, anime4k
    // HDR, as mpv has it now.
    std::string hint_mode;     // target-colorspace-hint-mode: "source" (as the console sends it), "target" (fitted)
    int target_peak = 0;       // target-peak in nits; 0: auto
    std::string tone_mapping;  // tone-mapping: the curve's name
    bool compute_peak = false; // hdr-compute-peak = yes
    // What mpv puts out (video-target-params): "pq" for HDR10, an SDR curve
    // ("srgb", "bt.1886") otherwise, and the peak it puts out up to. "": no
    // picture yet.
    std::string out_gamma;
    double out_peak = 0;
    // What the console sends now, as the app found or was told
    // (user-data/app/signal): "hdr" or "sdr". "": not said.
    std::string signal;
    // osd-dimensions: the window's size and the black bars around the video
    // (margins left, top, right, bottom); w = 0 until known.
    struct Osd {
        int w = 0, h = 0, ml = 0, mt = 0, mr = 0, mb = 0;
    } osd;
};

// A short message to show over the picture (a toast): from a key mpv
// handled (volume, mute) or one sent with "script-message app-toast
// <text> [ms]". serial grows with each; 0: none yet.
struct Toast {
    std::string text;
    double seconds = 1.5;
    bool from_property = false;  // a value changed (also by the menu itself)
    unsigned serial = 0;
};

class MpvClient {
public:
    MpvClient() = default;
    MpvClient(const MpvClient &) = delete;
    MpvClient &operator=(const MpvClient &) = delete;
    ~MpvClient() { stop(); }

    void start(const std::wstring &pipe);
    void stop();
    MpvState state() const;
    // Queues {"command":[words...]}; sent in order.
    void command(const std::vector<std::string> &words);
    Toast toast() const;
    // How many times "script-message app-screenshot" (the s key) came.
    unsigned screenshot_requests() const;

private:
    void run();
    void on_line(const std::string &line);

    std::wstring pipe_;
    std::thread thread_;
    HANDLE wake_ = nullptr;  // a command is queued, or stop
    bool stopping_ = false;  // under m_
    mutable std::mutex m_;
    MpvState state_;
    Toast toast_;
    unsigned screenshots_ = 0;
    bool seen_volume_ = false, seen_mute_ = false;  // since connecting: the first values are no change
    std::vector<std::string> queue_;  // JSON lines
};

// One JSON string, quoted and escaped.
std::string json_quote(const std::string &s);

}  // namespace app::overlay
