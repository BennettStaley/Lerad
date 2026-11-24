// SPDX-License-Identifier: MIT
// Areas of the picture that frame generation leaves alone: a HUD, a map, a
// line of subtitles. A frame in between shows the nearer real frame there, as
// it is, so small text and thin lines stay as the game drew them instead of
// being moved along with what is behind them.
//
// One setting's text holds them all ([framegen] keep in the settings file, Keep in
// ReShade.ini): "r:2,80,20,15;e:85,5,12,20" - r a rectangle, e an ellipse,
// then left, top, width and height in percent of the picture.
//
// An area is left alone as a whole, or - with ",hud" after its numbers,
// "r:2,80,20,15,hud" - only where frame generation finds a HUD in it
// (find_hud; src/framegen/fg_hud.h): the area then says where to look, and
// the scenery seen beside and through the HUD is still generated.
//
// ReShade reads a comma in its ini as the end of a value, so there the same
// list is written with other marks (reshade_keep_text): "r:2/80/20/15|e:...".
// Both forms are read.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace app {

struct KeepRegion {
    bool ellipse = false;
    float x = 0, y = 0, w = 0, h = 0;  // percent of the picture: left, top, width, height
    // false: the whole area shows the nearer real frame. true: only the
    // pixels in it that frame generation takes for a HUD do.
    bool find_hud = false;
};

constexpr int kMaxKeepRegions = 8;

// False for text that is not a list of regions (empty text is an empty list).
// Regions are clipped to the picture; more than kMaxKeepRegions is an error.
inline bool parse_keep_regions(std::string text, std::vector<KeepRegion> &out) {
    out.clear();
    for (char &c : text) c = c == '/' ? ',' : c == '|' ? ';' : c;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(';', at);
        if (end == std::string::npos) end = text.size();
        std::string item = text.substr(at, end - at);
        at = end + 1;
        item.erase(std::remove(item.begin(), item.end(), ' '), item.end());
        if (item.empty()) continue;
        if (item.size() < 3 || item[1] != ':' || (item[0] != 'r' && item[0] != 'e')) return out.clear(), false;
        KeepRegion r;
        r.ellipse = item[0] == 'e';
        float v[4];
        const char *p = item.c_str() + 2;
        for (int i = 0; i < 4; ++i) {
            char *stop = nullptr;
            v[i] = std::strtof(p, &stop);
            if (stop == p || !std::isfinite(v[i])) return out.clear(), false;
            p = stop;
            if (i < 3) {
                if (*p != ',') return out.clear(), false;
                ++p;
            }
        }
        if (*p == ',' && (p[1] == 'h' || p[1] == 'H') && (p[2] == 'u' || p[2] == 'U') && (p[3] == 'd' || p[3] == 'D')) {
            r.find_hud = true;
            p += 4;
        }
        if (*p) return out.clear(), false;
        r.x = std::clamp(v[0], 0.0f, 100.0f);
        r.y = std::clamp(v[1], 0.0f, 100.0f);
        r.w = std::clamp(v[2], 0.0f, 100.0f - r.x);
        r.h = std::clamp(v[3], 0.0f, 100.0f - r.y);
        if (r.w <= 0 || r.h <= 0) return out.clear(), false;
        if (int(out.size()) >= kMaxKeepRegions) return out.clear(), false;
        out.push_back(r);
    }
    return true;
}

inline std::string format_keep_regions(const std::vector<KeepRegion> &regions) {
    std::string out;
    for (const KeepRegion &r : regions) {
        char text[80];
        std::snprintf(text, sizeof(text), "%s%c:%g,%g,%g,%g%s", out.empty() ? "" : ";", r.ellipse ? 'e' : 'r', r.x, r.y,
                      r.w, r.h, r.find_hud ? ",hud" : "");
        out += text;
    }
    return out;
}

// The list for ReShade.ini: no commas, no semicolons.
inline std::string reshade_keep_text(const std::vector<KeepRegion> &regions) {
    std::string text = format_keep_regions(regions);
    for (char &c : text) c = c == ',' ? '/' : c == ';' ? '|' : c;
    return text;
}

struct PixelBox {
    unsigned left = 0, top = 0, right = 0, bottom = 0;  // right and bottom are one past the last pixel
};

// The box around a region on a width x height picture, in pixels (an
// ellipse fills it from edge to edge). False for a region smaller than a pixel.
inline bool region_box(const KeepRegion &r, unsigned width, unsigned height, PixelBox &box) {
    const auto px = [](float percent, unsigned size) {
        return unsigned(std::lround(std::clamp(percent, 0.0f, 100.0f) * 0.01f * float(size)));
    };
    box.left = px(r.x, width);
    box.top = px(r.y, height);
    box.right = std::min(width, px(r.x + r.w, width));
    box.bottom = std::min(height, px(r.y + r.h, height));
    return box.right > box.left && box.bottom > box.top;
}

// The pixels of a region on a width x height picture, as boxes: one for a
// rectangle; an ellipse as rows of at most `strip` pixels, each as wide as
// the ellipse is at its middle. Nothing for a region smaller than a pixel.
inline void region_boxes(const KeepRegion &r, unsigned width, unsigned height, std::vector<PixelBox> &out,
                         unsigned strip = 8) {
    PixelBox box;
    if (!region_box(r, width, height, box)) return;
    const unsigned left = box.left, top = box.top, right = box.right, bottom = box.bottom;
    if (!r.ellipse) {
        out.push_back({left, top, right, bottom});
        return;
    }
    const double cx = (left + right) * 0.5, cy = (top + bottom) * 0.5;
    const double rx = (right - left) * 0.5, ry = (bottom - top) * 0.5;
    for (unsigned y = top; y < bottom; y += std::max(1u, strip)) {
        const unsigned y1 = std::min(bottom, y + std::max(1u, strip));
        const double dy = ((y + y1) * 0.5 - cy) / ry;
        if (std::fabs(dy) >= 1.0) continue;
        const double half = rx * std::sqrt(1.0 - dy * dy);
        const unsigned x0 = unsigned(std::max(double(left), std::floor(cx - half)));
        const unsigned x1 = unsigned(std::min(double(right), std::ceil(cx + half)));
        if (x1 > x0) out.push_back({x0, y, x1, y1});
    }
}

}  // namespace app
