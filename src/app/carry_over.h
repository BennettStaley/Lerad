// SPDX-License-Identifier: MIT
// An app folder from before the app had its present name, with the new
// version unpacked over it: its settings are taken along, and what the old
// version installed under names of its own is removed. mpv loads every script
// in its scripts folder and ReShade every add-on beside mpv.exe, so a file
// left there would run a second time, beside its successor.
//
// The old names are written in carry_over.cpp's table and nowhere else.
#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace app {

// What there is to do in the app folder dir. Nothing is read or changed
// here: exists says which files are there.
struct CarryOverPlan {
    // The old settings file, to be copied to settings_to (the new one is
    // missing); both empty when there is nothing to copy.
    std::wstring settings_from, settings_to;
    // Files of the old version that would still load.
    std::vector<std::wstring> remove;
    // ReShade.ini, if there is one: the add-on's section may have its old name.
    std::wstring reshade_ini;
};
CarryOverPlan carry_over_plan(const std::wstring &dir, const std::function<bool(const std::wstring &)> &exists);

// [framegen] backend names the app's own generator "own". It used to go by
// the app's name: whether value (any case) is such a name, the present one or
// an older one, so a file written then still loads (settings.cpp).
bool was_own_backend(std::string_view value);

// The old settings file's text as the new file gets it: a value that was the
// old name is the new one, everything else as it was written.
std::string carry_over_settings(const std::string &text);
// ReShade.ini's text with the frame generation add-on's section under its
// present name, so the keys only ReShade's own page changes are kept. The
// text as it is when there is no old section, or the new one is there too.
std::string carry_over_reshade_ini(const std::string &text);

// Does all of it and logs each step. Once at start, before the launcher reads
// the settings and before mpv starts. A step that fails is logged and tried
// again at the next start.
void carry_over(const std::wstring &dir);

}  // namespace app
