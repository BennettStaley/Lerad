// SPDX-License-Identifier: MIT
// Sample conversion and the bounded, resampling sample queue of the app's
// audio plugin.
//
// Based on native-audio/audio_core.h of the WASAPI audio plugin by z-er
// (Josh S) for MPV-SW-Capture, https://github.com/z-er/MPV-SW-Capture,
// MIT licence, Copyright (c) 2026 TyRaS-SW. Modified for this app.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace app::audio {

struct Stereo {
    float left = 0, right = 0;
};

// One sample of a PCM (16/24/32-bit) or float32 stream, as -1..1.
inline float decode(const unsigned char *p, unsigned bits, bool floating) {
    if (floating) {
        float v;
        std::memcpy(&v, p, 4);
        return std::isfinite(v) ? v : 0;
    }
    if (bits == 16) {
        int16_t v;
        std::memcpy(&v, p, 2);
        return v / 32768.0f;
    }
    if (bits == 24) {
        int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
        if (v & 0x800000) v -= 0x1000000;
        return v / 8388608.0f;
    }
    int32_t v;
    std::memcpy(&v, p, 4);
    return float(v / 2147483648.0);
}

// Writes v (clipped to -1..1) as a PCM or float32 sample.
inline void encode(unsigned char *p, float v, unsigned bits, bool floating) {
    v = std::isfinite(v) ? std::clamp(v, -1.0f, 1.0f) : 0;
    if (floating) {
        std::memcpy(p, &v, 4);
        return;
    }
    if (bits == 16) {
        const int16_t n = int16_t(std::clamp(std::llround(v * 32768.0), -32768LL, 32767LL));
        std::memcpy(p, &n, 2);
    } else if (bits == 24) {
        const int32_t n = int32_t(std::clamp(std::llround(v * 8388608.0), -8388608LL, 8388607LL));
        p[0] = n & 255;
        p[1] = (n >> 8) & 255;
        p[2] = (n >> 16) & 255;
    } else {
        const int32_t n = int32_t(std::clamp(std::llround(v * 2147483648.0), -2147483648LL, 2147483647LL));
        std::memcpy(p, &n, 4);
    }
}

// Owned by the one audio thread. No allocations in push or read.
class Ring {
public:
    explicit Ring(size_t capacity) : data_(std::max(size_t(2), capacity)) {}

    size_t size() const { return count_; }
    size_t capacity() const { return data_.size(); }

    // Drops the oldest samples down to keep (bounds latency after a stall,
    // or when the audio delay is lowered).
    void trim(size_t keep) {
        if (count_ > keep) {
            const size_t n = count_ - keep;
            head_ = (head_ + n) % data_.size();
            count_ -= n;
            dropped += n;
            phase_ = 0;
        }
    }

    void push(Stereo v) {
        if (count_ == data_.size()) trim(count_ - 1);
        data_[(head_ + count_) % data_.size()] = v;
        ++count_;
    }

    // The next output sample, linearly interpolated, advancing by step
    // input samples (input rate / output rate). False when too few are queued.
    bool read(double step, Stereo &v) {
        if (!(step > 0) || !std::isfinite(step)) return false;
        const size_t advance = size_t(phase_ + step);
        if (count_ < 2 || advance >= count_) return false;
        const Stereo a = data_[head_], b = data_[(head_ + 1) % data_.size()];
        v = {float(a.left + (b.left - a.left) * phase_), float(a.right + (b.right - a.right) * phase_)};
        phase_ += step;
        phase_ -= advance;
        head_ = (head_ + advance) % data_.size();
        count_ -= advance;
        return true;
    }

    uint64_t dropped = 0;

private:
    std::vector<Stereo> data_;
    size_t head_ = 0, count_ = 0;
    double phase_ = 0;
};

// Samples to keep queued: what the two devices need to run without gaps
// (base) plus the audio delay the user asked for.
inline size_t queue_target(double input_rate, size_t base, int delay_ms) {
    return base + size_t(std::max(0, delay_ms) * input_rate / 1000.0);
}

// Takes the click out of a break in the sound. A waveform that stops or
// jumps at once is heard as a pop: here the last level dies away over about
// 2 ms instead, and sound that starts (again) comes in over 5 ms.
class Smooth {
public:
    explicit Smooth(double output_rate)
        : decay_(float(std::exp(-1.0 / (output_rate * .002)))), rise_(float(1.0 / (output_rate * .005))) {}

    // A sample of real sound.
    Stereo play(Stereo v) {
        level_ = std::min(1.0f, level_ + rise_);
        fade_tail();
        last_ = {v.left * level_ + tail_.left, v.right * level_ + tail_.right};
        return last_;
    }

    // A sample with nothing to play.
    Stereo gap() {
        cut();
        fade_tail();
        last_ = tail_;
        return last_;
    }

    // The sound is about to jump (samples were dropped): what was playing
    // dies away under what comes in.
    void cut() {
        if (level_ > 0) {
            tail_ = last_;
            level_ = 0;
        }
    }

private:
    void fade_tail() {
        tail_.left = std::abs(tail_.left) < 1e-6f ? 0 : tail_.left * decay_;
        tail_.right = std::abs(tail_.right) < 1e-6f ? 0 : tail_.right * decay_;
    }

    float decay_, rise_, level_ = 0;
    Stereo tail_, last_;
};

// Spare samples kept queued beyond what the two devices' periods need, so
// that a packet a few milliseconds late does not empty the queue. It starts
// small (latency), grows at once each time the queue runs dry anyway, and
// comes back down a millisecond at a time after ten minutes in which the
// spare was never needed. Times in ms from any steady clock.
class Margin {
public:
    static constexpr double kStart = .005, kGrow = .004, kMost = .040;  // seconds of sound
    static constexpr double kUnused = .008;     // this much always left: the spare is more than needed
    static constexpr double kStallMs = 60;      // a wait this long is a stall, not a late packet
    static constexpr double kQuietMs = 600000;

    explicit Margin(double rate) : rate_(rate), extra_(rate * kStart) {}

    size_t samples() const { return size_t(extra_); }
    double ms() const { return extra_ * 1000 / rate_; }

    // The queue ran dry after waiting waited_ms for the card's next packet.
    // A stall (the whole program held up) would not be cured by a deeper
    // queue, so it does not grow it.
    void ran_dry(double now_ms, double waited_ms) {
        restart(now_ms);
        if (waited_ms < kStallMs) extra_ = std::min(rate_ * kMost, extra_ + rate_ * kGrow);
    }

    // After sound was written: how many samples stayed queued.
    void left(double now_ms, size_t queued) {
        if (!started_) restart(now_ms);
        low_ = std::min(low_, double(queued));
        if (now_ms - quiet_from_ < kQuietMs) return;
        if (low_ > rate_ * kUnused) extra_ = std::max(rate_ * kStart, extra_ - rate_ * .001);
        restart(now_ms);
    }

private:
    void restart(double now_ms) {
        started_ = true;
        quiet_from_ = now_ms;
        low_ = 1e300;
    }

    double rate_, extra_, quiet_from_ = 0, low_ = 1e300;
    bool started_ = false;
};

// Volume 0-100 %, boost 100-400 %, mute.
inline double output_gain(double volume, int boost_percent, bool muted) {
    if (muted) return 0;
    return std::clamp(volume, 0.0, 100.0) / 100.0 * std::clamp(boost_percent, 100, 400) / 100.0;
}

}  // namespace app::audio
