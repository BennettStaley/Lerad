// SPDX-License-Identifier: MIT
// The widgets both of the app's UIs are made of (the launcher and the
// in-game menu), drawn once here in the look of ui_style.h: a switch, a
// segmented choice, a status pill, a notice with an action, a key cap, and
// the small parts around them (a section label, a chip, a slider, a list
// box, the strip of real and added frames). Header only; Dear ImGui's public
// API alone, so it builds wherever imgui.h does.
//
// Sizes follow the text: everything is measured in the current font size and
// the style's scale, so the same code draws the launcher at 96 DPI and the
// menu on a 2160-line picture.
//
// Every widget that can be worked with the keyboard notes when it holds the
// keyboard's place (item_focused): the menu's help line follows that.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include <imgui.h>

#include "ui_style.h"

namespace app::ui {

enum class Status { Info, Ok, Warn, Bad };

inline constexpr float kSmall = 0.88f;  // hints, beside the text's size

// A colour for a draw list, with the style's alpha (a faded or disabled part).
inline ImU32 u32(const ImVec4 &c) { return ImGui::GetColorU32(c); }
inline float scale() { return ImGui::GetStyle().FontScaleMain; }
// The text's size before scaling, whatever font size is pushed now.
inline float base_font() { return detail::g_base_font; }

// The semibold font, if Windows has it (null: the one font there is).
inline ImFont *bold_font() {
    ImGuiIO &io = ImGui::GetIO();
    return io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : nullptr;
}

// ---------------------------------------------------------------- the keyboard's place

namespace detail {
inline bool g_item_focused = false;
}
// Call after an item of Dear ImGui's own (a list box, a button) in a row
// whose help follows the keyboard; the widgets here do it themselves.
inline void note_item() {
    if (ImGui::IsItemFocused()) detail::g_item_focused = true;
}
// Whether an item noted since the last call holds the keyboard's place.
inline bool take_focused() {
    const bool focused = detail::g_item_focused;
    detail::g_item_focused = false;
    return focused;
}

// ---------------------------------------------------------------- text

inline void text(const char *t, const ImVec4 &colour) {
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
}

// The text cut to max_width, with three dots where it was cut.
inline std::string fit_text(const std::string &t, float max_width) {
    if (ImGui::CalcTextSize(t.c_str()).x <= max_width) return t;
    std::string s = t;
    const float dots = ImGui::CalcTextSize("...").x;
    while (!s.empty() && ImGui::CalcTextSize(s.c_str()).x + dots > max_width) {
        // One character: its last byte and the UTF-8 bytes before it that continue it.
        while (!s.empty()) {
            const unsigned char c = static_cast<unsigned char>(s.back());
            s.pop_back();
            if ((c & 0xC0) != 0x80) break;
        }
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s + "...";
}

// Capitals with space between the letters, as the section labels are set.
// Plain ASCII only.
inline void spaced_text(const char *t, const ImVec4 &colour, float spacing) {
    ImFont *font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *d = ImGui::GetWindowDrawList();
    float x = p.x;
    for (const char *c = t; *c; ++c) {
        const char one[2] = {*c >= 'a' && *c <= 'z' ? char(*c - 'a' + 'A') : *c, 0};
        d->AddText(font, size, ImVec2(x, p.y), u32(colour), one, one + 1);
        x += ImGui::CalcTextSize(one, one + 1).x + spacing;
    }
    ImGui::Dummy(ImVec2(x - p.x, size));
}

// A section's label: small quiet capitals, with `above` pixels (before
// scaling) of room over it.
inline void section_label(const char *t, float above = 14.0f) {
    const ImGuiStyle &st = ImGui::GetStyle();
    const float extra = above * scale() - st.ItemSpacing.y;
    if (extra > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + extra);
    ImGui::PushFont(bold_font(), base_font() * 0.74f);
    spaced_text(t, kTextDim, ImGui::GetFontSize() * 0.10f);
    ImGui::PopFont();
}

// Draws the text from `at` down in lines no wider than `width`, broken at
// spaces only, in the current font; with no draw list it only measures. The
// size it takes. (Dear ImGui's own wrapping also breaks after a full stop or
// a comma, which cuts a figure like 59.94 in two.)
inline ImVec2 draw_wrapped(ImDrawList *d, ImVec2 at, ImU32 colour, const char *t, float width) {
    ImFont *font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    float y = 0, widest = 0;
    const auto flush = [&](const char *a, const char *b) {
        if (d && b > a) d->AddText(font, fs, ImVec2(at.x, at.y + y), colour, a, b);
        widest = std::max(widest, ImGui::CalcTextSize(a, b).x);
        y += fs;
    };
    const char *p = t;
    while (*p == ' ') ++p;
    const char *line = p, *end = p;  // the line so far ends after its last word, at `end`
    while (*p) {
        if (*p == '\n') {
            flush(line, end);
            ++p;
            while (*p == ' ') ++p;
            line = end = p;
            continue;
        }
        const char *word = p;
        while (*p && *p != ' ' && *p != '\n') ++p;
        if (end > line && ImGui::CalcTextSize(line, p).x > width) {
            flush(line, end);
            line = word;
        }
        end = p;
        while (*p == ' ') ++p;
    }
    if (end > line || y == 0) flush(line, end);
    return ImVec2(widest, y);
}

// The same as an item of the window, where the cursor is.
inline void wrapped_text(const char *t, float width, const ImVec4 &colour = kText) {
    const ImVec2 size = draw_wrapped(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), u32(colour), t, width);
    ImGui::Dummy(size);
}

// Quiet small text on the line of a control (after SameLine): on the
// control's text line when it fits in one, else wrapped and centred on the
// control.
inline void row_hint(const char *t, const ImVec4 &colour = kTextDim) {
    const ImGuiStyle &st = ImGui::GetStyle();
    const float base = ImGui::GetFontSize(), frame = ImGui::GetFrameHeight();
    ImGui::PushFont(nullptr, base_font() * kSmall);
    const float fs = ImGui::GetFontSize();  // (not "small": a Windows header defines that word)
    const float avail = std::max(ImGui::GetContentRegionAvail().x, fs * 4);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size = draw_wrapped(nullptr, p, 0, t, avail);
    const bool one_line = size.y <= fs * 1.5f;
    const float y = one_line ? p.y + st.FramePadding.y + (base - fs) * 0.72f : p.y + std::max(0.0f, (frame - size.y) * 0.5f);
    draw_wrapped(ImGui::GetWindowDrawList(), ImVec2(p.x, y), u32(colour), t, avail);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(size.x, std::max(frame, size.y)));
}

// Quiet small text on lines of its own, wrapped at the window's right edge.
inline void hint(const char *t, const ImVec4 &colour = kTextDim) {
    ImGui::PushFont(nullptr, base_font() * kSmall);
    wrapped_text(t, ImGui::GetContentRegionAvail().x, colour);
    ImGui::PopFont();
}

// What a tooltip says about the item before it, wrapped.
inline void tooltip(const char *t) {
    if (!t || !*t || !ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * scale(), 9 * scale()));
    if (ImGui::BeginTooltip()) {
        ImGui::PushFont(nullptr, base_font() * kSmall);
        wrapped_text(t, ImGui::GetFontSize() * 22, kText);
        ImGui::PopFont();
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------- status

// The sign that goes with a status's word, drawn with lines in a square of
// `size` around `centre`: a tick in a ring, a warning triangle, a stop sign,
// an i in a ring.
inline void status_icon(ImDrawList *d, ImVec2 centre, float size, Status status, ImU32 colour) {
    const float r = size * 0.5f, w = std::max(1.3f, size * 0.10f);
    const auto at = [&](float x, float y) { return ImVec2(centre.x + x * r, centre.y + y * r); };
    switch (status) {
    case Status::Ok: {
        d->AddCircle(centre, r * 0.92f, colour, 0, w);
        const ImVec2 tick[3] = {at(-0.42f, 0.04f), at(-0.10f, 0.36f), at(0.44f, -0.30f)};
        d->AddPolyline(tick, 3, colour, 0, w);
        return;
    }
    case Status::Warn: {
        const ImVec2 tri[3] = {at(0, -0.88f), at(0.98f, 0.80f), at(-0.98f, 0.80f)};
        d->AddPolyline(tri, 3, colour, ImDrawFlags_Closed, w);
        d->AddLine(at(0, -0.30f), at(0, 0.22f), colour, w);
        d->AddLine(at(0, 0.46f), at(0, 0.56f), colour, w);
        return;
    }
    case Status::Bad: {
        ImVec2 stop[8];
        for (int i = 0; i < 8; ++i) {
            const float a = (22.5f + 45.0f * float(i)) * 3.14159265f / 180.0f;
            stop[i] = at(std::cos(a) * 0.98f, std::sin(a) * 0.98f);
        }
        d->AddPolyline(stop, 8, colour, ImDrawFlags_Closed, w);
        d->AddLine(at(0, -0.48f), at(0, 0.10f), colour, w);
        d->AddLine(at(0, 0.38f), at(0, 0.48f), colour, w);
        return;
    }
    default:
        d->AddCircle(centre, r * 0.92f, colour, 0, w);
        d->AddLine(at(0, -0.10f), at(0, 0.46f), colour, w);
        d->AddLine(at(0, -0.48f), at(0, -0.38f), colour, w);
        return;
    }
}

inline const ImVec4 &status_colour(Status status) {
    return status == Status::Ok ? kOk : status == Status::Warn ? kWarn : status == Status::Bad ? kBad : kTextDim;
}

// A status and its word in a rounded pill, on a control's line: all well is
// an outline with a green tick, a warning and a problem are filled.
inline void pill(Status status, const char *label) {
    const float frame = ImGui::GetFrameHeight();
    ImGui::PushFont(bold_font(), base_font() * kSmall);
    const float fs = ImGui::GetFontSize();
    const float icon = fs * 0.86f, left = fs * 0.46f, gap = fs * 0.34f, right = fs * 0.66f, h = std::round(fs * 1.34f);
    const ImVec2 words = ImGui::CalcTextSize(label);
    const float w = left + icon + gap + words.x + right;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 a(p.x, p.y + std::round((frame - h) * 0.5f)), b(a.x + w, a.y + h);
    ImDrawList *d = ImGui::GetWindowDrawList();
    const bool filled = status == Status::Warn || status == Status::Bad;
    if (filled)
        d->AddRectFilled(a, b, u32(status_colour(status)), h * 0.5f);
    else
        d->AddRect(a, b, u32(kLine), h * 0.5f, 0, std::max(1.0f, scale()));
    status_icon(d, ImVec2(a.x + left + icon * 0.5f, a.y + h * 0.5f), icon, status,
                u32(filled ? kOnStatus : status_colour(status)));
    d->AddText(ImVec2(a.x + left + icon + gap, a.y + (h - fs) * 0.5f), u32(filled ? kOnStatus : kText), label);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(w, frame));
}

// A fact in a thin box ("2160p", "HDR", "120 Hz VRR"), in the current font.
inline void chip(const char *label, const ImVec4 &colour = kText) {
    const float fs = ImGui::GetFontSize();
    const float pad = fs * 0.36f, h = std::round(fs * 1.22f);
    const ImVec2 words = ImGui::CalcTextSize(label);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *d = ImGui::GetWindowDrawList();
    d->AddRect(p, ImVec2(p.x + words.x + pad * 2, p.y + h), u32(kLine), 6 * scale(), 0, std::max(1.0f, scale()));
    d->AddText(ImVec2(p.x + pad, p.y + (h - fs) * 0.5f), u32(colour), label);
    ImGui::Dummy(ImVec2(words.x + pad * 2, h));
}
inline float chip_width(const char *label) { return ImGui::CalcTextSize(label).x + ImGui::GetFontSize() * 0.72f; }

// A key of the keyboard ("Tab", "F8"), centred on a line `line` high
// (0: a line of the current text).
inline void key_cap(const char *key, float line = 0.0f) {
    const float text_line = ImGui::GetFontSize();
    if (line <= 0) line = text_line;
    ImGui::PushFont(bold_font(), base_font() * 0.74f);
    const float fs = ImGui::GetFontSize(), s = scale();
    const float pad = fs * 0.42f, h = std::round(fs * 1.24f);
    const ImVec2 words = ImGui::CalcTextSize(key);
    const float w = std::max(words.x + pad * 2, h);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 a(p.x, p.y + std::round((line - h) * 0.5f)), b(a.x + w, a.y + h);
    ImDrawList *d = ImGui::GetWindowDrawList();
    d->AddRectFilled(a, b, u32(kControl), 5 * s);
    d->AddRect(a, b, u32(kLine), 5 * s, 0, std::max(1.0f, s));
    d->AddLine(ImVec2(a.x + 4 * s, b.y), ImVec2(b.x - 4 * s, b.y), u32(kLine), std::max(1.0f, s));  // the key's lower edge
    d->AddText(ImVec2(a.x + (w - words.x) * 0.5f, a.y + (h - fs) * 0.5f), u32(kText), key);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(w, line));
}

// ---------------------------------------------------------------- buttons

namespace detail {
inline const char *label_end(const char *label) {
    const char *hash = std::strstr(label, "##");
    return hash ? hash : label + std::strlen(label);
}
}  // namespace detail

// A small button for something to do about what is beside it ("Use",
// "Edit", "Download"): quiet at rest, the accent when pointed at. On a
// control's line (in_frame), or as tall as itself.
inline float action_width(const char *label) {
    ImGui::PushFont(bold_font(), base_font() * 0.9f);
    const float w = ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetFontSize() * 1.24f;
    ImGui::PopFont();
    return w;
}
inline bool action_button(const char *label, bool in_frame = true) {
    const float frame = ImGui::GetFrameHeight();
    ImGui::PushFont(bold_font(), base_font() * 0.9f);
    const float fs = ImGui::GetFontSize();
    const ImVec2 words = ImGui::CalcTextSize(label, nullptr, true);
    const float w = words.x + fs * 1.24f, h = std::round(fs * 1.42f), line = in_frame ? frame : h;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, ImVec2(w, line), ImGuiButtonFlags_EnableNav);
    note_item();
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    const ImVec2 a(p.x, p.y + std::round((line - h) * 0.5f)), b(a.x + w, a.y + h);
    ImDrawList *d = ImGui::GetWindowDrawList();
    d->AddRectFilled(a, b, u32(held ? kAccentActive : hovered ? kAccent : kControlHover), 6 * scale());
    d->AddText(ImVec2(a.x + (w - words.x) * 0.5f, a.y + (h - fs) * 0.5f), u32(hovered || held ? kOnAccent : kText), label,
               detail::label_end(label));
    ImGui::PopFont();
    return pressed;
}

// A plain wide button (Close, Quit).
inline bool quiet_button(const char *label, ImVec2 size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, size, ImGuiButtonFlags_EnableNav);
    note_item();
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    const float fs = ImGui::GetFontSize();
    const ImVec2 words = ImGui::CalcTextSize(label, nullptr, true);
    ImDrawList *d = ImGui::GetWindowDrawList();
    d->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), u32(held ? kControlActive : hovered ? kControlHover : kControl),
                     8 * scale());
    d->AddText(ImVec2(p.x + (size.x - words.x) * 0.5f, p.y + (size.y - fs) * 0.5f), u32(kText), label,
               detail::label_end(label));
    return pressed;
}

// The one thing to press (Play): filled with the accent.
inline bool primary_button(const char *label, ImVec2 size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, size, ImGuiButtonFlags_EnableNav);
    note_item();
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    ImGui::PushFont(bold_font(), base_font() * 1.12f);
    const float fs = ImGui::GetFontSize();
    const ImVec2 words = ImGui::CalcTextSize(label, nullptr, true);
    ImDrawList *d = ImGui::GetWindowDrawList();
    d->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), u32(held ? kAccentActive : hovered ? kAccentHover : kAccent),
                     8 * scale());
    d->AddText(ImVec2(p.x + (size.x - words.x) * 0.5f, p.y + (size.y - fs) * 0.5f), u32(kOnAccent), label,
               detail::label_end(label));
    ImGui::PopFont();
    return pressed;
}

// Quiet words to press for more ("2 notes about this card"), with a small
// arrowhead before them.
inline bool link(const char *label) {
    ImGui::PushFont(nullptr, base_font() * kSmall);
    const float fs = ImGui::GetFontSize();
    const ImVec2 words = ImGui::CalcTextSize(label, nullptr, true);
    const float mark = fs * 0.62f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, ImVec2(mark + words.x, fs * 1.15f), ImGuiButtonFlags_EnableNav);
    note_item();
    const ImVec4 &colour = ImGui::IsItemHovered() || ImGui::IsItemActive() ? kText : kTextDim;
    ImDrawList *d = ImGui::GetWindowDrawList();
    const float cy = p.y + fs * 0.60f, r = fs * 0.20f;
    d->AddTriangleFilled(ImVec2(p.x + r * 0.2f, cy - r), ImVec2(p.x + r * 0.2f, cy + r), ImVec2(p.x + r * 1.5f, cy), u32(colour));
    d->AddText(ImVec2(p.x + mark, p.y), u32(colour), label, detail::label_end(label));
    ImGui::PopFont();
    return pressed;
}

// ---------------------------------------------------------------- the switch

// How wide a switch is, to place one at the right end of a row.
inline float toggle_width() { return std::round(std::round(ImGui::GetFontSize() * 1.10f) * 1.84f); }

// On or off. True when it was switched. On a control's line.
inline bool toggle(const char *id, bool *on) {
    const float fs = ImGui::GetFontSize(), frame = ImGui::GetFrameHeight();
    const float h = std::round(fs * 1.10f), w = std::round(h * 1.84f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, frame), ImGuiButtonFlags_EnableNav);
    note_item();
    if (pressed) *on = !*on;
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 a(p.x, p.y + std::round((frame - h) * 0.5f)), b(a.x + w, a.y + h);
    ImDrawList *d = ImGui::GetWindowDrawList();
    d->AddRectFilled(a, b, u32(*on ? (hovered ? kAccentHover : kAccent) : (hovered ? kControlActive : kControlHover)), h * 0.5f);
    const float cx = *on ? b.x - h * 0.5f : a.x + h * 0.5f;
    d->AddCircleFilled(ImVec2(cx, a.y + h * 0.5f), h * 0.37f, u32(*on ? kOnAccent : kTextDim));
    return pressed;
}

// ---------------------------------------------------------------- the segmented choice

// One of a few words in a row, the chosen one filled with the accent. The
// choice pressed if it is not the current one, else -1.
//   disabled  a bit per choice that cannot be picked here (the current one
//             stays pressable-looking: it is what is set)
//   width     > 0: that wide in all, the choices sharing it (evenly, if the
//             longest word has room in an even share); 0: as wide as the
//             words need
//   height    0: a control's height
// How wide segmented_n draws these words (with width 0), to place it at the
// right end of a row.
inline float segmented_width(const char *const *labels, int count) {
    const float s = scale(), inner = ImGui::GetFontSize() * 0.60f;
    float total = std::round(3 * s) * 2 + std::round(2 * s) * float(count - 1);
    ImGui::PushFont(bold_font(), 0.0f);
    for (int i = 0; i < count; ++i) total += ImGui::CalcTextSize(labels[i], nullptr, true).x + inner * 2;
    ImGui::PopFont();
    return total;
}

inline int segmented_n(const char *id, const char *const *labels, int count, int current, unsigned disabled = 0,
                       float width = 0.0f, float height = 0.0f) {
    const float s = scale(), fs = ImGui::GetFontSize();
    if (height <= 0) height = ImGui::GetFrameHeight();
    const float pad = std::round(3 * s), gap = std::round(2 * s), inner = fs * 0.60f;
    constexpr int kMost = 8;
    count = std::min(count, kMost);
    float w[kMost] = {}, total = pad * 2 + gap * float(count - 1);
    ImGui::PushFont(bold_font(), 0.0f);  // the chosen one is bold: every one is given that room
    for (int i = 0; i < count; ++i) w[i] = ImGui::CalcTextSize(labels[i], nullptr, true).x + inner * 2;
    ImGui::PopFont();
    if (width > 0) {
        // Even shares where every word has room in one; else each word's
        // own width and an even share of what is left (or short).
        const float each = (width - total) / float(count);
        float words = 0;
        bool even = true;
        for (int i = 0; i < count; ++i) {
            words += w[i];
            even = even && w[i] - inner * 2 + fs * 0.7f <= each;
        }
        const float spare = (width - total - words) / float(count);
        for (int i = 0; i < count; ++i) w[i] = even ? each : w[i] + spare;
    }
    for (int i = 0; i < count; ++i) total += w[i];

    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *d = ImGui::GetWindowDrawList();
    d->AddRectFilled(p, ImVec2(p.x + total, p.y + height), u32(kControl), 8 * s);
    ImGui::BeginGroup();
    ImGui::PushID(id);
    int clicked = -1;
    float x = p.x + pad;
    for (int i = 0; i < count; ++i) {
        const bool on = i == current;
        const bool off = (disabled >> i & 1u) != 0 && !on;
        ImGui::SetCursorScreenPos(ImVec2(x, p.y + pad));
        if (off) ImGui::BeginDisabled();
        ImGui::PushID(i);
        const ImVec2 size(w[i], height - pad * 2);
        const bool pressed = ImGui::InvisibleButton("##choice", size, ImGuiButtonFlags_EnableNav);
        ImGui::PopID();
        note_item();
        const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        const ImVec2 a(x, p.y + pad), b(a.x + size.x, a.y + size.y);
        if (on)
            d->AddRectFilled(a, b, u32(hovered ? kAccentHover : kAccent), 6 * s);
        else if (held || hovered)
            d->AddRectFilled(a, b, u32(held ? kControlActive : kControlHover), 6 * s);
        if (on) ImGui::PushFont(bold_font(), 0.0f);
        const ImVec2 words = ImGui::CalcTextSize(labels[i], nullptr, true);
        d->AddText(ImVec2(a.x + (size.x - words.x) * 0.5f, a.y + (size.y - fs) * 0.5f), u32(on ? kOnAccent : kText), labels[i],
                   detail::label_end(labels[i]));
        if (on) ImGui::PopFont();
        if (off) ImGui::EndDisabled();
        if (pressed && !on) clicked = i;
        x += w[i] + gap;
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(total, height));
    ImGui::EndGroup();
    return clicked;
}

// The same for a fixed list of words.
template <int N>
int segmented(const char *id, const char *const (&labels)[N], int current, unsigned disabled = 0, float width = 0.0f,
              float height = 0.0f) {
    return segmented_n(id, labels, N, current, disabled, width, height);
}

// The same choice as cells of two lines each: a large word and a line under
// it saying what it comes to ("x4", "30 -> 120 fps"). `width` wide in all.
inline int cells(const char *id, const char *const *titles, const char *const *notes, int count, int current,
                 unsigned disabled, float width) {
    const float s = scale();
    const float pad = std::round(3 * s), gap = std::round(3 * s);
    const float title_size = base_font() * 1.04f, note_size = base_font() * 0.76f;
    ImGui::PushFont(bold_font(), title_size);
    const float title_h = ImGui::GetFontSize();
    ImGui::PopFont();
    ImGui::PushFont(nullptr, note_size);
    const float note_h = ImGui::GetFontSize();
    ImGui::PopFont();
    const float cell_h = std::round(title_h + note_h + 9 * s), height = cell_h + pad * 2;
    const float cell_w = (width - pad * 2 - gap * float(count - 1)) / float(count);

    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *d = ImGui::GetWindowDrawList();
    d->AddRectFilled(p, ImVec2(p.x + width, p.y + height), u32(kControl), 8 * s);
    ImGui::BeginGroup();
    ImGui::PushID(id);
    int clicked = -1;
    for (int i = 0; i < count; ++i) {
        const bool on = i == current;
        const bool off = (disabled >> i & 1u) != 0 && !on;
        const ImVec2 a(p.x + pad + (cell_w + gap) * float(i), p.y + pad), b(a.x + cell_w, a.y + cell_h);
        ImGui::SetCursorScreenPos(a);
        if (off) ImGui::BeginDisabled();
        ImGui::PushID(i);
        const bool pressed = ImGui::InvisibleButton("##cell", ImVec2(cell_w, cell_h), ImGuiButtonFlags_EnableNav);
        ImGui::PopID();
        note_item();
        const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        if (on)
            d->AddRectFilled(a, b, u32(hovered ? kAccentHover : kAccent), 6 * s);
        else if (held || hovered)
            d->AddRectFilled(a, b, u32(held ? kControlActive : kControlHover), 6 * s);
        const ImVec4 colour = on ? kOnAccent : kText;
        ImGui::PushFont(bold_font(), title_size);
        ImVec2 words = ImGui::CalcTextSize(titles[i]);
        d->AddText(ImVec2(a.x + (cell_w - words.x) * 0.5f, a.y + 3 * s), u32(colour), titles[i]);
        ImGui::PopFont();
        ImGui::PushFont(nullptr, note_size);
        words = ImGui::CalcTextSize(notes[i]);
        d->AddText(ImVec2(a.x + (cell_w - words.x) * 0.5f, a.y + 4 * s + title_h), u32(faded(colour, 0.85f)), notes[i]);
        ImGui::PopFont();
        if (off) ImGui::EndDisabled();
        if (pressed && !on) clicked = i;
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, height));
    ImGui::EndGroup();
    return clicked;
}

// ---------------------------------------------------------------- the slider

// A thin track with a round knob, `width` wide (0: what is left of the
// line; under 0: that much less), on a control's line. t is where the knob
// is, 0 to 1. True while the mouse moves it. It is the last item after the
// call (IsItemActive, IsItemFocused, IsItemDeactivated).
inline bool slider_track(const char *id, float *t, float width = 0.0f) {
    const float fs = ImGui::GetFontSize(), frame = ImGui::GetFrameHeight(), s = scale();
    if (width <= 0) width = ImGui::GetContentRegionAvail().x + width;
    width = std::max(width, fs * 3);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(width, frame), ImGuiButtonFlags_EnableNav);
    note_item();
    const bool held = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const float knob = fs * 0.36f;
    const float x0 = p.x + knob, x1 = p.x + width - knob;
    bool changed = false;
    if (held && x1 > x0) {
        const float to = std::clamp((ImGui::GetIO().MousePos.x - x0) / (x1 - x0), 0.0f, 1.0f);
        changed = to != *t;
        *t = to;
    }
    const float at = x0 + (x1 - x0) * std::clamp(*t, 0.0f, 1.0f), cy = p.y + frame * 0.5f;
    const float half = std::max(1.5f, 3 * s);
    ImDrawList *d = ImGui::GetWindowDrawList();
    d->AddRectFilled(ImVec2(p.x, cy - half), ImVec2(p.x + width, cy + half), u32(kControlHover), half);
    if (at > p.x + half) d->AddRectFilled(ImVec2(p.x, cy - half), ImVec2(at, cy + half), u32(kAccent), half);
    d->AddCircleFilled(ImVec2(at, cy), knob + (held || ImGui::IsItemHovered() ? s : 0.0f), u32(kText));
    return changed;
}

// A whole number between lo and hi. Left and Right step it by `step` while
// it holds the keyboard's place (no Enter first). True when it changed.
inline bool slider_int(const char *id, int *value, int lo, int hi, int step, float width = 0.0f) {
    hi = std::max(lo, hi);
    float t = hi > lo ? float(*value - lo) / float(hi - lo) : 0.0f;
    bool changed = false;
    if (slider_track(id, &t, width)) {
        const int to = lo + int(std::lround(t * float(hi - lo)));
        changed = to != *value;
        *value = to;
    }
    if (ImGui::IsItemFocused() && !ImGui::IsItemActive()) {
        int by = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) by -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) by += step;
        const int to = std::clamp(*value + by, lo, hi);
        if (to != *value) {
            *value = to;
            changed = true;
        }
    }
    return changed;
}

// The same for a number with a fraction.
inline bool slider_float(const char *id, float *value, float lo, float hi, float step, float width = 0.0f) {
    hi = std::max(lo, hi);
    float t = hi > lo ? (*value - lo) / (hi - lo) : 0.0f;
    bool changed = false;
    if (slider_track(id, &t, width)) {
        const float to = lo + t * (hi - lo);
        changed = to != *value;
        *value = to;
    }
    if (ImGui::IsItemFocused() && !ImGui::IsItemActive()) {
        float by = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) by -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) by += step;
        const float to = std::clamp(*value + by, lo, hi);
        if (by != 0 && to != *value) {
            *value = to;
            changed = true;
        }
    }
    return changed;
}

// ---------------------------------------------------------------- the list box

// Dear ImGui's combo in this look: the chosen item's text, cut with dots if
// it is too long, and an arrowhead at the right. Width as SetNextItemWidth
// says. End it with ImGui::EndCombo() when it returns true.
inline bool begin_combo(const char *id, const std::string &preview, ImGuiComboFlags flags = 0) {
    const ImGuiStyle &st = ImGui::GetStyle();
    const float s = scale(), fs = ImGui::GetFontSize(), frame = ImGui::GetFrameHeight();
    const float w = ImGui::CalcItemWidth();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *d = ImGui::GetWindowDrawList();
    const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(p, ImVec2(p.x + w, p.y + frame));
    d->AddRectFilled(p, ImVec2(p.x + w, p.y + frame), u32(hovered ? kControlHover : kControl), st.FrameRounding);
    const float arrow = frame * 0.86f;
    const std::string shown = fit_text(preview, w - st.FramePadding.x - arrow);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * s, 8 * s));
    const bool open = ImGui::BeginCombo(id, shown.c_str(), flags | ImGuiComboFlags_NoArrowButton);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    if (!open) note_item();
    const ImVec2 c(p.x + w - arrow * 0.58f, p.y + frame * 0.5f);
    const float r = fs * 0.20f;
    const ImVec2 head[3] = {ImVec2(c.x - r, c.y - r * 0.5f), ImVec2(c.x, c.y + r * 0.5f), ImVec2(c.x + r, c.y - r * 0.5f)};
    d->AddPolyline(head, 3, u32(kTextDim), 0, std::max(1.4f, 1.7f * s));
    return open;
}

// ---------------------------------------------------------------- the notice

// A boxed message: the status's sign, a bold first line (title, may be
// null) and text, with a button for what can be done about it (action, may
// be null) at the right, or under the text where there is no room beside
// it. A warning and a problem have an outline in their colour, and always
// their sign and words. As wide as the window has left. True when the
// button was pressed.
inline bool notice(const char *id, Status status, const char *title, const char *body, const char *action = nullptr) {
    const ImGuiStyle &st = ImGui::GetStyle();
    const float s = scale();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::PushFont(nullptr, base_font() * 0.9f);
    const float fs = ImGui::GetFontSize();
    const float pad_x = fs * 0.62f, pad_y = fs * 0.50f, icon = fs * 0.92f, gap = fs * 0.52f;
    const float button = action ? action_width(action) : 0.0f;
    const float beside = width - pad_x * 2 - icon - gap - button - gap;
    const bool below = action && (button > width * 0.36f || beside < fs * 8);
    const float text_w = below || !action ? width - pad_x * 2 - icon - gap : beside;

    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *d = ImGui::GetWindowDrawList();
    ImDrawListSplitter layers;
    layers.Split(d, 2);
    layers.SetCurrentChannel(d, 1);
    ImGui::BeginGroup();
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(st.ItemSpacing.x, 2 * s));
    const ImVec2 text_at(p.x + pad_x + icon + gap, p.y + pad_y);
    ImGui::SetCursorScreenPos(text_at);
    if (title && *title) {
        ImGui::PushFont(bold_font(), 0.0f);
        wrapped_text(title, text_w);
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(text_at.x, ImGui::GetCursorScreenPos().y));
    }
    if (body && *body) wrapped_text(body, text_w);
    float bottom = ImGui::GetItemRectMax().y;
    bool pressed = false;
    if (action) {
        ImGui::SetCursorScreenPos(below ? ImVec2(text_at.x, bottom + 7 * s) : ImVec2(p.x + width - pad_x - button, text_at.y - 2 * s));
        pressed = action_button(action, false);
        bottom = std::max(bottom, ImGui::GetItemRectMax().y);
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    const float height = bottom - p.y + pad_y;
    status_icon(d, ImVec2(p.x + pad_x + icon * 0.5f, text_at.y + fs * 0.54f), icon, status, u32(status_colour(status)));
    layers.SetCurrentChannel(d, 0);
    const ImVec2 end(p.x + width, p.y + height);
    d->AddRectFilled(p, end, u32(kControl), 8 * s);
    if (status == Status::Warn || status == Status::Bad)
        d->AddRect(p, end, u32(status_colour(status)), 8 * s, 0, std::max(1.2f, 1.5f * s));
    layers.Merge(d);
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, height));
    ImGui::EndGroup();
    ImGui::PopFont();
    return pressed;
}

// ---------------------------------------------------------------- frames, real and added

// Four game frames in a strip, each followed by the frames added to it:
// the game's own in the text's colour, the added ones in the accent.
// multiplier: frames shown per game frame (1: none added).
inline void cadence_strip(int multiplier, float width, float height) {
    multiplier = std::clamp(multiplier, 1, 8);
    const float s = scale(), gap = std::max(2.0f, 3 * s);
    const int bars = 4 * multiplier;
    const float real_part = 1.3f, units = 4 * (real_part + float(multiplier - 1));
    const float unit = (width - gap * float(bars - 1)) / units;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *d = ImGui::GetWindowDrawList();
    float x = p.x;
    for (int i = 0; i < bars; ++i) {
        const bool real = i % multiplier == 0;
        const float w = unit * (real ? real_part : 1.0f);
        d->AddRectFilled(ImVec2(x, p.y), ImVec2(x + w, p.y + height), u32(real ? kText : faded(kAccent, 0.8f)), 2 * s);
        x += w + gap;
    }
    ImGui::Dummy(ImVec2(width, height));
}

}  // namespace app::ui
