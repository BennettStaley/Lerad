// SPDX-License-Identifier: MIT
// The view shift: how far the picture is to be moved on the screen right now,
// told by the input program (the console played with this PC's mouse and
// keyboard) to the add-on that draws every frame.
//
// The console answers a turn of the view a tenth of a second or more after
// the mouse moved. The input program knows what it has sent and how fast the
// game turns for it, so it knows how far the view is about to turn. The
// add-on moves the picture by that much at once, on every frame that reaches
// the screen; as the console's own frames arrive, the amount comes back to
// nothing.
//
// A named file mapping holds one of these. The input program writes it (seq
// is odd while it does), the add-on reads it for each frame.
#pragma once
#include <cmath>
#include <cstdint>

namespace app {

constexpr uint32_t kViewShiftMagic = 0x31485356;  // "VSH1"
constexpr uint32_t kViewShiftCrosshair = 1;       // draw a fixed dot at the picture's centre (aiming)

struct ViewShift {
    uint32_t magic;
    volatile uint32_t seq;
    // Where each point of the picture takes its colour from, as a share of
    // the picture's width and height: (0.02, 0) shows what was 2 % of the
    // width further right, as after a turn to the right.
    float x, y;
    uint32_t flags;
    uint32_t reserved;
    int64_t written;  // QueryPerformanceCounter at the write: an old one counts as none
};
static_assert(sizeof(ViewShift) == 32);

// What to do to a frame: the shift in pixels and the dot.
struct ShiftNow {
    float x = 0, y = 0;  // pixels
    bool dot = false;
    bool any() const { return x != 0 || y != 0 || dot; }
};

// The asked shift as pixels of a picture width x height pixels large, no
// further than limit of its width. Nothing for a value that is not the input
// program's, is being written, or is older than max_age seconds (the input
// program has stopped: the picture must not stay moved).
inline ShiftNow shift_pixels(const ViewShift &asked, int64_t now, int64_t per_second, double max_age, float width,
                             float height, float limit) {
    ShiftNow out;
    if (asked.magic != kViewShiftMagic || (asked.seq & 1)) return out;
    if (now - asked.written > int64_t(max_age * double(per_second)) || now < asked.written) return out;
    if (!std::isfinite(asked.x) || !std::isfinite(asked.y)) return out;
    float x = asked.x * width, y = asked.y * height;
    const float most = limit * width;
    const float length = std::sqrt(x * x + y * y);
    if (length > most) {
        x *= most / length;
        y *= most / length;
    }
    // Less than a tenth of a pixel is nothing: the frame is then left alone.
    if (length >= 0.1f) out.x = x, out.y = y;
    out.dot = (asked.flags & kViewShiftCrosshair) != 0;
    return out;
}

}  // namespace app
