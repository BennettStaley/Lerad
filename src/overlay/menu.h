// SPDX-License-Identifier: MIT
// What the in-game menu shows and does (Dear ImGui, between NewFrame and
// Render).
#pragma once
#include <string>

#include <reshade.hpp>

#include "compositor.h"
#include "mpv_client.h"
#include "overlay.h"

namespace glasir::overlay {

struct MenuFrame {
    reshade::api::effect_runtime *runtime = nullptr;
    const Host *host = nullptr;
    MpvClient *mpv = nullptr;
    Encoding encoding = Encoding::Sdr;
    std::wstring settings_path;  // glasir.ini, where the menu keeps what it changes ("" = nowhere)
    bool source_hdr = false;     // the capture is shown as HDR, whatever mpv outputs
    bool source_auto = false;    // glasir.ini video.mode = auto: SDR or HDR as the signal turns out to be
    bool hdr_available = true;   // the card has an HDR mode (false: no "Switch to HDR")
    // An HDR capture, as Glasir started mpv (its hdr.h, through the
    // environment); what mpv does now comes from MpvState.
    std::string hdr_output;        // "hdr": HDR10 to the screen; "sdr": tone mapped; "": not said
    std::string hdr_why;           // "sdr": why ("HDR is off in Windows")
    int hdr_peak_nits = 0;         // the screen's peak fitting uses (glasir.ini's, else the screen's); 0: unknown
    bool hdr_dynamic_peak = false; // glasir.ini hdr.dynamic_peak
    bool borderless = true;      // glasir.ini window.mode when mpv started (the menu follows its own switches)
    // The screen Glasir.exe chose and its variable refresh as it told the
    // add-ons (GLASIR_DISPLAY_*): "1 (PA279CDV)"; "on", "off" or "unknown";
    // whether that is glasir.ini's display.vrr. Empty: not told.
    std::string display_name, display_vrr;
    bool display_vrr_set = false;
    double display_hz = 0;
    float scale = 1;             // 1 at 1080 lines
    bool *show_rates = nullptr;  // the readout in the corner (glasir.ini framegen.show_rates); the menu switches it
};

// The frame rate readout in the top left corner.
void draw_rates(const Host &host, float scale);

// A toast at the top of the picture; alpha fades it out.
void draw_toast(const std::string &text, float alpha, float scale);

// The theme, at this scale.
void apply_style(float scale);

// Draws the menu. False when it asks to be closed.
bool draw_menu(const MenuFrame &frame);

}  // namespace glasir::overlay
