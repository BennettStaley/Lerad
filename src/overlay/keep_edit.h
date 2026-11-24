// SPDX-License-Identifier: MIT
// The mouse on the areas frame generation leaves alone (keep_regions.h): which
// part of an area is under it, and what an area becomes when that part is
// dragged. Plain arithmetic, no Dear ImGui: the menu (menu.cpp) reads the
// mouse and draws, tests/test_keep_edit.cpp checks this.
//
// Places are pixels of the image the menu is drawn on, from its top left
// corner. That is the whole back buffer, black bars included: an area's
// percent is of that (frame generation cuts its boxes from it, region_boxes).
// Areas stay in percent, to hundredths.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

#include "keep_regions.h"

namespace app::keep_edit {

struct Point {
    float x = 0, y = 0;
};

struct Size {
    float w = 0, h = 0;
};

struct Box {
    float left = 0, top = 0, right = 0, bottom = 0;
    bool has(Point p) const { return p.x >= left && p.x < right && p.y >= top && p.y < bottom; }
};

// An area's least width and height in percent, drawn or dragged.
constexpr float kLeast = 1.0f;

// The part of an area a press takes hold of.
enum class Grip { None, Move, Left, Top, Right, Bottom, TopLeft, TopRight, BottomLeft, BottomRight, Remove };

inline bool moves_left(Grip g) { return g == Grip::Left || g == Grip::TopLeft || g == Grip::BottomLeft; }
inline bool moves_right(Grip g) { return g == Grip::Right || g == Grip::TopRight || g == Grip::BottomRight; }
inline bool moves_top(Grip g) { return g == Grip::Top || g == Grip::TopLeft || g == Grip::TopRight; }
inline bool moves_bottom(Grip g) { return g == Grip::Bottom || g == Grip::BottomLeft || g == Grip::BottomRight; }

inline float hundredths(float percent) { return std::round(percent * 100.0f) / 100.0f; }

inline bool same(const KeepRegion &a, const KeepRegion &b) {
    return a.ellipse == b.ellipse && a.find_hud == b.find_hud && a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

inline bool same(const std::vector<KeepRegion> &a, const std::vector<KeepRegion> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!same(a[i], b[i])) return false;
    return true;
}

// The area's pixels (an ellipse's: the box around it), as the menu outlines it.
inline Box box_of(const KeepRegion &r, Size picture) {
    return {picture.w * r.x * 0.01f, picture.h * r.y * 0.01f, picture.w * (r.x + r.w) * 0.01f,
            picture.h * (r.y + r.h) * 0.01f};
}

// The area made to fit: at least `least` percent each way, at most the
// picture, moved (not shrunk) back inside the picture, to hundredths.
inline KeepRegion fitted(KeepRegion r, float least = kLeast) {
    r.w = hundredths(std::clamp(r.w, least, 100.0f));
    r.h = hundredths(std::clamp(r.h, least, 100.0f));
    r.x = std::clamp(hundredths(r.x), 0.0f, 100.0f - r.w);
    r.y = std::clamp(hundredths(r.y), 0.0f, 100.0f - r.h);
    return r;
}

// The part of the area at `at`. A corner or an edge reaches `reach` pixels
// outside the area and as far inside, but no more than a quarter of the
// area's width or height: the middle of even a small area moves it.
// An ellipse has its box's corners and edges only while it is the chosen one
// (the menu draws the box then); otherwise the ellipse itself moves it.
inline Grip grip_at(const KeepRegion &r, Point at, Size picture, float reach, bool chosen) {
    const Box b = box_of(r, picture);
    if (b.right <= b.left || b.bottom <= b.top) return Grip::None;
    if (at.x < b.left - reach || at.x > b.right + reach || at.y < b.top - reach || at.y > b.bottom + reach)
        return Grip::None;
    if (r.ellipse && !chosen) {
        const float rx = (b.right - b.left) * 0.5f + reach, ry = (b.bottom - b.top) * 0.5f + reach;
        const float dx = (at.x - (b.left + b.right) * 0.5f) / rx, dy = (at.y - (b.top + b.bottom) * 0.5f) / ry;
        return dx * dx + dy * dy <= 1.0f ? Grip::Move : Grip::None;
    }
    const float in_x = std::min(reach, (b.right - b.left) * 0.25f), in_y = std::min(reach, (b.bottom - b.top) * 0.25f);
    const bool left = at.x <= b.left + in_x, right = at.x >= b.right - in_x;
    const bool top = at.y <= b.top + in_y, bottom = at.y >= b.bottom - in_y;
    if (top) return left ? Grip::TopLeft : right ? Grip::TopRight : Grip::Top;
    if (bottom) return left ? Grip::BottomLeft : right ? Grip::BottomRight : Grip::Bottom;
    return left ? Grip::Left : right ? Grip::Right : Grip::Move;
}

// Where the small box that removes the chosen area is drawn, `side` pixels
// square and `gap` from the area: right of its top right corner; where the
// picture ends there, left of the area, above it, below it, and last inside
// its top right corner. Always on the picture (if the picture is that large).
inline Box remove_box(const KeepRegion &r, Size picture, float side, float gap) {
    const Box b = box_of(r, picture);
    const auto box = [&](float left, float top) {
        left = std::clamp(left, 0.0f, std::max(0.0f, picture.w - side));
        top = std::clamp(top, 0.0f, std::max(0.0f, picture.h - side));
        return Box{left, top, left + side, top + side};
    };
    if (b.right + gap + side <= picture.w) return box(b.right + gap, b.top);
    if (b.left - gap - side >= 0) return box(b.left - gap - side, b.top);
    if (b.top - gap - side >= 0) return box(b.right - side, b.top - gap - side);
    if (b.bottom + gap + side <= picture.h) return box(b.right - side, b.bottom + gap);
    return box(b.right - gap - side, b.top + gap);
}

struct Hit {
    int index = -1;  // which area; -1: none
    Grip grip = Grip::None;
};

// What a press at `at` takes hold of. The chosen area (-1: none) comes first
// with its remove box, corners and edges; then the smallest area there, so
// that one lying inside another can be reached.
inline Hit hit_at(const std::vector<KeepRegion> &regions, int chosen, Point at, Size picture, float reach,
                  float remove_side) {
    const bool has_chosen = chosen >= 0 && chosen < int(regions.size());
    if (has_chosen) {
        const KeepRegion &r = regions[size_t(chosen)];
        if (remove_box(r, picture, remove_side, reach).has(at)) return {chosen, Grip::Remove};
        const Grip g = grip_at(r, at, picture, reach, true);
        if (g != Grip::None && g != Grip::Move) return {chosen, g};
    }
    Hit hit;
    float smallest = 0;
    for (int i = 0; i < int(regions.size()); ++i) {
        const KeepRegion &r = regions[size_t(i)];
        const Grip g = grip_at(r, at, picture, reach, i == chosen);
        if (g == Grip::None) continue;
        const float area = r.w * r.h;
        // Two of one size: the chosen one, else the later one (drawn on top).
        if (hit.index < 0 || area < smallest || (area == smallest && hit.index != chosen)) {
            hit = {i, g};
            smallest = area;
        }
    }
    return hit;
}

// The area `start` after its part `grip` was dragged from `from` to `to`:
// moved, staying whole inside the picture, or sized with the opposite side
// where it was. Never over the picture's edge and never under `least`
// percent (an area already smaller is not made smaller).
inline KeepRegion dragged(const KeepRegion &start, Grip grip, Point from, Point to, Size picture, float least = kLeast) {
    KeepRegion r = start;
    if (picture.w <= 0 || picture.h <= 0 || grip == Grip::None || grip == Grip::Remove) return r;
    const float dx = (to.x - from.x) / picture.w * 100.0f, dy = (to.y - from.y) / picture.h * 100.0f;
    // v between lo and hi; lo where they cross (an area read from elsewhere may not fit).
    const auto between = [](float v, float lo, float hi) { return std::max(lo, std::min(v, hi)); };
    if (grip == Grip::Move) {
        r.x = between(hundredths(start.x + dx), 0.0f, 100.0f - start.w);
        r.y = between(hundredths(start.y + dy), 0.0f, 100.0f - start.h);
        return r;
    }
    const float least_w = std::min(least, start.w), least_h = std::min(least, start.h);
    const float right = std::min(100.0f, start.x + start.w), bottom = std::min(100.0f, start.y + start.h);
    if (moves_left(grip)) {
        r.x = between(hundredths(start.x + dx), 0.0f, right - least_w);
        r.w = right - r.x;
    } else if (moves_right(grip)) {
        r.w = between(hundredths(right + dx), start.x + least_w, 100.0f) - start.x;
    }
    if (moves_top(grip)) {
        r.y = between(hundredths(start.y + dy), 0.0f, bottom - least_h);
        r.h = bottom - r.y;
    } else if (moves_bottom(grip)) {
        r.h = between(hundredths(bottom + dy), start.y + least_h, 100.0f) - start.y;
    }
    // The differences above, back to hundredths (12.3 - 4.1 is not 8.2 in floats).
    r.w = std::min(hundredths(r.w), 100.0f - r.x);
    r.h = std::min(hundredths(r.h), 100.0f - r.y);
    return r;
}

// The area drawn by a drag from `from` to `to`, in any direction: cut to the
// picture, and at least `least` percent each way (a short drag grows on from
// where it began, and back from the picture's edge).
inline KeepRegion drawn(bool ellipse, Point from, Point to, Size picture, float least = kLeast) {
    KeepRegion r;
    r.ellipse = ellipse;
    if (picture.w <= 0 || picture.h <= 0) return fitted(r, least);
    const auto span = [least](float a, float b, float size, float &start, float &length) {
        a = std::clamp(a / size * 100.0f, 0.0f, 100.0f);
        b = std::clamp(b / size * 100.0f, 0.0f, 100.0f);
        float lo = std::min(a, b), hi = std::max(a, b);
        if (hi - lo < least) {
            if (b >= a)
                hi = lo + least;
            else
                lo = hi - least;
        }
        start = lo;
        length = hi - lo;
    };
    span(from.x, to.x, picture.w, r.x, r.w);
    span(from.y, to.y, picture.h, r.y, r.h);
    return fitted(r, least);
}

// A press and a release with the mouse moved at least `least` pixels between
// them; less is a click, which draws nothing.
inline bool is_drag(Point from, Point to, float least) {
    return std::fabs(to.x - from.x) >= least || std::fabs(to.y - from.y) >= least;
}

}  // namespace app::keep_edit
