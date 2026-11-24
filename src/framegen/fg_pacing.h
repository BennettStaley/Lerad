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
//   StepDown        x4 or x3 that fits the screen but cannot be held there
//                   (frames left out because presents wait, or mpv falling
//                   behind): one fewer for a while, so that every game frame
//                   gets the same number of frames, and when to try again
//   judge_cadence   how evenly that output can reach this screen: inside a
//                   VRR range, at its top, or on a fixed refresh grid it
//                   does or does not divide
//   squeezed_step   the timed hold's step when the next frame is due sooner
//   judge_refresh   whether the screen was seen refreshing at its fixed rate
//                   (VRR not engaging) or following the frames
//   RefreshWatch    that, from a count of the screen's refreshes read once a
//                   second, starting over when the window changes screens

#pragma once

#include <algorithm>
#include <cmath>

#include "framegen_status.h"

namespace fg::pacing {

using Vrr = app::FramegenStatus::Vrr;
using Cadence = app::FramegenStatus::Cadence;
using Seen = app::FramegenStatus::Seen;
using StepCause = app::FramegenStatus::StepCause;

constexpr int kMaxMultiplier = 4;

// Values with the times they were noted at, for their mean over the last
// second (the menu's delay from the card's frame to its present).
class MeanWindow {
public:
    void add(double now, double value) {
        times_[head_] = now;
        values_[head_] = value;
        head_ = (head_ + 1) % kMax;
        if (count_ < kMax) ++count_;
    }

    // 0 with nothing in the last `window` seconds.
    double mean(double now, double window = 1.0) const {
        double sum = 0;
        int n = 0;
        for (int i = 1; i <= count_; ++i) {
            const int at = (head_ - i + kMax) % kMax;
            if (now - times_[at] > window) break;
            sum += values_[at];
            ++n;
        }
        return n ? sum / n : 0;
    }

private:
    static constexpr int kMax = 256;
    double times_[kMax] = {}, values_[kMax] = {};
    int head_ = 0, count_ = 0;
};

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

    // From the oldest of the events in the last `window` seconds to the
    // newest: 0 with fewer than two.
    double span(double now, double window = 1.0) const {
        const int n = count(now, window);
        if (n < 2) return 0;
        return times_[(head_ - 1 + kMax) % kMax] - times_[(head_ - n + kMax) % kMax];
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
// way of the app's link), and what they say about its rate.
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

// How many frames per source frame fit: 1 = the real ones only.
//
// The most that fits, up to what was asked for. On a screen known to refresh
// at a fixed rate, a smaller one that falls on whole refreshes is preferred
// to a larger one that does not (60 fps on a fixed 240 Hz, up to x3: x2 is
// 2 refreshes a frame, x3 would be 1, 1, 2). `current`: how many are shown
// now (they get a little more room, see fits).
inline int fitting(double source_fps, int wanted, Limit limit, Vrr vrr, double display_hz, int current) {
    wanted = std::clamp(wanted, 1, kMaxMultiplier);
    if (source_fps <= 0) return 1;
    int even = 0, any = 0;
    for (int k = 2; k <= wanted; ++k) {
        if (!fits(k * source_fps, limit, k <= current)) continue;
        any = k;
        if (divides(k * source_fps, display_hz)) even = k;
    }
    return std::max(1, vrr == Vrr::Off && even ? even : any);
}

// How many frames to show per source frame: what fits (above), without
// switching back and forth.
//
// Going up again after going down waits two seconds: a source whose rate
// wanders across the edge (a VRR game at 80-90 fps on a 165 Hz screen) would
// otherwise switch every moment, which shows more than staying down does.
class MultiplierGate {
public:
    int choose(double now, double source_fps, int wanted, Limit limit, Vrr vrr, double display_hz) {
        int best = fitting(source_fps, wanted, limit, vrr, display_hz, current_);
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

// A multiplier that fits the screen and still cannot be held there.
//
// With x3 and x4 all of a game frame's frames must be out before the next
// one comes. Where presents wait for the screen (a window Windows composes
// takes one frame a refresh: 30 fps x4 on 120 Hz leaves it no spare one) or
// the GPU is late with the first added frame, presents are still queued when
// the next game frame comes, and that one is shown alone ("queue full"):
// four frames, then one that stays four times as long - measured in a
// composed window, every other game frame. One fewer for every game frame
// shows fewer frames a second and looks better: each stays the same time.
//
// So, of the game frames that would get added frames at x3 or x4:
//   - kLeftOut of them shown alone within kWindow seconds, the first and the
//     last of those kSpread apart (a single hiccup of mpv or of Windows
//     leaves out a few in a row and is over), or mpv dropping new frames
//     while generating (where all frame generation would pause for seconds):
//     one fewer from the next game frame on.
//   - After kFirstWait seconds the one above is tried again. For kTrial
//     seconds kTrialLeftOut frames shown alone are then enough to go back
//     down, and the next try waits twice as long, kMaxWait at most. A try
//     that held for kHeld seconds was not a failed one: the waits start over.
//   - Tried at once (retry) when something changed that may have been the
//     cause: the window, the screen, the picture's size, the source's rate,
//     ReShade's effects - but the lower step is kept kMinHold seconds at
//     least, so a window being dragged does not try every moment. A setting
//     of frame generation changed by the user forgets everything (again).
//   - x2 is never stepped down from: mpv not keeping up there is what the
//     add-on's pause is for, and frames left out there are said in the menu.
//
// Why: the GPU, when mpv dropped frames or when most of the pairs made
// lately had their frames let go early for the next game frame (`squeezed`:
// the first added frame was done too late for the others to keep their
// step); else the screen. The same for either generator: it only counts
// frames.
class StepDown {
public:
    static constexpr double kWindow = 2.0;
    static constexpr int kLeftOut = 4;
    static constexpr double kSpread = 0.5;
    static constexpr double kTrial = 3.0;
    static constexpr int kTrialLeftOut = 2;
    static constexpr double kFirstWait = 20.0, kMaxWait = 300.0;
    static constexpr double kHeld = 60.0;
    static constexpr double kMinHold = 5.0;
    // A multiplier just begun (frame generation started, a setting changed)
    // loses frames for a moment while it finds its rhythm: seen on the card
    // as 8 game frames left out in the first 1.7 s of x4 that then held
    // without one, and in another start as 4 between 2.3 and 4.3 s. Those
    // say nothing about what can be held.
    static constexpr double kSettle = 6.0;
    static constexpr double kRateMoved = 0.10;  // the source's rate is another one from this far off

    // What a call changed, for the log. `true` when it changed something.
    struct Change {
        enum class Kind { None, Down, Try };
        Kind kind = Kind::None;
        int from = 0, to = 0;  // the most frames per game frame before and after
        StepCause cause = StepCause::None;  // Down: why
        int left_out = 0;      // Down: game frames shown alone in the window (0: mpv dropped frames)
        double wait = 0;       // Down: seconds until `from` is tried again
        explicit operator bool() const { return kind != Kind::None; }
    };

    // The most frames per game frame for now: `wanted`, or fewer.
    int most(int wanted) const { return std::min(wanted, limit_); }
    // Whether fewer than kMaxMultiplier are allowed for now.
    bool stepped() const { return limit_ < kMaxMultiplier; }
    StepCause cause() const { return stepped() ? cause_ : StepCause::None; }
    // Until the one above is tried again; 0 when nothing is held back.
    double seconds_left(double now) const { return stepped() ? std::max(0.0, retry_at_ - now) : 0.0; }
    // Since the last step down or try.
    double since_change(double now) const { return now - changed_at_; }

    // Before each game frame's count is chosen: the one above is tried when
    // its time has come. wanted: the setting.
    Change due(double now, int wanted) {
        if (!stepped() || limit_ >= wanted || now < retry_at_) return {};
        Change c;
        c.kind = Change::Kind::Try;
        c.from = limit_;
        c.to = ++limit_;
        tried_at_ = changed_at_ = now;
        trial_until_ = now + kTrial;
        retry_at_ = now + kFirstWait;  // the one above that, if this one holds
        return c;
    }

    // A game frame that would get its added frames with `count` shown for
    // each: left_out = it is shown alone, presents were still queued.
    Change frame(double now, int count, bool left_out) {
        if (count != count_) {
            // What was counted was another multiplier's.
            count_ = count;
            left_.clear();
            made_.clear();
            squeezed_.clear();
            // Not while the one above is being tried: that is judged at once.
            settle_until_ = now >= trial_until_ ? now + kSettle : 0;
        }
        if (left_out && now < settle_until_) return {};
        (left_out ? left_ : made_).add(now);
        if (!left_out || count <= 2) return {};
        const int n = left_.count(now, kWindow);
        const bool enough = now < trial_until_ ? n >= kTrialLeftOut : n >= kLeftOut && left_.span(now, kWindow) >= kSpread;
        if (!enough) return {};
        const int made = made_.count(now, kWindow);
        const bool gpu = made > 0 && 2 * squeezed_.count(now, kWindow) > made;
        return down(now, count, gpu ? StepCause::Gpu : StepCause::Screen, n);
    }

    // Game frames whose added frames were let go early for the next game
    // frame, since the last call (the hold's count).
    void squeezed(double now, unsigned frames) {
        for (unsigned i = 0; i < std::min(frames, 64u); ++i) squeezed_.add(now);
    }

    // mpv dropped new frames while `count` frames were shown for each: with
    // more than two, one fewer is tried before all frame generation pauses.
    // Nothing changed (the pause it is) at x2.
    Change behind(double now, int count) {
        if (count <= 2) return {};
        return down(now, count, StepCause::Gpu, 0);
    }

    // Something changed that may have been the cause: the one above is tried
    // now, or once the lower step has been held kMinHold seconds.
    void retry(double now) {
        if (stepped()) retry_at_ = std::min(retry_at_, std::max(now, down_at_ + kMinHold));
    }

    // The source's rate, looked at now and then. steady: not one that moves
    // by itself (a VRR game), which would try at every swing. A rate
    // kRateMoved from the one it was stepped down at is another matter: tried
    // (retry), and true.
    bool source(double now, double fps, bool steady) {
        if (!stepped() || fps <= 0) return false;
        if (fps_ <= 0) fps_ = fps;
        if (!steady || std::fabs(fps - fps_) <= kRateMoved * fps_) return false;
        fps_ = fps;
        retry(now);
        return true;
    }

    // A setting of frame generation was changed, or it starts over: nothing
    // is known about what holds now.
    void again() { *this = StepDown(); }

private:
    Change down(double now, int count, StepCause cause, int left_out) {
        Change c;
        c.kind = Change::Kind::Down;
        c.from = count;
        c.to = count - 1;
        c.cause = cause;
        c.left_out = left_out;
        if (now - tried_at_ >= kHeld) wait_ = kFirstWait;  // not a failed try
        c.wait = wait_;
        limit_ = count - 1;
        cause_ = cause;
        down_at_ = changed_at_ = now;
        retry_at_ = now + wait_;
        wait_ = std::min(wait_ * 2.0, kMaxWait);
        tried_at_ = -1e9;
        trial_until_ = 0;
        fps_ = 0;
        left_.clear();
        made_.clear();
        squeezed_.clear();
        return c;
    }

    int limit_ = kMaxMultiplier;  // the most frames per game frame for now
    StepCause cause_ = StepCause::None;
    double wait_ = kFirstWait;    // how long the next step down lasts
    double retry_at_ = 0, down_at_ = -1e9, tried_at_ = -1e9, trial_until_ = 0, changed_at_ = -1e9;
    double fps_ = 0;              // the source's rate when it was stepped down
    int count_ = 0;               // the multiplier the windows below are of
    double settle_until_ = 0;     // frames left out before this are not counted
    RateWindow left_, made_, squeezed_;
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
// presented: `seen_hz` refreshes a second (that screen's own, counted:
// fg_vblank.h). Only tells anything when the output is clearly below the refresh
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

// The refreshes of the screen the window is on, looked at once a second:
// how many a second it was seen making, and what that says (judge_refresh).
// The count is the screen's own (fg_vblank.h), and it starts over whenever
// the window is on another screen, that screen's mode changes or the counting
// had a hole: `epoch` is another number then, and nothing is worked out
// across the change - a second with refreshes of two screens in it is never
// judged.
class RefreshWatch {
public:
    // Forget everything: the window is on another screen.
    void reset() { *this = RefreshWatch(); }

    // epoch       which run of counting `count` belongs to
    // count, at   the refreshes counted so far and when the latest was
    //             (seconds); at <= 0: none yet
    // now         the same clock, when this was read
    // display_hz  the screen's refresh rate
    // shown_fps   the frames presented a second over about the last second
    void sample(unsigned epoch, unsigned count, double at, double now, double display_hz, double shown_fps) {
        if (!based_ || epoch != epoch_) {
            // Another run: start from here, with nothing known.
            unknown();
            epoch_ = epoch;
            based_ = at > 0;
            count_ = count;
            at_ = at;
            return;
        }
        if (at <= 0 || now - at > kStale) {
            // The count stands still: the screen is off or asleep, or its
            // refreshes cannot be waited for.
            unknown();
            based_ = false;
            return;
        }
        const double span = at - at_;
        if (span < 0.5) return;  // asked again too soon: wait for more
        if (span < 3.0) {
            // Unsigned on purpose: the count may wrap.
            const double counted = double(count - count_) / span;
            seen_hz_ = counted > 0 && counted < 1000 ? counted : 0;
            const Seen seen = judge_refresh(seen_hz_, display_hz, shown_fps);
            // "Fixed" only after three seconds of it: one sample can straddle
            // a change of rate.
            fixed_ = seen == Seen::Fixed ? fixed_ + 1 : 0;
            seen_ = seen == Seen::Fixed && fixed_ < 3 ? Seen::Unknown : seen;
        } else {
            // Not looked at for seconds (mpv stood still): the frames shown
            // are the last second's, the count is not.
            unknown();
        }
        count_ = count;
        at_ = at;
    }

    double seen_hz() const { return seen_hz_; }  // refreshes a second, 0 unknown
    Seen seen() const { return seen_; }

private:
    static constexpr double kStale = 0.5;  // no refresh for this long: not counting
    void unknown() {
        seen_hz_ = 0;
        seen_ = Seen::Unknown;
        fixed_ = 0;
    }

    bool based_ = false;  // count_ and at_ are a sample of epoch_
    unsigned epoch_ = 0, count_ = 0;
    double at_ = 0, seen_hz_ = 0;
    Seen seen_ = Seen::Unknown;
    int fixed_ = 0;  // samples in a row that read "fixed"
};

}  // namespace fg::pacing
