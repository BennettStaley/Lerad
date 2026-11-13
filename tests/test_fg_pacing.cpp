// SPDX-License-Identifier: MIT
// Checks frame generation's pacing arithmetic (src/framegen/fg_pacing.h) and
// the menu's words for it (src/overlay/framegen_text.h): the rates shown,
// how many frames fit which screen, how they are spaced on a fixed-refresh
// screen, and what is said when the screen makes the result uneven.
#include <cmath>
#include <cstdio>
#include <string>

#include "fg_pacing.h"
#include "framegen_text.h"

using namespace fg::pacing;
using glasir::FramegenStatus;

namespace {

int g_failures = 0;

void expect(bool ok, const char *what) {
    std::printf("%-72s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_failures;
}

bool near(double a, double b, double by) { return std::fabs(a - b) <= by; }

bool has(const std::string &text, const char *part) { return text.find(part) != std::string::npos; }

// Frames at these gaps (seconds), repeated until `seconds` have passed.
double feed(SourceCadence &c, double t, std::initializer_list<double> gaps, double seconds) {
    const double end = t + seconds;
    while (t < end)
        for (double g : gaps) c.add(t += g);
    return t;
}

void rates() {
    RateWindow w;
    double t = 10;
    for (int i = 0; i < 90; ++i) w.add(t += 1.0 / 30);
    expect(near(w.rate(t), 30.0, 0.01), "a steady 30 a second reads 30.0");
    expect(w.count(t) == 30 || w.count(t) == 31, "about 30 events in the last second");
    expect(w.rate(t + 0.5) < 20, "half a second after they stop, the rate has fallen");
    expect(w.rate(t + 2.0) == 0, "two seconds after, it is 0");
    RateWindow fast;
    t = 0;
    for (int i = 0; i < 2000; ++i) fast.add(t += 1.0 / 240);
    expect(near(fast.rate(t), 240.0, 0.1), "240 a second fit the window");
}

void cadence() {
    {
        SourceCadence c;
        const double t = feed(c, 5, {1.0 / 30}, 4);
        const auto s = c.summary(t);
        expect(near(s.fps, 30, 0.05) && !s.variable && !s.uneven, "30 fps: steady");
    }
    {
        // 45 fps inside a 60 Hz signal: two short steps and a long one.
        SourceCadence c;
        const double t = feed(c, 5, {1.0 / 60, 1.0 / 60, 2.0 / 60}, 4);
        const auto s = c.summary(t);
        expect(near(s.fps, 45, 1.0) && !s.variable && s.uneven, "45 fps in 60 Hz: steady rate, uneven steps");
    }
    {
        // A VRR game on a 120 Hz grid: a second around 58 fps, then one around 40.
        SourceCadence c;
        double t = feed(c, 5, {2.0 / 120, 2.0 / 120, 2.0 / 120, 2.0 / 120, 3.0 / 120}, 1.5);
        t = feed(c, t, {3.0 / 120}, 1.5);
        const auto s = c.summary(t);
        expect(s.variable && s.low < 42 && s.high > 52, "40-55 fps on a 120 Hz grid: variable");
        FramegenStatus st;
        st.enabled = true;
        st.source_fps = s.fps, st.source_low = s.low, st.source_high = s.high, st.source_variable = true;
        st.real_fps = 40, st.generated_fps = 40;
        expect(has(glasir::overlay::rates_text(st), "40-5"), "the menu gives the range");
    }
    {
        // 120 fps with two milliseconds of arrival jitter is not "uneven".
        SourceCadence c;
        const double t = feed(c, 5, {0.0073, 0.0093, 0.0083, 0.0084}, 4);
        const auto s = c.summary(t);
        expect(near(s.fps, 120, 1.0) && !s.uneven && !s.variable, "120 fps with jitter: steady");
    }
    {
        // A pause is left out of the rate.
        SourceCadence c;
        double t = feed(c, 5, {1.0 / 60}, 1);
        t = feed(c, t + 0.8, {1.0 / 60}, 0.6);
        const auto s = c.summary(t);
        expect(near(s.fps, 60, 0.5), "a pause does not lower the rate");
    }
}

void multiplier() {
    const Limit screen120{120, false}, screen240{240, false}, screen144{144, false};
    {
        MultiplierGate g;
        expect(g.choose(0, 30, 2, screen120, Vrr::On, 120) == 2, "30 fps, up to x2, 120 Hz: x2");
        g.reset();
        expect(g.choose(0, 30, 3, screen120, Vrr::On, 120) == 3, "30 fps, up to x3, 120 Hz: x3 (90)");
        g.reset();
        expect(g.choose(0, 30, 4, screen120, Vrr::On, 120) == 4, "30 fps, up to x4, 120 Hz: x4 (120)");
        g.reset();
        expect(g.choose(0, 60, 4, screen120, Vrr::On, 120) == 2, "60 fps, up to x4, 120 Hz: x2");
        g.reset();
        expect(g.choose(0, 60, 2, Limit{119.88, false}, Vrr::On, 119.88) == 2, "60.0 fps on a 119.88 Hz screen: x2");
        g.reset();
        expect(g.choose(0, 120, 4, screen120, Vrr::On, 120) == 1, "120 fps on 120 Hz: nothing added");
        g.reset();
        expect(g.choose(0, 60, 4, screen240, Vrr::Unknown, 240) == 4, "60 fps, up to x4, 240 Hz: x4");
    }
    {
        // The user's limit has no tolerance: 100 on a 120 Hz screen keeps 30 fps at x3.
        MultiplierGate g;
        expect(g.choose(0, 30, 4, Limit{100, true}, Vrr::On, 120) == 3, "limit 100: 30 fps at x3, not x4");
        g.reset();
        expect(g.choose(0, 60, 2, Limit{116, true}, Vrr::On, 120) == 1, "limit 116: 60 fps is not doubled");
    }
    {
        // Fixed refresh: whole refreshes are preferred to more frames.
        MultiplierGate g;
        expect(g.choose(0, 60, 3, screen240, Vrr::Off, 240) == 2, "fixed 240 Hz, 60 fps, up to x3: x2 (x3 is uneven)");
        g.reset();
        expect(g.choose(0, 60, 4, screen240, Vrr::Off, 240) == 4, "fixed 240 Hz, 60 fps, up to x4: x4");
        g.reset();
        expect(g.choose(0, 60, 2, screen144, Vrr::Off, 144) == 2, "fixed 144 Hz, 60 fps: x2 though uneven");
        g.reset();
        expect(g.choose(0, 30, 4, screen120, Vrr::Off, 120) == 4, "fixed 120 Hz, 30 fps, up to x4: x4");
        g.reset();
        expect(g.choose(0, 30, 3, screen120, Vrr::Off, 120) == 2, "fixed 120 Hz, 30 fps, up to x3: x2 (90 is uneven)");
    }
    {
        // A rate wandering across the edge does not switch every moment.
        MultiplierGate g;
        const Limit screen165{165, false};
        expect(g.choose(0.0, 80, 2, screen165, Vrr::On, 165) == 2, "80 fps on 165 Hz: x2");
        expect(g.choose(0.1, 88, 2, screen165, Vrr::On, 165) == 2, "88 fps: still x2 (room for one already running)");
        expect(g.choose(0.2, 92, 2, screen165, Vrr::On, 165) == 1, "92 fps: too fast");
        expect(g.choose(0.3, 80, 2, screen165, Vrr::On, 165) == 1, "80 fps a moment later: stays down");
        expect(g.choose(2.5, 80, 2, screen165, Vrr::On, 165) == 2, "80 fps two seconds later: x2 again");
    }
}

void spacing() {
    expect(near(squeezed_step(0.008, 1.0, 1.1, 3), 0.008, 1e-9), "room for every step: the step as it is");
    expect(near(squeezed_step(0.008, 1.0, 1.012, 3), 0.004, 1e-9), "the next frame due sooner: even, smaller steps");
    expect(squeezed_step(0.008, 1.0, 0.99, 3) == 0, "already due: at once");
}

void verdicts() {
    expect(judge_cadence(60, 120, Vrr::On).cadence == Cadence::VrrEven, "30 -> 60 on 120 Hz VRR: even");
    expect(judge_cadence(90, 120, Vrr::On).cadence == Cadence::VrrEven, "30 -> 90 on 120 Hz VRR: even");
    expect(judge_cadence(120, 120, Vrr::On).cadence == Cadence::VrrCeiling, "60 -> 120 on 120 Hz VRR: at the top");
    expect(judge_cadence(119.88, 120, Vrr::On).cadence == Cadence::VrrCeiling, "59.94 -> 119.88: at the top too");
    expect(judge_cadence(120, 240, Vrr::Off).cadence == Cadence::FixedEven, "60 -> 120 on fixed 240 Hz: even");
    const Verdict v = judge_cadence(120, 144, Vrr::Off);
    expect(v.cadence == Cadence::FixedUneven && v.better_hz == 120, "60 -> 120 on fixed 144 Hz: uneven, try 120 Hz");
    expect(judge_cadence(120, 165, Vrr::Unknown).cadence == Cadence::MaybeUneven, "165 Hz, VRR unknown: maybe uneven");
    expect(judge_cadence(60, 240, Vrr::Unknown).cadence == Cadence::MaybeEven, "30 -> 60 on 240 Hz, unknown: even anyway");

    expect(judge_refresh(120.0, 120, 60) == Seen::Fixed, "60 fps shown, screen at 120: VRR is not engaging");
    expect(judge_refresh(60.1, 120, 60) == Seen::Following, "60 fps shown, screen at 60: VRR follows");
    expect(judge_refresh(120.0, 120, 119) == Seen::Unknown, "at the top of the range nothing can be told");
    expect(judge_refresh(0, 120, 60) == Seen::Unknown, "no reading: unknown");
    expect(judge_refresh(120.0, 120, 30) == Seen::Unknown, "30 fps shown: a VRR screen may repeat frames, unknown");
}

void words() {
    using namespace glasir::overlay;
    FramegenStatus s;
    s.enabled = true;
    s.source_fps = 30, s.real_fps = 30, s.generated_fps = 30;
    s.display_hz = 120, s.vrr = FramegenStatus::Vrr::On, s.multiplier = 2;
    s.cadence = FramegenStatus::Cadence::VrrEven;
    expect(rates_text(s) == "30 " GLASIR_ARROW " 60 fps", "30 -> 60 fps");
    expect(screen_text(s) == "120 Hz VRR", "120 Hz VRR");
    expect(reason_text(s).text.empty() && cadence_text(s).text.empty(), "all well: nothing more is said");

    s.seen = FramegenStatus::Seen::Fixed, s.seen_hz = 120;
    expect(has(cadence_text(s).text, "not engaging") && cadence_text(s).matters, "VRR on but the screen stays at 120 Hz");
    s.seen = FramegenStatus::Seen::Unknown;

    s.enabled = false, s.real_fps = 60, s.generated_fps = 0;
    expect(rates_text(s) == "30 fps", "off: the game's rate, not the repeats");
    s.enabled = true;

    s.source_fps = 60, s.real_fps = 60, s.generated_fps = 44, s.reason = FramegenStatus::Reason::QueueFull;
    s.reason_count = 16;
    expect(rates_text(s) == "60 " GLASIR_ARROW " 104 fps", "short of double: the rate really shown");
    expect(has(reason_text(s).text, "104 of 120 fps"), "and why: 104 of 120");

    s.reason = FramegenStatus::Reason::TooFast, s.multiplier = 1, s.source_fps = 120, s.generated_fps = 0;
    expect(has(reason_text(s).text, "120 Hz screen"), "too fast for the screen");
    s.reason = FramegenStatus::Reason::FewerFit, s.multiplier = 2, s.wanted = 4, s.source_fps = 60;
    expect(has(reason_text(s).text, "x2, not x4"), "x2, not x4");
    s.reason = FramegenStatus::Reason::BackingOff, s.seconds_left = 4.2;
    expect(has(reason_text(s).text, "back in 5 s") && reason_text(s).matters, "paused, back in 5 s");

    s.reason = FramegenStatus::Reason::None, s.multiplier = 2;
    s.cadence = FramegenStatus::Cadence::VrrCeiling;
    expect(has(cadence_text(s).text, "120 fps fills this 120 Hz screen"), "60 -> 120 on 120 Hz: no room for VRR");
    s.display_hz = 144, s.vrr = FramegenStatus::Vrr::Off, s.cadence = FramegenStatus::Cadence::FixedUneven;
    s.better_hz = 120;
    expect(has(cadence_text(s).text, "Set the screen to 120 Hz") && screen_text(s) == "144 Hz fixed",
           "fixed 144 Hz: set it to 120 Hz");
    s.vrr = FramegenStatus::Vrr::Unknown, s.cadence = FramegenStatus::Cadence::MaybeUneven;
    expect(has(cadence_text(s).text, "even only with VRR") && screen_text(s) == "144 Hz", "VRR unknown: says when it is even");
    s.seen = FramegenStatus::Seen::Following;
    expect(cadence_text(s).text.empty(), "unknown, but the screen follows the frames: nothing to say");
}

}  // namespace

int main() {
    rates();
    cadence();
    multiplier();
    spacing();
    verdicts();
    words();
    std::printf("%d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
