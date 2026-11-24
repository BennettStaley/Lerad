// SPDX-License-Identifier: MIT
// Checks what is done with an HDR capture on each kind of screen: hdr_plan,
// its text, mpv's options and what the in-game menu is told.
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "app_version.h"
#include "hdr.h"
#include "mpv_launch.h"
#include "settings.h"

using namespace app;

namespace {

int g_failures = 0;

void expect(bool ok, const char *what) {
    std::printf("%-76s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_failures;
}

bool has(const std::vector<std::wstring> &args, const std::wstring &arg) {
    for (const auto &a : args)
        if (a == arg) return true;
    return false;
}

// An argument that starts with this.
bool has_prefix(const std::vector<std::wstring> &args, const std::wstring &prefix) {
    for (const auto &a : args)
        if (a.rfind(prefix, 0) == 0) return true;
    return false;
}

DisplayInfo screen(bool hdr_supported, bool hdr_on, double peak, double sdr_white = 0) {
    DisplayInfo d;
    d.found = true;
    d.hdr_supported = hdr_supported;
    d.hdr_on = hdr_on;
    d.peak_nits = peak;
    d.sdr_white_nits = sdr_white;
    return d;
}

MpvPlan p010(const DisplayInfo &d) {
    MpvPlan plan;
    plan.layout.format = PixelFormat::P010;
    plan.layout.width = 1920;
    plan.layout.height = 1080;
    plan.fps = 60;
    plan.display = d;
    return plan;
}

std::wstring env(const std::vector<std::pair<std::wstring, std::wstring>> &e, const wchar_t *name) {
    for (const auto &[k, v] : e)
        if (k == name) return v;
    return L"(unset)";
}

// HDR10 to the screen with the values untouched.
bool passes_through(const std::vector<std::wstring> &a) {
    return has(a, L"--target-colorspace-hint=yes") && has(a, L"--target-colorspace-hint-mode=source") &&
           has(a, L"--d3d11-output-csp=pq") && has(a, L"--d3d11-output-format=rgb10_a2") &&
           has(a, L"--hdr-compute-peak=no") && !has_prefix(a, L"--target-peak");
}

// HDR10 to the screen, tone mapped to this peak.
bool fits(const std::vector<std::wstring> &a, int peak) {
    return has(a, L"--target-colorspace-hint=yes") && has(a, L"--target-colorspace-hint-mode=target") &&
           has(a, L"--d3d11-output-csp=pq") && has(a, L"--d3d11-output-format=rgb10_a2") &&
           has(a, L"--target-peak=" + std::to_wstring(peak));
}

// Tone mapped into an SDR swap chain.
bool to_sdr(const std::vector<std::wstring> &a) {
    return has(a, L"--target-colorspace-hint=no") && has(a, L"--d3d11-output-csp=srgb") &&
           !has_prefix(a, L"--d3d11-output-format") && !has_prefix(a, L"--target-peak") &&
           !has_prefix(a, L"--target-colorspace-hint-mode");
}

}  // namespace

int main() {
    const DisplayInfo hdr_on = screen(true, true, 400, 420);    // like the Odyssey G9: reports 400 nits
    const DisplayInfo hdr_off = screen(true, false, 1000);      // an HDR screen, "Use HDR" off in Windows
    const DisplayInfo no_hdr = screen(false, false, 0);         // an SDR screen
    const DisplayInfo hdr_on_no_peak = screen(true, true, 0);   // HDR on, the screen reports no peak
    const DisplayInfo unread;                                   // the screen could not be read

    Settings s;
    s.hdr = true;
    {
        // The defaults: output = auto, fit_to_screen = 0.
        HdrPlan p = hdr_plan(s, hdr_on);
        expect(p.output == HdrPlan::Output::Hdr && p.reason == HdrPlan::Reason::HdrOn && !p.fit &&
                   p.peak_nits == 400 && p.peak_reported && !p.forced_without_hdr,
               "auto, HDR on: HDR10, not fitted; the screen's 400 nits known");
        expect(hdr_plan_text(p) == L"HDR10 to the screen, as the console sends it", "its text");
        expect(passes_through(mpv_arguments(p010(hdr_on), s)),
               "auto, HDR on: mpv passes the picture through (source metadata, no peak)");

        p = hdr_plan(s, hdr_off);
        expect(p.output == HdrPlan::Output::Sdr && p.reason == HdrPlan::Reason::HdrOff && !p.fit,
               "auto, HDR off in Windows: SDR");
        expect(hdr_plan_text(p) == L"Tone mapped to SDR: HDR is off in Windows", "its text");
        expect(to_sdr(mpv_arguments(p010(hdr_off), s)), "auto, HDR off: mpv tone maps into an SDR swap chain");

        p = hdr_plan(s, no_hdr);
        expect(p.output == HdrPlan::Output::Sdr && p.reason == HdrPlan::Reason::NoHdr && p.peak_nits == 0,
               "auto, a screen without HDR: SDR");
        expect(hdr_plan_text(p) == L"Tone mapped to SDR: this screen has no HDR", "its text");
        expect(to_sdr(mpv_arguments(p010(no_hdr), s)), "auto, no HDR: mpv tone maps into an SDR swap chain");

        p = hdr_plan(s, unread);
        expect(p.output == HdrPlan::Output::Sdr && p.reason == HdrPlan::Reason::NoScreen,
               "auto, a screen that could not be read: SDR (right everywhere)");
        expect(to_sdr(mpv_arguments(p010(unread), s)), "auto, unread screen: SDR swap chain");

        expect(passes_through(mpv_arguments(p010(hdr_on_no_peak), s)), "auto, HDR on, no peak reported: passed through");
    }
    {
        // The same picture tags in every case: HDR10, the peak from the settings file.
        s.hdr_peak_nits = 800;
        const std::wstring tags = L"--vf=@app-colour:format=colormatrix=bt.2020-ncl:colorlevels=limited:primaries=bt.2020:"
                                  L"gamma=pq:max-luma=800:max_cll=800";
        expect(has(mpv_arguments(p010(hdr_on), s), tags) && has(mpv_arguments(p010(no_hdr), s), tags),
               "the frames are tagged HDR10 with " APP_INI "'s peak, whatever the output");
        s.hdr_peak_nits = 1000;
    }
    {
        // output = hdr / sdr: always that.
        Settings f = s;
        f.hdr_output = Settings::HdrOutput::Hdr;
        HdrPlan p = hdr_plan(f, no_hdr);
        expect(p.output == HdrPlan::Output::Hdr && p.reason == HdrPlan::Reason::Setting && p.forced_without_hdr,
               "output = hdr on an SDR screen: HDR10 all the same, and it says so");
        expect(hdr_plan_text(p).find(L"though HDR is off in Windows") != std::wstring::npos, "its text warns");
        expect(passes_through(mpv_arguments(p010(no_hdr), f)), "output = hdr, SDR screen: passed through");
        expect(passes_through(mpv_arguments(p010(unread), f)), "output = hdr, unread screen: passed through");
        p = hdr_plan(f, hdr_on);
        expect(p.output == HdrPlan::Output::Hdr && !p.forced_without_hdr, "output = hdr, HDR on: HDR10");

        f.hdr_output = Settings::HdrOutput::Sdr;
        p = hdr_plan(f, hdr_on);
        expect(p.output == HdrPlan::Output::Sdr && p.reason == HdrPlan::Reason::Setting && !p.fit,
               "output = sdr with HDR on: SDR");
        expect(hdr_plan_text(p) == L"Tone mapped to SDR: as chosen under Send it as", "its text");
        expect(to_sdr(mpv_arguments(p010(hdr_on), f)), "output = sdr, HDR on: SDR swap chain");
        expect(to_sdr(mpv_arguments(p010(no_hdr), f)), "output = sdr, SDR screen: SDR swap chain");
    }
    {
        // fit_to_screen = 1: tone mapped to the screen's peak, known or given.
        Settings f = s;
        f.hdr_fit_to_screen = true;
        HdrPlan p = hdr_plan(f, hdr_on);
        expect(p.fit && p.peak_nits == 400 && p.peak_reported, "fit, HDR on: fitted to the 400 nits the screen reports");
        expect(hdr_plan_text(p) == L"HDR10 to the screen, fitted to its 400 nits", "its text");
        auto a = mpv_arguments(p010(hdr_on), f);
        expect(fits(a, 400) && has(a, L"--hdr-compute-peak=no") && has(a, L"--tone-mapping=bt.2390"),
               "fit: mpv's target mode, --target-peak=400, no peak measuring; auto curve: bt.2390");
        expect(has(mpv_arguments(p010(hdr_off), f), L"--tone-mapping=auto"), "SDR output, auto curve: mpv's own");
        f.tone_mapping = L"hable";
        expect(has(mpv_arguments(p010(hdr_on), f), L"--tone-mapping=hable") &&
                   has(mpv_arguments(p010(hdr_off), f), L"--tone-mapping=hable"),
               "a named curve reaches mpv as it is");
        f.tone_mapping = L"auto";

        f.display_peak_nits = 1000;
        p = hdr_plan(f, hdr_on);
        expect(p.fit && p.peak_nits == 1000 && !p.peak_reported, "fit, display_peak_nits = 1000: that, not the screen's");
        expect(fits(mpv_arguments(p010(hdr_on), f), 1000), "fit: --target-peak=1000");
        expect(fits(mpv_arguments(p010(hdr_on_no_peak), f), 1000), "fit, no peak reported, one given: fitted to it");

        f.display_peak_nits = 0;
        p = hdr_plan(f, hdr_on_no_peak);
        expect(p.output == HdrPlan::Output::Hdr && !p.fit && p.peak_nits == 0,
               "fit, but no peak known: nothing to fit to, left to the screen");
        expect(passes_through(mpv_arguments(p010(hdr_on_no_peak), f)), "fit, no peak known: passed through");

        DisplayInfo silly = hdr_on;
        silly.peak_nits = 20;  // a screen reporting nonsense
        expect(!hdr_plan(f, silly).fit, "fit, a reported peak that makes no sense: not used");

        f.tone_mapping = L"mobius";
        f.hdr_dynamic_peak = true;
        a = mpv_arguments(p010(hdr_on), f);
        expect(fits(a, 400) && has(a, L"--tone-mapping=mobius") && has(a, L"--hdr-compute-peak=yes"),
               "fit: tone_mapping and dynamic_peak reach mpv");
        a = mpv_arguments(p010(hdr_off), f);
        expect(to_sdr(a) && has(a, L"--tone-mapping=mobius") && has(a, L"--hdr-compute-peak=yes"),
               "SDR output: tone_mapping and dynamic_peak reach mpv; fitting does not apply");
        f.hdr_fit_to_screen = false;
        a = mpv_arguments(p010(hdr_on), f);
        expect(passes_through(a) && has(a, L"--tone-mapping=mobius"),
               "passed through: no peak measuring whatever dynamic_peak says (it would tone map)");
    }
    {
        // An SDR capture: an SDR swap chain, said outright, on any screen.
        Settings sdr;
        for (const DisplayInfo &d : {hdr_on, hdr_off, no_hdr, unread}) {
            MpvPlan plan = p010(d);
            plan.layout.format = PixelFormat::NV12;
            const auto a = mpv_arguments(plan, sdr);
            expect(has(a, L"--target-colorspace-hint=no") && has(a, L"--d3d11-output-csp=srgb") &&
                       !has_prefix(a, L"--tone-mapping") && !has_prefix(a, L"--target-peak") &&
                       !has_prefix(a, L"--hdr-compute-peak"),
                   "SDR capture: an SDR swap chain and no HDR options");
        }
    }
    {
        // A 10-bit capture that goes from SDR to HDR or back while playing:
        // the same decisions as mpv's properties, set over IPC.
        using Properties = std::vector<std::pair<std::wstring, std::wstring>>;
        auto value = [](const Properties &p, const wchar_t *name) {
            for (const auto &[k, v] : p)
                if (k == name) return v;
            return std::wstring(L"(unset)");
        };
        auto never_rebuilds = [](const Properties &p) {
            for (const auto &[k, v] : p)
                if (k.rfind(L"d3d11-", 0) == 0) return false;
            return !p.empty() && p.back().first == L"target-colorspace-hint";
        };
        Properties p = hdr_live_properties(hdr_plan(s, hdr_on), s, true);
        expect(value(p, L"target-colorspace-hint") == L"yes" && value(p, L"target-colorspace-hint-mode") == L"source" &&
                   value(p, L"target-peak") == L"auto" && value(p, L"hdr-compute-peak") == L"no" &&
                   value(p, L"tone-mapping") == L"bt.2390" && never_rebuilds(p),
               "live to HDR, HDR on: passed through, as a start in HDR (the hint last, no d3d11 option)");
        p = hdr_live_properties(hdr_plan(s, hdr_off), s, true);
        expect(value(p, L"target-colorspace-hint") == L"no" && value(p, L"target-peak") == L"auto" &&
                   value(p, L"hdr-compute-peak") == L"no" && value(p, L"tone-mapping") == L"auto" &&
                   value(p, L"target-colorspace-hint-mode") == L"(unset)" && never_rebuilds(p),
               "live to HDR, HDR off in Windows: tone mapped into the SDR swap chain");
        Settings f = s;
        f.hdr_fit_to_screen = true;
        f.hdr_dynamic_peak = true;
        f.tone_mapping = L"hable";
        p = hdr_live_properties(hdr_plan(f, hdr_on), f, true);
        expect(value(p, L"target-colorspace-hint") == L"yes" && value(p, L"target-colorspace-hint-mode") == L"target" &&
                   value(p, L"target-peak") == L"400" && value(p, L"hdr-compute-peak") == L"yes" &&
                   value(p, L"tone-mapping") == L"hable" && never_rebuilds(p),
               "live to HDR, fitted: the screen's peak, the curve and peak measuring reach mpv");
        p = hdr_live_properties(hdr_plan(f, hdr_off), f, true);
        expect(value(p, L"target-colorspace-hint") == L"no" && value(p, L"hdr-compute-peak") == L"yes" &&
                   value(p, L"tone-mapping") == L"hable",
               "live to HDR, SDR output: the curve and peak measuring; fitting does not apply");
        for (const DisplayInfo &d : {hdr_on, hdr_off, no_hdr, unread}) {
            p = hdr_live_properties(hdr_plan(f, d), f, false);
            expect(p.size() == 2 && value(p, L"target-colorspace-hint") == L"no" && value(p, L"target-peak") == L"auto" &&
                       never_rebuilds(p),
                   "live to SDR: an SDR swap chain, and the peak from fitting taken back");
        }
    }
    {
        // What the menu is told, and the menu's white.
        auto e = hdr_environment(s, hdr_on);
        expect(env(e, L"HDR_OUTPUT") == L"hdr" && env(e, L"HDR_WHY").empty() &&
                   env(e, L"HDR_PEAK_NITS") == L"400" && env(e, L"HDR_DYNAMIC_PEAK") == L"0" &&
                   env(e, L"PAPER_WHITE_NITS") == L"420",
               "menu, HDR on: hdr, the screen's peak, white at Windows' SDR brightness");
        e = hdr_environment(s, hdr_off);
        expect(env(e, L"HDR_OUTPUT") == L"sdr" && env(e, L"HDR_WHY") == L"HDR is off in Windows" &&
                   env(e, L"PAPER_WHITE_NITS") == L"203",
               "menu, HDR off: sdr and why; white at 203 nits when Windows names none");
        Settings sdr;
        sdr.paper_white_nits = 300;
        e = hdr_environment(sdr, hdr_on);
        expect(env(e, L"HDR_OUTPUT").empty() && env(e, L"HDR_WHY").empty() &&
                   env(e, L"PAPER_WHITE_NITS") == L"300",
               "menu, SDR capture: nothing left of an HDR start; paper_white_nits from " APP_INI);
        e = hdr_environment(sdr, hdr_on, true);
        expect(env(e, L"HDR_OUTPUT") == L"hdr" && env(e, L"HDR_PEAK_NITS") == L"400" &&
                   env(e, L"HDR_DYNAMIC_PEAK") == L"0" && env(e, L"PAPER_WHITE_NITS") == L"300",
               "menu, a 10-bit capture shown as SDR: told what is done with HDR, it may come while playing");
        e = hdr_environment(sdr, hdr_off, true);
        expect(env(e, L"HDR_OUTPUT") == L"sdr" && env(e, L"HDR_WHY") == L"HDR is off in Windows",
               "menu, the same with HDR off in Windows: sdr and why");
        DisplayInfo odd = hdr_on;
        odd.sdr_white_nits = 5000;
        expect(paper_white_nits(s, odd) == 203.0, "a white that makes no sense: 203 nits");
    }
    {
        // The settings file: the new key, and the default file's [hdr] section parses.
        Settings t;
        std::wstring error;
        expect(apply_override(t, L"hdr.fit_to_screen=1", error) && t.hdr_fit_to_screen, "--set hdr.fit_to_screen=1");
        expect(!apply_override(t, L"hdr.fit_to_screen=maybe", error), "hdr.fit_to_screen = maybe is refused");
        expect(std::string(default_settings_text()).find("\nfit_to_screen = 0\n") != std::string::npos,
               "the default " APP_INI " has fit_to_screen = 0");
        expect(!Settings().hdr_fit_to_screen && Settings().hdr_output == Settings::HdrOutput::Auto,
               "defaults: output = auto, not fitted");
    }

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
