// SPDX-License-Identifier: MIT
#include "menu.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include <imgui.h>

#include "app_env.h"
#include "app_version.h"
#include "framegen_text.h"
#include "ini_edit.h"
#include "keep_edit.h"
#include "ui_style.h"
#include "ui_widgets.h"

// The menu, top to bottom (at 1080 lines it is 499 pixels wide and nothing
// in it scrolls):
//
//   the name, and how to close it
//   the readout: what is shown now in large figures, the strip of real and
//     added frames, the picture and the screen as chips, and a quiet line
//     when frame generation has something to say
//   a warning, when there is one, with a button for what can be done
//   four tabs, named like the launcher's: Frames, Picture, Sound, Screen;
//     or, in their place, the page of the areas left alone
//   the open tab's rows: a label at the left, its control at the right
//   one line of help, for the row the mouse or the keyboard is on
//   Close and Quit, and the keys that work while playing
namespace app::overlay {
namespace {

using ui::kTextDim;

const char *const kDot = " \xC2\xB7 ";

// A value the menu shows that mpv owns: mpv's value, except for a moment
// after the menu changed it (until mpv's answer comes back) and while it is
// being dragged.
struct Synced {
    int value = 0;
    double hold_until = -1;
    void follow(int from_mpv, double now) {
        if (now >= hold_until) value = from_mpv;
    }
    void changed(double now) { hold_until = now + 0.75; }
};

enum MenuTab { kTabFrames, kTabPicture, kTabSound, kTabScreen, kTabMouse, kTabCount };

struct MenuState {
    int last_frame = -10;
    Synced volume, boost, delay;
    int borderless = -1;  // full screen (1) or a window (0) since the menu switched it; -1: as mpv started
    Synced hdr_peak;      // the screen's peak the picture is fitted to (mpv's target-peak)
    int hdr_dynamic = -1; // measure each frame's peak, since the menu switched it; -1: as the settings file has it
    int signal = -1;      // Automatic (0), SDR (1) or HDR (2) since the menu switched it while playing; -1: as mpv started
    int tab = kTabFrames;
    bool areas = false;  // the page of the areas left alone is open in place of the tabs
    float scale = 1;     // 1 at 1080 lines
    // The help line: the help of the row the mouse or the keyboard came to
    // last. Empty: the page's first row's.
    std::string help;
    std::string hovered, focused;      // the rows they were on at the last frame
    std::string hover_now, focus_now;  // and on this one, as the rows are drawn
    std::string first;                 // the first row's help on this frame
    // A warning stays for a few seconds after what it warns of was last so:
    // one that came and went every second would move the tabs under the mouse.
    struct Held {
        Line line;
        double until = -1;
    } held[2];  // why fewer frames are added; what the screen does to their pacing
};
MenuState g_menu;

Line held_line(MenuState::Held &held, const Line &line, double now) {
    if (line.matters) {
        held.line = line;
        held.until = now + 3.0;
        return line;
    }
    return now < held.until ? held.line : line;
}

// The mouse on the areas frame generation leaves alone (keep_editor): which
// one is chosen, what the mouse holds, and whether the menu makes way.
struct KeepEdit {
    int frame = -10;       // the frame the editor was last drawn on
    int chosen = -1;       // the area the sliders, the remove box and Delete work on; -1: none
    int arm = -1;          // the next drag on the picture draws a rectangle (0) or an ellipse (1); -1: none
    bool drawing = false;  // that drag is under way
    keep_edit::Grip grip = keep_edit::Grip::None;  // the part of the chosen area the mouse holds
    keep_edit::Point from;  // where the button went down
    KeepRegion start;       // the held area as it was then
    bool moved = false;     // the mouse has gone far enough from there to mean a drag, not a click
    bool unsaved = false;   // changed since the settings file was written (it is written when the mouse lets go)
    bool left = false;      // the menu is on the left, to reach an area under it
    bool aside = false;     // the menu makes way on the next frame: an area is being drawn or dragged
};
KeepEdit g_keep;

void save(const std::wstring &settings_path, const char *section, const char *key, const std::string &value) {
    if (settings_path.empty()) return;
    if (!ini_save_values(settings_path, {{section, key, value}}))
        reshade::log::message(reshade::log::level::warning, APP_NAME " menu: could not save a setting to " APP_INI);
}

void save(const MenuFrame &f, const char *section, const char *key, const std::string &value) {
    save(f.settings_path, section, key, value);
}

// A 10-bit capture (env PICTURE_LIVE, from the exe): it carries SDR or HDR
// alike, and the app switches what it is shown as without starting the
// picture again.
bool picture_live() {
    static const bool live = env(L"PICTURE_LIVE") == L"1";
    return live;
}

// The Signal choice: Automatic (0), SDR (1) or HDR (2).
int signal_mode(const MenuFrame &f) {
    return g_menu.signal >= 0 ? g_menu.signal : f.source_auto ? 0 : f.source_hdr ? 2 : 1;
}

// Whether the capture is shown as HDR now. It is as mpv started unless the
// capture is a 10-bit one. Then SDR and HDR are as chosen; with Automatic the
// app follows the console, and mpv's output tells which it is where an HDR
// signal goes to the screen as HDR10 (an SDR one never does). Where HDR is
// tone mapped to SDR the output is the same for both: as mpv started.
bool signal_hdr(const MenuFrame &f, const MpvState &mpv) {
    if (!picture_live()) return f.source_hdr;
    // The app says what it shows the signal as: right also where HDR is tone
    // mapped to SDR, which the output cannot tell apart.
    if (!mpv.signal.empty()) return mpv.signal == "hdr";
    if (const int mode = signal_mode(f); mode != 0) return mode == 2;
    if (f.hdr_output != "hdr" || mpv.out_gamma.empty()) return f.source_hdr;
    return mpv.out_gamma == "pq";
}

// The editor of the areas is gone (closed, or the menu is): what the mouse
// held is let go, and a change not yet in the settings file is written there.
void keep_edit_over(const Host &host, const std::wstring &settings_path) {
    if (g_keep.unsaved && host.framegen_keep)
        save(settings_path, "framegen", "keep", format_keep_regions(host.framegen_keep()));
    g_keep = KeepEdit{};
}

// Another tab or page: the help line starts again at its first row.
void forget_help() {
    g_menu.help.clear();
    g_menu.hovered.clear();
    g_menu.focused.clear();
}

// ---------------------------------------------------------------- rows

// A row of the open tab: a label at the left, its control at the right (or,
// for a wide control, under the label), at least 48 pixels high at 1080
// lines. The row lights up a little under the mouse, and its help goes to
// the help line when the mouse or the keyboard comes to it.
struct RowDraw {
    ImVec2 start;
    float width = 0;
    ImDrawListSplitter layers;
};
RowDraw g_row;

float px(float v) { return v * g_menu.scale; }

void row_begin() {
    ui::take_focused();
    ImDrawList *d = ImGui::GetWindowDrawList();
    g_row.start = ImGui::GetCursorScreenPos();
    g_row.width = ImGui::GetContentRegionAvail().x;
    g_row.layers.Split(d, 2);
    g_row.layers.SetCurrentChannel(d, 1);
    ImGui::BeginGroup();
    ImGui::SetCursorScreenPos(ImVec2(g_row.start.x, g_row.start.y + px(4)));
}

void row_end(const std::string &help) {
    ImGui::EndGroup();
    const float height = std::max(px(48), ImGui::GetItemRectMax().y - g_row.start.y + px(4));
    const ImVec2 a(g_row.start.x - px(8), g_row.start.y), b(g_row.start.x + g_row.width + px(8), g_row.start.y + height);
    const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a, b);
    const bool focused = ui::take_focused();
    ImDrawList *d = ImGui::GetWindowDrawList();
    g_row.layers.SetCurrentChannel(d, 0);
    if (hovered) d->AddRectFilled(a, b, ui::u32(ui::rgb(0xffffff, 0.05f)), px(8));
    g_row.layers.Merge(d);
    ImGui::SetCursorScreenPos(g_row.start);
    ImGui::Dummy(ImVec2(g_row.width, height));
    if (g_menu.first.empty()) g_menu.first = help;
    if (hovered) g_menu.hover_now = help;
    if (focused) g_menu.focus_now = help;
}

// The row's label, on its control's line; with the key that does the same
// while playing.
void label(const char *text, const char *key = nullptr) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    if (key) {
        ImGui::SameLine(0, px(10));
        ui::key_cap(key, ImGui::GetFrameHeight());
    }
}

// Where a control `width` wide goes: the right end of the row.
void at_right(float width) {
    ImGui::SetCursorScreenPos(ImVec2(g_row.start.x + g_row.width - width, g_row.start.y + px(4)));
}

// A label with its control under it: the label's line, with a figure at its
// right end ("100 %") or a quiet word after it.
void label_above(const char *text, const std::string &figure = {}, const char *note = nullptr) {
    ImGui::TextUnformatted(text);
    if (note && *note) {
        ImGui::SameLine(0, px(10));
        ImGui::PushFont(nullptr, ui::base_font() * 0.85f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(3));
        ui::text(note, kTextDim);
        ImGui::PopFont();
    }
    if (!figure.empty()) {
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(
            ImVec2(g_row.start.x + g_row.width - ImGui::CalcTextSize(figure.c_str()).x, g_row.start.y + px(4)));
        ImGui::TextUnformatted(figure.c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(g_row.start.x, g_row.start.y + px(4) + ImGui::GetFontSize() + px(5)));
}

// A switch at the right end of a row. True when it was switched.
bool switch_row(const char *text, const char *key, const char *id, bool *on, bool enabled, const std::string &help) {
    row_begin();
    label(text, key);
    at_right(ui::toggle_width());
    if (!enabled) ImGui::BeginDisabled();
    const bool switched = ui::toggle(id, on);
    if (!enabled) ImGui::EndDisabled();
    row_end(help);
    return switched;
}

// A segmented choice at the right end of a row. The choice pressed, or -1.
int choice_row(const char *text, const char *id, const char *const *choices, int count, int current,
               const std::string &help) {
    row_begin();
    label(text);
    at_right(ui::segmented_width(choices, count));
    const int picked = ui::segmented_n(id, choices, count, current);
    row_end(help);
    return picked;
}

template <int N>
int choice_row(const char *text, const char *id, const char *const (&choices)[N], int current, const std::string &help) {
    return choice_row(text, id, choices, N, current, help);
}

// The same under its label, across the row.
int wide_choice_row(const char *text, const char *note, const char *id, const char *const *choices, int count,
                    int current, unsigned disabled, const std::string &help) {
    row_begin();
    label_above(text, {}, note);
    const int picked = ui::segmented_n(id, choices, count, current, disabled, g_row.width);
    row_end(help);
    return picked;
}

// A slider under its label, the figure at the label's right. True when moved.
bool slider_row(const char *text, const char *id, int *value, int lo, int hi, int step, const char *format,
                const std::string &help) {
    char figure[32];
    std::snprintf(figure, sizeof(figure), format, *value);
    row_begin();
    label_above(text, figure);
    const bool moved = ui::slider_int(id, value, lo, hi, step, g_row.width);
    row_end(help);
    return moved;
}

// Quiet small text, wrapped: what is, as opposed to what can be set.
void fact(const std::string &text) {
    if (text.empty()) return;
    ImGui::PushFont(nullptr, ui::base_font() * 0.85f);
    ui::wrapped_text(text.c_str(), ImGui::GetContentRegionAvail().x, kTextDim);
    ImGui::PopFont();
}

// ---------------------------------------------------------------- the areas left alone

// An area's outline on the picture: the accent between two dark edges, so
// that it shows on any picture.
void keep_outline(ImDrawList *draw, const KeepRegion &r, const keep_edit::Box &b, ImU32 colour, float thickness) {
    const ImU32 dark = IM_COL32(0, 0, 0, 140);
    const float edge = thickness + 2.0f * px(2);
    if (r.ellipse) {
        const ImVec2 centre((b.left + b.right) * 0.5f, (b.top + b.bottom) * 0.5f);
        const ImVec2 radius((b.right - b.left) * 0.5f, (b.bottom - b.top) * 0.5f);
        draw->AddEllipse(centre, radius, dark, 0.0f, 0, edge);
        draw->AddEllipse(centre, radius, colour, 0.0f, 0, thickness);
    } else {
        draw->AddRect(ImVec2(b.left, b.top), ImVec2(b.right, b.bottom), dark, 0.0f, 0, edge);
        draw->AddRect(ImVec2(b.left, b.top), ImVec2(b.right, b.bottom), colour, 0.0f, 0, thickness);
    }
}

void keep_fill(ImDrawList *draw, const KeepRegion &r, const keep_edit::Box &b, ImU32 colour) {
    if (r.ellipse)
        draw->AddEllipseFilled(ImVec2((b.left + b.right) * 0.5f, (b.top + b.bottom) * 0.5f),
                               ImVec2((b.right - b.left) * 0.5f, (b.bottom - b.top) * 0.5f), colour);
    else
        draw->AddRectFilled(ImVec2(b.left, b.top), ImVec2(b.right, b.bottom), colour);
}

// An area's number in a small filled box at its top left corner, with "HUD"
// after it where only the HUD in the area is left alone.
void keep_number(ImDrawList *draw, const KeepRegion &r, const keep_edit::Box &b, int number) {
    char text[16];
    std::snprintf(text, sizeof(text), r.find_hud ? "%d HUD" : "%d", number);
    ImFont *font = ui::bold_font() ? ui::bold_font() : ImGui::GetFont();
    const float size = ui::base_font() * 0.85f * g_menu.scale;
    const ImVec2 words = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 a(b.left, b.top), c(b.left + words.x + px(14), b.top + words.y + px(2));
    // (The colours as they are, not through the style: the menu fades while
    // an area is drawn, and what is on the picture must not.)
    draw->AddRectFilled(a, c, ImGui::ColorConvertFloat4ToU32(ui::kAccent));
    draw->AddText(font, size, ImVec2(a.x + px(7), a.y + px(1)), ImGui::ColorConvertFloat4ToU32(ui::kOnAccent), text);
}

// A percent of the picture, to a tenth, in a row of its own: the label, the
// slider, the figure. True when moved. held: set while the mouse holds it.
bool percent_row(const char *text, const char *id, float *value, float lo, float hi, bool &held) {
    hi = std::max(lo, hi);
    char figure[24];
    std::snprintf(figure, sizeof(figure), "%.1f %%", *value);
    row_begin();
    label(text);
    const float label_room = px(86), figure_room = px(92);
    ImGui::SetCursorScreenPos(ImVec2(g_row.start.x + label_room, g_row.start.y + px(4)));
    const bool moved = ui::slider_float(id, value, lo, hi, 0.5f, g_row.width - label_room - figure_room);
    held = held || ImGui::IsItemActive();
    at_right(ImGui::CalcTextSize(figure).x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(figure);
    row_end("Left and Top place the area, Width and Height size it, in percent of the picture.");
    return moved;
}

// The areas frame generation leaves alone (a HUD, a map, subtitles), outlined
// on the picture while their page is open and worked on there with the mouse
// (keep_edit.h): "Draw" and a drag make one, a drag inside one moves it, its
// edges and corners size it, the small box beside the chosen one or Delete
// removes it. The page has the chosen one's shape, what is left alone in it,
// and sliders for the rest.
//
// The picture here is the whole image the menu is drawn on (the back buffer,
// black bars included): frame generation counts an area's percent of that.
//
// The mouse is the picture's where Dear ImGui does not want it
// (WantCaptureMouse): not over the menu, and not while a slider is held. A
// drag that began on the picture stays the picture's over the menu too
// (Dear ImGui leaves such a drag alone). While one is under way, or one is
// awaited after "Draw", the menu fades and takes no mouse (draw_menu), so an
// area can be drawn where the menu is. mpv sees none of it: ReShade keeps
// the mouse from it while the menu is open (overlay.cpp).
//
// The areas go to frame generation at once (set_framegen_keep: its own list
// and ReShade's settings in memory, which ReShade writes a second after the
// last change); the settings file is written when the mouse lets go.
void keep_editor(const MenuFrame &f) {
    namespace ke = keep_edit;
    const Host &h = *f.host;
    if (!h.framegen_keep || !h.set_framegen_keep) return;
    KeepEdit &e = g_keep;
    std::vector<KeepRegion> regions = h.framegen_keep();
    const std::vector<KeepRegion> before = regions;
    e.frame = ImGui::GetFrameCount();  // draw_menu sees by this that the page was drawn
    if (e.chosen < 0 || e.chosen >= int(regions.size())) {
        e.chosen = -1;
        e.grip = ke::Grip::None;
    }
    const bool room = int(regions.size()) < kMaxKeepRegions;
    if (!room) {
        e.arm = -1;
        e.drawing = false;
    }

    const ImGuiIO &io = ImGui::GetIO();
    const ke::Size picture{io.DisplaySize.x, io.DisplaySize.y};
    const ke::Point mouse{io.MousePos.x, io.MousePos.y};
    const float reach = 8.0f * f.scale;         // how near an edge a press takes it
    const float remove_side = 18.0f * f.scale;  // the remove box
    const bool on_picture = ImGui::IsMousePosValid() && !io.WantCaptureMouse;
    const bool pressed = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool let_go = !ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool cancelled = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    int remove = -1;
    ke::Hit hover;  // what a press would take hold of now

    if (e.drawing) {
        // A new area: made when the mouse lets go after a real drag (a click
        // draws nothing and waits for the drag).
        if (cancelled) {
            e.drawing = false;
            e.arm = -1;
        } else if (let_go) {
            e.drawing = false;
            if (ke::is_drag(e.from, mouse, reach)) {
                regions.push_back(ke::drawn(e.arm == 1, e.from, mouse, picture));
                e.chosen = int(regions.size()) - 1;
                e.arm = -1;
            }
        }
    } else if (e.grip != ke::Grip::None) {
        // The chosen area, held: it follows the mouse from where it was when
        // the button went down; the right button puts it back.
        KeepRegion &r = regions[size_t(e.chosen)];
        if (cancelled) {
            r = e.start;
            e.grip = ke::Grip::None;
        } else if (e.grip == ke::Grip::Remove) {
            if (let_go) {
                if (ke::remove_box(r, picture, remove_side, reach).has(mouse)) remove = e.chosen;
                e.grip = ke::Grip::None;
            }
        } else {
            // A click that only chooses it does not nudge it.
            e.moved = e.moved || ke::is_drag(e.from, mouse, 3.0f * f.scale);
            if (e.moved) r = ke::dragged(e.start, e.grip, e.from, mouse, picture);
            if (let_go) e.grip = ke::Grip::None;
        }
    } else if (e.arm >= 0) {
        if (cancelled) {
            e.arm = -1;
        } else if (on_picture && pressed) {
            e.drawing = true;
            e.from = mouse;
        }
    } else {
        if (on_picture) {
            hover = ke::hit_at(regions, e.chosen, mouse, picture, reach, remove_side);
            if (pressed) {
                e.chosen = hover.index;  // the bare picture: none is chosen
                if (hover.index >= 0) {
                    e.grip = hover.grip;
                    e.from = mouse;
                    e.start = regions[size_t(hover.index)];
                    e.moved = false;
                }
            }
        }
        if (e.chosen >= 0 && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) remove = e.chosen;
    }
    // A press on the picture takes Dear ImGui's focus from the menu; the
    // keys need it there again.
    if (!e.drawing && e.grip == ke::Grip::None && let_go && !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        ImGui::SetWindowFocus();

    // (Every help text fits the help line's two lines: about 110 letters.)
    row_begin();
    label("Draw");
    int shape = -1;
    if (room) {
        const float both = ui::action_width("Rectangle##draw") + px(8) + ui::action_width("Ellipse##draw");
        at_right(both);
        if (ui::action_button("Rectangle##draw")) shape = 0;
        ImGui::SameLine(0, px(8));
        if (ui::action_button("Ellipse##draw")) shape = 1;
    }
    row_end(room ? "Press a shape, then drag on the picture: over a HUD, a map or the subtitles."
                 : "That is the most there can be. Drag an area to move it, an edge or a corner to size it.");
    if (shape >= 0) {
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            e.arm = shape;  // clicked: the mouse draws it
        } else {
            // Chosen with the keys: in the middle, small, to be placed with the sliders.
            regions.push_back({shape == 1, 40, 40, 20, 20});
            e.chosen = int(regions.size()) - 1;
        }
    }
    if (h.framegen_hud && h.set_framegen_hud) {
        bool everywhere = h.framegen_hud();
        if (switch_row("Find the HUD everywhere", nullptr, "##hud", &everywhere, true,
                       "Added frames show the game's own frame wherever a HUD is found, not only in the areas.")) {
            h.set_framegen_hud(everywhere);
            save(f, "framegen", "hud", everywhere ? "1" : "0");
        }
    }

    bool slider_held = false;
    if (!regions.empty()) {
        // The areas by number, and beside them the button that removes the
        // chosen one.
        static const char *const kNumbers[] = {"1", "2", "3", "4", "5", "6", "7", "8"};
        static_assert(kMaxKeepRegions <= int(sizeof(kNumbers) / sizeof(kNumbers[0])), "a number for every area");
        const int count = std::min(int(regions.size()), kMaxKeepRegions);
        const bool one_chosen = e.chosen >= 0 && e.chosen < int(regions.size());
        const float remove_w = one_chosen ? ui::action_width("Remove") + px(10) : 0.0f;
        row_begin();
        label("Area");
        at_right(ui::segmented_width(kNumbers, count) + remove_w);
        const int pick = ui::segmented_n("##area", kNumbers, count, e.chosen);
        if (one_chosen) {
            ImGui::SameLine(0, px(10));
            if (ui::action_button("Remove")) remove = e.chosen;
        }
        row_end("Drag an area to move it, an edge or a corner to size it. Delete or its small box removes it.");
        if (pick >= 0) {
            e.chosen = pick;
            e.grip = ke::Grip::None;
        }
    }
    if (e.chosen >= 0 && e.chosen < int(regions.size())) {
        KeepRegion &r = regions[size_t(e.chosen)];
        ImGui::PushID("chosen");
        static const char *const kShapes[] = {"Rectangle", "Ellipse"};
        const int new_shape = choice_row("Shape", "##shape", kShapes, r.ellipse ? 1 : 0,
                                         "An ellipse for a round map or a dial.");
        if (new_shape >= 0) r.ellipse = new_shape == 1;
        static const char *const kModes[] = {"Whole area", "Find the HUD in it"};
        const int mode = wide_choice_row("Left alone", nullptr, "##mode", kModes, 2, r.find_hud ? 1 : 0, 0,
                                         "Whole area: the game's own frame in all of it. Find the HUD in it: only where "
                                         "a HUD is found.");
        if (mode >= 0) r.find_hud = mode == 1;
        // Left and Top move it as it is; Width and Height end at the picture's edge.
        float x = r.x, y = r.y, w = r.w, hgt = r.h;
        bool moved = false;
        if (percent_row("Left", "##x", &x, 0, 100 - r.w, slider_held)) r.x = x, moved = true;
        if (percent_row("Top", "##y", &y, 0, 100 - r.h, slider_held)) r.y = y, moved = true;
        if (percent_row("Width", "##w", &w, ke::kLeast, 100 - r.x, slider_held)) r.w = w, moved = true;
        if (percent_row("Height", "##h", &hgt, ke::kLeast, 100 - r.y, slider_held)) r.h = hgt, moved = true;
        if (moved) {
            r.x = std::clamp(ke::hundredths(r.x), 0.0f, 100.0f);
            r.y = std::clamp(ke::hundredths(r.y), 0.0f, 100.0f);
            r.w = std::min(ke::hundredths(r.w), 100.0f - r.x);
            r.h = std::min(ke::hundredths(r.h), 100.0f - r.y);
        }
        ImGui::PopID();
    }
    if (!regions.empty()) {
        static const char *const kSides[] = {"Left", "Right"};
        const int side = choice_row("Menu side", "##side", kSides, e.left ? 0 : 1, "For an area under the menu.");
        if (side >= 0) e.left = side == 0;
    }

    if (remove >= 0 && remove < int(regions.size())) {
        regions.erase(regions.begin() + remove);
        e.chosen = -1;
        e.grip = ke::Grip::None;
        hover = {};
    }
    const bool held = e.drawing || e.grip != ke::Grip::None || slider_held;
    // The menu makes way (draw_menu) while one is awaited or drawn, and while
    // one is dragged; not for the click that chooses or removes one.
    e.aside = e.arm >= 0 || e.drawing || (e.grip != ke::Grip::None && e.grip != ke::Grip::Remove && e.moved);
    if (!ke::same(regions, before)) {
        h.set_framegen_keep(regions);
        e.unsaved = true;
    }
    if (e.unsaved && !held) {
        save(f, "framegen", "keep", format_keep_regions(regions));
        e.unsaved = false;
    }

    // On the picture, under the menu: every area's outline and number, the
    // chosen one heavier and last, with its handles and its remove box. What
    // the mouse holds, or would take hold of with a press, is lit: there is
    // no other sign of it, the mouse pointer being mpv's.
    ImDrawList *draw = ImGui::GetBackgroundDrawList();
    const ImU32 accent = ImGui::ColorConvertFloat4ToU32(ui::kAccent);
    const ImU32 quiet = ImGui::ColorConvertFloat4ToU32(ui::faded(ui::kAccent, 0.8f));
    const ImU32 white = IM_COL32(255, 255, 255, 255), dark = IM_COL32(0, 0, 0, 200);
    const ke::Hit lit = e.grip != ke::Grip::None ? ke::Hit{e.chosen, e.grip} : hover;
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < int(regions.size()); ++i) {
            const bool chosen = i == e.chosen;
            if (chosen != (pass == 1)) continue;
            const KeepRegion &r = regions[size_t(i)];
            const ke::Box b = ke::box_of(r, picture);
            const ke::Grip grip = lit.index == i ? lit.grip : ke::Grip::None;
            if (grip == ke::Grip::Move) keep_fill(draw, r, b, IM_COL32(255, 255, 255, 36));
            if (chosen && r.ellipse)  // the box its handles are on
                draw->AddRect(ImVec2(b.left, b.top), ImVec2(b.right, b.bottom),
                              ImGui::ColorConvertFloat4ToU32(ui::faded(ui::kAccent, 0.45f)), 0.0f, 0, f.scale);
            keep_outline(draw, r, b, chosen ? accent : quiet, (chosen || grip != ke::Grip::None ? 3.0f : 2.0f) * f.scale);
            keep_number(draw, r, b, i + 1);
            // The sides a drag would move.
            const float thick = 5.0f * f.scale;
            if (ke::moves_left(grip)) draw->AddLine(ImVec2(b.left, b.top), ImVec2(b.left, b.bottom), white, thick);
            if (ke::moves_right(grip)) draw->AddLine(ImVec2(b.right, b.top), ImVec2(b.right, b.bottom), white, thick);
            if (ke::moves_top(grip)) draw->AddLine(ImVec2(b.left, b.top), ImVec2(b.right, b.top), white, thick);
            if (ke::moves_bottom(grip)) draw->AddLine(ImVec2(b.left, b.bottom), ImVec2(b.right, b.bottom), white, thick);
            if (!chosen || e.arm >= 0) continue;
            static const struct {
                ke::Grip grip;
                float x, y;
            } kHandles[] = {
                {ke::Grip::TopLeft, 0, 0},      {ke::Grip::Top, 0.5f, 0},        {ke::Grip::TopRight, 1, 0},
                {ke::Grip::Left, 0, 0.5f},      {ke::Grip::Right, 1, 0.5f},      {ke::Grip::BottomLeft, 0, 1},
                {ke::Grip::Bottom, 0.5f, 1},    {ke::Grip::BottomRight, 1, 1},
            };
            const float half = 4.5f * f.scale;
            for (const auto &handle : kHandles) {
                const ImVec2 at(b.left + (b.right - b.left) * handle.x, b.top + (b.bottom - b.top) * handle.y);
                const ImVec2 a(at.x - half, at.y - half), c(at.x + half, at.y + half);
                draw->AddRectFilled(a, c, handle.grip == grip ? white : accent);
                draw->AddRect(a, c, dark, 0.0f, 0, f.scale);
            }
            // The remove box: a cross of two lines, lit when the mouse is on it.
            const ke::Box box = ke::remove_box(r, picture, remove_side, reach);
            const float in = remove_side * 0.28f, stroke = 2.0f * f.scale;
            const ImVec2 a(box.left, box.top), c(box.right, box.bottom);
            const bool on_box = grip == ke::Grip::Remove;
            draw->AddRectFilled(a, c, on_box ? white : dark);
            draw->AddRect(a, c, accent, 0.0f, 0, f.scale);
            draw->AddLine(ImVec2(a.x + in, a.y + in), ImVec2(c.x - in, c.y - in), on_box ? dark : white, stroke);
            draw->AddLine(ImVec2(c.x - in, a.y + in), ImVec2(a.x + in, c.y - in), on_box ? dark : white, stroke);
        }
    }
    // The one being drawn.
    if (e.drawing && ke::is_drag(e.from, mouse, reach)) {
        const KeepRegion r = ke::drawn(e.arm == 1, e.from, mouse, picture);
        const ke::Box b = ke::box_of(r, picture);
        keep_fill(draw, r, b, IM_COL32(255, 255, 255, 36));
        keep_outline(draw, r, b, accent, 3.0f * f.scale);
    }
}

// A small panel over the picture, for a line of text: a toast, the frame
// rate readout, the hint while an area is drawn. words: the text's size, so
// that the panel fits it on the very frame the text changes.
void small_panel(const char *id, ImVec2 at, ImVec2 pivot, float alpha, float pad_x, float pad_y, float rounding, ImVec2 words) {
    ImGui::SetNextWindowPos(at, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowSize(ImVec2(words.x + pad_x * 2, words.y + pad_y * 2));
    ImGui::SetNextWindowBgAlpha(alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad_x, pad_y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
}

void small_panel_end() {
    ImGui::End();
    ImGui::PopStyleVar(3);
}

// A line near the top of the picture, while the menu makes way for the mouse.
void picture_hint(const char *text, float scale) {
    const ImGuiIO &io = ImGui::GetIO();
    small_panel("##keep_hint", ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.12f), ImVec2(0.5f, 0.0f), 0.90f,
                20.0f * scale, 11.0f * scale, 10.0f * scale, ImGui::CalcTextSize(text));
    ImGui::TextUnformatted(text);
    small_panel_end();
}

// ---------------------------------------------------------------- the readout

// The top of the menu: the frames shown now in large figures, the strip of
// real and added frames, and the picture and the screen as chips.
void readout(const MenuFrame &f, const MpvState &mpv, const FramegenStatus &st, bool warning) {
    std::string big = rates_text(st);
    if (big.empty()) big = mpv.connected ? "No frames yet" : "Starting";
    ImGui::PushFont(ui::bold_font(), ui::base_font() * 1.72f);
    const float big_size = ImGui::GetFontSize();
    ui::text(big.c_str(), warning ? ui::kWarn : ui::kText);
    ImGui::PopFont();
    // After the figure, small: how many frames per game frame, and whose.
    std::string by;
    if (st.multiplier > 1) {
        by = "x" + std::to_string(st.multiplier);
        if (st.dlss) by += std::string(kDot) + "NVIDIA DLSS";
        if (st.anr) by += std::string(kDot) + "Apex ANR";
        if (st.video) by += std::string(kDot) + "NVIDIA Video";
    }
    if (!by.empty()) {
        ImGui::SameLine(0, px(10));
        ImGui::PushFont(nullptr, ui::base_font() * 0.88f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (big_size - ImGui::GetFontSize()) * 0.74f);
        ui::text(by.c_str(), kTextDim);
        ImGui::PopFont();
    }
    ui::cadence_strip(st.multiplier > 1 ? st.multiplier : 1, ImGui::GetContentRegionAvail().x, px(13));

    // The picture's size, SDR or HDR (an HDR signal tone mapped to SDR says
    // so: mpv's output when it has a picture, else what the app decided at
    // the start), the screen, and the delay on the PC.
    std::vector<std::string> chips;
    char size[40];
    if (mpv.width > 0 && mpv.height > 0) {
        std::snprintf(size, sizeof(size), "%d x %d", mpv.width, mpv.height);
        chips.push_back(size);
    }
    const bool output_hdr = !mpv.out_gamma.empty() ? mpv.out_gamma == "pq" : f.hdr_output != "sdr";
    chips.push_back(picture_text(signal_hdr(f, mpv), output_hdr));
    if (const std::string screen = screen_text(st); !screen.empty()) chips.push_back(screen);
    if (const std::string delay = delay_text(st); !delay.empty()) chips.push_back(delay);
    ImGui::PushFont(nullptr, ui::base_font() * 0.82f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(6), px(6)));
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    for (size_t i = 0; i < chips.size(); ++i) {
        if (i && ImGui::GetItemRectMax().x + px(6) + ui::chip_width(chips[i].c_str()) <= right) ImGui::SameLine();
        ui::chip(chips[i].c_str());
    }
    ImGui::PopStyleVar();
    ImGui::PopFont();
}

// What frame generation has to say about now: the quiet lines under the
// readout, and the warnings (with a button where the menu can do something
// about one).
void status_lines(const MenuFrame &f, const MpvState &mpv, const FramegenStatus &st, const Line &reason, const Line &cadence) {
    const Host &h = *f.host;
    // One quiet line, always there (empty when there is nothing to say), so
    // that the tabs under it do not move when it comes and goes. Cut with
    // dots if it is longer than the menu is wide; the whole of it shows when
    // the mouse rests on it.
    std::string quiet;
    for (const Line *line : {&reason, &cadence})
        if (quiet.empty() && !line->matters) quiet = line->text;
    if (quiet.empty() && st.source_uneven && st.multiplier > 1) quiet = "The game's frames come in uneven steps.";
    ImGui::PushFont(nullptr, ui::base_font() * 0.85f);
    const std::string shown = ui::fit_text(quiet, ImGui::GetContentRegionAvail().x);
    ui::text(shown.empty() ? " " : shown.c_str(), kTextDim);
    ImGui::PopFont();
    if (shown != quiet) ui::tooltip(quiet.c_str());
    int id = 0;
    for (const Line *line : {&reason, &cadence}) {
        ++id;
        if (!line->matters) continue;
        std::string text = line->text;
        const char *action = nullptr;
        if (line->fix == Line::Fix::FullScreen) {
            if (!mpv.fullscreen) {
                action = "Switch to full screen";
            } else {
                // Full screen already: Windows composing the window is not
                // it. What is left is a result at the screen's own rate,
                // where a frame that comes a moment early waits a refresh.
                static const std::string composing = " Windows may be composing the window.";
                if (text.size() >= composing.size() &&
                    text.compare(text.size() - composing.size(), composing.size(), composing) == 0)
                    text.erase(text.size() - composing.size());
                text += " The result is right at this screen's rate, which leaves no room for a frame that comes a "
                        "moment early.";
            }
        } else if (line->fix == Line::Fix::FastQuality && h.set_framegen_fast && !(h.framegen_fast && h.framegen_fast())) {
            action = "Use Fast quality";
        }
        ImGui::PushID(id);
        if (ui::notice("status", ui::Status::Warn, line->title.empty() ? "Warning" : line->title.c_str(), text.c_str(), action)) {
            g_menu.held[0].until = g_menu.held[1].until = -1;  // done about: it goes at once if that helped
            if (line->fix == Line::Fix::FullScreen) {
                f.mpv->command({"set", "fullscreen", "yes"});
                save(f, "window", "mode", "fullscreen");
            } else if (line->fix == Line::Fix::FastQuality && h.set_framegen_fast) {
                h.set_framegen_fast(true);
                save(f, "framegen", "quality", "fast");
            }
        }
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------- the tabs

void frames_page(const MenuFrame &f) {
    const Host &h = *f.host;
    const bool ready = h.framegen_ready && h.framegen_ready();
    bool fg = ready && h.framegen_enabled && h.framegen_enabled();
    if (switch_row("Frame generation", "F8", "##fg", &fg, ready,
                   ready ? "Adds frames between the game's own. It makes motion smoother, not the game quicker to react."
                         : "This card runs in basic mode: no frame generation.") &&
        h.set_framegen_enabled) {
        h.set_framegen_enabled(fg);
        save(f, "framegen", "enabled", fg ? "1" : "0");
    }
    const bool on = ready && fg;
    if (!on) ImGui::BeginDisabled();
    static const char *const kUpTo[] = {"x2", "x3", "x4"};
    const int multiplier = h.framegen_multiplier ? h.framegen_multiplier() : 2;
    const int up_to = choice_row("Up to", "##up_to", kUpTo, multiplier - 2,
                                 "x3, x4: smoother on a fast screen. More GPU time, and the game's own frame shows a "
                                 "little later.");
    if (up_to >= 0 && h.set_framegen_multiplier) {
        h.set_framegen_multiplier(up_to + 2);
        save(f, "framegen", "multiplier", std::to_string(up_to + 2));
    }
    // What makes the added frames, where another generator can be chosen.
    const bool dlss_there = h.framegen_dlss_offered && h.framegen_dlss_offered() && h.framegen_dlss && h.set_framegen_dlss;
    const bool video_there =
        h.framegen_video_offered && h.framegen_video_offered() && h.framegen_video && h.set_framegen_video;
    const bool anr_there = h.framegen_anr_offered && h.framegen_anr_offered() && h.framegen_anr && h.set_framegen_anr;
    if (dlss_there || video_there || anr_there) {
        const char *names[4] = {APP_NAME};
        const char *values[4] = {"own"};
        int n = 1, current = 0;
        if (anr_there) {
            names[n] = "Apex ANR", values[n] = "anr";
            if (h.framegen_anr()) current = n;
            ++n;
        }
        if (dlss_there) {
            names[n] = "DLSS", values[n] = "dlss";
            if (h.framegen_dlss()) current = n;
            ++n;
        }
        if (video_there) {
            names[n] = "NVIDIA Video", values[n] = "video";
            if (h.framegen_video()) current = n;
            ++n;
        }
        const char *about =
            video_there ? "NVIDIA Video is made for video: it works out the motion from the two pictures. DLSS "
                          "is made for games, which tell it the motion; here it has to do without."
            : anr_there ? "Apex ANR is made for games, which tell it the motion; here it gets the motion " APP_NAME
                          " finds in the two pictures. It is a beta."
                        : "DLSS is made for games, which tell it the motion; here it has to do without.";
        const int by = choice_row("Frames by", "##by", names, n, current, about);
        if (by >= 0) {
            // Choosing one switches the others off (the add-on's side); the
            // app's own is what is left with all of them off.
            const std::string value = values[by];
            if (value == "video") {
                h.set_framegen_video(true);
            } else if (value == "anr") {
                h.set_framegen_anr(true);
            } else if (value == "dlss") {
                h.set_framegen_dlss(true);
            } else {
                if (video_there && h.framegen_video()) h.set_framegen_video(false);
                if (anr_there && h.framegen_anr()) h.set_framegen_anr(false);
                if (dlss_there && h.framegen_dlss()) h.set_framegen_dlss(false);
            }
            save(f, "framegen", "backend", value);
        }
    }
    if (video_there && h.framegen_video() && h.framegen_video_model && h.set_framegen_video_model) {
        static const char *const kModel[] = {"Small", "Medium", "Large"};
        const int model = choice_row("Its model", "##video_model", kModel, h.framegen_video_model(),
                                     "Large: the cleanest frames and the most GPU time. Quality and Method below are for "
                                     APP_NAME "'s own frames.");
        if (model >= 0) h.set_framegen_video_model(model);
    }
    static const char *const kQuality[] = {"High", "Fast"};
    const int quality = choice_row("Quality", "##quality", kQuality, h.framegen_fast && h.framegen_fast() ? 1 : 0,
                                   "Fast: about half the GPU time, rougher edges.");
    if (quality >= 0 && h.set_framegen_fast) {
        h.set_framegen_fast(quality == 1);
        save(f, "framegen", "quality", quality == 1 ? "fast" : "high");
    }
    static const char *const kMethod[] = {"Motion", "Blend"};
    const int method = choice_row("Method", "##method", kMethod, h.framegen_blend && h.framegen_blend() ? 1 : 0,
                                  "Blend: a plain mix of two frames. The lightest, but moving things show double.");
    if (method >= 0 && h.set_framegen_blend) {
        h.set_framegen_blend(method == 1);
        save(f, "framegen", "method", method == 1 ? "blend" : "motion");
    }
    if (h.framegen_keep && h.set_framegen_keep) {
        const size_t areas = h.framegen_keep().size();
        const bool everywhere = h.framegen_hud && h.framegen_hud();
        char count[48];
        std::snprintf(count, sizeof(count), areas == 0 ? (everywhere ? "The HUD" : "None") : areas == 1 ? "1 area" : "%d areas",
                      int(areas));
        row_begin();
        label("Areas left alone");
        ImGui::PushFont(nullptr, ui::base_font() * 0.85f);
        const float count_w = ImGui::CalcTextSize(count).x;
        ImGui::PopFont();
        at_right(count_w + px(10) + ui::action_width("Edit"));
        ui::row_hint(count);
        ImGui::SameLine(0, px(10));
        if (ui::action_button("Edit")) {
            g_menu.areas = true;
            forget_help();
        }
        row_end("A HUD, a map or subtitles: added frames show the game's own frame there, so small text stays sharp.");
    }
    if (!on) ImGui::EndDisabled();
    if (f.show_rates &&
        switch_row("Show frame rates", nullptr, "##rates", f.show_rates, true,
                   "A small readout in the top left corner while playing."))
        save(f, "framegen", "show_rates", *f.show_rates ? "1" : "0");
}

// An HDR signal: what is done with it now, and what mpv can change while
// playing. mpv's own state is what is shown (it is what reaches the screen);
// what the app decided at the start fills in until mpv has a picture.
//
// HDR10 to the screen: by default the picture goes out as the console sends
// it. "Fit highlights to this screen" has mpv tone map it to the screen's
// peak instead (mpv's target-colorspace-hint-mode, target-peak). Tone mapped
// (that, or SDR output): the curve and whether each frame's peak is measured.
void hdr_controls(const MenuFrame &f, const MpvState &mpv, double now) {
    MpvClient &c = *f.mpv;
    const bool known = !mpv.out_gamma.empty();
    const bool out_hdr = known ? mpv.out_gamma == "pq" : f.hdr_output != "sdr";
    const bool fit = out_hdr && mpv.hint_mode == "target";
    // The peak fitting uses: mpv's while it fits, else the settings file's or the
    // screen's own; 1000 when nobody knows.
    const int known_peak = mpv.target_peak > 0 ? mpv.target_peak : f.hdr_peak_nits;
    g_menu.hdr_peak.follow(known_peak > 0 ? known_peak : 1000, now);
    const bool dynamic = g_menu.hdr_dynamic >= 0 ? g_menu.hdr_dynamic == 1 : f.hdr_dynamic_peak;

    ui::section_label("HDR", 12);
    char text[200];
    if (!out_hdr) {
        // mpv puts out SDR although the app asked for HDR: Windows gave it no HDR output.
        const char *why = f.hdr_output == "hdr" ? "mpv got no HDR output from Windows" : f.hdr_why.c_str();
        std::snprintf(text, sizeof(text), "Tone mapped to SDR%s%s.", *why ? ": " : "", why);
    } else if (fit) {
        std::snprintf(text, sizeof(text), "HDR10 to the screen, fitted to its %d nits.",
                      int(std::lround(mpv.out_peak > 0 ? mpv.out_peak : g_menu.hdr_peak.value)));
    } else if (mpv.out_peak > 0) {
        std::snprintf(text, sizeof(text), "HDR10 to the screen, as the console sends it (peak %d nits).",
                      int(std::lround(mpv.out_peak)));
    } else {
        std::snprintf(text, sizeof(text), "HDR10 to the screen, as the console sends it.");
    }
    fact(text);

    if (out_hdr) {
        bool want = fit;
        if (switch_row("Fit highlights to this screen", nullptr, "##fit", &want, true,
                       "Off: set the console's HDR calibration while playing here. On: for highlights that clip to "
                       "white.")) {
            if (want) {
                c.command({"set", "target-peak", std::to_string(g_menu.hdr_peak.value)});
                c.command({"set", "hdr-compute-peak", dynamic ? "yes" : "no"});
                c.command({"set", "target-colorspace-hint-mode", "target"});
                // Kept with the peak when the screen reports none: without
                // one, the next start could not fit.
                if (f.hdr_peak_nits <= 0) save(f, "hdr", "display_peak_nits", std::to_string(g_menu.hdr_peak.value));
            } else {
                c.command({"set", "target-colorspace-hint-mode", "source"});
                c.command({"set", "target-peak", "auto"});  // a peak would tone map even now
                c.command({"set", "hdr-compute-peak", "no"});
            }
            save(f, "hdr", "fit_to_screen", want ? "1" : "0");
        }
        if (!fit) return;
        char figure[32];
        std::snprintf(figure, sizeof(figure), "%d nits", g_menu.hdr_peak.value);
        row_begin();
        label_above("Screen peak", figure);
        if (ui::slider_int("##hdr_peak", &g_menu.hdr_peak.value, 100, 4000, 50, g_row.width)) {
            g_menu.hdr_peak.changed(now);
            c.command({"set", "target-peak", std::to_string(g_menu.hdr_peak.value)});
            // A step with the keys is kept at once, a drag when it ends.
            if (!ImGui::IsItemActive()) save(f, "hdr", "display_peak_nits", std::to_string(g_menu.hdr_peak.value));
        }
        if (ImGui::IsItemActive()) g_menu.hdr_peak.changed(now);
        if (ImGui::IsItemDeactivated()) save(f, "hdr", "display_peak_nits", std::to_string(g_menu.hdr_peak.value));
        row_end("The brightest this screen gets. Highlights above it are brought down to it.");
    }

    // Tone mapping (fitting, or SDR output): the curve, and the peak it
    // starts from. mpv's "auto" is its spline.
    static const char *const kCurves[] = {"bt.2390", "spline", "bt.2446a", "st2094-40", "hable", "clip"};
    static const char *const kCurveNames[] = {"BT.2390", "Spline", "BT.2446a", "ST 2094-40", "Hable", "Clip"};
    int current = mpv.tone_mapping == "auto" ? 1 : -1;
    for (int i = 0; i < 6; ++i)
        if (mpv.tone_mapping == kCurves[i]) current = i;
    row_begin();
    label("Curve");
    const float list_w = px(230);
    at_right(list_w);
    ImGui::SetNextItemWidth(list_w);
    if (ui::begin_combo("##curve", current >= 0 ? kCurveNames[current] : mpv.tone_mapping)) {
        for (int i = 0; i < 6; ++i)
            if (ImGui::Selectable(kCurveNames[i], i == current) && i != current) {
                c.command({"set", "tone-mapping", kCurves[i]});
                save(f, "hdr", "tone_mapping", kCurves[i]);
            }
        ImGui::EndCombo();
    }
    row_end("How highlights are brought down. BT.2390 leaves what is below them as it is.");
    bool measure = mpv.compute_peak;
    if (switch_row("Measure each frame's peak", nullptr, "##measure", &measure, true,
                   "Fits games that never reach their peak better, for some GPU time.")) {
        g_menu.hdr_dynamic = measure ? 1 : 0;
        c.command({"set", "hdr-compute-peak", measure ? "yes" : "no"});
        save(f, "hdr", "dynamic_peak", measure ? "1" : "0");
    }
}

void picture_page(const MenuFrame &f, const MpvState &mpv, double now) {
    reshade::api::effect_runtime *rt = f.runtime;
    const reshade::api::effect_technique feed = rt->find_technique("DLSS5_Feed.fx", "DLSS5_Feed");
    // LumeniteFX's motion vectors, in an install whose DLSS 5 is an early
    // one (reshade_config.cpp); switched with the feed.
    const reshade::api::effect_technique kernel = rt->find_technique("lumenite_Kernel.fx", "Lumenite_Kernel");
    const bool have = feed.handle != 0;
    bool dlss5 = have && rt->get_technique_state(feed);
    if (switch_row("DLSS 5", "F6", "##dlss5", &dlss5, have,
                   have ? "Neural rendering. It takes a few seconds to start."
                        : "Not available: it needs an NVIDIA RTX 30, 40 or 50 series GPU, and its download (the About tab of " APP_NAME
                          "'s window).")) {
        rt->set_technique_state(feed, dlss5);
        if (kernel.handle != 0) rt->set_technique_state(kernel, dlss5);
        save(f, "dlss5", "enabled", dlss5 ? "on" : "off");
    }

    // Scaling up (scaling.lua in mpv): switched at once.
    if (!mpv.scaling.empty()) {
        static const char *const kModes[] = {"mpv", "fsr", "vsr", "cnn", "anime4k"};
        static const char *const kNames[] = {"mpv", "FSR 1", "VSR", "ArtCNN", "Anime4K"};
        int current = -1;
        for (int i = 0; i < 5; ++i)
            if (mpv.scaling == kModes[i]) current = i;
        // A picture as large as its place in the window is not scaled at all.
        const int place = mpv.osd.h - mpv.osd.mt - mpv.osd.mb;
        const bool unused = mpv.osd.h > 0 && mpv.height > 0 && mpv.height >= place;
        const int m = wide_choice_row("Scaling", unused ? "not used at this size" : nullptr, "##scaling", kNames, 5, current, 0,
                                      "For a capture smaller than the screen. VSR: NVIDIA RTX or Intel GPUs. ArtCNN, "
                                      "Anime4K: more GPU time.");
        if (m >= 0) {
            f.mpv->command({"script-message", "app-scaling", kModes[m]});
            save(f, "video", "scaling", kModes[m]);
        }
    }

    const bool hdr = signal_hdr(f, mpv);
    if (!hdr && !f.hdr_available) return;  // a card without HDR modes: nothing to switch to
    static constexpr const char *kSignal[] = {"auto", "sdr", "hdr"};
    static const char *const kSignalNames[] = {"Automatic", "SDR", "HDR"};
    const bool live = picture_live();
    const bool automatic = signal_mode(f) == 0;
    // A 10-bit capture is switched as it plays; an 8-bit one starts again.
    const char *help =
        live ? (automatic ? (hdr ? "The console sends HDR. " APP_NAME " follows it when that changes."
                                 : "The console sends SDR. " APP_NAME " follows it when that changes.")
                          : "Automatic tells SDR from HDR by the picture.")
             : (automatic ? (hdr ? "The console sends HDR. " APP_NAME " follows it when that changes. Another "
                                   "choice starts the picture again."
                                 : "The console sends SDR. " APP_NAME " follows it when that changes. Another "
                                   "choice starts the picture again.")
                          : "Automatic tells SDR from HDR by the picture. A change starts the picture again.");
    const int signal = wide_choice_row("Signal", nullptr, "##signal", kSignalNames, 3, signal_mode(f),
                                       f.hdr_available ? 0 : 1u << 2, help);
    if (signal >= 0) {
        save(f, "video", "mode", kSignal[signal]);
        if (live) {
            // The app shows the frames as that from now on (main.cpp).
            g_menu.signal = signal;
            f.mpv->command({"script-message", "app-picture", kSignal[signal]});
        } else {
            // The app starts the picture again in that mode (mpv's exit
            // code 42 asks for that).
            f.mpv->command({"quit", "42"});
        }
    }
    if (hdr) hdr_controls(f, mpv, now);
}

void sound_page(const MenuFrame &f, const MpvState &mpv, double now) {
    if (!mpv.connected) {
        fact("Connecting to mpv...");
        return;
    }
    MpvClient &c = *f.mpv;
    if (mpv.volume >= 0) g_menu.volume.follow(int(std::lround(mpv.volume)), now);
    if (slider_row("Volume", "##volume", &g_menu.volume.value, 0, 100, 2, "%d %%", "Up and Down change it while playing.")) {
        g_menu.volume.changed(now);
        c.command({"set", "volume", std::to_string(g_menu.volume.value)});
    }
    bool mute = mpv.mute;
    if (switch_row("Mute", "M", "##mute", &mute, true, "Silences the sound. M does the same while playing."))
        c.command({"set", "mute", mute ? "yes" : "no"});

    if (mpv.boost < 0) {
        fact("The card's sound is off or not found.");
        return;
    }
    g_menu.boost.follow(mpv.boost, now);
    if (slider_row("Boost", "##boost", &g_menu.boost.value, 100, 400, 10, "%d %%",
                   "For a quiet source: up to 400 %. Ctrl+Up and Ctrl+Down while playing.")) {
        g_menu.boost.changed(now);
        c.command({"script-message", "app-audio-boost", std::to_string(g_menu.boost.value), "quiet"});
    }
    g_menu.delay.follow(std::max(mpv.delay, 0), now);
    if (slider_row("Audio delay", "##delay", &g_menu.delay.value, 0, 500, 5, "%d ms",
                   "Holds the sound back to match the picture.")) {
        g_menu.delay.changed(now);
        c.command({"script-message", "app-audio-delay", std::to_string(g_menu.delay.value), "quiet"});
    }
}

void screen_page(const MenuFrame &f, const MpvState &mpv, const FramegenStatus &st) {
    // The app moves mpv's window (and the black around a picture of another
    // shape than the screen); mpv changes its border.
    // Full screen is mpv's own (f and a double-click switch it too); leaving
    // it goes back to the borderless window or the window it came from.
    const bool borderless = g_menu.borderless < 0 ? f.borderless : g_menu.borderless == 1;
    static const char *const kShow[] = {"Full screen", "Borderless", "Window"};
    const int choice = wide_choice_row("Show as", nullptr, "##show", kShow, 3, mpv.fullscreen ? 0 : borderless ? 1 : 2, 0,
                                       "Full screen is also F or a double-click.");
    if (choice == 0) {
        f.mpv->command({"set", "fullscreen", "yes"});
        save(f, "window", "mode", "fullscreen");
    } else if (choice > 0) {
        g_menu.borderless = choice == 1 ? 1 : 0;
        f.mpv->command({"script-message", "app-window", choice == 1 ? "borderless" : "windowed"});
        save(f, "window", "mode", choice == 1 ? "borderless" : "windowed");
    }

    // A second, ordinary SDR window with a copy of the picture, on another
    // screen: what a stream can share while this one stays HDR.
    if (f.stream_offered && f.set_stream) {
        bool on = f.stream_on;
        if (switch_row("Stream window", nullptr, "##stream", &on, true,
                       "A second window in SDR on another screen, for sharing in Discord or OBS. Your picture stays "
                       "as it is.")) {
            f.set_stream(on);
            save(f, "stream", "window", on ? "1" : "0");
        }
        // Only an HDR picture is tone mapped: an SDR one is shown as it is.
        if (on && f.set_stream_brightness && signal_hdr(f, mpv)) {
            int percent = f.stream_brightness;
            if (slider_row("Its brightness", "##stream_brightness", &percent, 40, 160, 5, "%d %%",
                           "How bright the HDR picture is in the stream window. Lower if it looks too bright there.")) {
                f.set_stream_brightness(percent);
                save(f, "stream", "brightness", std::to_string(percent));
            }
        }
    }

    // The screen the window is on now, as frame generation goes by it (it
    // looks every second): which need not be the one the app started on.
    // Without frame generation's word, the screen the exe chose.
    const bool live = st.screen[0] != 0 || st.display_hz > 0;
    const std::string name = live ? std::string(st.screen) : f.display_name;
    const double hz = live ? st.display_hz : f.display_hz;
    const std::string line = screen_line(name, hz);
    if (line.empty() && !live && f.display_vrr.empty()) return;  // nothing was told
    ui::section_label("This screen", 12);
    fact(line);
    if (live)
        // What the user chose in the launcher speaks for the screen the app started on only.
        fact(vrr_line(st.vrr, st.screen_told, f.display_vrr_set && name == f.display_name));
    else
        fact(vrr_line(vrr_from_word(f.display_vrr), true, f.display_vrr_set));
}

// env MENU_DEMO (menu.h): a made-up state of frame generation in place of
// the real one, to look at the menu's warnings. Besides menu.h's: "step" and
// "step-gpu" (x4 could not be held, x3 is in use: the screen's doing, or the
// GPU's) and "step-two" (neither x4 nor x3: x2).
void demo_status(const std::string &demo, FramegenStatus &st) {
    st.enabled = true;
    st.source_fps = 30, st.real_fps = 30, st.source_variable = false;
    st.wanted = st.multiplier = 4;
    st.display_hz = 120;
    st.latency_ms = 14.2, st.arrival_spread_ms = 0.2;
    if (demo == "step" || demo == "step-gpu" || demo == "step-two") {
        st.multiplier = demo == "step-two" ? 2 : 3;
        st.generated_fps = 30.0 * (st.multiplier - 1);
        st.vrr = FramegenStatus::Vrr::On;
        st.reason = FramegenStatus::Reason::SteppedDown;
        st.step_from = 4;
        st.step_failed = st.multiplier + 1;
        st.step_cause = demo == "step-gpu" ? FramegenStatus::StepCause::Gpu : FramegenStatus::StepCause::Screen;
        st.cadence = FramegenStatus::Cadence::VrrEven;
    } else if (demo == "uneven") {
        st.multiplier = st.wanted = 3;
        st.generated_fps = 60;
        st.vrr = FramegenStatus::Vrr::Off;
        st.reason = FramegenStatus::Reason::None;
        st.cadence = FramegenStatus::Cadence::FixedUneven;
        st.better_hz = 90;
    } else if (demo == "paused") {
        st.multiplier = 1;
        st.generated_fps = 0;
        st.vrr = FramegenStatus::Vrr::On;
        st.reason = FramegenStatus::Reason::BackingOff;
        st.seconds_left = 4.2;
        st.cadence = FramegenStatus::Cadence::Unknown;
    } else {
        st.generated_fps = 60;
        st.vrr = FramegenStatus::Vrr::On;
        st.reason = FramegenStatus::Reason::QueueFull;
        st.reason_count = 30;
        st.cadence = FramegenStatus::Cadence::VrrCeiling;
    }
}

// The mouse, when the console is played with this PC's mouse and keyboard.
// The input program follows the settings file, so a change is felt at once.
void mouse_page(const MenuFrame &f) {
    if (!f.input_on || !f.mouse_sensitivity || !f.mouse_aim) {
        fact("Mouse and keyboard is off. Switch it on in the window the app starts with.");
        return;
    }
    int sensitivity = *f.mouse_sensitivity;
    if (slider_row("Sensitivity", "##mouse_sensitivity", &sensitivity, 5, 100, 1, "%d",
                   "How far the view turns for a movement of the mouse.")) {
        *f.mouse_sensitivity = sensitivity;
        save(f, "input", "sensitivity", std::to_string(sensitivity));
    }
    int aim = *f.mouse_aim;
    if (slider_row("While aiming", "##mouse_aim", &aim, 20, 200, 5, "%d %%",
                   "The sensitivity while aiming down the sights, in percent of the one above.")) {
        *f.mouse_aim = aim;
        save(f, "input", "aim_sensitivity", std::to_string(aim));
    }
    if (!f.mouse_forward || !f.mouse_lead) return;
    if (switch_row("Mouse forward", nullptr, "##mouse_forward", f.mouse_forward, true,
                   "The picture turns with the mouse at once, ahead of the console, which shows a turn only a moment "
                   "later. Needs the input program's measurement of the game's turning."))
        save(f, "input", "mouse_forward", *f.mouse_forward ? "1" : "0");
    if (*f.mouse_forward) {
        int lead = *f.mouse_lead;
        if (slider_row("Ahead by", "##mouse_lead", &lead, 0, 300, 5, "%d ms",
                       "How long the console takes to show a turn. Too high and the view swings past and comes "
                       "back; too low and it trails behind the mouse.")) {
            *f.mouse_lead = lead;
            save(f, "input", "lead_ms", std::to_string(lead));
        }
    }
}

// The tabs, or in their place the head of the areas' page.
void tabs_or_page_head() {
    if (!g_menu.areas) {
        static const char *const kTabs[] = {"Frames", "Picture", "Sound", "Screen", "Mouse"};
        const int tab = ui::segmented("##tabs", kTabs, g_menu.tab, 0u, ImGui::GetContentRegionAvail().x, px(50));
        if (tab >= 0) {
            g_menu.tab = tab;
            forget_help();
        }
        return;
    }
    const float frame = ImGui::GetFrameHeight();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::PushFont(ui::bold_font(), 0.0f);
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + (px(50) - frame) * 0.5f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Areas left alone");
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(at.x + width - ui::action_width("Done"), at.y + (px(50) - frame) * 0.5f));
    if (ui::action_button("Done")) {
        g_menu.areas = false;
        forget_help();
    }
    ImGui::SetCursorScreenPos(at);
    ImGui::Dummy(ImVec2(width, px(50)));
}

}  // namespace

void apply_style(float scale) {
    g_menu.scale = scale;
    ui::apply_style(scale, ui::kMenuAlpha, ui::kMenuFont);
}

void draw_toast(const std::string &text, float alpha, float scale) {
    const ImGuiIO &io = ImGui::GetIO();
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    // A long message wraps at half the picture's width.
    const float wrap = io.DisplaySize.x * 0.5f;
    ImGui::PushFont(ui::bold_font(), ui::base_font() * 1.22f);
    const ImVec2 words = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap);
    small_panel("##toast", ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.05f), ImVec2(0.5f, 0.0f), 0.90f,
                20.0f * scale, 11.0f * scale, 10.0f * scale, words);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap + 1.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    small_panel_end();
    ImGui::PopFont();
    ImGui::PopStyleVar();
}

void draw_rates(const Host &host, float scale) {
    if (!host.framegen_status) return;
    const FramegenStatus st = host.framegen_status();
    std::string text = rates_and_latency_text(st);
    const std::string screen = screen_text(st);
    if (!text.empty() && !screen.empty()) text += kDot;
    text += screen;
    if (text.empty()) return;
    ImGui::PushFont(ui::bold_font(), ui::base_font() * 0.88f);
    small_panel("##rates", ImVec2(14.0f * scale, 14.0f * scale), ImVec2(0, 0), 0.72f, 11.0f * scale, 5.0f * scale,
                6.0f * scale, ImGui::CalcTextSize(text.c_str()));
    ImGui::TextUnformatted(text.c_str());
    small_panel_end();
    ImGui::PopFont();
}

bool draw_menu(const MenuFrame &f) {
    ImGuiIO &io = ImGui::GetIO();
    const double now = ImGui::GetTime();
    const bool opened = ImGui::GetFrameCount() != g_menu.last_frame + 1;
    g_menu.last_frame = ImGui::GetFrameCount();
    g_menu.scale = f.scale;
    if (opened) {
        g_menu.areas = false;  // it opens on its tabs
        forget_help();
    }

    // The page of the areas left alone (keep_editor), if it was drawn on
    // the last frame: it can have the menu on the left, and while the mouse
    // draws or holds an area on the picture the menu makes way: it fades and
    // takes no mouse, so the picture under it can be seen and drawn on.
    const bool editing = !opened && g_keep.frame == ImGui::GetFrameCount() - 1;
    if (!editing) keep_edit_over(*f.host, f.settings_path);
    const bool aside = g_keep.aside;

    // 26 % of the picture's width, which is 499 pixels of a 1920 x 1080 one;
    // on a wider or narrower picture, no less than that and not much more.
    const float width = std::min(io.DisplaySize.x, std::clamp(io.DisplaySize.x * 0.26f, px(499), px(560)));
    ImGui::SetNextWindowPos(ImVec2(g_keep.left ? 0.0f : io.DisplaySize.x - width, 0));
    ImGui::SetNextWindowSize(ImVec2(width, io.DisplaySize.y));
    if (opened) ImGui::SetNextWindowFocus();
    bool keep = true;
    if (aside) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.2f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(24), px(20)));
    ImGui::Begin(APP_NAME, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse | (aside ? ImGuiWindowFlags_NoInputs : 0));
    ImGui::PopStyleVar();
    if (opened) ImGui::SetNavCursorVisible(true);  // the keyboard's place shows at once
    const MpvState mpv = f.mpv->state();
    const Host &h = *f.host;
    FramegenStatus st = h.framegen_status ? h.framegen_status() : FramegenStatus{};
    if (!f.demo.empty()) demo_status(f.demo, st);
    if (opened) g_menu.held[0].until = g_menu.held[1].until = -1;
    const Line reason = held_line(g_menu.held[0], reason_text(st), now);
    const Line cadence = held_line(g_menu.held[1], cadence_text(st), now);
    const float content = ImGui::GetContentRegionAvail().x;
    ImDrawList *d = ImGui::GetWindowDrawList();
    g_menu.hover_now.clear();
    g_menu.focus_now.clear();
    g_menu.first.clear();

    // The name, and how to close the menu.
    {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::PushFont(ui::bold_font(), 0.0f);
        ImGui::TextUnformatted(APP_NAME);
        ImGui::PopFont();
        ImGui::PushFont(nullptr, ui::base_font() * 0.82f);
        const char *const closes = "closes";
        const float words = ImGui::CalcTextSize(closes).x, line = ImGui::GetFontSize();
        ImGui::SetCursorScreenPos(ImVec2(at.x + content - words - px(8) - px(46), at.y + px(3)));
        ui::key_cap("Tab", line);
        ImGui::SetCursorScreenPos(ImVec2(at.x + content - words, at.y + px(3)));
        ui::text(closes, kTextDim);
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + ImGui::GetFontSize() + px(12)));
    }

    readout(f, mpv, st, reason.matters || cadence.matters);
    status_lines(f, mpv, st, reason, cadence);
    {
        // A line under the readout, and room on both sides of it.
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(6));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        d->AddLine(at, ImVec2(at.x + content, at.y), ui::u32(ui::kLine));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(12));
    }
    tabs_or_page_head();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(6));

    // The open tab's rows take what is left above the help line and the
    // buttons; if a warning and a long tab ever need more than that, the
    // rows scroll and the rest stays where it is.
    const float frame = ImGui::GetFrameHeight();
    ImGui::PushFont(nullptr, ui::base_font() * 0.85f);
    const float help_line = ImGui::GetFontSize();
    ImGui::PopFont();
    const float help_height = px(12) + help_line * 2 + px(4);
    const float buttons = frame + px(8), keys = help_line + px(4);
    const float below = help_height + px(12) + buttons + px(12) + keys;
    const float panel = std::max(px(48), ImGui::GetContentRegionAvail().y - below - px(6));
    // Wider than the menu's text by the margin the rows light up in.
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - px(16));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(16), 0));
    ImGui::BeginChild("rows", ImVec2(content + px(32), panel), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    if (g_menu.areas) {
        keep_editor(f);
    } else {
        switch (g_menu.tab) {
        case kTabPicture: picture_page(f, mpv, now); break;
        case kTabSound: sound_page(f, mpv, now); break;
        case kTabScreen: screen_page(f, mpv, st); break;
        case kTabMouse: mouse_page(f); break;
        default: frames_page(f); break;
        }
    }
    ImGui::EndChild();

    // The help line: for the row the keyboard or the mouse came to last.
    if (g_menu.focus_now != g_menu.focused) {
        g_menu.focused = g_menu.focus_now;
        if (!g_menu.focus_now.empty()) g_menu.help = g_menu.focus_now;
    }
    if (g_menu.hover_now != g_menu.hovered) {
        g_menu.hovered = g_menu.hover_now;
        if (!g_menu.hover_now.empty()) g_menu.help = g_menu.hover_now;
    }
    {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(2));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        d->AddLine(at, ImVec2(at.x + content, at.y), ui::u32(ui::kLine));
        const std::string &help = g_menu.help.empty() ? g_menu.first : g_menu.help;
        ImGui::PushFont(nullptr, ui::base_font() * 0.85f);
        // Two lines; a text that would take a third is cut there, not drawn over the buttons.
        d->PushClipRect(at, ImVec2(at.x + content, at.y + help_height), true);
        ui::draw_wrapped(d, ImVec2(at.x, at.y + px(10)), ui::u32(kTextDim), help.c_str(), content);
        d->PopClipRect();
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(content, help_height));
    }

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(4));
    const float half = (content - px(10)) * 0.5f;
    if (ui::quiet_button("Close", ImVec2(half, buttons))) keep = false;
    ImGui::SameLine(0, px(10));
    if (ui::quiet_button("Quit " APP_NAME, ImVec2(half, buttons))) f.mpv->command({"quit"});

    // The keys that work while playing and are not on a row.
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(4));
    ImGui::PushFont(nullptr, ui::base_font() * 0.8f);
    const float line = ImGui::GetFontSize();
    static const struct {
        const char *key, *does;
    } kKeys[] = {{"S", "screenshot"}, {"F9", "bug report"}, {"Esc", "close"}};
    for (const auto &k : kKeys) {
        if (&k != kKeys) ImGui::SameLine(0, px(16));
        ui::key_cap(k.key, line);
        ImGui::SameLine(0, px(6));
        ui::text(k.does, kTextDim);
    }
    ImGui::PopFont();
    ImGui::End();
    if (aside) {
        ImGui::PopStyleVar();
        // Said on the picture: the menu cannot be read now.
        if (g_keep.arm >= 0) picture_hint("Drag on the picture to draw the area. Right-click to cancel.", f.scale);
    }
    return keep;
}

void menu_closed(const Host &host, const std::wstring &settings_path) { keep_edit_over(host, settings_path); }

}  // namespace app::overlay
