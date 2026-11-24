// SPDX-License-Identifier: MIT
// The app's window before playing. A wide window: six tabs of choices on the
// left (Play, Picture, Frame generation, Screen, Sound, About), what they
// come to on this PC on the right, and Play in a bar along the bottom. An
// ordinary window (mpv and ReShade are not running yet), drawn with Dear
// ImGui in the app's look (ui_style.h, ui_widgets.h). What it changes goes
// into the settings file when Play is pressed.
//
// Two switches for tests, in the environment: UI_SCALE forces the window's
// scale (1 is 920 x 640), UI_TAB says which tab is open at the start (0 to 5).
#pragma once
#include <functional>
#include <string>

namespace app {

// Setup: download the components again.
enum class LauncherAction { Play, Setup, SetupDlssFg, SetupVideo, SetupAnr, Quit };

struct LauncherExtras {
    // Saves a bug report (run on a thread of its own); the zip's path.
    std::function<bool(std::wstring &zip, std::wstring &error)> bug_report;
};

LauncherAction run_launcher(const std::wstring &settings_path, const std::wstring &engine_dir,
                            const LauncherExtras &extras);

}  // namespace app
