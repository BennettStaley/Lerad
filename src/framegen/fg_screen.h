// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// What frame generation takes "the screen" to be: always the one mpv's
// window is on now, not the one the exe chose at start and not the PC's
// main one. Text and arithmetic only (tests/test_fg_pacing.cpp); the Windows
// calls are in fg_vblank.h.
//
// Two sources:
//   - Windows, asked by the add-on while it runs: which screen the window is
//     on, its mode and refresh rate (fg_vblank.h). Always current.
//   - The exe, once at start: every screen of the PC with what it found
//     about its variable refresh (NVIDIA's driver through NVAPI and the
//     monitor's EDID, src/app/vrr.h), in the environment variable
//     env SCREENS. The add-on does not read that again itself: NVAPI
//     inside mpv, next to the DLSS 5 add-ons, is not worth the risk. So a
//     screen that was not there at start, or was not told about, has VRR
//     "unknown" - which changes no pacing (MultiplierGate takes the most that
//     fits, and what the screen is seen doing settles the menu's words).
//
// env SCREENS: one entry per screen, entries apart by ';', fields by '|':
//   \\.\DISPLAY1|on|119.880|\\?\DISPLAY#AUS2704#...|1 (PA279CDV);\\.\DISPLAY2|unknown|239.760||2 (Odyssey G95SC)
// Windows' name for the output, VRR ("on", "off", "unknown"), the refresh
// rate at start, Windows' device path of the monitor plugged into it (may be
// empty), and a name for people (the screen's number as --system lists it,
// and the monitor's own name). The output's name alone does not say which
// monitor it is: when screens are plugged in or switched off, Windows may
// give a name to another one. With both paths known, a screen is only "the
// one the app saw" while the monitor is the same.

#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "fg_pacing.h"

namespace fg {

struct ScreenFacts {
    std::string device;  // "\\.\DISPLAY1"
    pacing::Vrr vrr = pacing::Vrr::Unknown;
    double hz = 0;       // at start; 0 unknown
    std::string monitor; // the monitor's device path ("\\?\DISPLAY#AUS2704#..."), "" unknown
    std::string name;    // "1 (PA279CDV)", "" none
};

inline const char *vrr_word(pacing::Vrr vrr) {
    return vrr == pacing::Vrr::On ? "on" : vrr == pacing::Vrr::Off ? "off" : "unknown";
}

inline pacing::Vrr vrr_from_word(const std::string &word) {
    return word == "on" ? pacing::Vrr::On : word == "off" ? pacing::Vrr::Off : pacing::Vrr::Unknown;
}

// The text of env SCREENS. A name's ';' and '|' become spaces; a monitor
// path with one (Windows makes none) is left out.
inline std::string screens_text(const std::vector<ScreenFacts> &screens) {
    std::string text;
    for (const ScreenFacts &s : screens) {
        if (s.device.empty()) continue;
        std::string name = s.name;
        for (char &c : name)
            if (c == ';' || c == '|') c = ' ';
        const std::string monitor = s.monitor.find_first_of(";|") == std::string::npos ? s.monitor : std::string();
        char hz[32];
        std::snprintf(hz, sizeof(hz), "%.3f", s.hz > 0 ? s.hz : 0.0);
        if (!text.empty()) text += ';';
        text += s.device + '|' + vrr_word(s.vrr) + '|' + hz + '|' + monitor + '|' + name;
    }
    return text;
}

// The screens in such a text. Entries without a device are left out; a VRR
// word that is none of the three, or a rate that is no number, is "unknown".
inline std::vector<ScreenFacts> parse_screens(const std::string &text) {
    std::vector<ScreenFacts> screens;
    for (size_t at = 0; at <= text.size();) {
        size_t end = text.find(';', at);
        if (end == std::string::npos) end = text.size();
        const std::string entry = text.substr(at, end - at);
        at = end + 1;
        constexpr int kFields = 5;
        std::string fields[kFields];
        size_t from = 0;
        for (int i = 0; i < kFields && from <= entry.size(); ++i) {
            // The name is the rest, whatever it holds.
            size_t to = i < kFields - 1 ? entry.find('|', from) : std::string::npos;
            if (to == std::string::npos) to = entry.size();
            fields[i] = entry.substr(from, to - from);
            from = to + 1;
        }
        if (fields[0].empty()) continue;
        ScreenFacts s;
        s.device = fields[0];
        s.vrr = vrr_from_word(fields[1]);
        s.hz = std::atof(fields[2].c_str());
        if (!(s.hz > 0 && s.hz < 10000)) s.hz = 0;
        s.monitor = fields[3];
        s.name = fields[4];
        screens.push_back(std::move(s));
    }
    return screens;
}

// Windows' device names are told apart without regard to case.
inline bool same_device(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

inline const ScreenFacts *find_screen(const std::vector<ScreenFacts> &screens, const std::string &device) {
    for (const ScreenFacts &s : screens)
        if (same_device(s.device, device)) return &s;
    return nullptr;
}

// What Windows says now about the screen the window is on.
struct ScreenNow {
    bool found = false;
    std::string device;   // "\\.\DISPLAY2"
    int hz_whole = 0;     // the mode's rate as Windows rounds it (120), 0 unknown
    double hz_exact = 0;  // with its fraction (119.88) from the display configuration, 0 unknown
    std::string monitor;  // the device path of the monitor on it now, "" unknown
};

// What frame generation goes by for that screen.
struct ScreenBelief {
    double hz = 0;  // 0: unknown (the pacing then takes 60)
    pacing::Vrr vrr = pacing::Vrr::Unknown;
    std::string name;  // for the log and the menu: the app's name for it, else Windows'; "" unknown
    // Where the VRR state is from:
    //   Told      the exe's list has this screen.
    //   NotTold   it has other screens only: this one was not there at start,
    //             or its output's name now belongs to another monitor.
    //   OneOnly   there is no list, only the older env DISPLAY_VRR and
    //             env DISPLAY_HZ about one screen that is not named: taken
    //             as this one's, as before (mpv started without the exe).
    //   Nothing   nothing was told at all.
    enum class Source { Nothing, Told, NotTold, OneOnly };
    Source source = Source::Nothing;
};

// told: env SCREENS. one_vrr, one_hz: env DISPLAY_VRR and
// env DISPLAY_HZ, used only without a list; one_told: either was set.
//
// The rate is always Windows' own for the screen the window is on now: with
// its fraction when the display configuration gave it, else the whole
// number - but a rate told at start that agrees with that whole number has
// the fraction and is taken. The VRR state is the list's for this very
// screen, or unknown.
inline ScreenBelief believe(const ScreenNow &now, const std::vector<ScreenFacts> &told, bool one_told,
                            pacing::Vrr one_vrr, double one_hz) {
    ScreenBelief b;
    if (!now.found) return b;
    b.name = now.device;
    b.hz = now.hz_exact > 0 ? now.hz_exact : now.hz_whole;
    const auto agrees = [&](double hz) { return hz > 0 && now.hz_whole > 0 && hz > now.hz_whole - 1.0 && hz < now.hz_whole + 1.0; };
    if (!told.empty()) {
        const ScreenFacts *facts = find_screen(told, now.device);
        // The same output with another monitor on it is another screen.
        if (facts && !facts->monitor.empty() && !now.monitor.empty() && !same_device(facts->monitor, now.monitor))
            facts = nullptr;
        b.source = facts ? ScreenBelief::Source::Told : ScreenBelief::Source::NotTold;
        if (!facts) return b;
        b.vrr = facts->vrr;
        if (!facts->name.empty()) b.name = facts->name;
        if (now.hz_exact <= 0 && agrees(facts->hz)) b.hz = facts->hz;
        return b;
    }
    if (one_told) {
        b.source = ScreenBelief::Source::OneOnly;
        b.vrr = one_vrr;
        if (now.hz_exact <= 0 && agrees(one_hz)) b.hz = one_hz;
    }
    return b;
}

}  // namespace fg
