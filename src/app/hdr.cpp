// SPDX-License-Identifier: MIT
#include "hdr.h"

#include <cmath>
#include <format>

#include "app_version.h"

namespace app {
namespace {

// A peak a screen can have. Screens that report nothing, or nonsense, count
// as unknown.
bool sane_peak(double nits) { return nits >= 80 && nits <= 10000; }

}  // namespace

HdrPlan hdr_plan(const Settings &s, const DisplayInfo &display) {
    HdrPlan p;
    switch (s.hdr_output) {
    case Settings::HdrOutput::Hdr:
        p.output = HdrPlan::Output::Hdr;
        p.reason = HdrPlan::Reason::Setting;
        p.forced_without_hdr = !display.hdr_on;
        break;
    case Settings::HdrOutput::Sdr:
        p.output = HdrPlan::Output::Sdr;
        p.reason = HdrPlan::Reason::Setting;
        break;
    default:
        // A screen that could not be read gets SDR: a tone mapped picture
        // looks right everywhere, PQ on an SDR screen never does.
        if (!display.found) {
            p.reason = HdrPlan::Reason::NoScreen;
        } else if (display.hdr_on) {
            p.output = HdrPlan::Output::Hdr;
            p.reason = HdrPlan::Reason::HdrOn;
        } else {
            p.reason = display.hdr_supported ? HdrPlan::Reason::HdrOff : HdrPlan::Reason::NoHdr;
        }
        break;
    }
    if (s.display_peak_nits > 0) {
        p.peak_nits = s.display_peak_nits;
    } else if (sane_peak(display.peak_nits)) {
        p.peak_nits = int(std::lround(display.peak_nits));
        p.peak_reported = true;
    }
    // Without a peak there is nothing to fit to: it is left to the screen.
    p.fit = p.output == HdrPlan::Output::Hdr && s.hdr_fit_to_screen && p.peak_nits > 0;
    return p;
}

std::wstring hdr_sdr_reason(const HdrPlan &plan) {
    if (plan.output != HdrPlan::Output::Sdr) return L"";
    switch (plan.reason) {
    case HdrPlan::Reason::Setting: return L"as chosen under Send it as";
    case HdrPlan::Reason::HdrOff: return L"HDR is off in Windows";
    case HdrPlan::Reason::NoHdr: return L"this screen has no HDR";
    default: return L"the screen could not be read";
    }
}

std::wstring hdr_plan_text(const HdrPlan &plan) {
    if (plan.output == HdrPlan::Output::Sdr) return L"Tone mapped to SDR: " + hdr_sdr_reason(plan);
    std::wstring text = plan.fit ? std::format(L"HDR10 to the screen, fitted to its {} nits", plan.peak_nits)
                                 : std::wstring(L"HDR10 to the screen, as the console sends it");
    if (plan.forced_without_hdr) text += L" (as chosen, though HDR is off in Windows)";
    return text;
}

const wchar_t *hdr_tone_mapping(const HdrPlan &plan, const Settings &s) {
    if (s.tone_mapping != L"auto") return s.tone_mapping.c_str();
    // HDR to an HDR screen: BT.2390 leaves everything below the highlights
    // as it is and only rolls off the top. mpv's own choice (spline) is made
    // for the long way down to SDR; between two HDR peaks it dims the whole
    // picture (1000 to 400 nits: white at 203 nits comes out at 124).
    return plan.output == HdrPlan::Output::Hdr ? L"bt.2390" : L"auto";
}

std::vector<std::wstring> hdr_mpv_options(const HdrPlan &plan, const Settings &s) {
    std::vector<std::wstring> a;
    // The curve is given in every case: the menu can switch fitting on while
    // playing, and then it is the one in use.
    a.push_back(std::wstring(L"--tone-mapping=") + hdr_tone_mapping(plan, s));
    if (plan.output == HdrPlan::Output::Sdr) {
        // An SDR swap chain, whatever the desktop is: with HDR on, Windows
        // shows it at its "SDR content brightness" like any SDR program.
        // mpv tone maps to it (the hint would ask for an HDR swap chain).
        a.push_back(L"--target-colorspace-hint=no");
        a.push_back(L"--d3d11-output-csp=srgb");
        a.push_back(s.hdr_dynamic_peak ? L"--hdr-compute-peak=yes" : L"--hdr-compute-peak=no");
        return a;
    }
    a.push_back(L"--target-colorspace-hint=yes");
    a.push_back(L"--d3d11-output-format=rgb10_a2");
    a.push_back(L"--d3d11-output-csp=pq");
    if (plan.fit) {
        // mpv's "target" mode: the picture is tone mapped to the screen's
        // peak, and the swap chain's metadata says that peak.
        a.push_back(L"--target-colorspace-hint-mode=target");
        a.push_back(std::format(L"--target-peak={}", plan.peak_nits));
        a.push_back(s.hdr_dynamic_peak ? L"--hdr-compute-peak=yes" : L"--hdr-compute-peak=no");
    } else {
        // mpv's "source" mode: the values go out untouched, with the
        // capture's own metadata. No --target-peak (it would tone map even
        // here) and no peak measuring (a frame brighter than its metadata
        // would be tone mapped too).
        a.push_back(L"--target-colorspace-hint-mode=source");
        a.push_back(L"--hdr-compute-peak=no");
    }
    return a;
}

std::vector<std::pair<std::wstring, std::wstring>> hdr_live_properties(const HdrPlan &plan, const Settings &s,
                                                                       bool hdr) {
    std::vector<std::pair<std::wstring, std::wstring>> p;
    if (!hdr) {
        p.emplace_back(L"target-peak", L"auto");
        p.emplace_back(L"target-colorspace-hint", L"no");
        return p;
    }
    const wchar_t *measure = s.hdr_dynamic_peak ? L"yes" : L"no";
    p.emplace_back(L"tone-mapping", hdr_tone_mapping(plan, s));
    if (plan.output == HdrPlan::Output::Sdr) {
        p.emplace_back(L"target-peak", L"auto");
        p.emplace_back(L"hdr-compute-peak", measure);
        p.emplace_back(L"target-colorspace-hint", L"no");
        return p;
    }
    // As hdr_mpv_options: fitted to the screen's peak, or untouched. The
    // hint comes last, so the swap chain changes once, for the right mode.
    p.emplace_back(L"target-colorspace-hint-mode", plan.fit ? L"target" : L"source");
    p.emplace_back(L"target-peak", plan.fit ? std::to_wstring(plan.peak_nits) : std::wstring(L"auto"));
    p.emplace_back(L"hdr-compute-peak", plan.fit ? measure : L"no");
    p.emplace_back(L"target-colorspace-hint", L"yes");
    return p;
}

double paper_white_nits(const Settings &s, const DisplayInfo &display) {
    if (s.paper_white_nits > 0) return s.paper_white_nits;
    if (display.sdr_white_nits >= 80 && display.sdr_white_nits <= 1000) return display.sdr_white_nits;
    return 203.0;
}

std::vector<std::pair<std::wstring, std::wstring>> hdr_environment(const Settings &s, const DisplayInfo &display,
                                                                   bool may_become_hdr) {
    std::vector<std::pair<std::wstring, std::wstring>> env;
    env.emplace_back(L"PAPER_WHITE_NITS", std::format(L"{:.0f}", paper_white_nits(s, display)));
    if (!s.hdr && !may_become_hdr) {
        for (const wchar_t *name : {L"HDR_OUTPUT", L"HDR_WHY", L"HDR_PEAK_NITS", L"HDR_DYNAMIC_PEAK"})
            env.emplace_back(name, L"");
        return env;
    }
    const HdrPlan p = hdr_plan(s, display);
    env.emplace_back(L"HDR_OUTPUT", p.output == HdrPlan::Output::Hdr ? L"hdr" : L"sdr");
    env.emplace_back(L"HDR_WHY", hdr_sdr_reason(p));
    // The peak fitting would use, and whether each frame's peak is measured
    // then: the menu switches fitting on with them.
    env.emplace_back(L"HDR_PEAK_NITS", std::to_wstring(p.peak_nits));
    env.emplace_back(L"HDR_DYNAMIC_PEAK", s.hdr_dynamic_peak ? L"1" : L"0");
    return env;
}

}  // namespace app
