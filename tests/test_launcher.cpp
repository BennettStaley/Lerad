// SPDX-License-Identifier: MIT
// Checks the launcher's words and what it saves: the cards, screens and GPUs
// as it lists them, "what you will get" as four steps, the line beside Play,
// the console's settings, where advice is shown, the frame generation choices
// on a screen, and that only changed values go into the settings file.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "app_version.h"
#include "launcher_text.h"

using namespace app;

namespace {

int g_failures = 0;

void expect(bool ok, const char *what) {
    std::printf("%-76s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_failures;
}

VideoMode mode(const wchar_t *subtype, UINT32 w, UINT32 h, double fps) {
    VideoMode m;
    m.subtype = subtype;
    m.width = w;
    m.height = h;
    m.fps = fps;
    for (PixelFormat c : {PixelFormat::NV12, PixelFormat::P010})
        if (m.subtype == format_name(c)) {
            m.format = c;
            m.supported = true;
        }
    return m;
}

// A 4K 120 Hz HDR monitor with FreeSync that the driver has switched off.
DisplayInfo proart() {
    DisplayInfo d;
    d.found = true;
    d.number = 1;
    d.name = L"PA279CDV";
    d.device = L"\\\\.\\DISPLAY1";
    d.width = 3840;
    d.height = 2160;
    d.refresh_hz = 120;
    d.gpu_vendor = kVendorNvidia;
    d.hdr_supported = d.hdr_on = true;
    d.peak_nits = 1000;
    d.vrr = DisplayInfo::Vrr::Off;
    d.vrr_kind = L"FreeSync";
    d.vrr_min_hz = 48;
    d.vrr_max_hz = 120;
    return d;
}

// The target use: a PS5 at 4K30 HDR through an Elgato 4K X on that screen.
AdviceInput target() {
    AdviceInput in;
    in.settings.dlss5 = Settings::Dlss5::Auto;  // asked for (it is off by default)
    in.display = proart();
    GpuInfo g;
    g.name = L"NVIDIA GeForce RTX 5090";
    g.vendor = kVendorNvidia;
    g.found = true;
    g.dedicated_mb = 32000;
    g.screens = {in.display.device};
    in.gpus = {g};
    in.gpu = choose_gpu(in.gpus, in.display.device, L"");
    in.card_found = true;
    in.card = L"Elgato 4K X";
    in.modes = {mode(L"NV12", 3840, 2160, 60), mode(L"NV12", 2560, 1440, 60), mode(L"P010", 3840, 2160, 30),
                mode(L"P010", 2560, 1440, 60)};
    std::wstring error;
    for (const wchar_t *o : {L"display.monitor=1", L"video.mode=hdr", L"capture.width=3840", L"capture.height=2160",
                             L"capture.fps=30"})
        apply_override(in.settings, o, error);
    return in;
}

bool has(const std::vector<SettingValue> &values, const wchar_t *setting) {
    for (const SettingValue &v : values)
        if (setting_text(v) == setting) return true;
    return false;
}

bool contains(const std::wstring &text, const wchar_t *part) { return text.find(part) != std::wstring::npos; }

}  // namespace

int main() {
    std::wstring error;
    {
        // The cards, screens and GPUs as the lists show them.
        expect(card_title(L"Video (00 Pro Capture HDMI 4K+)", true) == L"Magewell Pro Capture HDMI 4K+",
               "a card: the product's name out of the driver's wrapping, with its maker");
        expect(card_title(L"Elgato 4K X") == L"Elgato 4K X" && card_title(L"Video (Game Capture)") == L"Game Capture" &&
                   card_title(L"Magewell USB Capture", true) == L"Magewell USB Capture",
               "a card named plainly stays as it is; the maker is not said twice");
        DisplayInfo d = proart();
        expect(screen_title(d) == L"1 · PA279CDV · 3840 x 2160 · 120 Hz",
               "a screen's title: number, name, size, rate");
        expect(screen_details(d) == L"HDR on, peak 1000 nits · variable refresh off (FreeSync)",
               "its details: HDR with the peak, variable refresh with the screen's brand for it");
        expect(vrr_brand_text(d) == L"FreeSync, 48-120 Hz" && vrr_found_word(d) == L"off",
               "the brand with its range, and what was found in a word");
        d.primary = true;
        d.hdr_on = false;
        d.vrr = DisplayInfo::Vrr::On;
        d.vrr_kind = L"G-SYNC Compatible";
        expect(contains(screen_title(d), L"120 Hz, main") && screen_hdr_text(d) == L"HDR off in Windows" &&
                   screen_vrr_text(d) == L"variable refresh on (G-SYNC Compatible)" && vrr_found_word(d) == L"on",
               "the main screen, HDR off in Windows, variable refresh on");
        // An AMD or Intel GPU: what the screen can do, not whether it is on.
        d.vrr = DisplayInfo::Vrr::Supported;
        d.vrr_kind = L"FreeSync";
        d.hdr_supported = false;
        expect(screen_vrr_text(d) == L"variable refresh on or off: not known (FreeSync)" && screen_hdr_text(d) == L"no HDR" &&
                   vrr_found_word(d) == L"not known",
               "variable refresh only known from the EDID: said so; no HDR");
        d.vrr = DisplayInfo::Vrr::Unknown;
        d.vrr_kind.clear();
        d.vrr_min_hz = d.vrr_max_hz = 0;
        expect(screen_vrr_text(d) == L"variable refresh not known" && vrr_brand_text(d).empty(),
               "nothing known about variable refresh");
        d.vrr = DisplayInfo::Vrr::Unsupported;
        expect(screen_vrr_text(d) == L"no variable refresh" && vrr_found_word(d) == L"none", "no variable refresh");
        expect(monitor_setting(proart()) == L"1", "display.monitor for a screen: its number");
        d.number = 0;
        expect(monitor_setting(d) == L"\\\\.\\DISPLAY1", "... or Windows' name when it has none");
        GpuInfo g;
        expect(gpu_title(g) == L"not found", "no GPU");
        g.found = true;
        g.name = L"NVIDIA GeForce RTX 5090";
        g.dedicated_mb = 32000;
        expect(gpu_title(g) == L"NVIDIA GeForce RTX 5090 · 31 GB", "a GPU: name and memory");
        g.name = L"Intel(R) UHD Graphics";
        g.dedicated_mb = 128;
        expect(gpu_title(g) == L"Intel(R) UHD Graphics", "an integrated GPU: no memory to name");
    }
    {
        // What you will get, for the target use: four steps, in the order
        // the picture travels.
        using Kind = ResultStep::Kind;
        AdviceInput in = target();
        Advice a = advise(in);
        std::vector<ResultStep> steps = result_steps(in, a);
        expect(steps.size() == 4 && steps[0].kind == Kind::Console && steps[1].kind == Kind::Card &&
                   steps[2].kind == Kind::Frames && steps[3].kind == Kind::Screen,
               "the console, the capture card, the frames, the screen");
        expect(steps[0].title == L"Console, set it to" && steps[0].chips.size() == 4 && steps[0].chips[0] == L"2160p" &&
                   steps[0].chips[1] == L"HDR on" && steps[0].chips[2] == L"120 Hz output off" &&
                   steps[0].chips[3] == L"VRR off" && contains(console_hint(a), L"HDR calibration"),
               "the console: 2160p, HDR on, 120 Hz output off, VRR off, and the calibration hint");
        expect(steps[1].big == L"3840 x 2160" && steps[1].detail == L"30 fps · HDR (10-bit)" && !steps[1].matters,
               "the card: the size large, then the rate and the signal");
        expect(result_steps(in, a, true)[1].detail == L"30 fps · HDR (10-bit) · low latency",
               "... and that the card passes frames on early, where it does");
        expect(steps[2].big == L"30 → 60 fps" && steps[2].multiplier == 2 &&
                   steps[2].detail == L"x2 by " APP_NAME_W L". DLSS 5 on." && !steps[2].matters,
               "the frames: 30 shown at 60, by the app's own generator");
        expect(steps[3].big == L"120 Hz · HDR10" && steps[3].detail == L"PA279CDV, variable refresh off." &&
                   !steps[3].matters,
               "the screen: its rate and HDR10, its name and variable refresh");
        PlaySummary sum = play_summary(in, a);
        expect(sum.state == PlaySummary::State::Warning && sum.word == L"1 warning" &&
                   sum.text == L"A 30 fps game is shown at 60 fps, in HDR.",
               "beside Play: one warning (variable refresh is off), and what will be seen");

        // NVIDIA's generator where it is chosen, downloaded and the GPU has it.
        apply_override(in.settings, L"framegen.backend=dlss", error);
        a = advise(in);
        expect(result_steps(in, a)[2].detail == L"x2 by NVIDIA DLSS. DLSS 5 on.", "the frames by NVIDIA DLSS");
        in.dlssfg_installed = false;
        a = advise(in);
        expect(result_steps(in, a)[2].detail == L"x2 by " APP_NAME_W L". DLSS 5 on.",
               "... and by the app's own until it is downloaded");
        in.dlssfg_installed = true;
        apply_override(in.settings, L"framegen.backend=own", error);

        // Up to x4, HDR off in Windows, frame generation off.
        apply_override(in.settings, L"framegen.multiplier=4", error);
        in.display.hdr_on = false;
        a = advise(in);
        steps = result_steps(in, a);
        expect(steps[2].big == L"30 → 120 fps" && steps[2].multiplier == 4 && steps[3].big == L"120 Hz · SDR" &&
                   contains(steps[3].detail, L"Tone mapped to SDR: HDR is off in Windows.") && steps[3].matters &&
                   console_hint(a).empty(),
               "x4: 30 shown at 120; HDR tone mapped stands out, no calibration hint");
        expect(play_summary(in, a).text == L"A 30 fps game is shown at 120 fps, in SDR (tone mapped from HDR).",
               "... and the sentence beside Play says so");
        apply_override(in.settings, L"hdr.output=sdr", error);
        a = advise(in);
        expect(!result_steps(in, a)[3].matters, "tone mapped because it was asked for: nothing to stand out");
        apply_override(in.settings, L"framegen.enabled=0", error);
        a = advise(in);
        steps = result_steps(in, a);
        expect(steps[2].big == L"30 fps" && steps[2].multiplier == 1 && steps[2].detail == L"Frame generation off. DLSS 5 on." &&
                   contains(play_summary(in, a).text, L"Frame generation is off."),
               "frame generation off");
        // Frame generation on, but it cannot run (no dedup).
        apply_override(in.settings, L"framegen.enabled=1", error);
        apply_override(in.settings, L"frames.dedup=0", error);
        a = advise(in);
        steps = result_steps(in, a);
        expect(contains(steps[2].detail, L"Frame generation cannot run") && steps[2].matters, "... or unable to run");

        // An SDR 1440p60 capture on that screen, scaled up, VRR set by hand.
        in = target();
        for (const wchar_t *o : {L"video.mode=sdr", L"capture.width=0", L"capture.height=0", L"capture.fps=0",
                                 L"video.scaling=cnn", L"display.vrr=on", L"dlss5.enabled=off"})
            apply_override(in.settings, o, error);
        in.modes = {mode(L"NV12", 2560, 1440, 60)};
        a = advise(in);
        steps = result_steps(in, a);
        expect(steps.size() == 4 && steps[1].big == L"2560 x 1440" && steps[1].detail == L"60 fps · SDR" &&
                   steps[2].big == L"30 → 60 fps" &&
                   steps[2].detail == L"x2 by " APP_NAME_W L". A 60 fps game: 120 fps (x2). DLSS 5 off." &&
                   steps[3].big == L"120 Hz · SDR" &&
                   steps[3].detail == L"PA279CDV, variable refresh on (as set here). Scaled up from 1440 to 2160 lines "
                                      L"with ArtCNN.",
               "SDR 1440p60: two game rates, variable refresh as set, the scaling named");
        sum = play_summary(in, a);
        expect(sum.state == PlaySummary::State::Ready && sum.word == L"Ready" &&
                   sum.text == L"A 30 fps game is shown at 60 fps, in SDR.",
               "nothing in the way: Ready");

        // No card.
        in.card_found = false;
        a = advise(in);
        steps = result_steps(in, a);
        expect(steps.size() == 3 && steps[1].kind == Kind::Card && steps[1].matters && steps[1].problem &&
                   steps[1].big == L"No capture card" && steps[2].kind == Kind::Screen && !steps[2].problem,
               "no card: said at its step, and no frames to speak of");
        sum = play_summary(in, a);
        expect(sum.state == PlaySummary::State::Problem && sum.word == L"Cannot play" &&
                   sum.text == L"No capture card was found.",
               "... and beside Play: cannot play, and why");
        // A 60 Hz screen: 60 fps games get nothing.
        in = target();
        in.display.refresh_hz = 60;
        for (const wchar_t *o : {L"video.mode=sdr", L"capture.width=0", L"capture.height=0", L"capture.fps=0"})
            apply_override(in.settings, o, error);
        a = advise(in);
        expect(result_steps(in, a)[2].detail == L"x2 by " APP_NAME_W L". A 60 fps game: no frames added. DLSS 5 on.",
               "60 Hz: 30 at 60, 60 as it is");
    }
    {
        // Where advice goes, and what is hidden.
        expect(advice_place("hdr-off-in-windows") == AdvicePlace::Picture && advice_place("vrr-off") == AdvicePlace::Screen &&
                   advice_place("gpu-other-than-screen") == AdvicePlace::Screen &&
                   advice_place("screen-60hz") == AdvicePlace::Frames &&
                   advice_place("framegen-fewer-fit") == AdvicePlace::Frames &&
                   advice_place("rec-framegen-quality") == AdvicePlace::Frames &&
                   advice_place("dlss5-gpu") == AdvicePlace::Extras &&
                   advice_place("rec-scaling") == AdvicePlace::Picture &&
                   advice_place("card-hdr-limit") == AdvicePlace::Card &&
                   advice_place("capture-30fps") == AdvicePlace::Card,
               "each item beside the controls it is about");
        AdviceInput in = target();
        const Advice a = advise(in);
        const AdviceItem *vrr = find_advice(a, "vrr-off");
        const AdviceItem *quality = find_advice(a, "rec-framegen-quality");
        const AdviceItem *slow = find_advice(a, "capture-30fps");
        const AdviceItem *console = find_advice(a, "console-vrr");
        expect(vrr && advice_shown(*vrr) == AdviceShown::Notice, "a warning: above Play");
        expect(quality && quality->applied && advice_shown(*quality) == AdviceShown::Hidden,
               "a recommendation the settings already follow: not shown");
        expect(slow && advice_shown(*slow) == AdviceShown::Place, "a limit for information: in its place");
        expect(console && advice_shown(*console) == AdviceShown::Hidden, "the console's settings: on their own line");
        in.modes.push_back(mode(L"P010", 1920, 1080, 120));
        const Advice with120 = advise(in);
        const AdviceItem *fast = find_advice(with120, "capture-120");
        expect(fast && !fast->suggest.empty() && advice_shown(*fast) == AdviceShown::Place,
               "another way to set things up (a 120 fps mode): offered in its place, not above Play");
        in.modes.pop_back();
        apply_override(in.settings, L"video.scaling=fsr", error);
        apply_override(in.settings, L"capture.width=2560", error);
        apply_override(in.settings, L"capture.height=1440", error);
        apply_override(in.settings, L"capture.fps=60", error);
        const Advice scaled = advise(in);
        const AdviceItem *scaling = find_advice(scaled, "rec-scaling");
        expect(scaling && !scaling->applied && advice_shown(*scaling) == AdviceShown::Notice,
               "a setting that would suit this PC better: above Play, with Use");
        for (const wchar_t *o : {L"video.scaling=mpv", L"capture.width=3840", L"capture.height=2160", L"capture.fps=30"})
            apply_override(in.settings, o, error);
        // "Use" sets what an item suggests.
        Settings s = in.settings;
        apply_suggestion(s, *slow);
        expect(s.width == 2560 && s.height == 1440 && s.fps == 60, "Use on the 30 fps note: 1440p60");
        in.settings = s;
        expect(!find_advice(advise(in), "capture-30fps"), "... and the note is gone");
    }
    {
        // The frame generation choices on a screen.
        AdviceInput in = target();  // a fixed 120 Hz (VRR off)
        std::vector<MultiplierChoice> m = multiplier_choices(in);
        expect(m.size() == 3 && m[0].note == L"30 → 60 fps" && m[1].note == L"does not fit" &&
                   m[2].note == L"30 → 120 fps",
               "under each choice: what it comes to, or that it does not fit");
        expect(m.size() == 3 && m[0].usable && m[0].text == L"30 fps shown at 60" && !m[1].usable &&
                   contains(m[1].why, L"90 fps would not fall on whole refreshes of this fixed 120 Hz screen") &&
                   m[2].usable && m[2].text == L"30 fps shown at 120",
               "fixed 120 Hz, 30 fps: x2 and x4 usable, x3 not (and why)");
        expect(std::fabs(m[0].delay_ms - 16.7) < 0.1 && std::fabs(m[1].delay_ms - 22.2) < 0.1 && m[2].delay_ms == 25,
               "the game's own frame: 17, 22 and 25 ms later");
        in.display.vrr = DisplayInfo::Vrr::On;
        m = multiplier_choices(in);
        expect(m[1].usable && m[1].text == L"30 fps shown at 90", "with VRR: x3 too");
        in.display.refresh_hz = 60;
        m = multiplier_choices(in);
        expect(m[0].usable && !m[1].usable && !m[2].usable &&
                   contains(m[2].why, L"120 fps is more than this 60 Hz screen shows"),
               "60 Hz: x2 only, and why");
        in.display.refresh_hz = 120;
        apply_override(in.settings, L"framegen.max_output_hz=100", error);
        m = multiplier_choices(in);
        expect(m[1].usable && !m[2].usable && contains(m[2].why, L"over the limit of 100"), "a limit of 100: up to x3");
        in = target();
        apply_override(in.settings, L"framegen.enabled=0", error);
        expect(multiplier_choices(in).empty(), "frame generation off: nothing to choose from");
    }
    {
        // Only what changed is written.
        Settings from, to;
        expect(changed_settings(from, to).empty(), "nothing changed: nothing written");
        for (const wchar_t *o : {L"display.monitor=1", L"video.mode=hdr", L"capture.height=2160", L"capture.fps=30",
                                 L"capture.width=3840", L"framegen.multiplier=4", L"framegen.max_output_hz=100",
                                 L"framegen.show_rates=1", L"hdr.fit_to_screen=1", L"hdr.display_peak_nits=1000",
                                 L"hdr.tone_mapping=bt.2390", L"hdr.output=hdr", L"display.vrr=off",
                                 L"window.mode=fullscreen", L"video.sdr_range=full", L"video.hdr_peak_nits=4000",
                                 L"framegen.method=blend", L"hdr.dynamic_peak=1", L"dlss5.enabled=auto"})
            expect(apply_override(to, o, error), "an override is taken");
        const std::vector<SettingValue> v = changed_settings(from, to);
        expect(v.size() == 19, "19 values for 19 changes");
        expect(has(v, L"display.monitor=1") && has(v, L"video.mode=hdr") && has(v, L"capture.width=3840") &&
                   has(v, L"capture.height=2160") && has(v, L"capture.fps=30") && has(v, L"framegen.multiplier=4") &&
                   has(v, L"framegen.max_output_hz=100") && has(v, L"framegen.show_rates=1") &&
                   has(v, L"hdr.fit_to_screen=1") && has(v, L"hdr.display_peak_nits=1000") &&
                   has(v, L"hdr.tone_mapping=bt.2390") && has(v, L"hdr.output=hdr") && has(v, L"display.vrr=off") &&
                   has(v, L"window.mode=fullscreen") && has(v, L"video.sdr_range=full") &&
                   has(v, L"video.hdr_peak_nits=4000") && has(v, L"framegen.method=blend") &&
                   has(v, L"hdr.dynamic_peak=1") && has(v, L"dlss5.enabled=auto"),
               "each as " APP_INI " spells it");
        // Read back, they give the same settings again.
        Settings back = from;
        for (const SettingValue &value : v) apply_override(back, setting_text(value), error);
        expect(changed_settings(to, back).empty(), "written and read back: the same settings");
        // The capture's size and rate go together.
        Settings rate = from;
        apply_override(rate, L"capture.fps=59.94", error);
        const std::vector<SettingValue> r = changed_settings(from, rate);
        expect(r.size() == 3 && has(r, L"capture.width=0") && has(r, L"capture.fps=59.940"),
               "one of width, height and fps changed: all three written");
        // A file with other things in it keeps them.
        const std::string text = update_settings_text("[framegen]\nquality = fast ; mine\n[my]\nown = 1\n",
                                                      changed_settings(from, rate));
        expect(text.find("quality = fast ; mine") != std::string::npos && text.find("own = 1") != std::string::npos &&
                   text.find("fps = 59.940") != std::string::npos,
               "what was not touched stays in the file as it was");
    }

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
