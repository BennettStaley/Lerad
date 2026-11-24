// SPDX-License-Identifier: MIT
// What the in-game menu shows and does (Dear ImGui, between NewFrame and
// Render).
#pragma once
#include <string>

#include <reshade.hpp>

#include "compositor.h"
#include "mpv_client.h"
#include "overlay.h"

namespace app::overlay {

struct MenuFrame {
    reshade::api::effect_runtime *runtime = nullptr;
    const Host *host = nullptr;
    MpvClient *mpv = nullptr;
    Encoding encoding = Encoding::Sdr;
    std::wstring settings_path;  // the settings file, where the menu keeps what it changes ("" = nowhere)
    bool source_hdr = false;     // the capture is shown as HDR, whatever mpv outputs
    bool source_auto = false;    // the settings file's video.mode = auto: SDR or HDR as the signal turns out to be
    bool hdr_available = true;   // the card has an HDR mode (false: no "Switch to HDR")
    // An HDR capture, as the app started mpv (its hdr.h, through the
    // environment); what mpv does now comes from MpvState.
    std::string hdr_output;        // "hdr": HDR10 to the screen; "sdr": tone mapped; "": not said
    std::string hdr_why;           // "sdr": why ("HDR is off in Windows")
    int hdr_peak_nits = 0;         // the screen's peak fitting uses (the settings file's, else the screen's); 0: unknown
    bool hdr_dynamic_peak = false; // the settings file's hdr.dynamic_peak
    bool borderless = true;      // the settings file's window.mode when mpv started (the menu follows its own switches)
    // The screen the exe chose and its variable refresh as it told the
    // add-ons (env DISPLAY_*): "1 (PA279CDV)"; "on", "off" or "unknown";
    // whether that is the settings file's display.vrr. Empty: not told.
    // The menu's screen line goes by the screen the window is on now
    // (FramegenStatus::screen); these fill in where frame generation says
    // nothing, and say whether the user set the start screen's state.
    std::string display_name, display_vrr;
    bool display_vrr_set = false;
    double display_hz = 0;
    // env MENU_DEMO, a test switch: the readout shows a made-up state of
    // frame generation instead of the real one, to look at the menu's
    // warnings without bringing them about: "queue" (the screen takes
    // frames late), "uneven" (a fixed refresh rate the frames do not
    // divide), "paused" (the GPU could not keep up), "step" (x3 instead of
    // x4: the screen takes frames late), "step-gpu" (the same, for the
    // GPU), "step-two" (x2 instead of x4). Empty: the real state.
    std::string demo;
    // The stream window (src/common/stream_link.h): whether the app offers
    // one, whether it is on, and the switch.
    bool stream_offered = false, stream_on = false;
    void (*set_stream)(bool on) = nullptr;
    int stream_brightness = 80;  // percent, for an HDR picture
    void (*set_stream_brightness)(int percent) = nullptr;
    // The input program (the console played with this PC's mouse and
    // keyboard) runs: the mouse's sensitivity (5-100) and the same while
    // aiming (percent), as the settings file has them; the menu changes them.
    bool input_on = false;
    int *mouse_sensitivity = nullptr;
    int *mouse_aim = nullptr;
    // The picture moved with the mouse ahead of the console, and by how many
    // milliseconds (the settings file's input.mouse_forward and lead_ms).
    bool *mouse_forward = nullptr;
    int *mouse_lead = nullptr;
    float scale = 1;             // 1 at 1080 lines
    bool *show_rates = nullptr;  // the readout in the corner (the settings file's framegen.show_rates); the menu switches it
};

// The frame rate readout in the top left corner.
void draw_rates(const Host &host, float scale);

// A toast at the top of the picture; alpha fades it out.
void draw_toast(const std::string &text, float alpha, float scale);

// The theme, at this scale.
void apply_style(float scale);

// Draws the menu. False when it asks to be closed.
bool draw_menu(const MenuFrame &frame);

// The menu was closed: what the mouse held on the picture (an area frame
// generation leaves alone, being drawn or dragged) is let go, and a change
// not yet in the settings file (settings_path) is written there.
void menu_closed(const Host &host, const std::wstring &settings_path);

}  // namespace app::overlay
