// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2026 Abhito
// The arithmetic of frame generation's pacing and of the numbers it reports,
// kept apart from Direct3D so it can be tested (tests/test_fg_pacing.cpp):
//
//   RateWindow      events per second over the last second (frames presented,
//                   frames skipped and why)
//   SourceCadence   what the card delivers: its rate now, and whether that
//                   rate is steady, moving (a VRR or unlocked game) or steady
//                   in uneven steps (45 fps inside a 60 Hz signal)
//   MultiplierGate  how many frames to show per source frame (x2, x3, x4 or
//                   none) so the result fits the screen, without flapping
//                   when the source's rate sits at the edge
//   judge_cadence   how evenly that output can reach this screen: inside a
//                   VRR range, at its top, or on a fixed refresh grid it
//                   does or does not divide
//   squeezed_step   the timed hold's step when the next frame is due sooner
//   judge_refresh   whether the screen was seen refreshing at its fixed rate
//                   (VRR not engaging) or following the frames

#pragma once

#include <algorithm>
#include <cmath>

#include "framegen_status.h"

namespace fg::pacing {

using Vrr = glasir::FramegenStatus::Vrr;
using Cadence = glasir::FramegenStatus::Cadence;
using Seen = glasir::FramegenStatus::Seen;

constexpr int kMaxMultiplier = 4;

// Times of the latest events, for a rate over the last second.
class RateWindow {
public:
    void add(double now) {
        times_[head_] = now;
        head_ = (head_ + 1) % kMax;
        if (count_ < kMax) ++count_;
    }
    void clear() { head_ = count_ = 0; }

    // Events in the last `window` seconds.
    int count(double now, double window = 1.0) const {
        int n = 0;
        for (int i = 1; i <= count_; ++i) {
            if (now - times_[(head_ - i + kMax) % kMax] > window) break;
            ++n;
        }
        return n;
    }

    // Events per second over the last `window` seconds. With three or more,
    // from the time they span (a steady 30 a second reads 30.0, not 29 or 31
    // by where the window's edge falls) - unless the latest is older than two
    // of their gaps: then they have stopped, and the plain count says so.
    double rate(double now, double window = 1.0) const {
        const int n = count(now, window);
        if (n < 3) return n / window;
        const double newest = times_[(head_ - 1 + kMax) % kMax];
        const double oldest = times_[(head_ - n + kMax) % kMax];
        const double gap = (newest - oldest) / (n - 1);
        if (gap <= 0 || now - newest > 2.0 * gap + 0.01) return n / window;
        return 1.0 / gap;
    }

private:
    static constexpr int kMax = 512;  // more than any screen shows in a second
    double times_[kMax] = {};
    int head_ = 0, count_ = 0;
};

// The arrival times of the source's new frames (the capture card's clock, by
// way of Glasir's link), and what they say about its rate.
class SourceCadence {
public:
    struct Summary {
        double fps = 0;             // over the last second, pauses left out; 0 = too few frames
        double low = 0, high = 0;   // half-second rates over the last three seconds
        bool variable = false;      // the rate itself moves
        bool uneven = false;        // a steady rate, delivered in uneven steps
    };

    void add(double stamp) {
        if (count_ > 0 && stamp <= at(1)) return;  // out of order: not a new frame
        stamps_[head_] = stamp;
        head_ = (head_ + 1) % kMax;
        if (count_ < kMax) ++count_;
    }
    void clear() { head_ = count_ = 0; }

    // pause: a gap longer than this is the game standing still, not its rate.
    Summary summary(double now, double pause = 0.25) const {
        Summary s;
        // Half-second bins over the last three seconds: gaps and their sum.
        constexpr int kBins = 6;
        constexpr double kBin = 0.5;
        int n[kBins] = {};
        double sum[kBins] = {};
        double gaps[kMax];
        int recent = 0;  // gaps in the last second, for the steps' evenness
        for (int i = 1; i < count_; ++i) {
            const double newer = at(i), gap = newer - at(i + 1);
            const double age = now - newer;
            if (age >= kBins * kBin) break;
            if (age < 0 || gap <= 0 || gap > pause) continue;
            const int bin = int(age / kBin);
            ++n[bin];
            sum[bin] += gap;
            if (age < 1.0) gaps[recent++] = gap;
        }
        const int n1 = n[0] + n[1];
        if (n1 < 3) return s;
        s.fps = n1 / (sum[0] + sum[1]);
        s.low = s.high = s.fps;
        for (int b = 0; b < kBins; ++b) {
            // A bin with a pause in it holds few gaps: not a rate.
            if (n[b] < 4 || sum[b] < kBin * 0.6) continue;
            const double r = n[b] / sum[b];
            s.low = std::min(s.low, r);
            s.high = std::max(s.high, r);
        }
        // A steady 45 fps inside 60 Hz reads 44-45.5 by where a bin's edge
        // falls among its uneven steps; a moving rate moves by more.
        s.variable = s.high - s.low > std::max(2.5, 0.08 * s.fps);
        // Uneven steps: the long gaps (the 90th percentile) well over the
        // short ones (the 10th). Arrival jitter of a millisecond or two on
        // an 8 ms gap stays under this.
        if (recent >= 8) {
            std::sort(gaps, gaps + recent);
            s.uneven = gaps[recent - 1 - recent / 10] > 1.6 * gaps[recent / 10];
        }
        return s;
    }

private:
    static constexpr int kMax = 512;
    double at(int back) const { return stamps_[(head_ - back + kMax) % kMax]; }  // 1 = the newest
    double stamps_[kMax] = {};
    int head_ = 0, count_ = 0;
};

// The most frames a second the screen (or the user's limit) takes.
struct Limit {
    double hz = 0;      // the screen's refresh rate, or max_output_hz
    bool exact = false; // a limit the user set: no tolerance
};

// Whether `fps` frames a second fit under the limit. A screen's own rate
// gets 5 % of tolerance: a 60.0 fps source doubled is 120.0 on a 119.88 Hz
// screen, and the two clocks are never the same anyway. `staying`: already
// showing this many - a little more room, so a rate at the edge does not
// switch back and forth.
inline bool fits(double fps, Limit limit, bool staying) {
    if (limit.hz <= 0) return false;
    const double room = (limit.exact ? 1.005 : 1.05) * (staying ? 1.03 : 1.0);
    return fps <= limit.hz * room;
}

// The most frames a second that may be shown on a `display_hz` screen with
// [framegen] max_output_hz: the user's limit when it is below the screen's
// rate (kept exactly), else the screen's own (60 when it is not known). A
// limit at or above the screen's rate changes nothing.
inline Limit output_limit(double display_hz, int max_output_hz) {
    const double screen = display_hz > 0 ? display_hz : 60.0;
    if (max_output_hz > 0 && max_output_hz < screen) return {double(max_output_hz), true};
    return {screen, false};
}

// Whether `fps` frames a second fall on whole refreshes of a `hz` screen.
inline bool divides(double fps, double hz) {
    if (fps <= 0 || hz <= 0) return false;
    const double refreshes = hz / fps;
    return std::round(refreshes) >= 1 && std::fabs(refreshes - std::round(refreshes)) < 0.03;
}

// How many frames to show per source frame: 1 = the real ones only.
//
// The most that fits, up to what was asked for. On a screen known to refresh
// at a fixed rate, a smaller one that falls on whole refreshes is preferred
// to a larger one that does not (60 fps on a fixed 240 Hz, up to x3: x2 is
// 2 refreshes a frame, x3 would be 1, 1, 2).
//
// Going up again after going down waits two seconds: a source whose rate
// wanders across the edge (a VRR game at 80-90 fps on a 165 Hz screen) would
// otherwise switch every moment, which shows more than staying down does.
class MultiplierGate {
public:
    int choose(double now, double source_fps, int wanted, Limit limit, Vrr vrr, double display_hz) {
        wanted = std::clamp(wanted, 1, kMaxMultiplier);
        int best = 1;
        if (source_fps > 0) {
            int even = 0, any = 0;
            for (int k = 2; k <= wanted; ++k) {
                if (!fits(k * source_fps, limit, k <= current_)) continue;
                any = k;
                if (divides(k * source_fps, display_hz)) even = k;
            }
            best = std::max(1, vrr == Vrr::Off && even ? even : any);
        }
        if (best > current_ && now < up_after_) best = current_;
        if (best < current_) up_after_ = now + 2.0;
        current_ = best;
        return best;
    }
    int current() const { return current_; }
    // Frame generation was switched or started over: no memory of before.
    void reset() {
        current_ = 1;
        up_after_ = 0;
    }

private:
    int current_ = 1;
    double up_after_ = 0;
};

struct Verdict {
    Cadence cadence = Cadence::Unknown;
    int better_hz = 0;  // a refresh rate the output would divide, when it does not divide this one
};

// How evenly `output_fps` frames a second can reach a `hz` screen.
inline Verdict judge_cadence(double output_fps, double hz, Vrr vrr) {
    Verdict v;
    if (output_fps <= 0 || hz <= 0) return v;
    const bool whole = divides(output_fps, hz);
    if (vrr == Vrr::On) {
        // From 97 % of the refresh rate up there is no room to move a frame.
        v.cadence = output_fps >= hz * 0.97 ? Cadence::VrrCeiling : Cadence::VrrEven;
        return v;
    }
    if (whole) {
        v.cadence = vrr == Vrr::Off ? Cadence::FixedEven : Cadence::MaybeEven;
        return v;
    }
    v.cadence = vrr == Vrr::Off ? Cadence::FixedUneven : Cadence::MaybeUneven;
    // The output rate itself, if the screen is faster than that (a 144 Hz
    // screen set to 120 Hz for 60 -> 120 fps).
    const int rounded = int(std::lround(output_fps));
    if (rounded >= 24 && rounded < hz * 0.97) v.better_hz = rounded;
    return v;
}

// The timed hold: `held` frames are let go one `step` apart, the first a step
// after the first frame in between was done (`drawn`). All must be out by
// `latest` (the next source frame's effects are due then); when that is
// sooner, the steps shrink evenly rather than the last frames bunching up.
inline double squeezed_step(double step, double drawn, double latest, int held) {
    if (held < 1) return step;
    return std::clamp((latest - drawn) / held, 0.0, step);
}

// What the screen was seen doing while `output_fps` frames a second were
// presented: `seen_hz` refreshes a second (from the swap chain's refresh
// counter). Only tells anything when the output is clearly below the refresh
// rate (at the top, a VRR screen refreshes at its full rate too) and not
// below 45 a second (under its range a VRR screen shows each frame several
// times, which can add up to its full rate).
inline Seen judge_refresh(double seen_hz, double display_hz, double output_fps) {
    if (seen_hz <= 0 || display_hz <= 0 || output_fps < 45) return Seen::Unknown;
    if (output_fps > display_hz * 0.9) return Seen::Unknown;
    if (std::fabs(seen_hz - display_hz) < display_hz * 0.03) return Seen::Fixed;
    if (seen_hz < display_hz * 0.93) return Seen::Following;
    return Seen::Unknown;
}

}  // namespace fg::pacing
