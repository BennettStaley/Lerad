// SPDX-License-Identifier: MIT
// Checks what frame generation takes "the screen" to be
// (src/framegen/fg_screen.h, and RefreshWatch in fg_pacing.h): the list of
// screens the exe hands over, which of them the window's screen is and
// what is believed about it, and the count of refreshes - which must start
// over when the window is on another screen.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "app_version.h"
#include "fg_pacing.h"
#include "fg_screen.h"

using namespace fg;
using fg::pacing::RefreshWatch;
using fg::pacing::Seen;
using fg::pacing::Vrr;

namespace {

int g_failures = 0;

void expect(bool ok, const char *what) {
    std::printf("%-78s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_failures;
}

bool near(double a, double b, double by) { return std::fabs(a - b) <= by; }

// The owner's two screens, as the exe writes them (tests/test_display.cpp
// checks that side).
const char *const kPa279 = "\\\\?\\DISPLAY#AUS2704#5&1a2b3c&0&UID4352#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";
const char *const kTwo =
    "\\\\.\\DISPLAY2|unknown|239.760||2 (Odyssey G95SC);"
    "\\\\.\\DISPLAY1|on|119.880|\\\\?\\DISPLAY#AUS2704#5&1a2b3c&0&UID4352#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}|1 (PA279CDV)";

void list() {
    const std::vector<ScreenFacts> two = parse_screens(kTwo);
    expect(two.size() == 2, "two screens in the list");
    if (two.size() == 2) {
        expect(two[0].device == "\\\\.\\DISPLAY2" && two[0].vrr == Vrr::Unknown && near(two[0].hz, 239.76, 1e-6) &&
                   two[0].name == "2 (Odyssey G95SC)",
               "the first: device, VRR unknown, 239.76 Hz, its name");
        expect(two[1].device == "\\\\.\\DISPLAY1" && two[1].vrr == Vrr::On && near(two[1].hz, 119.88, 1e-6),
               "the second: VRR on, 119.88 Hz");
        expect(two[0].monitor.empty() && two[1].monitor == kPa279 && two[1].name == "1 (PA279CDV)",
               "the monitor's device path, where there is one");
    }
    expect(screens_text(two) == kTwo, "written again it is the same text");
    expect(find_screen(two, "\\\\.\\display1") && find_screen(two, "\\\\.\\display1")->name == "1 (PA279CDV)",
           "a screen is found by Windows' name, any case");
    expect(find_screen(two, "\\\\.\\DISPLAY3") == nullptr, "a screen that was not there at start is not found");
    expect(find_screen(two, "\\\\.\\DISPLAY") == nullptr && find_screen(two, "\\\\.\\DISPLAY11") == nullptr,
           "DISPLAY1 is neither DISPLAY nor DISPLAY11");

    expect(parse_screens("").empty() && parse_screens(";;").empty(), "nothing told: no screens");
    const std::vector<ScreenFacts> odd =
        parse_screens("\\\\.\\DISPLAY5;|on|60||no device;\\\\.\\DISPLAY6|maybe|fast||a|b");
    expect(odd.size() == 2 && odd[0].device == "\\\\.\\DISPLAY5" && odd[0].vrr == Vrr::Unknown && odd[0].hz == 0 &&
               odd[0].name.empty(),
           "a device alone: VRR and rate unknown");
    expect(odd.size() == 2 && odd[1].vrr == Vrr::Unknown && odd[1].hz == 0 && odd[1].name == "a|b",
           "a word that is no VRR state and a rate that is no number: unknown; the name is the rest");
    std::vector<ScreenFacts> named(1);
    named[0].device = "\\\\.\\DISPLAY1";
    named[0].vrr = Vrr::Off;
    named[0].hz = 60;
    named[0].name = "1 (A|B;C)";
    const std::vector<ScreenFacts> back = parse_screens(screens_text(named));
    expect(back.size() == 1 && back[0].name == "1 (A B C)" && back[0].vrr == Vrr::Off && back[0].hz == 60,
           "a name's separators are taken out, so it cannot break the list");
}

ScreenNow now(const char *device, int whole, double exact) {
    ScreenNow s;
    s.found = true;
    s.device = device;
    s.hz_whole = whole;
    s.hz_exact = exact;
    return s;
}

void beliefs() {
    using Source = ScreenBelief::Source;
    const std::vector<ScreenFacts> two = parse_screens(kTwo);

    // The app plays on screen 1 (120 Hz, VRR on); the PC's main screen is 2.
    ScreenBelief b = believe(now("\\\\.\\DISPLAY1", 120, 119.88), two, true, Vrr::On, 119.88);
    expect(b.source == Source::Told && b.vrr == Vrr::On && near(b.hz, 119.88, 1e-6) && b.name == "1 (PA279CDV)",
           "the window on screen 1: its rate and its VRR state");
    // The window is moved to the main screen: that one's facts, not the
    // chosen screen's (the old env DISPLAY_VRR said "on").
    b = believe(now("\\\\.\\DISPLAY2", 240, 239.76), two, true, Vrr::On, 119.88);
    expect(b.source == Source::Told && b.vrr == Vrr::Unknown && near(b.hz, 239.76, 1e-6) &&
               b.name == "2 (Odyssey G95SC)",
           "moved to screen 2: 239.76 Hz and VRR unknown, not screen 1's \"on\"");
    // The set of screens changed and Windows gave screen 1's name to another
    // monitor: it is not the screen the app saw.
    ScreenNow swapped = now("\\\\.\\DISPLAY1", 120, 119.88);
    swapped.monitor = "\\\\?\\DISPLAY#SAM7346#5&1a2b3c&0&UID4353#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";
    b = believe(swapped, two, true, Vrr::On, 119.88);
    expect(b.source == Source::NotTold && b.vrr == Vrr::Unknown && b.name == "\\\\.\\DISPLAY1",
           "another monitor under screen 1's name: VRR unknown, not screen 1's \"on\"");
    swapped.monitor = "\\\\?\\display#aus2704#5&1a2b3c&0&uid4352#{E6F07B5F-EE97-4A90-B076-33F57BF4EAA7}";
    b = believe(swapped, two, true, Vrr::On, 119.88);
    expect(b.source == Source::Told && b.vrr == Vrr::On, "the same monitor, its path in another case: screen 1");
    swapped.device = "\\\\.\\DISPLAY2";
    b = believe(swapped, two, true, Vrr::On, 119.88);
    expect(b.source == Source::Told && b.vrr == Vrr::Unknown, "no path told for screen 2: the output's name decides");
    // A screen switched on after start.
    b = believe(now("\\\\.\\DISPLAY3", 60, 59.94), two, true, Vrr::On, 119.88);
    expect(b.source == Source::NotTold && b.vrr == Vrr::Unknown && near(b.hz, 59.94, 1e-6) && b.name == "\\\\.\\DISPLAY3",
           "a screen " APP_NAME " did not see: VRR unknown, the rate Windows gives, Windows' name");
    // The same screen in another mode: Windows' rate, never the one at start.
    b = believe(now("\\\\.\\DISPLAY1", 60, 60.0), two, true, Vrr::On, 119.88);
    expect(near(b.hz, 60.0, 1e-6) && b.vrr == Vrr::On, "screen 1 set to 60 Hz: 60, not the 119.88 of the start");
    b = believe(now("\\\\.\\DISPLAY1", 60, 0), two, true, Vrr::On, 119.88);
    expect(near(b.hz, 60.0, 1e-6), "... also when only the whole number is known");
    // No exact rate from Windows: the fraction told at start, while it agrees.
    b = believe(now("\\\\.\\DISPLAY1", 120, 0), two, true, Vrr::On, 119.88);
    expect(near(b.hz, 119.88, 1e-6), "no fraction from Windows: the one told at start, as it agrees with 120");
    b = believe(now("\\\\.\\DISPLAY1", 0, 0), two, false, Vrr::Unknown, 0);
    expect(b.hz == 0 && b.vrr == Vrr::On, "no rate at all: unknown (the pacing takes 60), VRR as told");

    // Without the list (mpv not started by this exe): the one pair is
    // taken as the window's screen, as it always was.
    b = believe(now("\\\\.\\DISPLAY2", 240, 0), {}, true, Vrr::Off, 239.76);
    expect(b.source == Source::OneOnly && b.vrr == Vrr::Off && near(b.hz, 239.76, 1e-6),
           "no list, the old pair: VRR and the fraction from it");
    b = believe(now("\\\\.\\DISPLAY2", 240, 0), {}, true, Vrr::Off, 119.88);
    expect(near(b.hz, 240, 1e-6), "... but not a rate that is plainly another screen's");
    b = believe(now("\\\\.\\DISPLAY2", 240, 239.76), {}, false, Vrr::Unknown, 0);
    expect(b.source == Source::Nothing && b.vrr == Vrr::Unknown && near(b.hz, 239.76, 1e-6),
           "nothing told at all: VRR unknown, the rate from Windows");
    b = believe(ScreenNow{}, two, true, Vrr::On, 119.88);
    expect(b.source == Source::Nothing && b.vrr == Vrr::Unknown && b.hz == 0 && b.name.empty(),
           "the window's screen not found: nothing is assumed");
}

// One sample a second for `seconds`, of a screen refreshing `hz` times a
// second, while `shown` frames a second are presented. Returns the count.
unsigned run(RefreshWatch &w, unsigned epoch, unsigned count, double &t, double hz, double display_hz, double shown,
             int seconds) {
    for (int i = 0; i < seconds; ++i) {
        t += 1.0;
        count += unsigned(std::lround(hz));
        w.sample(epoch, count, t - 0.001, t, display_hz, shown);
    }
    return count;
}

void refreshes() {
    {
        // A fixed 120 Hz screen showing 60 frames a second.
        RefreshWatch w;
        double t = 100;
        unsigned count = 5000;
        w.sample(1, count, t - 0.001, t, 120, 60);
        expect(w.seen_hz() == 0 && w.seen() == Seen::Unknown, "the first sample says nothing yet");
        count = run(w, 1, count, t, 120, 120, 60, 2);
        expect(near(w.seen_hz(), 120, 0.01) && w.seen() == Seen::Unknown, "120 seen for two seconds: not yet \"fixed\"");
        count = run(w, 1, count, t, 120, 120, 60, 1);
        expect(w.seen() == Seen::Fixed, "three seconds of it: fixed, VRR is not engaging");
        // VRR engages: the refreshes follow the 60 frames.
        count = run(w, 1, count, t, 60, 120, 60, 1);
        expect(near(w.seen_hz(), 60, 0.01) && w.seen() == Seen::Following, "60 seen while 60 are shown: following");
    }
    {
        // The window goes from a fixed 240 Hz screen to a 120 Hz one. The
        // count is the new screen's and starts at nothing: across the change
        // 28800 - 240 counts in a second would read as nonsense, and "fixed"
        // must not be carried over.
        RefreshWatch w;
        double t = 10;
        unsigned count = run(w, 1, 28000, t, 240, 240, 60, 4);
        expect(w.seen() == Seen::Fixed && near(w.seen_hz(), 240, 0.01), "on the 240 Hz screen: fixed at 240");
        w.reset();  // the add-on, when it finds the window on another screen
        expect(w.seen() == Seen::Unknown && w.seen_hz() == 0, "the window moved: nothing is known");
        count = 0;
        t += 1.0;
        w.sample(2, count, 0, t, 120, 60);  // the new count has not begun
        expect(w.seen() == Seen::Unknown && w.seen_hz() == 0, "the new screen, nothing counted yet: unknown");
        count = run(w, 2, count, t, 120, 120, 60, 1);
        expect(w.seen_hz() == 0, "its first counted second is the base, not a reading");
        count = run(w, 2, count, t, 120, 120, 60, 1);
        expect(near(w.seen_hz(), 120, 0.01) && w.seen() == Seen::Unknown,
               "then 120 is seen - and \"fixed\" waits three seconds again");
        count = run(w, 2, count, t, 120, 120, 60, 2);
        expect(w.seen() == Seen::Fixed, "three seconds on the new screen: fixed there");
    }
    {
        // Without a reset too: another epoch alone keeps the counts apart
        // (the counting thread started over after a hole).
        RefreshWatch w;
        double t = 10;
        unsigned count = run(w, 1, 0, t, 240, 240, 60, 4);
        expect(w.seen() == Seen::Fixed, "fixed at 240");
        t += 1.0;
        w.sample(2, 37, t - 0.001, t, 240, 60);  // far fewer than before: a new count
        expect(w.seen() == Seen::Unknown && w.seen_hz() == 0, "a new epoch is never subtracted from the old one");
        count = run(w, 2, 37, t, 240, 240, 60, 1);
        expect(near(w.seen_hz(), 240, 0.01), "the next second of the new epoch reads 240 again");
        (void)count;
    }
    {
        // The count stands still (the screen went to sleep).
        RefreshWatch w;
        double t = 10;
        const unsigned count = run(w, 1, 0, t, 120, 120, 60, 4);
        expect(w.seen() == Seen::Fixed, "fixed at 120");
        const double last = t - 0.001;
        t += 1.0;
        w.sample(1, count, last, t, 120, 60);
        expect(w.seen() == Seen::Unknown && w.seen_hz() == 0, "no refresh for a second: unknown, not the old reading");
        t += 1.0;
        w.sample(1, count + 3, t - 0.001, t, 120, 60);
        expect(w.seen_hz() == 0, "counting again: this sample is the new base");
        run(w, 1, count + 3, t, 120, 120, 60, 1);
        expect(near(w.seen_hz(), 120, 0.01), "and the next one reads 120");
    }
    {
        // mpv stood still for five seconds: the frames shown are the last
        // second's, the count is five seconds'.
        RefreshWatch w;
        double t = 10;
        unsigned count = run(w, 1, 0, t, 120, 120, 60, 4);
        t += 5.0;
        count += 600;
        w.sample(1, count, t - 0.001, t, 120, 60);
        expect(w.seen() == Seen::Unknown && w.seen_hz() == 0, "a sample five seconds after the last: not judged");
        run(w, 1, count, t, 120, 120, 60, 1);
        expect(near(w.seen_hz(), 120, 0.01), "the second after it reads again");
    }
    {
        // Asked twice within the same half second: the second one waits.
        RefreshWatch w;
        double t = 10;
        unsigned count = run(w, 1, 0, t, 120, 120, 60, 2);
        w.sample(1, count + 12, t + 0.099, t + 0.1, 120, 60);
        expect(near(w.seen_hz(), 120, 0.01), "a sample a tenth of a second later changes nothing");
        t += 1.0;
        w.sample(1, count + 120, t - 0.001, t, 120, 60);
        expect(near(w.seen_hz(), 120, 0.01), "and the full second still reads 120");
    }
    {
        // The count wraps (four billion refreshes: unsigned arithmetic).
        RefreshWatch w;
        double t = 10;
        w.sample(1, 0xFFFFFFC4u, t - 0.001, t, 120, 60);  // 60 before the wrap
        t += 1.0;
        w.sample(1, 60, t - 0.001, t, 120, 60);
        expect(near(w.seen_hz(), 120, 0.01), "a count that wraps still reads 120");
    }
}

}  // namespace

int main() {
    list();
    beliefs();
    refreshes();
    std::printf("\n%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
