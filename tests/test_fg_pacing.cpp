// SPDX-License-Identifier: MIT
// Checks frame generation's pacing arithmetic (src/framegen/fg_pacing.h) and
// the menu's words for it (src/overlay/framegen_text.h): the rates shown,
// how many frames fit which screen, when a multiplier that fits is stepped
// down from and tried again, how the frames are spaced on a fixed-refresh
// screen, and what is said when the screen makes the result uneven.
#include <cmath>
#include <cstdio>
#include <string>

#include "app_version.h"
#include "fg_pacing.h"
#include "framegen_text.h"
#include "keep_regions.h"

using namespace fg::pacing;
using app::FramegenStatus;

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
        expect(has(app::overlay::rates_text(st), "40-5"), "the menu gives the range");
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

// What a run of game frames did to a StepDown.
struct Steps {
    int downs = 0, tries = 0;
    StepDown::Change down, tried;  // the latest of each
    double down_at = -1, tried_at = -1;
    void note(const StepDown::Change &c, double t) {
        if (c.kind == StepDown::Change::Kind::Down) ++downs, down = c, down_at = t;
        if (c.kind == StepDown::Change::Kind::Try) ++tries, tried = c, tried_at = t;
    }
};

// `seconds` of a 30 fps game from t on, as the add-on feeds it: up to
// `wanted` frames for each game frame, and with `cannot` or more of them
// every other game frame is left out (what a composed window did at x4).
Steps play(StepDown &s, double &t, double seconds, int wanted, int cannot) {
    Steps r;
    for (int i = 0, n = int(seconds * 30 + 0.5); i < n; ++i) {
        t += 1.0 / 30;
        r.note(s.due(t, wanted), t);
        const int count = s.most(wanted);
        r.note(s.frame(t, count, count >= cannot && i % 2 == 0), t);
    }
    return r;
}

// A multiplier's first kSettle seconds are not judged: `count` held for that
// long, so that what follows is.
void settle(StepDown &s, double &t, int count) {
    for (int i = 0, n = int(StepDown::kSettle * 30) + 2; i < n; ++i) {
        t += 1.0 / 30;
        s.frame(t, count, false);
    }
}

// A multiplier that fits the screen and cannot be held: one fewer, and when
// the one above is tried again.
void step_down() {
    using Kind = StepDown::Change::Kind;
    {
        RateWindow w;
        w.add(1.0), w.add(1.2), w.add(1.9);
        expect(near(w.span(2.0, 2.0), 0.9, 1e-9) && w.span(3.5, 2.0) == 0, "the time the events of a window span");
        expect(fitting(30, 4, Limit{120, false}, Vrr::On, 120, 1) == 4 && fitting(30, 3, Limit{120, false}, Vrr::Off, 120, 4) == 2 &&
                   fitting(60, 4, Limit{120, false}, Vrr::On, 120, 1) == 2 && fitting(0, 4, Limit{120, false}, Vrr::On, 120, 1) == 1,
               "what fits, asked without the gate's memory");
    }
    {
        // Holds: nothing happens, however long.
        StepDown s;
        double t = 100;
        const Steps r = play(s, t, 300, 4, 5);
        expect(r.downs == 0 && r.tries == 0 && !s.stepped() && s.most(4) == 4, "x4 that holds for five minutes stays x4");
    }
    {
        // One game frame left out now and then: a screen at the top of its
        // VRR range does that; it is not stepped down for.
        StepDown s;
        double t = 100;
        int downs = 0;
        for (int i = 0; i < 30 * 120; ++i) {
            t += 1.0 / 30;
            if (s.frame(t, 4, i % 30 == 0)) ++downs;
        }
        expect(downs == 0, "one game frame a second left out at x4: not enough");
        // A hiccup: six in a row, then all well.
        for (int i = 0; i < 30 * 10; ++i) {
            t += 1.0 / 30;
            if (s.frame(t, 4, i >= 100 && i < 106)) ++downs;
        }
        expect(downs == 0 && !s.stepped(), "six in a row and then none (a hiccup): not stepped down for");
    }
    {
        // The measured case: 30 fps x4 in a composed window on 120 Hz, every
        // other game frame without its frames.
        StepDown s;
        double t = 100;
        settle(s, t, 4);
        const double start = t;
        Steps r = play(s, t, 2, 4, 4);
        expect(r.downs == 1 && r.down.from == 4 && r.down.to == 3 && r.down.cause == StepCause::Screen && r.down.wait == 20 &&
                   r.down.left_out >= StepDown::kLeftOut && r.down_at - start > 0.5 && r.down_at - start < 0.75,
               "every other game frame left out at x4: x3 after half a second, for 20 s, the screen's doing");
        expect(s.stepped() && s.most(4) == 3 && s.most(2) == 2 && s.cause() == StepCause::Screen &&
                   near(s.seconds_left(t), 20 - (t - r.down_at), 1e-6),
               "x3 is the most for now; a setting below that is not touched");
        const double down_at = r.down_at;
        r = play(s, t, 17.5, 4, 4);
        expect(r.downs == 0 && r.tries == 0 && s.most(4) == 3, "x3 holds and is kept: no change for the 20 s");
        r = play(s, t, 1.5, 4, 4);
        expect(r.tries == 1 && r.tried.from == 3 && r.tried.to == 4 && near(r.tried_at - down_at, 20, 0.04),
               "after 20 s x4 is tried again");
        expect(r.downs == 1 && r.down.to == 3 && r.down.wait == 40 && r.down.left_out == StepDown::kTrialLeftOut &&
                   r.down_at - r.tried_at < 0.15,
               "it still cannot be held: back to x3 after two game frames left out, now for 40 s");
    }
    {
        // The waits one by one: twice as long each time, five minutes at most.
        StepDown s;
        double t = 0;
        settle(s, t, 4);
        Steps r = play(s, t, 1, 4, 4);
        bool grows = r.downs == 1 && r.down.wait == 20;
        for (const double wait : {40.0, 80.0, 160.0, 300.0, 300.0}) {
            const double down_at = r.down_at;
            r = play(s, t, down_at + r.down.wait + 1 - t, 4, 4);
            grows = grows && r.tries == 1 && r.downs == 1 && r.down.wait == wait;
        }
        expect(grows, "each failed try doubles the wait: 20, 40, 80, 160, 300, 300 s");
        // Then x4 holds for more than a minute and fails again: a new matter.
        const double down_at = r.down_at;
        r = play(s, t, down_at + 300 - t - 0.5, 4, 4);
        r = play(s, t, 70, 4, 5);
        expect(r.tries == 1 && r.downs == 0 && !s.stepped(), "a try that holds: x4 again");
        r = play(s, t, 2, 4, 4);
        expect(r.downs == 1 && r.down.wait == 20 && r.down.left_out >= StepDown::kLeftOut,
               "failing after it held a minute is not a failed try: 20 s again, and four left out are needed");
    }
    {
        // Neither x4 nor x3 can be held: x2, where it stays whatever is left out.
        StepDown s;
        double t = 50;
        settle(s, t, 4);
        Steps r = play(s, t, 8, 4, 3);  // x3 has its own seconds to settle before it is judged
        expect(r.downs == 2 && s.most(4) == 2 && r.down.from == 3 && r.down.to == 2, "x4 and x3 cannot be held: x2");
        const double down_at = r.down_at;
        r = play(s, t, 30, 4, 2);
        expect(r.tries == 1 && r.tried.to == 3 && near(r.tried_at - down_at, 20, 0.04) && r.downs == 1 && s.most(4) == 2,
               "x3 is tried first, and fails; x2 itself is never stepped down from");
        // Now x3 holds and x4 does not: x3 at the next try, x4 tried 20 s after that.
        r = play(s, t, r.down_at + r.down.wait + 1 - t, 4, 4);
        expect(r.tries == 1 && r.downs == 0 && s.most(4) == 3, "x3 holds at the next try");
        const double tried_at = r.tried_at;
        r = play(s, t, 21, 4, 4);
        expect(r.tries == 1 && r.tried.to == 4 && near(r.tried_at - tried_at, 20, 0.04) && r.downs == 1 && s.most(4) == 3,
               "x4 is tried 20 s after x3 held, and x3 it is again");
    }
    {
        StepDown s;
        double t = 10;
        play(s, t, 1, 2, 2);
        Steps r = play(s, t, 20, 2, 2);
        expect(r.downs == 0 && !s.stepped(), "x2 with frames left out: the add-on's pause is for that, not this");
        r = play(s, t, 8, 3, 3);
        expect(r.downs == 1 && r.down.from == 3 && r.down.to == 2, "x3 that cannot be held: x2");
        expect(!s.due(t + 100, 2) && s.most(2) == 2, "with the setting at x2 there is nothing above to try");
    }
    {
        // Something changed: tried at once, but the lower step stays 5 s at least.
        StepDown s;
        double t = 10;
        settle(s, t, 4);
        Steps r = play(s, t, 1, 4, 4);
        const double down_at = r.down_at;
        s.retry(down_at + 1);
        expect(!s.due(down_at + 1.1, 4) && !s.due(down_at + 4.9, 4), "a change a second after the step down: not yet");
        expect(s.due(down_at + 5.01, 4).kind == Kind::Try, "5 s after the step down it is tried");
        t = down_at + 5.01;
        r = play(s, t, 1, 4, 4);
        expect(r.downs == 1 && r.down.wait == 40, "and when that fails, the wait is the next longer one");
        s.retry(r.down_at + 8);
        expect(s.due(r.down_at + 8, 4).kind == Kind::Try, "a change later than 5 s after: tried at once");
        t = r.down_at + 8;
        play(s, t, 1, 4, 4);
        s.again();
        expect(!s.stepped() && s.most(4) == 4 && s.seconds_left(t) == 0, "a setting changed: nothing is held back");
        r = play(s, t, 8, 4, 4);
        expect(r.downs == 1 && r.down.wait == 20, "and nothing is remembered: 20 s again");
    }
    {
        // A multiplier just begun: what it loses in its first three seconds
        // is not held against it (frame generation starting, a setting changed).
        StepDown s;
        double t = 20;
        Steps r = play(s, t, StepDown::kSettle - 0.5, 4, 4);
        expect(r.downs == 0 && !s.stepped(), "every other game frame left out while x4 settles: not judged yet");
        r = play(s, t, 30, 4, 5);
        expect(r.downs == 0 && !s.stepped(), "and it holds after that: x4 stays");
        s.again();
        r = play(s, t, StepDown::kSettle - 0.5, 4, 4);
        expect(r.downs == 0, "the same after a setting changed");
        r = play(s, t, 2, 4, 4);
        expect(r.downs == 1 && r.down.to == 3, "left out past the three seconds: stepped down");
    }
    {
        // mpv dropped frames while generating: one fewer before any pause, the GPU's doing.
        StepDown s;
        const StepDown::Change c = s.behind(5, 4);
        expect(c.kind == Kind::Down && c.from == 4 && c.to == 3 && c.cause == StepCause::Gpu && c.left_out == 0 && c.wait == 20 &&
                   s.cause() == StepCause::Gpu,
               "mpv fell behind at x4: x3, because of the GPU");
        expect(s.behind(6, 3).to == 2 && !s.behind(7, 2) && s.most(4) == 2, "at x3: x2; at x2 nothing (the pause it is)");
    }
    {
        // Frames left out while the first added frame is done late: the GPU, not the screen.
        StepDown s;
        double t = 0;
        for (int i = 0, n = int(StepDown::kSettle * 30) + 2; i < n; ++i) {  // its first seconds, squeezed as well
            t += 1.0 / 30;
            s.squeezed(t, 1);
            s.frame(t, 4, false);
        }
        StepDown::Change down;
        for (int i = 0; i < 60 && !down; ++i) {
            t += 1.0 / 30;
            if (i % 2) s.squeezed(t, 1);
            down = s.frame(t, 4, i % 2 == 0);
        }
        expect(down.kind == Kind::Down && down.cause == StepCause::Gpu, "left out with the frames squeezed: the GPU's doing");
    }
    {
        // The source's rate.
        StepDown s;
        double t = 0;
        settle(s, t, 4);
        const Steps r = play(s, t, 1, 4, 4);
        expect(!s.source(t, 30, true) && !s.source(t + 1, 31, true) && !s.source(t + 2, 48, false),
               "the same rate, or one that moves by itself: no reason to try");
        expect(s.source(r.down_at + 6, 60, true) && s.due(r.down_at + 6, 4).kind == Kind::Try, "another steady rate: tried at once");
        StepDown clean;
        expect(!clean.source(1, 30, true) && !clean.source(2, 60, true), "not stepped down: the rate is no matter");
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
    using namespace app::overlay;
    FramegenStatus s;
    s.enabled = true;
    s.source_fps = 30, s.real_fps = 30, s.generated_fps = 30;
    s.display_hz = 120, s.vrr = FramegenStatus::Vrr::On, s.multiplier = 2;
    s.cadence = FramegenStatus::Cadence::VrrEven;
    expect(rates_text(s) == "30 " APP_ARROW " 60 fps", "30 -> 60 fps");
    expect(rates_and_latency_text(s) == "30 " APP_ARROW " 60 fps", "no delay known: the rates alone");
    s.latency_ms = 23.44;
    expect(rates_and_latency_text(s) == "30 " APP_ARROW " 60 fps, 23.4 ms", "the delay from the card's frame to its present");
    s.arrival_spread_ms = 0.3;
    expect(rates_and_latency_text(s) == "30 " APP_ARROW " 60 fps, 23.4 ms \xC2\xB1" "0.1", "and half the spread of the card's frame times");
    s.latency_ms = 0, s.arrival_spread_ms = -1;
    expect(screen_text(s) == "120 Hz VRR", "120 Hz VRR");
    expect(reason_text(s).text.empty() && cadence_text(s).text.empty(), "all well: nothing more is said");

    s.seen = FramegenStatus::Seen::Fixed, s.seen_hz = 120;
    expect(has(cadence_text(s).title, "not engaging") && has(cadence_text(s).text, "keeps refreshing at 120 Hz") &&
               cadence_text(s).matters,
           "VRR on but the screen stays at 120 Hz: a warning, what happens first");
    s.seen = FramegenStatus::Seen::Unknown;

    s.enabled = false, s.real_fps = 60, s.generated_fps = 0;
    expect(rates_text(s) == "30 fps", "off: the game's rate, not the repeats");
    s.enabled = true;

    s.source_fps = 60, s.real_fps = 60, s.generated_fps = 44, s.reason = FramegenStatus::Reason::QueueFull;
    s.reason_count = 16;
    expect(rates_text(s) == "60 " APP_ARROW " 104 fps", "short of double: the rate really shown");
    expect(reason_text(s).title == "104 of 120 fps" && has(reason_text(s).text, "takes frames late") &&
               reason_text(s).matters && reason_text(s).fix == Line::Fix::FullScreen,
           "and why: 104 of 120, the screen takes frames late, full screen may help");
    expect(!has(reason_text(s).text, "max_output_hz") && !has(reason_text(s).text, ".ini"), "no key or file is named");

    s.reason = FramegenStatus::Reason::TooFast, s.multiplier = 1, s.source_fps = 120, s.generated_fps = 0;
    expect(has(reason_text(s).text, "120 Hz screen"), "too fast for the screen");
    s.reason = FramegenStatus::Reason::FewerFit, s.multiplier = 2, s.wanted = 4, s.source_fps = 60;
    expect(has(reason_text(s).text, "x2, not x4"), "x2, not x4");
    s.reason = FramegenStatus::Reason::BackingOff, s.seconds_left = 4.2;
    expect(has(reason_text(s).title, "back in 5 s") && reason_text(s).matters &&
               reason_text(s).fix == Line::Fix::FastQuality,
           "paused, back in 5 s: a warning, Fast quality may help");
    s.reason = FramegenStatus::Reason::FewerFit, s.cap_hz = 100;
    expect(has(reason_text(s).text, "over the limit of 100 fps"), "a limit is called the limit, with its figure");
    s.cap_hz = 0;

    // Stepped down: what is in use and what is not, why, and what the menu's button can do.
    s.reason = FramegenStatus::Reason::SteppedDown, s.source_fps = 30, s.wanted = 4, s.multiplier = 3, s.step_from = 4;
    s.step_cause = FramegenStatus::StepCause::Screen;
    expect(reason_text(s).title == "x3, not x4" &&
               reason_text(s).text == "The screen takes frames late at x4, so every game frame gets two added frames instead "
                                      "of three. Windows may be composing the window." &&
               reason_text(s).matters && reason_text(s).fix == Line::Fix::FullScreen,
           "x3, not x4: the screen takes frames late, full screen may help");
    s.step_cause = FramegenStatus::StepCause::Gpu;
    expect(reason_text(s).title == "x3, not x4" && has(reason_text(s).text, "The GPU cannot keep up at x4, so every game frame") &&
               has(reason_text(s).text, "Fast quality or DLSS 5 off") && reason_text(s).fix == Line::Fix::FastQuality,
           "x3, not x4: the GPU cannot keep up, Fast quality may help");
    s.multiplier = 2, s.step_failed = 3, s.step_cause = FramegenStatus::StepCause::Screen;
    expect(reason_text(s).title == "x2, not x4" && has(reason_text(s).text, "late at x3 and x4") &&
               has(reason_text(s).text, "gets one added frame instead of three"),
           "two steps down: x2, not x4, and both that failed are named");
    s.step_failed = 4;
    expect(reason_text(s).title == "x2, not x4" && has(reason_text(s).text, "late at x4,") &&
               has(reason_text(s).text, "gets one added frame instead of three"),
           "x2 where x3 would be uneven on a fixed screen: only x4 failed, only x4 is named");
    s.step_from = 3, s.step_failed = 3;
    expect(reason_text(s).title == "x2, not x3" && has(reason_text(s).text, "late at x3,") &&
               has(reason_text(s).text, "one added frame instead of two"),
           "x2, not x3");
    expect(!has(reason_text(s).text, "StepDown") && !has(reason_text(s).text, ".ini"), "no key or file is named");
    s.step_from = s.step_failed = 0, s.step_cause = FramegenStatus::StepCause::None, s.source_fps = 60;

    s.reason = FramegenStatus::Reason::None, s.multiplier = 2;
    s.cadence = FramegenStatus::Cadence::VrrCeiling;
    expect(has(cadence_text(s).text, "120 fps fills this 120 Hz screen"), "60 -> 120 on 120 Hz: no room for VRR");
    s.display_hz = 144, s.vrr = FramegenStatus::Vrr::Off, s.cadence = FramegenStatus::Cadence::FixedUneven;
    s.better_hz = 120;
    expect(has(cadence_text(s).text, "Set the screen to 120 Hz") && screen_text(s) == "144 Hz fixed",
           "fixed 144 Hz: set it to 120 Hz");
    s.vrr = FramegenStatus::Vrr::Unknown, s.cadence = FramegenStatus::Cadence::MaybeUneven;
    expect(has(cadence_text(s).text, "even only with variable refresh on") && screen_text(s) == "144 Hz",
           "VRR unknown: says when it is even");
    s.seen = FramegenStatus::Seen::Following;
    expect(cadence_text(s).text.empty(), "unknown, but the screen follows the frames: nothing to say");
}

// The areas frame generation leaves alone: their text and their pixels.
void keep() {
    using namespace app;
    std::vector<KeepRegion> r;
    expect(parse_keep_regions("", r) && r.empty(), "keep: no text, no areas");
    expect(parse_keep_regions("r:2,80,20,15; e:85,5,12,20", r) && r.size() == 2 && !r[0].ellipse && r[1].ellipse &&
               near(r[0].y, 80, 0.01) && near(r[1].w, 12, 0.01),
           "keep: a rectangle and an ellipse");
    expect(format_keep_regions(r) == "r:2,80,20,15;e:85,5,12,20", "keep: written back as it was read");
    expect(reshade_keep_text(r) == "r:2/80/20/15|e:85/5/12/20" && parse_keep_regions(reshade_keep_text(r), r) &&
               r.size() == 2 && r[1].ellipse && near(r[1].h, 20, 0.01),
           "keep: ReShade.ini's form has no commas and reads the same");
    expect(parse_keep_regions("r:90,90,50,50", r) && near(r[0].w, 10, 0.01) && near(r[0].h, 10, 0.01),
           "keep: an area over the edge ends at the edge");
    expect(!parse_keep_regions("x:1,2,3,4", r) && !parse_keep_regions("r:1,2,3", r) && !parse_keep_regions("r:1,2,3,0", r) &&
               !parse_keep_regions("r:1,2,3,4,5", r) && !parse_keep_regions("hud", r),
           "keep: other text is refused");
    std::string many;
    for (int i = 0; i < 9; ++i) many += "r:1,1,5,5;";
    expect(!parse_keep_regions(many, r), "keep: more than eight are refused");

    // An area that only says where to look for a HUD: ",hud" after its numbers.
    expect(parse_keep_regions("r:2,80,20,15", r) && r.size() == 1 && !r[0].find_hud,
           "keep: an area as it was written before is kept whole");
    expect(parse_keep_regions("r:2,80,20,15,hud; e:85,5,12,20 ;r:40,0,20,10 , HUD", r) && r.size() == 3 && r[0].find_hud &&
               !r[0].ellipse && !r[1].find_hud && r[1].ellipse && r[2].find_hud && near(r[2].w, 20, 0.01),
           "keep: ,hud marks an area that looks for the HUD; the others are kept whole");
    expect(format_keep_regions(r) == "r:2,80,20,15,hud;e:85,5,12,20;r:40,0,20,10,hud", "keep: ,hud is written back");
    const std::string plain_form = reshade_keep_text(r);
    expect(plain_form == "r:2/80/20/15/hud|e:85/5/12/20|r:40/0/20/10/hud" && parse_keep_regions(plain_form, r) &&
               r.size() == 3 && r[0].find_hud && !r[1].find_hud && r[2].find_hud,
           "keep: ReShade.ini's form carries it too");
    expect(parse_keep_regions("e:10/10/30/30/hud", r) && r[0].ellipse && r[0].find_hud,
           "keep: an ellipse may look for the HUD as well");
    expect(!parse_keep_regions("r:1,2,3,4,hu", r) && !parse_keep_regions("r:1,2,3,4,hud,hud", r) &&
               !parse_keep_regions("r:1,2,3,4,hudx", r) && !parse_keep_regions("r:1,2,3,hud", r) &&
               !parse_keep_regions("r:1,2,3,4,", r),
           "keep: nothing else may follow the numbers");
    KeepRegion whole{false, 25, 50, 50, 25};
    expect(!whole.find_hud, "keep: an area made from its numbers alone is kept whole");
    PixelBox around;
    expect(region_box({true, 25, 50, 50, 25}, 1920, 1080, around) && around.left == 480 && around.top == 540 &&
               around.right == 1440 && around.bottom == 810 && !region_box({false, 10, 10, 0.01f, 0.01f}, 1920, 1080, around),
           "keep: the box around an area, and none for one smaller than a pixel");

    std::vector<PixelBox> boxes;
    region_boxes({false, 25, 50, 50, 25}, 1920, 1080, boxes);
    expect(boxes.size() == 1 && boxes[0].left == 480 && boxes[0].top == 540 && boxes[0].right == 1440 &&
               boxes[0].bottom == 810,
           "keep: a rectangle is one box of its pixels");
    boxes.clear();
    region_boxes({true, 0, 0, 100, 100}, 800, 800, boxes);
    bool inside = !boxes.empty();
    unsigned widest = 0, narrowest = 800;
    for (const PixelBox &b : boxes) {
        inside = inside && b.right <= 800 && b.bottom <= 800 && b.left < b.right && b.top < b.bottom;
        widest = std::max(widest, b.right - b.left);
        narrowest = std::min(narrowest, b.right - b.left);
    }
    expect(inside && boxes.size() == 100 && widest >= 798 && narrowest < 200,
           "keep: an ellipse is rows, widest in the middle");
    boxes.clear();
    region_boxes({false, 10, 10, 0.01f, 0.01f}, 100, 100, boxes);
    expect(boxes.empty(), "keep: an area smaller than a pixel is nothing");
}

// The menu's top line, and what the exe told the add-ons about the screen.
void told() {
    using app::overlay::picture_text;
    using app::overlay::screen_line;
    using app::overlay::vrr_from_word;
    using app::overlay::vrr_line;
    using Vrr = app::FramegenStatus::Vrr;
    expect(std::string(picture_text(false, false)) == "SDR" && std::string(picture_text(true, true)) == "HDR" &&
               std::string(picture_text(true, false)) == "HDR to SDR",
           "the picture: SDR, HDR, or an HDR capture tone mapped to SDR");
    // The screen the window is on, written as the launcher writes a screen.
    expect(screen_line("1 (PA279CDV)", 120) == "1 \xC2\xB7 PA279CDV \xC2\xB7 120 Hz", "the screen: number, name, rate");
    expect(screen_line("2 (Odyssey G95SC)", 239.76) == "2 \xC2\xB7 Odyssey G95SC \xC2\xB7 240 Hz", "another, at 239.76 Hz");
    expect(screen_line("1", 60) == "1 \xC2\xB7 60 Hz" && screen_line("\\\\.\\DISPLAY3", 0) == "\\\\.\\DISPLAY3",
           "a screen without a name, and one only Windows names");
    expect(screen_line("", 0).empty() && screen_line("", 144) == "144 Hz", "nothing told: nothing said");
    expect(vrr_line(Vrr::Off, true, false) ==
               "Variable refresh off, as found. Change it on the Screen tab of " APP_NAME "'s window if that is wrong.",
           "variable refresh as found, and where to change it");
    expect(vrr_line(Vrr::On, true, true) == "Variable refresh on, as set on the Screen tab of " APP_NAME "'s window.",
           "variable refresh as the user set it");
    expect(has(vrr_line(Vrr::Unknown, true, false), "not known") && has(vrr_line(Vrr::On, false, false), "was not there"),
           "not known; a screen plugged in since the start");
    for (const Vrr vrr : {Vrr::On, Vrr::Off, Vrr::Unknown})
        for (const bool told : {true, false})
            expect(!has(vrr_line(vrr, told, true), ".ini") && !has(vrr_line(vrr, told, false), "display.vrr"),
                   "no file or key is named");
    expect(vrr_from_word("on") == Vrr::On && vrr_from_word("off") == Vrr::Off && vrr_from_word("unknown") == Vrr::Unknown &&
               vrr_from_word("") == Vrr::Unknown,
           "the exe's word for it");
    // The limit both the add-on and the exe's advice go by.
    using fg::pacing::output_limit;
    expect(output_limit(120, 0).hz == 120 && !output_limit(120, 0).exact, "no limit set: the screen's rate");
    expect(output_limit(120, 100).hz == 100 && output_limit(120, 100).exact, "a limit below it: kept exactly");
    expect(output_limit(120, 240).hz == 120 && !output_limit(120, 240).exact, "a limit above it: no limit");
    expect(output_limit(0, 0).hz == 60, "an unknown screen: 60");
}

}  // namespace

int main() {
    rates();
    cadence();
    multiplier();
    step_down();
    spacing();
    verdicts();
    words();
    told();
    keep();
    std::printf("%d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
