// SPDX-License-Identifier: MIT
#include "input_program.h"

#include <format>
#include <vector>

#include "log.h"
#include "mpv_launch.h"

namespace app {

std::wstring input_program_file(const Settings &s, const std::wstring &app_dir) {
    const std::wstring &p = s.input_program;
    if (p.empty()) return {};
    const bool full = p.size() > 2 && (p[1] == L':' || (p[0] == L'\\' && p[1] == L'\\'));
    return full ? p : app_dir + L"\\" + p;
}

bool input_program_found(const Settings &s, const std::wstring &app_dir) {
    const std::wstring file = input_program_file(s, app_dir);
    if (file.empty()) return false;
    const DWORD attributes = GetFileAttributesW(file.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

InputProgram::~InputProgram() {
    if (pi_.hProcess) {
        TerminateProcess(pi_.hProcess, 0);
        CloseHandle(pi_.hProcess);
        CloseHandle(pi_.hThread);
    }
    if (job_) CloseHandle(job_);
}

bool InputProgram::start(const std::wstring &file, const std::wstring &settings_file, const std::wstring &mpv_pipe,
                         const std::wstring &view_shift, const std::wstring &log_file, std::wstring &error) {
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        error = std::format(L"Could not prepare the input program's job object (error {}).", GetLastError());
        return false;
    }

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE out = CreateFileW(log_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                             OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = out != INVALID_HANDLE_VALUE ? out : nul;
    si.hStdError = si.hStdOutput;

    const std::wstring command_line =
        join_command_line(file, {L"--exit-with", std::to_wstring(GetCurrentProcessId()), L"--tuning", settings_file,
                                 L"--toast-pipe", mpv_pipe, L"--shift-map", view_shift});
    std::vector<wchar_t> cmd(command_line.begin(), command_line.end());
    cmd.push_back(0);
    // Its own folder as the current one: its settings and libraries are there.
    const size_t slash = file.find_last_of(L"\\/");
    const std::wstring folder = slash == std::wstring::npos ? std::wstring() : file.substr(0, slash);
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                   nullptr, folder.empty() ? nullptr : folder.c_str(), &si, &pi_);
    const DWORD err = GetLastError();
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        pi_ = {};
        error = std::format(L"Could not start the input program (error {}).", err);
        log(L"could not start the input program (error {}): {}", err, command_line);
        return false;
    }
    if (!AssignProcessToJobObject(job_, pi_.hProcess))
        log(L"the input program is not in a job object (error {}): it ends itself when the app does", GetLastError());
    ResumeThread(pi_.hThread);
    log(L"the input program runs (process {}): {}", pi_.dwProcessId, command_line);
    return true;
}

}  // namespace app
