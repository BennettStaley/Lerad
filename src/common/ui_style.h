// SPDX-License-Identifier: MIT
// The app's look in Dear ImGui (the in-game menu and the launcher): a nearly
// solid dark panel, one accent, and three status colours. Header only;
// include it where imgui.h is available. The widgets drawn in this look are
// in ui_widgets.h.
//
// Colour has two jobs and they never mix:
//   the accent   "chosen" or "press this": the open tab, a switch that is
//                on, the picked choice, Play
//   status       green, amber, red: all well, a warning, a problem - always
//                with a word beside it, never colour alone
#pragma once
#include <imgui.h>

namespace app::ui {

constexpr ImVec4 rgb(unsigned hex, float alpha = 1.0f) {
    return ImVec4(((hex >> 16) & 255) / 255.0f, ((hex >> 8) & 255) / 255.0f, (hex & 255) / 255.0f, alpha);
}
constexpr ImVec4 mix(const ImVec4 &a, const ImVec4 &b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}
constexpr ImVec4 faded(const ImVec4 &c, float alpha) { return ImVec4(c.x, c.y, c.z, c.w * alpha); }
constexpr float luminance(const ImVec4 &c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

// The accent. This line is the only place it is set: everything else that
// is accent-coloured (and the text drawn on it) is worked out from it.
inline constexpr ImVec4 kAccent = rgb(0x7cc8f8);  // "Ice"

// Text and marks on the accent: dark on a light accent, light on a dark one.
inline constexpr ImVec4 kOnAccent =
    luminance(kAccent) > 0.4f ? mix(rgb(0x06090c), kAccent, 0.14f) : rgb(0xf4f6f8);
inline constexpr ImVec4 kAccentHover = mix(kAccent, rgb(0xffffff), 0.22f);
inline constexpr ImVec4 kAccentActive = mix(kAccent, rgb(0x000000), 0.14f);

inline constexpr ImVec4 kBg = rgb(0x0e1014);            // the panel
inline constexpr ImVec4 kSurface = rgb(0x15181e);       // a part set off from it (the launcher's result column)
inline constexpr ImVec4 kControl = rgb(0x20242c);       // a control at rest
inline constexpr ImVec4 kControlHover = rgb(0x2b303a);  // pointed at; a switch that is off
inline constexpr ImVec4 kControlActive = rgb(0x363c48); // held
inline constexpr ImVec4 kLine = rgb(0xffffff, 0.10f);
inline constexpr ImVec4 kText = rgb(0xeef0f3);
inline constexpr ImVec4 kTextDim = rgb(0xa4abb7);       // hints, labels, what is not chosen

inline constexpr ImVec4 kOk = rgb(0x57d08b);
inline constexpr ImVec4 kWarn = rgb(0xf5b942);
inline constexpr ImVec4 kBad = rgb(0xff7b6e);
inline constexpr ImVec4 kOnStatus = rgb(0x15120a);      // text on a filled amber or red

// The text size before scaling: the launcher's at 96 DPI and the menu's at
// 1080 lines. (Dear ImGui counts a font's whole line; Segoe UI at 21.5 is
// what a browser calls 16 px.)
inline constexpr float kLauncherFont = 21.5f;
inline constexpr float kMenuFont = 26.5f;
// The panel over the picture: nearly solid, so a bright HDR highlight behind
// it does not grey the text.
inline constexpr float kMenuAlpha = 0.95f;

namespace detail {
// The text's size before scaling, as apply_style was given it. (The style's
// own FontSizeBase follows every PushFont, so it cannot be asked.)
inline float g_base_font = kLauncherFont;
}  // namespace detail

// The style at this scale (1 = 1080 lines for the menu, 96 DPI for the
// launcher); window_alpha: the panel's opacity; font_size: the text's size
// before scaling.
inline void apply_style(float scale, float window_alpha, float font_size) {
    detail::g_base_font = font_size;
    ImGuiStyle s;
    ImGui::StyleColorsDark(&s);
    s.WindowPadding = ImVec2(22, 16);
    s.FramePadding = ImVec2(12, 6.5f);
    s.ItemSpacing = ImVec2(10, 6);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.GrabMinSize = 14;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.PopupBorderSize = 1;
    s.FrameRounding = 8;
    s.GrabRounding = 8;
    s.PopupRounding = 8;
    s.ChildRounding = 0;
    s.WindowRounding = 0;
    s.ScrollbarSize = 10;
    s.ScrollbarRounding = 5;
    s.DisabledAlpha = 0.4f;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg] = faded(kBg, window_alpha);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = rgb(0x1a1d24, 0.99f);
    c[ImGuiCol_Border] = kLine;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kTextDim;
    c[ImGuiCol_FrameBg] = kControl;
    c[ImGuiCol_FrameBgHovered] = kControlHover;
    c[ImGuiCol_FrameBgActive] = kControlActive;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kText;
    c[ImGuiCol_SliderGrabActive] = kText;
    c[ImGuiCol_Button] = kControl;
    c[ImGuiCol_ButtonHovered] = kControlHover;
    c[ImGuiCol_ButtonActive] = kControlActive;
    // A row of a list, a tree or a header: the chosen one is tinted, the one
    // pointed at is lighter. The text on them stays the panel's text colour,
    // so none of these may be the full accent.
    c[ImGuiCol_Header] = faded(kAccent, 0.20f);
    c[ImGuiCol_HeaderHovered] = rgb(0xffffff, 0.09f);
    c[ImGuiCol_HeaderActive] = rgb(0xffffff, 0.14f);
    c[ImGuiCol_Separator] = kLine;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = kControlHover;
    c[ImGuiCol_ScrollbarGrabHovered] = kControlActive;
    c[ImGuiCol_ScrollbarGrabActive] = kControlActive;
    c[ImGuiCol_NavCursor] = kAccent;  // the keyboard's place: an accent outline
    s.ScaleAllSizes(scale);
    s.FontSizeBase = font_size;
    s.FontScaleMain = scale;
    ImGui::GetStyle() = s;
}

}  // namespace app::ui
