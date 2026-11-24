// SPDX-License-Identifier: MIT
// Checks the mouse on the areas frame generation leaves alone
// (src/overlay/keep_edit.h): which part of an area a press takes hold of, what
// a drag makes of it, and that every result stays on the picture.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "keep_edit.h"
#include "keep_regions.h"

using namespace app;
using namespace app::keep_edit;

namespace {

int g_failures = 0;

void expect(bool ok, const char *what) {
    std::printf("%-76s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_failures;
}

bool near(double a, double b, double by = 0.006) { return std::fabs(a - b) <= by; }

bool is(const KeepRegion &r, double x, double y, double w, double h) {
    return near(r.x, x) && near(r.y, y) && near(r.w, w) && near(r.h, h);
}

// On the picture, at least `least` each way.
bool valid(const KeepRegion &r, double least = kLeast) {
    return r.x >= 0 && r.y >= 0 && r.w >= least - 0.001 && r.h >= least - 0.001 && r.x + r.w <= 100.001 &&
           r.y + r.h <= 100.001;
}

const Size kHd{1920, 1080};
const Size kSquare{1000, 1000};
const float kReach = 8;

void places() {
    const Box b = box_of({false, 25, 50, 50, 25}, kHd);
    expect(near(b.left, 480) && near(b.top, 540) && near(b.right, 1440) && near(b.bottom, 810),
           "an area's pixels are its percent of the whole image");
    std::vector<PixelBox> boxes;
    region_boxes({false, 25, 50, 50, 25}, 1920, 1080, boxes);
    expect(boxes.size() == 1 && near(b.left, boxes[0].left) && near(b.bottom, boxes[0].bottom),
           "the same pixels frame generation leaves alone");
    expect(is(fitted({false, 120, -5, 0.2f, 300}), 99, 0, 1, 100), "fitted: least 1 percent, inside the picture");
    expect(is(fitted({false, 90, 90, 20, 20}), 80, 80, 20, 20), "fitted: moved back inside, not shrunk");
    expect(near(hundredths(12.3449f), 12.34) && near(hundredths(12.346f), 12.35), "percent is kept to hundredths");
}

void grips() {
    const KeepRegion r{false, 25, 50, 50, 25};  // 480..1440 x 540..810
    expect(grip_at(r, {960, 675}, kHd, kReach, false) == Grip::Move, "the middle of an area moves it");
    expect(grip_at(r, {483, 675}, kHd, kReach, false) == Grip::Left, "its left edge, from just inside");
    expect(grip_at(r, {474, 675}, kHd, kReach, false) == Grip::Left, "its left edge, from just outside");
    expect(grip_at(r, {1441, 675}, kHd, kReach, false) == Grip::Right, "its right edge");
    expect(grip_at(r, {960, 545}, kHd, kReach, false) == Grip::Top, "its top edge");
    expect(grip_at(r, {960, 815}, kHd, kReach, false) == Grip::Bottom, "its bottom edge");
    expect(grip_at(r, {478, 536}, kHd, kReach, false) == Grip::TopLeft, "its top left corner");
    expect(grip_at(r, {1436, 544}, kHd, kReach, false) == Grip::TopRight, "its top right corner");
    expect(grip_at(r, {484, 806}, kHd, kReach, false) == Grip::BottomLeft, "its bottom left corner");
    expect(grip_at(r, {1445, 815}, kHd, kReach, false) == Grip::BottomRight, "its bottom right corner");
    expect(grip_at(r, {960, 530}, kHd, kReach, false) == Grip::None && grip_at(r, {470, 675}, kHd, kReach, false) == Grip::None,
           "further out than the reach: nothing");

    // 1 percent of 1920 x 1080 is 19 x 11 pixels: less than twice the reach.
    const KeepRegion small{false, 10, 10, 1, 1};
    expect(grip_at(small, {201.6f, 113.4f}, kHd, kReach, false) == Grip::Move, "the middle of a small area still moves it");
    expect(grip_at(small, {193, 113.4f}, kHd, kReach, false) == Grip::Left, "and its edge still sizes it");

    const KeepRegion e{true, 25, 50, 50, 25};
    expect(grip_at(e, {960, 675}, kHd, kReach, false) == Grip::Move, "an ellipse: inside it moves it");
    expect(grip_at(e, {485, 545}, kHd, kReach, false) == Grip::None, "an ellipse: its box's corner is not the ellipse");
    expect(grip_at(e, {485, 545}, kHd, kReach, true) == Grip::TopLeft, "the chosen ellipse has its box's corners");
    expect(grip_at(e, {700, 600}, kHd, kReach, true) == Grip::Move, "and moves from anywhere in the box");
    expect(grip_at({false, 10, 10, 0, 0}, {192, 108}, kHd, kReach, true) == Grip::None, "an area of no size: nothing");
}

void remove_boxes() {
    const float side = 20, gap = 8;
    Box b = remove_box({false, 10, 10, 20, 20}, kSquare, side, gap);
    expect(near(b.left, 308) && near(b.top, 100) && near(b.right, 328), "remove box: right of the top right corner");
    b = remove_box({false, 80, 10, 20, 20}, kSquare, side, gap);
    expect(near(b.left, 772) && near(b.top, 100), "at the right edge of the picture: left of the area");
    b = remove_box({false, 0, 80, 100, 10}, kSquare, side, gap);
    expect(near(b.left, 980) && near(b.top, 772), "an area as wide as the picture: above it");
    b = remove_box({false, 0, 0, 100, 10}, kSquare, side, gap);
    expect(near(b.left, 980) && near(b.top, 108), "as wide, at the top: below it");
    b = remove_box({false, 0, 0, 100, 100}, kSquare, side, gap);
    expect(near(b.left, 972) && near(b.top, 8), "the whole picture: inside its top right corner");
    bool inside = true;
    for (float x : {0.0f, 37.0f, 99.0f})
        for (float y : {0.0f, 52.0f, 99.0f})
            for (float size : {1.0f, 30.0f, 100.0f}) {
                const Box at = remove_box(fitted({false, x, y, size, size}), kHd, side, gap);
                inside = inside && at.left >= 0 && at.top >= 0 && at.right <= kHd.w && at.bottom <= kHd.h;
            }
    expect(inside, "remove box: always on the picture");
}

void hits() {
    const std::vector<KeepRegion> areas = {{false, 0, 0, 100, 100}, {false, 40, 40, 10, 10}, {true, 70, 10, 20, 20}};
    const float side = 20;
    Hit h = hit_at(areas, 0, {450, 450}, kSquare, kReach, side);
    expect(h.index == 1 && h.grip == Grip::Move, "a small area inside the chosen large one can be reached");
    h = hit_at(areas, 0, {200, 200}, kSquare, kReach, side);
    expect(h.index == 0 && h.grip == Grip::Move, "the large one where nothing lies on it");
    h = hit_at(areas, 1, {402, 450}, kSquare, kReach, side);
    expect(h.index == 1 && h.grip == Grip::Left, "the chosen area's edge");
    h = hit_at(areas, 1, {518, 410}, kSquare, kReach, side);  // its remove box: 508..528 x 400..420
    expect(h.index == 1 && h.grip == Grip::Remove, "the chosen area's remove box");
    h = hit_at(areas, -1, {518, 410}, kSquare, kReach, side);
    expect(h.index == 0 && h.grip == Grip::Move, "no remove box when no area is chosen");
    h = hit_at(areas, 2, {702, 102}, kSquare, kReach, side);
    expect(h.index == 2 && h.grip == Grip::TopLeft, "the chosen ellipse's corner, over the area behind it");
    h = hit_at(areas, 1, {702, 102}, kSquare, kReach, side);
    expect(h.index == 0 && h.grip == Grip::Move, "the same place when it is not chosen: the area behind");
    h = hit_at({{false, 10, 10, 20, 20}}, 0, {600, 600}, kSquare, kReach, side);
    expect(h.index == -1 && h.grip == Grip::None, "the bare picture: nothing");
    h = hit_at({}, 3, {600, 600}, kSquare, kReach, side);
    expect(h.index == -1, "no areas, a stale choice: nothing");
    const std::vector<KeepRegion> twins = {{false, 10, 10, 20, 20}, {false, 10, 10, 20, 20}};
    expect(hit_at(twins, -1, {200, 200}, kSquare, kReach, side).index == 1 &&
               hit_at(twins, 0, {200, 200}, kSquare, kReach, side).index == 0,
           "two in one place: the chosen one, else the later one");
}

void drags() {
    const KeepRegion r{false, 10, 10, 20, 20};  // 100..300 on the 1000 x 1000 picture
    const Point from{200, 200};
    expect(is(dragged(r, Grip::Move, from, {300, 250}, kSquare), 20, 15, 20, 20), "moved by the mouse's way in percent");
    expect(is(dragged(r, Grip::Move, from, {2000, -900}, kSquare), 80, 0, 20, 20), "moved to the picture's edge, whole");
    expect(is(dragged(r, Grip::Move, from, {2000, 200}, kSquare), 80, 10, 20, 20) &&
               is(dragged(r, Grip::Move, from, {250, 200}, kSquare), 15, 10, 20, 20),
           "coming back from the edge follows the mouse again");
    expect(is(dragged(r, Grip::Left, {100, 200}, {50, 900}, kSquare), 5, 10, 25, 20), "left edge: the right side stays");
    expect(is(dragged(r, Grip::Left, {100, 200}, {900, 200}, kSquare), 29, 10, 1, 20), "left edge past the right: 1 percent left");
    expect(is(dragged(r, Grip::Left, {100, 200}, {-500, 200}, kSquare), 0, 10, 30, 20), "left edge off the picture: to its edge");
    expect(is(dragged(r, Grip::Right, {300, 200}, {450, 200}, kSquare), 10, 10, 35, 20), "right edge: the left side stays");
    expect(is(dragged(r, Grip::Right, {300, 200}, {5000, 200}, kSquare), 10, 10, 90, 20), "right edge off the picture");
    expect(is(dragged(r, Grip::Right, {300, 200}, {0, 200}, kSquare), 10, 10, 1, 20), "right edge past the left");
    expect(is(dragged(r, Grip::Top, {200, 100}, {200, 40}, kSquare), 10, 4, 20, 26), "top edge: the bottom stays");
    expect(is(dragged(r, Grip::Bottom, {200, 300}, {200, 0}, kSquare), 10, 10, 20, 1), "bottom edge past the top");
    expect(is(dragged(r, Grip::TopLeft, {100, 100}, {50, 60}, kSquare), 5, 6, 25, 24), "top left corner: both");
    expect(is(dragged(r, Grip::BottomRight, {300, 300}, {400, 350}, kSquare), 10, 10, 30, 25), "bottom right corner: both");
    expect(is(dragged(r, Grip::TopRight, {300, 100}, {2000, -2000}, kSquare), 10, 0, 90, 30), "top right corner off the picture");
    expect(is(dragged(r, Grip::BottomLeft, {100, 300}, {900, 0}, kSquare), 29, 10, 1, 1), "bottom left corner through the area");
    expect(same(dragged(r, Grip::None, from, {900, 900}, kSquare), r) && same(dragged(r, Grip::Remove, from, {900, 900}, kSquare), r) &&
               same(dragged(r, Grip::Move, from, {900, 900}, Size{0, 0}), r),
           "no grip, or no picture: as it was");
    expect(dragged({true, 10, 10, 20, 20}, Grip::Right, {300, 200}, {450, 200}, kSquare).ellipse, "an ellipse stays one");

    const Size uhd{3840, 2160};
    expect(is(dragged(r, Grip::Move, {0, 0}, {1, 1}, uhd), 10.03, 10.05, 20, 20), "one pixel at 4K: hundredths of a percent");
    const KeepRegion thin{false, 10, 10, 0.5f, 5};  // from the settings file, under the least
    const KeepRegion t = dragged(thin, Grip::Left, {100, 100}, {400, 100}, kSquare);
    expect(near(t.w, 0.5) && near(t.x, 10), "an area under 1 percent is not made smaller, nor larger");

    // Any grip, any drag: on the picture, at least 1 percent, and what it
    // writes reads back as it was.
    bool all = true, text = true;
    for (int g = int(Grip::Move); g <= int(Grip::BottomRight); ++g)
        for (float x : {-3000.0f, -40.0f, 3.0f, 777.0f, 4000.0f})
            for (float y : {-3000.0f, -7.0f, 11.0f, 555.0f, 4000.0f}) {
                const KeepRegion d = dragged({true, 33.33f, 41.5f, 12.25f, 30}, Grip(g), {640, 480}, {x, y}, kHd);
                all = all && valid(d);
                std::vector<KeepRegion> back;
                text = text && parse_keep_regions(reshade_keep_text({d}), back) && back.size() == 1 &&
                       is(back[0], d.x, d.y, d.w, d.h) && back[0].ellipse;
            }
    expect(all, "every drag ends on the picture, at least 1 percent each way");
    expect(text, "and is read back from its text as it was");
}

void drawing() {
    expect(is(drawn(false, {100, 100}, {300, 200}, kSquare), 10, 10, 20, 10), "drawn from top left to bottom right");
    expect(is(drawn(false, {300, 200}, {100, 100}, kSquare), 10, 10, 20, 10), "drawn the other way: the same area");
    expect(is(drawn(false, {100, 100}, {2000, -50}, kSquare), 10, 0, 90, 10), "drawn past the picture: cut to it");
    expect(drawn(true, {100, 100}, {300, 200}, kSquare).ellipse && !drawn(false, {100, 100}, {300, 200}, kSquare).ellipse,
           "a rectangle or an ellipse, as asked");
    expect(is(drawn(false, {500, 500}, {502, 501}, kSquare), 50, 50, 1, 1), "a short drag: 1 percent, on from where it began");
    expect(is(drawn(false, {500, 500}, {498, 499}, kSquare), 49, 49, 1, 1), "a short drag up and left: back from where it began");
    expect(is(drawn(false, {999, 999}, {1000, 1000}, kSquare), 99, 99, 1, 1), "a short drag at the picture's corner: inside it");
    expect(is(drawn(false, {1, 1}, {0, 0}, kSquare), 0, 0, 1, 1), "and at its top left corner");
    expect(is(drawn(false, {-50, 2000}, {100, 900}, kSquare), 0, 90, 10, 10), "begun off the picture: from its edge");
    expect(valid(drawn(false, {5, 5}, {9, 9}, Size{0, 0})), "no picture: still an area that can be kept");
    expect(is_drag({10, 10}, {10, 16}, 6) && is_drag({10, 10}, {3, 10}, 6) && !is_drag({10, 10}, {14, 14}, 6),
           "a click is not a drag");
    bool all = true;
    for (float x0 : {-100.0f, 0.0f, 640.5f, 1919.0f, 2500.0f})
        for (float y0 : {-100.0f, 0.0f, 333.3f, 1079.0f, 2500.0f})
            for (float x1 : {-100.0f, 0.0f, 641.0f, 1920.0f, 2500.0f})
                for (float y1 : {-100.0f, 0.0f, 334.0f, 1080.0f, 2500.0f}) all = all && valid(drawn(true, {x0, y0}, {x1, y1}, kHd));
    expect(all, "every drawing ends on the picture, at least 1 percent each way");
}

}  // namespace

int main() {
    places();
    grips();
    remove_boxes();
    hits();
    drags();
    drawing();
    std::printf("%d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
