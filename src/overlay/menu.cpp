// SPDX-License-Identifier: MIT
#include "menu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>

#include <imgui.h>

#include "framegen_text.h"
#include "ini_edit.h"
#include "ui_style.h"

namespace glasir::overlay {
namespace {

using ui::kGold;
using ui::kTextDim;

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

struct MenuState {
    int last_frame = -10;
    Synced volume, boost, delay;
    int borderless = -1;  // full screen (1) or a window (0) since the menu switched it; -1: as mpv started
    Synced hdr_peak;      // the screen's peak the picture is fitted to (mpv's target-peak)
    int hdr_dynamic = -1; // measure each frame's peak, since the menu switched it; -1: as glasir.ini has it
};
MenuState g_menu;

void save(const MenuFrame &f, const char *section, const char *key, const std::string &value) {
    if (f.settings_path.empty()) return;
    if (!ini_save_values(f.settings_path, {{section, key, value}}))
        reshade::log::message(reshade::log::level::warning, "Glasir menu: could not save a setting to glasir.ini");
}

ImFont *bold_font() {
    ImGuiIO &io = ImGui::GetIO();
    return io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : nullptr;
}

void section(const char *title) {
    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y));
    ImFont *bold = bold_font();
    ImGui::PushFont(bold, ImGui::GetStyle().FontSizeBase * 0.8f);
    ImGui::PushStyleColor(ImGuiCol_Text, kGold);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Separator();
}

void note(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// A note that matters now (gold).
void warn(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kGold);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// A labelled slider across the panel; true when moved.
// Left and Right move it by step while it is highlighted (no need to press
// Enter on it first, as Dear ImGui would want).
// A label and one radio button per choice, on as many lines as the menu's
// width needs (a second line lines up under the first button). The choice
// clicked, if it is not the current one; else -1.
int radio_row(const char *label, std::initializer_list<const char *> choices, int current) {
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    const ImGuiStyle &style = ImGui::GetStyle();
    float left = 0;
    int clicked = -1, i = 0;
    for (const char *choice : choices) {
        const float w = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize(choice).x;
        if (left == 0 || ImGui::GetItemRectMax().x + style.ItemSpacing.x + w <= right)
            ImGui::SameLine();
        else
            ImGui::SetCursorPosX(left);
        if (left == 0) left = ImGui::GetCursorPosX();
        if (ImGui::RadioButton(choice, i == current) && i != current) clicked = i;
        ++i;
    }
    return clicked;
}

bool slider(const char *label, const char *id, int *value, int lo, int hi, const char *format, int step) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    bool changed = ImGui::SliderInt(id, value, lo, hi, format, ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemFocused() && !ImGui::IsItemActive()) {
        int by = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) by -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) by += step;
        const int next = std::clamp(*value + by, lo, hi);
        if (next != *value) {
            *value = next;
            changed = true;
        }
    }
    return changed;
}

// The line under the title: the picture's size, SDR or HDR, what the card
// delivers and what is shown (over the last second), and the screen.
std::string status_line(const MenuFrame &f, const MpvState &mpv) {
    char text[160];
    std::string out;
    const auto add = [&out](const std::string &part) {
        if (part.empty()) return;
        out += out.empty() ? "" : "  \xC2\xB7  ";
        out += part;
    };
    if (mpv.width > 0 && mpv.height > 0) {
        std::snprintf(text, sizeof(text), "%dx%d", mpv.width, mpv.height);
        add(text);
    }
    // An HDR capture tone mapped to SDR says so: mpv's output when it has a
    // picture, else what Glasir decided at the start.
    const bool output_hdr = !mpv.out_gamma.empty() ? mpv.out_gamma == "pq" : f.hdr_output != "sdr";
    add(picture_text(f.source_hdr, output_hdr));
    if (f.host && f.host->framegen_status) {
        const FramegenStatus st = f.host->framegen_status();
        add(rates_text(st));
        add(screen_text(st));
    }
    return out;
}

void picture(const MenuFrame &f, const MpvState &mpv) {
    section("PICTURE");
    reshade::api::effect_runtime *rt = f.runtime;
    const reshade::api::effect_technique feed = rt->find_technique("DLSS5_Feed.fx", "DLSS5_Feed");
    const reshade::api::effect_technique kernel = rt->find_technique("lumenite_Kernel.fx", "Lumenite_Kernel");
    if (feed.handle == 0) {
        ImGui::BeginDisabled();
        bool off = false;
        ImGui::Checkbox("DLSS 5", &off);
        ImGui::EndDisabled();
        note("Not available: needs an NVIDIA RTX 30, 40 or 50 series GPU and its download (Glasir.exe --setup).");
    } else {
        bool on = rt->get_technique_state(feed);
        static double switched_on = -100;
        if (ImGui::Checkbox("DLSS 5", &on)) {
            rt->set_technique_state(feed, on);
            if (kernel.handle != 0) rt->set_technique_state(kernel, on);
            save(f, "dlss5", "enabled", on ? "on" : "off");
            if (on) switched_on = ImGui::GetTime();
        }
        if (on && ImGui::GetTime() - switched_on < 10) note("DLSS 5 takes a few seconds to start.");
    }

    const Host &h = *f.host;
    const bool ready = h.framegen_ready && h.framegen_ready();
    if (!ready) ImGui::BeginDisabled();
    bool fg = ready && h.framegen_enabled && h.framegen_enabled();
    if (ImGui::Checkbox("Frame generation", &fg) && h.set_framegen_enabled) {
        h.set_framegen_enabled(fg);
        save(f, "framegen", "enabled", fg ? "1" : "0");
    }
    if (ready && fg) {
        ImGui::Indent();
        // What it is doing now, then what is in its way, then the choices.
        if (h.framegen_status) {
            const FramegenStatus st = h.framegen_status();
            char text[200];
            if (st.multiplier > 1 && st.source_fps >= 1) {
                std::snprintf(text, sizeof(text), "Game %.0f fps%s, shown %.0f (x%d)", st.source_fps,
                              st.source_variable ? ", variable" : st.source_uneven ? ", in uneven steps" : "",
                              shown_fps(st), st.multiplier);
                note(text);
            }
            for (const Line &line : {reason_text(st), cadence_text(st)}) {
                if (line.text.empty()) continue;
                if (line.matters)
                    warn(line.text.c_str());
                else
                    note(line.text.c_str());
            }
        }
        const bool fast = h.framegen_fast && h.framegen_fast();
        const int quality = radio_row("Quality", {"High", "Fast"}, fast ? 1 : 0);
        if (quality >= 0 && h.set_framegen_fast) {
            h.set_framegen_fast(quality == 1);
            save(f, "framegen", "quality", quality == 1 ? "fast" : "high");
        }
        const bool blend = h.framegen_blend && h.framegen_blend();
        const int method = radio_row("Method", {"Motion", "Blend"}, blend ? 1 : 0);
        if (method >= 0 && h.set_framegen_blend) {
            h.set_framegen_blend(method == 1);
            save(f, "framegen", "method", method == 1 ? "blend" : "motion");
        }
        const int multiplier = h.framegen_multiplier ? h.framegen_multiplier() : 2;
        const int up_to = radio_row("Up to", {"x2", "x3", "x4"}, multiplier - 2);
        if (up_to >= 0 && h.set_framegen_multiplier) {
            h.set_framegen_multiplier(up_to + 2);
            save(f, "framegen", "multiplier", std::to_string(up_to + 2));
        }
        note("Fast: about half the GPU time, rougher edges. Blend: lightest, moving things show double. "
             "x3, x4: smoother on a fast screen; more GPU time, and the game's own frame shows a little later.");
        ImGui::Unindent();
    }
    if (!ready) {
        ImGui::EndDisabled();
        note("Frame generation needs Glasir's own capture with dedup (not the DirectShow fallback).");
    }
    if (f.show_rates && ImGui::Checkbox("Show frame rates", f.show_rates))
        save(f, "framegen", "show_rates", *f.show_rates ? "1" : "0");

    // Scaling up (glasir_scaling.lua in mpv): switched at once.
    if (!mpv.scaling.empty()) {
        static const char *const kModes[] = {"mpv", "fsr", "vsr", "cnn", "anime4k"};
        int current = -1;
        for (int i = 0; i < 5; ++i)
            if (mpv.scaling == kModes[i]) current = i;
        const int m = radio_row("Scaling", {"mpv", "FSR 1", "VSR", "ArtCNN", "Anime4K"}, current);
        if (m >= 0) {
            f.mpv->command({"script-message", "glasir-scaling", kModes[m]});
            save(f, "video", "scaling", kModes[m]);
        }
        note("For a capture smaller than the screen. VSR: NVIDIA RTX or Intel GPUs. ArtCNN, Anime4K: more GPU time.");
    }
}

void sound(const MenuFrame &f, const MpvState &mpv, double now) {
    section("SOUND");
    MpvClient &c = *f.mpv;
    if (mpv.volume >= 0) g_menu.volume.follow(int(std::lround(mpv.volume)), now);
    if (slider("Volume", "##volume", &g_menu.volume.value, 0, 100, "%d %%", 2)) {
        g_menu.volume.changed(now);
        c.command({"set", "volume", std::to_string(g_menu.volume.value)});
    }
    if (ImGui::IsItemActive()) g_menu.volume.changed(now);
    bool mute = mpv.mute;
    if (ImGui::Checkbox("Mute", &mute)) c.command({"set", "mute", mute ? "yes" : "no"});

    if (mpv.boost < 0) {
        note("The card's sound is off or not found (logs\\audio.log).");
        return;
    }
    g_menu.boost.follow(mpv.boost, now);
    if (slider("Boost", "##boost", &g_menu.boost.value, 100, 400, "%d %%", 10)) {
        g_menu.boost.changed(now);
        c.command({"script-message", "glasir-audio-boost", std::to_string(g_menu.boost.value), "quiet"});
    }
    if (ImGui::IsItemActive()) g_menu.boost.changed(now);
    g_menu.delay.follow(std::max(mpv.delay, 0), now);
    if (slider("Audio delay", "##delay", &g_menu.delay.value, 0, 500, "%d ms", 5)) {
        g_menu.delay.changed(now);
        c.command({"script-message", "glasir-audio-delay", std::to_string(g_menu.delay.value), "quiet"});
    }
    if (ImGui::IsItemActive()) g_menu.delay.changed(now);
    note("Delay holds the sound back to match the picture.");
}

// An HDR capture: what is done with it now, and what mpv can change while
// playing. mpv's own state is what is shown (it is what reaches the screen);
// what Glasir decided at the start fills in until mpv has a picture.
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
    // The peak fitting uses: mpv's while it fits, else glasir.ini's or the
    // screen's own; 1000 when nobody knows.
    const int known_peak = mpv.target_peak > 0 ? mpv.target_peak : f.hdr_peak_nits;
    g_menu.hdr_peak.follow(known_peak > 0 ? known_peak : 1000, now);
    const bool dynamic = g_menu.hdr_dynamic >= 0 ? g_menu.hdr_dynamic == 1 : f.hdr_dynamic_peak;

    char text[200];
    if (!out_hdr) {
        // mpv puts out SDR although Glasir asked for HDR: Windows gave it no HDR output.
        const char *why = f.hdr_output == "hdr" ? "mpv got no HDR output from Windows" : f.hdr_why.c_str();
        std::snprintf(text, sizeof(text), "HDR tone mapped to SDR%s%s.", *why ? ": " : "", why);
    } else if (fit) {
        std::snprintf(text, sizeof(text), "HDR10 to the screen, fitted to its %d nits.",
                      int(std::lround(mpv.out_peak > 0 ? mpv.out_peak : g_menu.hdr_peak.value)));
    } else if (mpv.out_peak > 0) {
        std::snprintf(text, sizeof(text), "HDR10 to the screen, as the console sends it (peak %d nits).",
                      int(std::lround(mpv.out_peak)));
    } else {
        std::snprintf(text, sizeof(text), "HDR10 to the screen, as the console sends it.");
    }
    note(text);

    if (out_hdr) {
        bool want = fit;
        if (ImGui::Checkbox("Fit highlights to this screen", &want)) {
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
        if (!fit) {
            note("Off: set the console's HDR calibration while playing here. On: for highlights that clip to white.");
            return;
        }
        ImGui::Indent();
        if (slider("Screen peak", "##hdr_peak", &g_menu.hdr_peak.value, 100, 4000, "%d nits", 50)) {
            g_menu.hdr_peak.changed(now);
            c.command({"set", "target-peak", std::to_string(g_menu.hdr_peak.value)});
            // A step with the keys is kept at once, a drag when it ends.
            if (!ImGui::IsItemActive()) save(f, "hdr", "display_peak_nits", std::to_string(g_menu.hdr_peak.value));
        }
        if (ImGui::IsItemActive()) g_menu.hdr_peak.changed(now);
        if (ImGui::IsItemDeactivatedAfterEdit())
            save(f, "hdr", "display_peak_nits", std::to_string(g_menu.hdr_peak.value));
    }

    // Tone mapping (fitting, or SDR output): the curve, and the peak it
    // starts from. mpv's "auto" is its spline.
    static const char *const kCurves[] = {"bt.2390", "spline", "bt.2446a", "st2094-40", "hable", "clip"};
    int current = mpv.tone_mapping == "auto" ? 1 : -1;
    for (int i = 0; i < 6; ++i)
        if (mpv.tone_mapping == kCurves[i]) current = i;
    const int curve = radio_row("Curve", {"BT.2390", "Spline", "BT.2446a", "ST 2094-40", "Hable", "Clip"}, current);
    if (curve >= 0) {
        c.command({"set", "tone-mapping", kCurves[curve]});
        save(f, "hdr", "tone_mapping", kCurves[curve]);
    }
    bool measure = mpv.compute_peak;
    if (ImGui::Checkbox("Measure each frame's peak", &measure)) {
        g_menu.hdr_dynamic = measure ? 1 : 0;
        c.command({"set", "hdr-compute-peak", measure ? "yes" : "no"});
        save(f, "hdr", "dynamic_peak", measure ? "1" : "0");
    }
    if (out_hdr) ImGui::Unindent();
}

void display(const MenuFrame &f, const MpvState &mpv) {
    section("DISPLAY");
    // Glasir moves mpv's window (and the black around a picture of another
    // shape than the screen); mpv changes its border.
    // Full screen is mpv's own (f and a double-click switch it too); leaving
    // it goes back to the borderless window or the window it came from.
    const bool borderless = g_menu.borderless < 0 ? f.borderless : g_menu.borderless == 1;
    const int current = mpv.fullscreen ? 0 : borderless ? 1 : 2;
    const int choice = radio_row("Show as", {"Full screen", "Borderless", "Window"}, current);
    if (choice == 0) {
        f.mpv->command({"set", "fullscreen", "yes"});
        save(f, "window", "mode", "fullscreen");
    } else if (choice > 0) {
        g_menu.borderless = choice == 1 ? 1 : 0;
        f.mpv->command({"script-message", "glasir-window", choice == 1 ? "borderless" : "windowed"});
        save(f, "window", "mode", choice == 1 ? "borderless" : "windowed");
    }
    // Which screen Glasir.exe chose and what it took its variable refresh to
    // be: frame generation paces by that, so a wrong word shows here.
    const std::string told = told_text(f.display_name, f.display_hz, f.display_vrr, f.display_vrr_set);
    if (!told.empty()) note(told.c_str());
    const bool hdr = f.source_hdr;
    if (!hdr && !f.hdr_available) return;  // a card without HDR modes: nothing to switch to
    if (hdr) hdr_controls(f, mpv, ImGui::GetTime());
    if (ImGui::Button(hdr ? "Switch to SDR" : "Switch to HDR", ImVec2(-1, 0))) {
        // Glasir starts the picture again in the other mode (mpv's exit
        // code 42 asks for that).
        save(f, "video", "mode", hdr ? "sdr" : "hdr");
        f.mpv->command({"quit", "42"});
    }
    note(hdr ? "Restarts the picture in SDR." : "Restarts the picture in HDR (needs HDR on the console and a card "
                                                "with a 10-bit mode).");
}

}  // namespace

void apply_style(float scale) { ui::apply_style(scale, 0.90f); }

void draw_toast(const std::string &text, float alpha, float scale) {
    const ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.05f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * scale, 10.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushFont(bold_font(), ImGui::GetStyle().FontSizeBase * 1.25f);
    ImGui::PushTextWrapPos(io.DisplaySize.x * 0.5f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar(4);
}

void draw_rates(const Host &host, float scale) {
    if (!host.framegen_status) return;
    const FramegenStatus st = host.framegen_status();
    std::string text = rates_text(st);
    const std::string screen = screen_text(st);
    if (!text.empty() && !screen.empty()) text += "  \xC2\xB7  ";
    text += screen;
    if (text.empty()) return;
    ImGui::SetNextWindowPos(ImVec2(12.0f * scale, 12.0f * scale));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * scale, 4.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##rates", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar(3);
}

bool draw_menu(const MenuFrame &f) {
    ImGuiIO &io = ImGui::GetIO();
    const double now = ImGui::GetTime();
    const bool opened = ImGui::GetFrameCount() != g_menu.last_frame + 1;
    g_menu.last_frame = ImGui::GetFrameCount();

    const float width = std::min(io.DisplaySize.x, std::max(io.DisplaySize.x * 0.26f, 380.0f * f.scale));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - width, 0));
    ImGui::SetNextWindowSize(ImVec2(width, io.DisplaySize.y));
    if (opened) ImGui::SetNextWindowFocus();
    bool keep = true;
    ImGui::Begin("Glasir", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    if (opened) ImGui::SetNavCursorVisible(true);  // the first item shows as chosen at once
    const MpvState mpv = f.mpv->state();

    ImGui::PushFont(bold_font(), ImGui::GetStyle().FontSizeBase * 1.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, kGold);
    ImGui::TextUnformatted("Glasir");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    note(status_line(f, mpv).c_str());

    picture(f, mpv);
    if (mpv.connected) {
        sound(f, mpv, now);
        display(f, mpv);
    } else {
        section("SOUND");
        note("Connecting to mpv...");
    }

    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y * 2));
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Close", ImVec2(half, 0))) keep = false;
    ImGui::SameLine();
    if (ImGui::Button("Quit Glasir", ImVec2(half, 0))) f.mpv->command({"quit"});
    note("Tab or Esc: close   \xC2\xB7   F6: DLSS 5   \xC2\xB7   F8: frame generation   \xC2\xB7   "
         "S: screenshot   \xC2\xB7   F9: save a bug report");
    ImGui::End();
    return keep;
}

}  // namespace glasir::overlay
