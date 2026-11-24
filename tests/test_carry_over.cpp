// SPDX-License-Identifier: MIT
// Checks what is done with an app folder from before the app's present name
// (src/app/carry_over.h): which files are found, the settings and ReShade.ini
// texts, and all of it on a folder made for the test. The old names are
// written out here on purpose: they are what such a folder really holds.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <set>
#include <string>

#include "app_version.h"
#include "carry_over.h"
#include "files.h"
#include "settings.h"

using namespace app;

namespace {

int g_failures = 0;

void expect(bool ok, const char *what) {
    std::printf("%-76s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_failures;
}

const wchar_t *const kOldFiles[] = {
    L"engine\\zz-glasir-framegen.addon64",
    L"engine\\portable_config\\scripts\\glasir_audio.dll",
    L"engine\\portable_config\\scripts\\glasir_scaling.lua",
    L"engine\\portable_config\\scripts\\glasir_screenshot.lua",
    L"engine\\portable_config\\scripts\\glasir_window.lua",
    L"engine\\portable_config\\shaders\\glasir_fsr1.glsl",
};
// What stays: other people's files, the new version's, the user's old exe.
const wchar_t *const kKeptFiles[] = {
    L"Glasir.exe",
    L"engine\\mpv.exe",
    L"engine\\dlss5-feed.addon64",
    L"engine\\zz-framegen.addon64",
    L"engine\\portable_config\\scripts\\scaling.lua",
    L"engine\\portable_config\\scripts\\capture_audio.dll",
    L"engine\\portable_config\\shaders\\fsr1.glsl",
    L"logs\\glasir.log",
};

void write(const std::wstring &path, const std::string &text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD written = 0;
    if (f != INVALID_HANDLE_VALUE) WriteFile(f, text.data(), DWORD(text.size()), &written, nullptr);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
}

std::string read(const std::wstring &path) {
    std::string text;
    read_shared(path, text);
    return text;
}

void remove_tree(const std::wstring &dir) {
    WIN32_FIND_DATAW found;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &found);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = found.cFileName, path = dir + L"\\" + name;
            if (name == L"." || name == L"..") continue;
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(path);
            else DeleteFileW(path.c_str());
        } while (FindNextFileW(h, &found));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

}  // namespace

int main() {
    const std::wstring dir = L"C:\\Games\\App";
    const std::wstring new_settings = dir + L"\\" APP_INI_W, old_settings = dir + L"\\glasir.ini";

    // Which files: decided from what is there, nothing touched.
    {
        const CarryOverPlan fresh = carry_over_plan(dir, [](const std::wstring &) { return false; });
        expect(fresh.settings_from.empty() && fresh.settings_to.empty() && fresh.remove.empty() &&
                   fresh.reshade_ini.empty(),
               "a new folder: nothing to do");

        std::set<std::wstring> there = {old_settings, dir + L"\\engine\\ReShade.ini", dir + L"\\Glasir.exe",
                                        dir + L"\\engine\\mpv.exe", dir + L"\\engine\\zz-framegen.addon64"};
        for (const wchar_t *f : kOldFiles) there.insert(dir + L"\\" + f);
        const auto exists = [&](const std::wstring &path) { return there.count(path) != 0; };
        CarryOverPlan old = carry_over_plan(dir, exists);
        expect(old.settings_from == old_settings && old.settings_to == new_settings,
               "an old folder: its settings file is copied to the new name");
        bool all = old.remove.size() == std::size(kOldFiles);
        for (const wchar_t *f : kOldFiles)
            all = all && std::find(old.remove.begin(), old.remove.end(), dir + L"\\" + f) != old.remove.end();
        expect(all, "every file the old version installed for mpv and ReShade is removed, and no other");
        expect(old.reshade_ini == dir + L"\\engine\\ReShade.ini", "ReShade.ini is looked at");

        there.insert(new_settings);
        old = carry_over_plan(dir, exists);
        expect(old.settings_from.empty() && old.settings_to.empty(), "the new settings file is there: never overwritten");
        expect(old.remove.size() == std::size(kOldFiles), "old files are removed whichever settings file there is");

        there = {old_settings, dir + L"\\engine\\portable_config\\scripts\\glasir_window.lua"};
        old = carry_over_plan(dir, exists);
        expect(old.remove.size() == 1 && old.reshade_ini.empty(), "only what is there is listed");
    }

    // The settings text: the generator's old name, and nothing else.
    {
        const std::string old_text = "; Glasir settings.\r\n[capture]\r\nbackend = glasir\r\n[framegen]\r\n"
                                     "; glasir: Glasir's own\r\nbackend = glasir\r\nmultiplier = 3 ; mine\r\n";
        const std::string text = carry_over_settings(old_text);
        expect(text.find("[framegen]\r\n; glasir: Glasir's own\r\nbackend = own\r\nmultiplier = 3 ; mine") !=
                   std::string::npos,
               "[framegen] backend: the app's own generator under its present name, \"own\"");
        expect(text.find("; Glasir settings.\r\n[capture]\r\nbackend = glasir\r\n") == 0,
               "comments and other sections stay as they were written");
        const std::string dlss = "[framegen]\nbackend = dlss\nquality = fast\n";
        expect(carry_over_settings(dlss) == dlss, "backend = dlss: the text is not touched");
        expect(carry_over_settings("") == "" && carry_over_settings("[framegen]\n") == "[framegen]\n",
               "no backend key: not touched");
        expect(carry_over_settings("[FrameGen]\nBackend=GLASIR\n") == "[FrameGen]\nBackend=own\n",
               "section, key and value match without case");
        Settings s;
        std::wstring error;
        expect(apply_override(s, L"framegen.backend=own", error) && !s.framegen_dlss, "\"own\" is what the settings take");
        // Files written while the generator went by the app's name still load: the name it has now, and the one before.
        s.framegen_dlss = true;
        expect(apply_override(s, L"framegen.backend=" APP_ID_W, error) && !s.framegen_dlss,
               "the app's present name is read as the app's own generator");
        s.framegen_dlss = true;
        expect(apply_override(s, L"framegen.backend=Glasir", error) && !s.framegen_dlss,
               "and so is the name it had before, in any case");
        expect(was_own_backend(APP_ID) && was_own_backend("GLASIR") && !was_own_backend("dlss") && !was_own_backend("own") &&
                   !was_own_backend(""),
               "the names the generator went by, and no others");
        expect(apply_override(s, L"framegen.backend=DLSS", error) && s.framegen_dlss &&
                   !apply_override(s, L"framegen.backend=other", error),
               "dlss is dlss, anything else is refused");
        expect(std::string(default_settings_text()).find("\nbackend = own\n") != std::string::npos,
               "a new settings file says own");
        Settings from, to;
        to.framegen_dlss = true;
        const std::vector<SettingValue> there = changed_settings(from, to), back = changed_settings(to, from);
        expect(there.size() == 1 && there[0].key == L"backend" && there[0].value == L"dlss" && back.size() == 1 &&
                   back[0].value == L"own",
               "what a window that edits the settings saves: dlss, and own");
    }

    // ReShade.ini: the add-on's section under its present name.
    {
        const std::string now = "[" APP_ENV_PREFIX "FRAMEGEN]";
        const std::string old_text = "[GENERAL]\r\nEffectSearchPaths=.\\\r\n\r\n[GLASIR_FRAMEGEN]\r\nHistorySearch=1\r\n"
                                     "MpvPipe=glasir\r\n\r\n[OVERLAY]\r\nx=1\r\n";
        const std::string text = carry_over_reshade_ini(old_text);
        expect(text == "[GENERAL]\r\nEffectSearchPaths=.\\\r\n\r\n" + now + "\r\nHistorySearch=1\r\nMpvPipe=glasir\r\n\r\n"
                       "[OVERLAY]\r\nx=1\r\n",
               "the old section is renamed, its keys kept");
        expect(carry_over_reshade_ini(text) == text, "done once: nothing more to do");
        const std::string both = now + "\nEnabled=1\n[GLASIR_FRAMEGEN]\nEnabled=0\n";
        expect(now == "[GLASIR_FRAMEGEN]" || carry_over_reshade_ini(both) == both,
               "the new section is there already: the old one is left alone");
        expect(carry_over_reshade_ini("[GENERAL]\nx=[GLASIR_FRAMEGEN]\n") == "[GENERAL]\nx=[GLASIR_FRAMEGEN]\n",
               "only a section line counts");
    }

    // All of it, on a folder as an old version leaves it with the new one unpacked over it.
    {
        wchar_t temp[MAX_PATH + 1] = {};
        GetTempPathW(MAX_PATH + 1, temp);
        const std::wstring folder = std::wstring(temp) + L"app_test_carry_over";
        remove_tree(folder);
        for (const wchar_t *d : {L"", L"\\logs", L"\\engine", L"\\engine\\portable_config",
                                 L"\\engine\\portable_config\\scripts", L"\\engine\\portable_config\\shaders"})
            CreateDirectoryW((folder + d).c_str(), nullptr);
        for (const wchar_t *f : kOldFiles) write(folder + L"\\" + f, "old");
        for (const wchar_t *f : kKeptFiles) write(folder + L"\\" + f, "kept");
        SetFileAttributesW((folder + L"\\" + kOldFiles[0]).c_str(), FILE_ATTRIBUTE_READONLY);
        const std::string old_ini = "[framegen]\r\nbackend = glasir\r\nmultiplier = 4\r\n";
        write(folder + L"\\glasir.ini", old_ini);
        write(folder + L"\\engine\\ReShade.ini", "[GLASIR_FRAMEGEN]\r\nHistorySearch=1\r\n");

        carry_over(folder);
        const std::wstring settings = folder + L"\\" APP_INI_W;
        expect(read(settings) == "[framegen]\r\nbackend = own\r\nmultiplier = 4\r\n",
               "the settings file is made from the old one");
        expect(read(folder + L"\\glasir.ini") == old_ini, "the old settings file is left as it was");
        Settings s;
        std::wstring error;
        expect(load_settings(settings, s, error) && !s.framegen_dlss && s.framegen_multiplier == 4,
               "and it loads, with what the user had set");
        bool gone = true, kept = true;
        for (const wchar_t *f : kOldFiles) gone = gone && !file_exists(folder + L"\\" + f);
        for (const wchar_t *f : kKeptFiles) kept = kept && read(folder + L"\\" + f) == "kept";
        expect(gone, "the old version's scripts, plugin, shader and add-on are gone (a read-only one too)");
        expect(kept, "the old exe, its log, the new files and other people's add-ons stay");
        expect(read(folder + L"\\engine\\ReShade.ini") == "[" APP_ENV_PREFIX "FRAMEGEN]\r\nHistorySearch=1\r\n",
               "ReShade.ini keeps the add-on's keys under the new section");

        // The next start: the user's changes to the new file are not overwritten.
        write(settings, "[framegen]\r\nbackend = dlss\r\n");
        carry_over(folder);
        expect(read(settings) == "[framegen]\r\nbackend = dlss\r\n", "a second start changes nothing");
        remove_tree(folder);
    }

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
