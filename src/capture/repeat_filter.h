// SPDX-License-Identifier: MIT
// Dedup: tells new frames from repeats.
//
// A 30 fps game captured at 60 arrives as A A B B; with dedup mpv only gets
// A B, so DLSS 5 runs on real frames only and frame generation sees an even
// 30 fps instead of pairs of identical frames.
//
// A sparse grid of luma samples (every 4th row, every 2nd column) is compared
// with the same grid of the last frame sent, not the last frame captured, so a
// slow fade still goes out once it adds up. Samples are 10-bit code values
// for every format (pixel_format.h), so the threshold means the same.
//
// Some sources are not bit-exact on repeats: the PS5 adds faint noise to
// every refresh (a quarter of the samples move by 1-3 8-bit steps, never in
// the same direction across an area), so one sample moving a little says
// nothing. Two things count as a change instead: a 16x16 pixel block whose
// average moved by more than the threshold (the noise cancels out in it,
// while a fade, a pan or a moving shape moves it) and one sample that moved by
// more than kSpotFactor times the threshold (a small sharp change, like a HUD
// digit, that a block average would hide). A frame is new from kMinChanged
// changes.
//
// A repeat is still sent after keepalive_ms, so a still picture keeps mpv's
// OSD and menus drawing.
#pragma once
#include <windows.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

#include "log.h"
#include "pixel_format.h"

namespace app {

// Whether a capture at this rate carries a game frame in every frame: a
// console game runs at 30 fps or more, so a capture of 30 or less has no
// repeats to remove (a game that stutters aside). Pictures are then not
// compared at all: on a still picture (a menu) only the source's noise
// differs between frames, and a threshold near that noise calls each frame new
// or a repeat by chance - an Elgato 4K X at 3840x2160 HDR measured 2.2 to 3.0
// a block against the threshold of 3, and frame generation saw a game at
// "14-30 fps". Frame generators inside a game pace by its presents the same
// way, still picture or not.
inline bool capture_rate_is_game_rate(double capture_fps) { return capture_fps > 0 && capture_fps < 31.0; }

// The game's rhythm in a capture faster than 30 fps: every captured frame a
// game frame (60 in 60), or every other one (30 in 60: A A B B).
//
// Comparing pictures cannot tell a new frame from a repeat when the picture
// hardly moves: a real frame of a slow fade or an idle scene changes a handful
// of blocks, about what the source's noise does to a repeat, so each frame was
// called new or a repeat by chance (a PS5 Pro at 3840x2160 HDR 59.94 through a
// Magewell Pro Capture HDMI 4K Plus, 11-17: "11-48 new frames/s of 60.4
// captured", a source of "8-60 fps, variable", the frames shown swinging
// between 60 and the low teens). The rhythm is read from the frames that leave
// no doubt instead - each compared with the frame captured before it: many
// changes (Strong), or next to none (Quiet) - and then kept through the frames
// that do.
//
//  - Every: 14 of the last 16 frames were Strong. Every frame is new.
//  - Alternate: of the last 16, the frames of one parity were Strong (6 of 8
//    at least) and the others all Quiet. That parity's frames are new, moving
//    or not; a Strong frame out of turn (the game ran a frame late) is new and
//    the turn is its from then on.
//  - Free: neither, with new frames back to back and single repeats between
//    new frames (an uneven frame rate). Pictures are compared one by one, as
//    without the hold.
// A still picture is no evidence and changes nothing.
class CadenceHold {
public:
    enum class Mode { Free, Every, Alternate };

    // moved: this frame's changes against the frame captured before it.
    void add(size_t moved) {
        const bool strong = moved >= kStrong, quiet = moved < kQuietBelow;
        strong_ = strong_ << 1 | (strong ? 1u : 0u);
        quiet_ = quiet_ << 1 | (quiet ? 1u : 0u);
        if (seen_ < kWindow) ++seen_;
        if (mode_ == Mode::Alternate) {
            on_turn_ = !on_turn_;
            if (!on_turn_ && strong) on_turn_ = true;
        }
        changed_ = false;
        if (seen_ < kWindow) return;
        constexpr unsigned kMine = 0x5555, kOthers = 0xAAAA;  // bit 0 is this frame
        const unsigned s = strong_ & 0xFFFF, q = quiet_ & 0xFFFF;
        const auto alternates = [&](unsigned turn, unsigned rest) {
            return std::popcount(s & turn) >= 6 && (s & rest) == 0 && (q & rest) == rest;
        };
        Mode next = mode_;
        if (std::popcount(s) >= 14) {
            next = Mode::Every;
        } else if (alternates(kMine, kOthers)) {
            next = Mode::Alternate;
            on_turn_ = true;
        } else if (alternates(kOthers, kMine)) {
            next = Mode::Alternate;
            on_turn_ = false;
        } else if (std::popcount(s & (s >> 1)) >= 3 && std::popcount(q & (s << 1) & (s >> 1)) >= 2) {
            // New frames back to back and single repeats between new frames:
            // moving, and neither rhythm. (Motion that stops is not this.)
            next = Mode::Free;
        }
        changed_ = next != mode_;
        mode_ = next;
    }

    Mode mode() const { return mode_; }
    bool on_turn() const { return on_turn_; }    // Alternate: this frame is one of the game's
    bool changed() const { return changed_; }    // the mode changed with the last add()

private:
    static constexpr unsigned kWindow = 16;
    static constexpr size_t kStrong = 16;     // changes: far above what noise does (0 measured on repeats)
    static constexpr size_t kQuietBelow = 4;  // RepeatFilter::kMinChanged

    Mode mode_ = Mode::Free;
    unsigned strong_ = 0, quiet_ = 0, seen_ = 0;
    bool on_turn_ = false, changed_ = false;
};

class RepeatFilter {
public:
    // hold_cadence: keep the game's rhythm through pictures that hardly move
    // (CadenceHold above).
    // trusted_fps: the capture mode's rate when it is one a game fills
    // (capture_rate_is_game_rate). Every frame is then new without a look at
    // the picture, for as long as the card really sends that rate: a Magewell
    // Pro Capture listed only a 30 fps mode and sent the signal's 59.94 in it
    // (11-23), every game frame twice.
    RepeatFilter(PixelFormat format, UINT32 width, UINT32 height, int threshold, DWORD keepalive_ms,
                 bool hold_cadence = false, double trusted_fps = 0)
        : format_(format), width_(width), threshold_(threshold), keepalive_ms_(keepalive_ms), hold_(hold_cadence),
          trusted_fps_(trusted_fps), trusted_(trusted_fps > 0),
          grid_w_((width + kColStep - 1) / kColStep), grid_h_((height + kRowStep - 1) / kRowStep),
          blocks_w_((grid_w_ + kBlockCols - 1) / kBlockCols), blocks_h_((grid_h_ + kBlockRows - 1) / kBlockRows),
          sent_(size_t(grid_w_) * grid_h_), scratch_(sent_.size()), block_sum_(size_t(blocks_w_) * blocks_h_),
          block_samples_(block_sum_.size()) {
        for (UINT32 gy = 0; gy < grid_h_; ++gy)
            for (UINT32 gx = 0; gx < grid_w_; ++gx)
                ++block_samples_[size_t(gy / kBlockRows) * blocks_w_ + gx / kBlockCols];
        if (hold_) {
            before_.resize(sent_.size());
            moved_sum_.resize(block_sum_.size());
        }
    }

    // True when the frame (tightly packed) should be sent.
    bool take(const BYTE *frame) {
        if (trusted_fps_ > 0) {
            const auto at = std::chrono::steady_clock::now();
            if (rate_frames_ == 0) rate_start_ = at;
            const double span = std::chrono::duration<double>(at - rate_start_).count();
            if (span >= 1.0) {
                measured(rate_frames_ / span);
                rate_start_ = at;
                rate_frames_ = 0;
            }
            ++rate_frames_;
        }
        std::fill(block_sum_.begin(), block_sum_.end(), 0);
        std::fill(moved_sum_.begin(), moved_sum_.end(), 0);
        const int spot = threshold_ * kSpotFactor;
        size_t changed = 0, moved = 0, i = 0;
        int sample_max = 0;
        for (UINT32 gy = 0; gy < grid_h_; ++gy) {
            const size_t row = size_t(gy) * kRowStep * width_;
            int *sums = block_sum_.data() + size_t(gy / kBlockRows) * blocks_w_;
            int *moved_sums = hold_ ? moved_sum_.data() + size_t(gy / kBlockRows) * blocks_w_ : nullptr;
            for (UINT32 gx = 0; gx < grid_w_; ++gx, ++i) {
                const int v = luma10(format_, frame, row + size_t(gx) * kColStep);
                scratch_[i] = static_cast<UINT16>(v);
                const int d = v - int(sent_[i]);
                sums[gx / kBlockCols] += d;
                const int a = std::abs(d);
                sample_max = std::max(sample_max, a);
                if (a > spot) ++changed;
                if (hold_) {
                    const int m = v - int(before_[i]);
                    moved_sums[gx / kBlockCols] += m;
                    if (std::abs(m) > spot) ++moved;
                }
            }
        }
        double block_max = 0.0;
        for (size_t b = 0; b < block_sum_.size(); ++b) {
            const double mean = std::abs(block_sum_[b]) / double(block_samples_[b]);
            block_max = std::max(block_max, mean);
            if (mean > threshold_) ++changed;
            if (hold_ && std::abs(moved_sum_[b]) / double(block_samples_[b]) > threshold_) ++moved;
        }
        const ULONGLONG now = GetTickCount64();
        bool fresh = !primed_ || changed >= kMinChanged;
        if (hold_ && primed_) {
            cadence_.add(moved);
            if (cadence_.mode() == CadenceHold::Mode::Every) fresh = true;
            else if (cadence_.mode() == CadenceHold::Mode::Alternate) fresh = cadence_.on_turn();
            if (cadence_.changed())
                log(L"dedup: {}", cadence_.mode() == CadenceHold::Mode::Every
                                      ? L"every captured frame is a game frame; kept so while the picture hardly moves"
                                      : cadence_.mode() == CadenceHold::Mode::Alternate
                                            ? L"every other captured frame is a game frame; kept so while the picture "
                                              L"hardly moves"
                                            : L"the game's frames come unevenly; pictures are compared one by one");
        }
        if (trusted_) fresh = true;
        keepalive_ = !fresh && now - last_sent_ms_ >= keepalive_ms_;
        count(fresh, changed, sample_max, block_max);
        const bool send = fresh || keepalive_;
        if (hold_) {
            if (send) sent_ = scratch_;
            before_.swap(scratch_);
        } else if (send) {
            sent_.swap(scratch_);
        }
        if (!send) return false;
        primed_ = true;
        last_sent_ms_ = now;
        return true;
    }

    // The rate the card sent over the last second or so (take() calls this).
    // Two seconds in a row well above the mode's rate: the mode's rate is not
    // what comes, and pictures are compared; two at it again, and it is
    // trusted again.
    void measured(double fps) {
        if (trusted_fps_ <= 0) return;
        const bool faster = fps > trusted_fps_ * 1.3;
        against_ = faster == trusted_ ? against_ + 1 : 0;
        if (against_ < 2) return;
        against_ = 0;
        trusted_ = !trusted_;
        if (trusted_)
            log(L"dedup: the card sends {:.1f} frames a second again, its mode's {:.3f}: every captured frame is a "
                L"game frame, pictures are not compared",
                fps, trusted_fps_);
        else
            log(L"dedup: the card sends {:.1f} frames a second, not its mode's {:.3f}: pictures are compared and "
                L"repeats left out",
                fps, trusted_fps_);
    }

    // Whether every frame is taken as new on the word of the mode's rate.
    bool trusted() const { return trusted_; }

    // The rhythm held, for tests (Free without hold_cadence).
    CadenceHold::Mode cadence() const { return cadence_.mode(); }

    // Whether the last frame take() let through was a repeat sent only to keep
    // mpv drawing.
    bool keepalive() const { return keepalive_; }

private:
    static constexpr UINT32 kRowStep = 4;
    static constexpr UINT32 kColStep = 2;
    // A block of 4 grid rows by 8 grid columns: 16x16 pixels, 32 samples.
    static constexpr UINT32 kBlockRows = 4;
    static constexpr UINT32 kBlockCols = 8;
    static constexpr int kSpotFactor = 8;
    static constexpr size_t kMinChanged = 4;

    // Logs the rate of new frames whenever it settles somewhere else, so the
    // log shows e.g. "30.0 new of 60.0 captured" for a 30 fps game, with how
    // many changes the repeats had at most and the new frames at least, and
    // how far the repeats' samples and block averages moved (the source's
    // noise). Repeats near the line, or noise near the threshold, mean a
    // threshold to raise.
    void count(bool fresh, size_t changed, int sample_max, double block_max) {
        const auto now = std::chrono::steady_clock::now();
        if (window_frames_ == 0) {
            window_start_ = now;
            repeat_max_ = 0;
            new_min_ = SIZE_MAX;
            noise_sample_ = 0;
            noise_block_ = 0.0;
        }
        ++window_frames_;
        if (fresh) {
            ++window_new_;
            if (primed_) new_min_ = std::min(new_min_, changed);
        } else {
            repeat_max_ = std::max(repeat_max_, changed);
            // Only repeats without a change: one with a small real change
            // under the line (a blinking dot) would read as huge noise.
            if (changed == 0) {
                noise_sample_ = std::max(noise_sample_, sample_max);
                noise_block_ = std::max(noise_block_, block_max);
            }
        }
        const double seconds = std::chrono::duration<double>(now - window_start_).count();
        if (seconds < 2.0) return;
        const double rate = window_new_ / seconds;
        if (std::fabs(rate - logged_rate_) >= 3.0) {
            log(L"dedup: {:.1f} new frames/s of {:.1f} captured (changes: repeats up to {}, new frames from {}, "
                L"new from {}; repeats' noise up to {} a sample, {:.1f} a block; threshold {})",
                rate, window_frames_ / seconds, repeat_max_, new_min_ == SIZE_MAX ? size_t(0) : new_min_,
                kMinChanged, noise_sample_, noise_block_, threshold_);
            logged_rate_ = rate;
        }
        window_frames_ = window_new_ = 0;
    }

    PixelFormat format_;
    UINT32 width_;
    int threshold_;
    DWORD keepalive_ms_;
    bool hold_;
    double trusted_fps_;
    bool trusted_;
    unsigned against_ = 0;  // seconds in a row that said the other thing
    std::chrono::steady_clock::time_point rate_start_;
    unsigned rate_frames_ = 0;
    UINT32 grid_w_, grid_h_, blocks_w_, blocks_h_;
    std::vector<UINT16> sent_, scratch_;
    std::vector<int> block_sum_, block_samples_;
    // hold_: the frame captured before this one, and the block sums against it.
    std::vector<UINT16> before_;
    std::vector<int> moved_sum_;
    CadenceHold cadence_;
    bool primed_ = false;
    bool keepalive_ = false;
    ULONGLONG last_sent_ms_ = 0;
    std::chrono::steady_clock::time_point window_start_;
    UINT64 window_frames_ = 0, window_new_ = 0;
    size_t repeat_max_ = 0, new_min_ = SIZE_MAX;
    int noise_sample_ = 0;
    double noise_block_ = 0.0;
    double logged_rate_ = -100.0;
};

}  // namespace app
