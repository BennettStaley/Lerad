// SPDX-License-Identifier: MIT
#include "carry_over.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <string_view>

#include "app_version.h"
#include "files.h"
#include "ini_edit.h"
#include "log.h"

namespace app {

namespace {

// Everything the app was called before, and the one place it is written.
struct OldNames {
    const wchar_t *settings;           // the settings file, beside the exe
    const char *own_backend;           // [framegen] backend: the app's own generator, now "own"
    const char *addon_section;         // the add-on's section of ReShade.ini, now APP_ENV_PREFIX "FRAMEGEN"
    const wchar_t *installed[6];       // in the app folder: loaded by mpv or ReShade just for being there
};
constexpr OldNames kOld = {
    L"glasir.ini",
    "glasir",
    "GLASIR_FRAMEGEN",
    {
        L"engine\\zz-glasir-framegen.addon64",                       // ReShade: the frame generation add-on
        L"engine\\portable_config\\scripts\\glasir_audio.dll",       // mpv: the sound, which would play twice
        L"engine\\portable_config\\scripts\\glasir_scaling.lua",
        L"engine\\portable_config\\scripts\\glasir_screenshot.lua",
        L"engine\\portable_config\\scripts\\glasir_window.lua",
        L"engine\\portable_config\\shaders\\glasir_fsr1.glsl",       // only the old scaling script asked for it
    },
};
constexpr char kAddonSection[] = APP_ENV_PREFIX "FRAMEGEN";

std::string_view trimmed(std::string_view s) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}

bool same(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return tolower(static_cast<unsigned char>(x)) == tolower(static_cast<unsigned char>(y));
           });
}

// Calls line(text, begin, end) for every line of text, without its line end.
template <class F>
void each_line(const std::string &text, F line) {
    for (size_t begin = 0; begin < text.size();) {
        size_t end = text.find('\n', begin);
        if (end == std::string::npos) end = text.size();
        line(std::string_view(text).substr(begin, end - begin), begin);
        begin = end + 1;
    }
}

// "[name]" on a line of its own: the name, else empty.
std::string_view section_name(std::string_view line) {
    line = trimmed(line);
    if (line.size() < 2 || line.front() != '[' || line.back() != ']') return {};
    return trimmed(line.substr(1, line.size() - 2));
}

bool write_new_file(const std::wstring &path, const std::string &data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(f, data.data(), DWORD(data.size()), &written, nullptr) && written == data.size();
    CloseHandle(f);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

bool replace_file(const std::wstring &path, const std::string &data) {
    const std::wstring part = path + L".new";
    DeleteFileW(part.c_str());
    if (!write_new_file(part, data)) return false;
    if (MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileW(part.c_str());
    return false;
}

}  // namespace

CarryOverPlan carry_over_plan(const std::wstring &dir, const std::function<bool(const std::wstring &)> &exists) {
    CarryOverPlan plan;
    const std::wstring settings = dir + L"\\" APP_INI_W, old_settings = dir + L"\\" + kOld.settings;
    if (!exists(settings) && exists(old_settings)) {
        plan.settings_from = old_settings;
        plan.settings_to = settings;
    }
    for (const wchar_t *file : kOld.installed)
        if (const std::wstring path = dir + L"\\" + file; exists(path)) plan.remove.push_back(path);
    if (const std::wstring ini = dir + L"\\engine\\ReShade.ini"; exists(ini)) plan.reshade_ini = ini;
    return plan;
}

bool was_own_backend(std::string_view value) { return same(value, APP_ID) || same(value, kOld.own_backend); }

std::string carry_over_settings(const std::string &text) {
    bool old_backend = false, in_framegen = false;
    each_line(text, [&](std::string_view line, size_t) {
        if (const std::string_view name = section_name(line); !name.empty()) {
            in_framegen = same(name, "framegen");
            return;
        }
        const size_t eq = line.find('=');
        if (!in_framegen || eq == std::string_view::npos || trimmed(line).front() == ';') return;
        if (same(trimmed(line.substr(0, eq)), "backend")) old_backend = same(trimmed(line.substr(eq + 1)), kOld.own_backend);
    });
    return old_backend ? ini_set_values(text, {{"framegen", "backend", "own"}}) : text;
}

std::string carry_over_reshade_ini(const std::string &text) {
    size_t old_at = std::string::npos, old_size = 0;
    bool has_new = false;
    each_line(text, [&](std::string_view line, size_t begin) {
        const std::string_view name = section_name(line);
        if (name == kAddonSection) {
            has_new = true;
        } else if (name == kOld.addon_section && old_at == std::string::npos) {
            old_at = begin + (name.data() - line.data());
            old_size = name.size();
        }
    });
    if (old_at == std::string::npos || has_new) return text;
    std::string out = text;
    out.replace(old_at, old_size, kAddonSection);
    return out;
}

void carry_over(const std::wstring &dir) {
    const CarryOverPlan plan = carry_over_plan(dir, file_exists);
    if (!plan.settings_from.empty()) {
        std::string text;
        if (read_shared(plan.settings_from, text) && write_new_file(plan.settings_to, carry_over_settings(text)))
            log(L"carried over: {} is a copy of {}, which is left as it is", plan.settings_to, plan.settings_from);
        else
            log(L"carry over: could not copy {} to {} (error {})", plan.settings_from, plan.settings_to, GetLastError());
    }
    for (const std::wstring &file : plan.remove) {
        SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);  // not held up by a read-only flag
        if (DeleteFileW(file.c_str()))
            log(L"carried over: removed {} (the older version's; it would load beside its successor)", file);
        else
            log(L"carry over: could not remove {} (error {}); is an older version still running?", file,
                GetLastError());
    }
    if (!plan.reshade_ini.empty()) {
        std::string text;
        if (!read_shared(plan.reshade_ini, text)) return;
        const std::string renamed = carry_over_reshade_ini(text);
        if (renamed == text) return;
        if (replace_file(plan.reshade_ini, renamed))
            log(L"carried over: frame generation's section of {} is now [{}]", plan.reshade_ini, widen(kAddonSection));
        else
            log(L"carry over: could not write {} (error {})", plan.reshade_ini, GetLastError());
    }
}

}  // namespace app
