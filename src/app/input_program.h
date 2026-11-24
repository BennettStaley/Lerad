// SPDX-License-Identifier: MIT
// The input program: a separate program, not part of the app, that plays the
// console with this PC's mouse and keyboard while the picture is shown. The
// app only starts it with the picture and ends it with the picture.
#pragma once
#include <windows.h>

#include <string>

#include "settings.h"

namespace app {

// The program's file: [input] program, from the app's folder when it is not
// a full path. Empty when none is set.
std::wstring input_program_file(const Settings &s, const std::wstring &app_dir);
bool input_program_found(const Settings &s, const std::wstring &app_dir);

// The running program, in a job object so it ends with the app.
class InputProgram {
public:
    InputProgram() = default;
    InputProgram(const InputProgram &) = delete;
    InputProgram &operator=(const InputProgram &) = delete;
    ~InputProgram();

    // Starts file with "--exit-with <this process>" and "--tuning
    // <settings_file>": the program follows the [input] sensitivity values
    // there while it runs, as the menu changes them. What it prints goes to
    // log_file. False, with why, if it could not be started.
    // With "--toast-pipe <mpv_pipe>" the program says on the picture who has
    // the mouse and keyboard (script-message app-toast over mpv's IPC).
    // With "--shift-map <view_shift>" it writes how far the picture is to be
    // moved ahead of the console into that file mapping (view_shift.h).
    bool start(const std::wstring &file, const std::wstring &settings_file, const std::wstring &mpv_pipe,
               const std::wstring &view_shift, const std::wstring &log_file, std::wstring &error);

private:
    PROCESS_INFORMATION pi_{};
    HANDLE job_ = nullptr;
};

}  // namespace app
