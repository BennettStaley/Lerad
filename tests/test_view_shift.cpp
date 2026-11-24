// SPDX-License-Identifier: MIT
// Checks what the add-on makes of the view shift the input program asks for
// (src/common/view_shift.h): pixels of the picture, limited, and nothing
// when the value is not to be trusted.
#include <cmath>
#include <cstdio>
#include <limits>

#include "view_shift.h"

using namespace app;

namespace {

int g_failures = 0;

void expect(bool ok, const char *what) {
    std::printf("%-76s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_failures;
}

bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

const int64_t kSecond = 10'000'000;
const float kWide = 3840, kHigh = 2160, kLimit = 0.10f;

ViewShift asked(float x, float y, uint32_t flags = 0) {
    ViewShift v{};
    v.magic = kViewShiftMagic;
    v.seq = 2;
    v.x = x;
    v.y = y;
    v.flags = flags;
    v.written = 100 * kSecond;
    return v;
}

ShiftNow at(const ViewShift &v, double seconds_later = 0.004) {
    return shift_pixels(v, v.written + int64_t(seconds_later * kSecond), kSecond, 0.1, kWide, kHigh, kLimit);
}

}  // namespace

int main() {
    {
        const ShiftNow s = at(asked(0.02f, -0.01f));
        expect(near(s.x, 76.8f) && near(s.y, -21.6f) && !s.dot, "a share of the picture is that many of its pixels");
    }
    {
        const ShiftNow s = at(asked(0.5f, 0));
        expect(near(s.x, 384) && s.y == 0, "no further than the limit");
        const ShiftNow d = at(asked(0.3f, 0.3f * kWide / kHigh));  // the same pixels each way
        expect(near(std::sqrt(d.x * d.x + d.y * d.y), 384) && near(d.x, d.y), "the limit keeps the direction");
    }
    expect(!at(asked(0, 0)).any(), "nothing asked: the frame is left alone");
    expect(!at(asked(0.00001f, 0)).any(), "less than a tenth of a pixel is nothing");
    expect(at(asked(0, 0, kViewShiftCrosshair)).dot && at(asked(0, 0, kViewShiftCrosshair)).any(),
           "the dot alone is something to draw");
    expect(!at(asked(0.02f, 0), 0.25).any(), "an old value counts as none: the input program stopped");
    expect(!at(asked(0.02f, 0), -1.0).any(), "a value from the future is not trusted");
    {
        ViewShift v = asked(0.02f, 0);
        v.seq = 3;
        expect(!at(v).any(), "a value being written is not used");
        v = asked(0.02f, 0);
        v.magic = 0;
        expect(!at(v).any(), "memory that is not the input program's is not used");
        v = asked(std::numeric_limits<float>::quiet_NaN(), 0);
        expect(!at(v).any(), "not a number is nothing");
    }
    std::printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
