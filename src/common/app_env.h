// SPDX-License-Identifier: MIT
// The app's environment variables: what the exe tells the add-ons inside mpv
// (the settings file's path, the screens, what is done with HDR), and the
// switches tests set. Every name starts with APP_ENV_PREFIX (the product's
// name in capitals and an underscore), which is added here and written
// nowhere else: env("SCREENS") reads the variable APP_ENV("SCREENS").
// Comments call that variable "env SCREENS".
#pragma once
#include <windows.h>

#include <string>

#include "app_version.h"

#define APP_WIDEN_(text) L##text
#define APP_WIDEN(text) APP_WIDEN_(text)
// The variable's whole name as a string literal, for a log line.
#define APP_ENV(name) APP_ENV_PREFIX name
#define APP_ENV_W(name) APP_ENV_PREFIX_W APP_WIDEN(name)

namespace app {

// The variable's value; empty when it is not set.
inline std::wstring env(const wchar_t *name) {
    const std::wstring full = std::wstring(APP_ENV_PREFIX_W) + name;
    std::wstring value(260, L'\0');
    for (;;) {
        const DWORD n = GetEnvironmentVariableW(full.c_str(), value.data(), DWORD(value.size()));
        if (n < value.size()) {
            value.resize(n);
            return value;
        }
        value.resize(n);  // too small: n is the size it needs
    }
}

// The same in the process's code page, for values that are plain text (a
// number, a word) or a path given to a narrow file function.
inline std::string env(const char *name) {
    const std::string full = std::string(APP_ENV_PREFIX) + name;
    std::string value(260, '\0');
    for (;;) {
        const DWORD n = GetEnvironmentVariableA(full.c_str(), value.data(), DWORD(value.size()));
        if (n < value.size()) {
            value.resize(n);
            return value;
        }
        value.resize(n);
    }
}

// Sets the variable for this process and the ones it starts.
inline void set_env(const wchar_t *name, const std::wstring &value) {
    SetEnvironmentVariableW((std::wstring(APP_ENV_PREFIX_W) + name).c_str(), value.c_str());
}

}  // namespace app
