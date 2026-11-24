// SPDX-License-Identifier: MIT
#include "launcher.h"

#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "advice.h"
#include "app_env.h"
#include "audio_endpoint.h"
#include "components.h"
#include "devices.h"
#include "magewell.h"
#include "display.h"
#include "app_version.h"
#include "gpu.h"
#include "hdr.h"
#include "input_program.h"
#include "keep_regions.h"
#include "launcher_text.h"
#include "log.h"
#include "settings.h"
#include "system_report.h"
#include "ui_style.h"
#include "ui_widgets.h"
#include "vrr.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using Microsoft::WRL::ComPtr;

namespace app {
namespace {

// ---------------------------------------------------------------- what the PC has

struct Found {
    std::vector<CaptureDevice> devices;
    std::vector<std::wstring> directshow_only;
    std::vector<AudioInput> audio;
};

// Looked up on a thread of its own, so the window is there at once: the
// screens and GPUs first (some tens of milliseconds: the GPU driver is asked
// about VRR), then the cards (opening each to read its modes takes a
// moment). The screens and GPUs can be read again later: HDR and VRR are
// switched in Windows and the driver while this window is open.
class Survey {
public:
    void start() {
        thread_ = std::thread([this] {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            read_system();
            Found f;
            f.devices = list_devices();
            for (const auto &name : list_directshow_devices()) {
                bool known = false;
                for (const auto &d : f.devices) known = known || names_match(d.name, name);
                if (!known) f.directshow_only.push_back(name);
            }
            f.audio = list_audio_inputs();
            {
                std::lock_guard lock(m_);
                found_ = std::move(f);
                cards_done_ = true;
            }
            std::unique_lock lock(m_);
            for (;;) {
                wake_.wait(lock, [this] { return quit_ || again_; });
                if (quit_) break;
                again_ = false;
                lock.unlock();
                read_system();
                lock.lock();
            }
            lock.unlock();
            if (SUCCEEDED(com)) CoUninitialize();
        });
    }
    ~Survey() {
        {
            std::lock_guard lock(m_);
            quit_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    bool cards_done() const { return cards_done_; }
    const Found &found() const { return found_; }  // once cards_done
    // Reads the screens and GPUs again.
    void look_again() {
        {
            std::lock_guard lock(m_);
            again_ = true;
        }
        wake_.notify_all();
    }
    // The screens and GPUs, when they were read since the last call.
    bool take_system(std::vector<DisplayInfo> &displays, std::vector<GpuInfo> &gpus) {
        std::lock_guard lock(m_);
        if (!system_new_) return false;
        system_new_ = false;
        displays = displays_;
        gpus = gpus_;
        return true;
    }

private:
    void read_system() {
        std::vector<DisplayInfo> displays = list_displays();
        std::vector<GpuInfo> gpus = list_gpus();
        std::lock_guard lock(m_);
        displays_ = std::move(displays);
        gpus_ = std::move(gpus);
        system_new_ = true;
    }

    std::thread thread_;
    std::mutex m_;
    std::condition_variable wake_;
    std::atomic<bool> cards_done_{false};
    bool quit_ = false, again_ = false, system_new_ = false;
    Found found_;
    std::vector<DisplayInfo> displays_;
    std::vector<GpuInfo> gpus_;
};

// ---------------------------------------------------------------- the choices

struct ModeItem {
    UINT32 width, height;
    double fps;
};

std::string utf8(const std::wstring &s) { return narrow(s); }

// 60 and 59.94 are different modes on most cards, so only a rate within a
// hundredth of a whole number (144.001) is shown as that number.
std::string fps_text(double fps) {
    const double r = std::round(fps);
    return std::abs(fps - r) < 0.01 ? std::format("{:.0f}", r) : std::format("{:.2f}", fps);
}

std::string mode_text(UINT32 w, UINT32 h, double fps) { return std::format("{} x {} \xC2\xB7 {} fps", w, h, fps_text(fps)); }

// The device's modes the app can read in this picture mode, largest first.
std::vector<ModeItem> modes_for(const CaptureDevice *d, bool hdr) {
    std::vector<ModeItem> out;
    if (!d) return out;
    for (const auto &m : d->modes) {
        if (!m.supported || (m.format == PixelFormat::P010) != hdr) continue;
        bool dup = false;
        for (const auto &o : out) dup = dup || (o.width == m.width && o.height == m.height && std::abs(o.fps - m.fps) < 0.01);
        if (!dup) out.push_back({m.width, m.height, m.fps});
    }
    std::sort(out.begin(), out.end(), [](const ModeItem &a, const ModeItem &b) {
        if (a.width * a.height != b.width * b.height) return a.width * a.height > b.width * b.height;
        return a.fps > b.fps;
    });
    return out;
}

// Whether the card also offers this mode in HDR (10-bit, P010).
bool has_hdr(const CaptureDevice *d, UINT32 w, UINT32 h, double fps) {
    if (!d) return false;
    for (const auto &m : d->modes)
        if (m.supported && m.format == PixelFormat::P010 && m.width == w && m.height == h && std::abs(m.fps - fps) < 0.01)
            return true;
    return false;
}

// ---------------------------------------------------------------- the window

// The window's inside at 96 DPI: a wide window, the choices on the left,
// what they come to on the right, Play in a bar along the bottom. Everything
// in it is laid out in these pixels times the scale.
constexpr float kWidth = 920, kHeight = 640;
constexpr float kTopBar = 54, kPlayBar = 68, kResult = 330;
constexpr float kGutter = 22, kLabel = 150;

struct Window {
    HWND hwnd = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapchain;
    ComPtr<ID3D11RenderTargetView> rtv;
    UINT resize_w = 0, resize_h = 0;
    // The UI's scale: the monitor's DPI over 96, less on a screen too small
    // for the window at that size (so that nothing has to scroll).
    float scale = 1;
    float forced_scale = 0;  // env UI_SCALE, a test switch: that scale whatever the monitor; 0: not set
    bool closed = false;
    // The screens may have changed (a display mode, HDR switched in Windows)
    // or the user comes back from Windows' or the GPU's settings.
    bool look_again = false;
};
Window *g_window = nullptr;

void make_target(Window &w) {
    ComPtr<ID3D11Texture2D> back;
    w.swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
    w.device->CreateRenderTargetView(back.Get(), nullptr, &w.rtv);
}

float scale_for(const Window &w, UINT dpi, const RECT &work, DWORD style) {
    if (w.forced_scale > 0) return w.forced_scale;
    RECT frame{0, 0, 0, 0};
    AdjustWindowRectExForDpi(&frame, style, FALSE, 0, dpi);
    const float room_w = float(work.right - work.left - (frame.right - frame.left));
    const float room_h = float(work.bottom - work.top - (frame.bottom - frame.top));
    return std::clamp(std::min({dpi / 96.0f, room_w / kWidth, room_h / kHeight}), 0.5f, 4.0f);
}

// The window's outer size for the inside at this scale.
SIZE window_size(float scale, UINT dpi, DWORD style) {
    RECT r{0, 0, LONG(std::lround(kWidth * scale)), LONG(std::lround(kHeight * scale))};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    return {r.right - r.left, r.bottom - r.top};
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return 1;
    Window *w = g_window;
    switch (msg) {
    case WM_SIZE:
        if (w && wparam != SIZE_MINIMIZED) {
            w->resize_w = LOWORD(lparam);
            w->resize_h = HIWORD(lparam);
        }
        return 0;
    case WM_DPICHANGED:
        if (w) {
            // Moved to a monitor with another DPI: the same layout at its scale.
            const UINT dpi = HIWORD(wparam);
            const RECT *r = reinterpret_cast<RECT *>(lparam);
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromRect(r, MONITOR_DEFAULTTONEAREST), &mi);
            const DWORD style = DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE));
            w->scale = scale_for(*w, dpi, mi.rcWork, style);
            const SIZE size = window_size(w->scale, dpi, style);
            SetWindowPos(hwnd, nullptr, r->left, r->top, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_DISPLAYCHANGE:
        if (w) w->look_again = true;
        break;
    case WM_ACTIVATEAPP:
        if (w && wparam) w->look_again = true;
        break;
    case WM_SYSCOMMAND:
        if ((wparam & 0xFFF0) == SC_KEYMENU) return 0;  // no Alt menu
        break;
    case WM_CLOSE:
        if (w) w->closed = true;
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool open_window(Window &w) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = APP_NAME_W L"Launcher";
    RegisterClassExW(&wc);

    // Sized for the monitor the mouse is on, in its DPI. (Where the launcher
    // opens says nothing about where the app plays: that is the screen chosen
    // in it.)
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    UINT dpi = 96;
    if (HWND desktop = GetDesktopWindow()) dpi = GetDpiForWindow(desktop);
    using GetDpiForMonitorFn = HRESULT(WINAPI *)(HMONITOR, int, UINT *, UINT *);
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
        if (auto fn = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"))) {
            UINT x = 96, y = 96;
            if (SUCCEEDED(fn(monitor, 0, &x, &y))) dpi = x;
        }
    }
    const double forced = _wtof(env(L"UI_SCALE").c_str());
    if (forced >= 0.5 && forced <= 4) w.forced_scale = float(forced);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const RECT &work = mi.rcWork;
    w.scale = scale_for(w, dpi, work, style);
    const SIZE size = window_size(w.scale, dpi, style);
    const int x = work.left + std::max(0L, (work.right - work.left - size.cx) / 2);
    const int y = work.top + std::max(0L, (work.bottom - work.top - size.cy) / 2);
    w.hwnd = CreateWindowExW(0, wc.lpszClassName, APP_NAME_W, style, x, y, size.cx, size.cy, nullptr, nullptr,
                             wc.hInstance, nullptr);
    if (!w.hwnd) return false;
    const BOOL dark = TRUE;  // a dark title bar to match
    DwmSetWindowAttribute(w.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = w.hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &w.swapchain, &w.device, nullptr, &w.context);
    if (hr == DXGI_ERROR_UNSUPPORTED)  // no GPU driver: the software one will do for a window
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                           &w.swapchain, &w.device, nullptr, &w.context);
    if (FAILED(hr)) {
        log(L"launcher: no Direct3D 11 ({})", hresult_text(hr));
        DestroyWindow(w.hwnd);
        return false;
    }
    make_target(w);
    ShowWindow(w.hwnd, SW_SHOWDEFAULT);
    UpdateWindow(w.hwnd);
    SetForegroundWindow(w.hwnd);
    return true;
}

// ---------------------------------------------------------------- what the window holds

const char *const kDot = " \xC2\xB7 ";

// "Save a bug report", on a thread of its own (it takes a second or two).
struct BugReport {
    enum State { Idle, Saving, Saved, Failed };
    std::atomic<int> state{Idle};
    std::wstring error;  // Failed (set before state)
    std::thread thread;
    ~BugReport() {
        if (thread.joinable()) thread.join();
    }
    void start(const std::function<bool(std::wstring &, std::wstring &)> &save) {
        if (state == Saving) return;
        if (thread.joinable()) thread.join();
        state = Saving;
        thread = std::thread([this, save] {
            std::wstring zip, e;
            if (save(zip, e)) {
                // Explorer shows it, chosen, ready to be dragged into a message.
                const std::wstring select = L"/select,\"" + zip + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
                state = Saved;
            } else {
                error = e;
                state = Failed;
            }
        });
    }
};

enum Tab { kTabPlay, kTabPicture, kTabFrames, kTabScreen, kTabSound, kTabAbout, kTabCount };

struct Launcher {
    // The settings file as the window shows it, and as it was read: what differs is
    // saved when Play is pressed (settings.h, changed_settings).
    Settings now;
    const Settings original;
    std::wstring settings_path;
    std::wstring engine_dir;
    const Found *found = nullptr;  // the cards, once they are read
    // The screens and GPUs, once they are read; display and gpu follow the
    // choices in `now`.
    bool system_known = false;
    SystemInfo system;
    // What `now` comes to on this PC (advice.h), worked out again when a
    // choice or the PC changes. Only with both the cards and the screens.
    bool advice_valid = false;
    std::string gsync_fix;  // why the driver's G-SYNC "Fix" failed, to show
    Settings advised;
    AdviceInput input;
    Advice advice;
    std::vector<ResultStep> steps;
    PlaySummary summary;
    std::vector<MultiplierChoice> multipliers;

    int tab = kTabPlay;
    float scale = 1;  // the window's (Window::scale)
    bool dlss5_installed = false;
    bool dlssfg_installed = false;
    bool video_installed = false;  // NVIDIA Video Frame Generation's files
    bool anr_installed = false;    // ANR Frame Generation's files
    bool input_found = false;      // the input program's file is there (input_program.h)
    std::string versions;  // "mpv 20261031 · ReShade 6.8.0 · DLSS 5 10.8.0", "" if none
    bool other_versions = false;  // not the ones this version of the app was tested with
    const LauncherExtras *extras = nullptr;
    BugReport *bug = nullptr;
    bool overflow_logged[kTabCount] = {};
    LauncherAction action = LauncherAction::Quit;
    bool finished = false;
};

// The capture card `now` names (or the automatic one), as the window lists
// them: one the app reads itself, or a name only DirectShow knows.
struct Card {
    const CaptureDevice *automatic = nullptr;
    const CaptureDevice *device = nullptr;
    bool directshow = false;
};

Card find_card(const Launcher &s) {
    Card c;
    if (!s.found) return c;
    ModeRequest request;
    request.hdr = s.now.hdr && !s.now.hdr_auto;
    request.max_auto_height = s.system.display.found ? UINT32(s.system.display.height) : 0;
    c.automatic = choose_device(s.found->devices, L"", request);
    if (!c.automatic && s.now.hdr_auto) {
        // A card whose only modes are 10-bit (main.cpp does the same).
        request.hdr = true;
        c.automatic = choose_device(s.found->devices, L"", request);
    }
    c.device = s.now.device.empty() ? c.automatic : nullptr;
    for (const auto &d : s.found->devices)
        if (!s.now.device.empty() && names_match(d.name, s.now.device)) c.device = &d;
    for (const auto &n : s.found->directshow_only)
        c.directshow = c.directshow || (!s.now.device.empty() && names_match(n, s.now.device));
    return c;
}

// Works out what the choices come to, when they or the PC changed.
void refresh(Launcher &s) {
    if (!s.found || !s.system_known) return;
    if (s.advice_valid && changed_settings(s.advised, s.now).empty()) return;
    s.system.display = {};
    if (const DisplayInfo *d = choose_display(s.system.displays, s.now.monitor, &s.system.monitor_matched))
        s.system.display = *d;
    s.system.gpu = choose_gpu(s.system.gpus, s.system.display.device, s.now.gpu);
    s.input = advice_input(s.now, s.system, s.found->devices, s.engine_dir);
    if (!s.input.card_found) {
        // A card only DirectShow sees: mpv opens it, as when playing.
        for (const auto &n : s.found->directshow_only)
            if (!s.input.card_found && (s.now.device.empty() || names_match(n, s.now.device))) {
                s.input.card_found = true;
                s.input.card = n;
            }
    }
    s.advice = advise(s.input);
    const Card card = find_card(s);
    const bool low_latency = s.now.low_latency && card.device && is_magewell(card.device->symbolic_link);
    s.steps = result_steps(s.input, s.advice, low_latency);
    s.summary = play_summary(s.input, s.advice);
    s.multipliers = multiplier_choices(s.input);
    s.advised = s.now;
    s.advice_valid = true;
}

void setup(Launcher &s, LauncherAction what) {
    s.action = what;
    s.finished = true;
}

void open_in_windows(const std::wstring &what) {
    ShellExecuteW(nullptr, L"open", what.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------- rows

// Where the values start: after the gutter and the labels' column.
float value_x(const Launcher &s) { return (kGutter + kLabel + 12) * s.scale; }

// A label in the left column. What comes next is on its line, where the
// values start.
void row(const Launcher &s, const char *label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(value_x(s));
}

// The next control takes what is left of the row.
void fill() { ImGui::SetNextItemWidth(-FLT_MIN); }

// One line of quiet text after a control, on its row.
void after(const std::string &text, const ImVec4 &colour = ui::kTextDim) {
    ImGui::SameLine();
    ui::row_hint(text.c_str(), colour);
}

// One line of quiet text under a row, where the values start.
void sub(const Launcher &s, const std::string &text, const ImVec4 &colour = ui::kTextDim) {
    ImGui::SetCursorPosX(value_x(s));
    ui::hint(text.c_str(), colour);
}

// A value at the right end of a slider's row ("100 %").
void value_text(const Launcher &s, const std::string &text) {
    ImGui::SameLine();
    const float right = ImGui::GetWindowWidth() - kGutter * s.scale;
    ImGui::SetCursorPosX(right - ImGui::CalcTextSize(text.c_str()).x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text.c_str());
}

// What a notice's button does about an item of advice.
void act_on(Launcher &s, const AdviceItem &item) {
    if (item.id == "vrr-program-profile") {
        // Not a setting of the app's: the driver's, for this mpv.exe (vrr.h).
        const std::wstring dir = s.engine_dir.substr(0, s.engine_dir.find_last_of(L'\\'));
        std::wstring error;
        s.gsync_fix = allow_nvidia_windowed_vrr(s.engine_dir + L"\\mpv.exe", dir + L"\\nvidia-gsync-backup.txt", error)
                          ? std::string()
                          : utf8(error);
        forget_mpv_profile();
        s.advice_valid = false;
    } else if (item.id == "framegen-dlss-not-downloaded") {
        setup(s, LauncherAction::SetupDlssFg);
    } else if (item.id == "framegen-anr-not-downloaded") {
        setup(s, LauncherAction::SetupAnr);
    } else if (item.id == "framegen-no-reshade") {
        setup(s, LauncherAction::Setup);
    } else {
        apply_suggestion(s.now, item);
    }
}

// One piece of advice as a notice: a problem and a warning with their word
// and sign, a suggestion quiet, and a button where something can be done.
void advice_notice(Launcher &s, const AdviceItem &item) {
    const ui::Status status = item.severity == AdviceSeverity::Problem   ? ui::Status::Bad
                              : item.severity == AdviceSeverity::Warning ? ui::Status::Warn
                                                                         : ui::Status::Info;
    const char *title = status == ui::Status::Bad ? "Problem" : status == ui::Status::Warn ? "Warning" : nullptr;
    const char *action = item.id == "vrr-program-profile"                                                 ? "Fix"
                         : item.id == "framegen-dlss-not-downloaded" || item.id == "framegen-no-reshade" ? "Download"
                         : !item.suggest.empty() && !item.applied                                        ? "Use"
                                                                                                         : nullptr;
    if (ui::notice(item.id.c_str(), status, title, utf8(item.text).c_str(), action)) act_on(s, item);
    if (item.id == "vrr-program-profile" && !s.gsync_fix.empty()) ui::hint(s.gsync_fix.c_str(), ui::kBad);
}

int notes_about(const Launcher &s, AdvicePlace place) {
    int n = 0;
    if (s.advice_valid)
        for (const AdviceItem &item : s.advice.items)
            n += advice_shown(item) == AdviceShown::Place && advice_place(item.id) == place;
    return n;
}

// A section's label. With a place: at the right of its line, the notes
// about that part of the PC ("2 notes about this card"), which open in a
// small window of their own: limits for information, and other ways to set
// things up, each with "Use" where it has something to set.
void section(Launcher &s, const char *label, int place = -1, const char *about = "") {
    ui::section_label(label);
    const int n = place < 0 ? 0 : notes_about(s, AdvicePlace(place));
    if (!n) return;
    const std::string id = std::format("notes{}", place);
    const std::string text = std::format("{} note{} about {}##{}", n, n == 1 ? "" : "s", about, id);
    ImGui::PushFont(nullptr, ui::base_font() * ui::kSmall);
    const float width = ImGui::CalcTextSize(text.c_str(), nullptr, true).x + ImGui::GetFontSize() * 0.62f;
    ImGui::PopFont();
    ImGui::SameLine(ImGui::GetWindowWidth() - kGutter * s.scale - width);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 5 * s.scale);
    if (ui::link(text.c_str())) ImGui::OpenPopup(id.c_str());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * s.scale, 14 * s.scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10 * s.scale, 8 * s.scale));
    ImGui::SetNextWindowSize(ImVec2(470 * s.scale, 0));
    if (ImGui::BeginPopup(id.c_str())) {
        for (const AdviceItem &item : s.advice.items)
            if (advice_shown(item) == AdviceShown::Place && advice_place(item.id) == AdvicePlace(place))
                advice_notice(s, item);
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

// A list item with "HDR" at its right end when hdr is set.
bool mode_item(const std::string &label, bool on, bool hdr) {
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const bool clicked = ImGui::Selectable(label.c_str(), on);
    if (hdr) {
        const char *tag = "HDR";
        ImGui::SameLine(right - ImGui::CalcTextSize(tag).x - ImGui::GetStyle().ItemSpacing.x);
        ImGui::TextColored(ImGui::GetStyleColorVec4(on || ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled), "%s", tag);
    }
    return clicked;
}

// The screen the app plays on: a list on a PC with more than one.
void play_on_row(Launcher &s) {
    row(s, "Play on");
    if (!s.system_known) {
        ui::row_hint("Looking at the screens...");
        return;
    }
    Settings &c = s.now;
    const SystemInfo &sys = s.system;
    const DisplayInfo &d = sys.display;
    if (!d.found) {
        ui::row_hint("No screen could be read: the main one is used.");
    } else if (sys.displays.size() > 1) {
        fill();
        if (ui::begin_combo("##screen", utf8(screen_title(d)), ImGuiComboFlags_HeightLarge)) {
            int id = 0;
            for (const DisplayInfo &other : sys.displays) {
                ImGui::PushID(id++);
                // Two lines each: what it is, then its HDR and variable refresh.
                const std::string label = utf8(screen_title(other)) + "\n" + utf8(screen_details(other));
                if (ImGui::Selectable(label.c_str(), other.device == d.device))
                    // The main screen is what an empty display.monitor means.
                    c.monitor = other.primary ? std::wstring() : monitor_setting(other);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(utf8(screen_title(d)).c_str());
    }
}

// The frame generation choices that cannot be used on this screen, a bit
// each for x2, x3 and x4.
unsigned unusable_multipliers(const Launcher &s) {
    unsigned off = 0;
    for (const MultiplierChoice &m : s.multipliers)
        if (!m.usable && m.multiplier >= 2 && m.multiplier <= 4) off |= 1u << (m.multiplier - 2);
    return off;
}

// ---------------------------------------------------------------- the tabs

// What a new user has to set: where the picture comes from, where it goes,
// and the two extras.
void tab_play(Launcher &s) {
    Settings &c = s.now;
    const Found *f = s.found;
    section(s, "Source", int(AdvicePlace::Card), "this card");
    if (!f) {
        row(s, "Capture card");
        ui::row_hint("Looking for capture cards...");
    } else {
        const Card card = find_card(s);
        const CaptureDevice *device = card.device;
        // Automatic watches the signal in the card's 10-bit mode, if it has one.
        const bool ten_bit_modes = device && !modes_for(device, true).empty();
        const bool ten_bit = c.hdr_auto ? ten_bit_modes : c.hdr;
        const auto title = [](const CaptureDevice &d) { return utf8(card_title(d.name, is_magewell(d.symbolic_link))); };
        const char *const basic = "basic mode";  // a card only mpv can open
        row(s, "Capture card");
        fill();
        const std::string auto_label =
            std::string("Automatic") + kDot + (card.automatic ? title(*card.automatic) : std::string("no card found"));
        const std::string current = c.device.empty() ? auto_label
                                    : device         ? title(*device)
                                                     : utf8(card_title(c.device)) + (card.directshow ? kDot + std::string(basic) : "");
        if (ui::begin_combo("##card", current)) {
            if (ImGui::Selectable(auto_label.c_str(), c.device.empty())) c.device.clear();
            // Items are told apart by their place, not their text: two cards
            // can share a name, and ImGui warns about matching IDs.
            int id = 0;
            for (const auto &d : f->devices) {
                ImGui::PushID(id++);
                if (ImGui::Selectable(title(d).c_str(), names_match(d.name, c.device))) c.device = d.name;
                ImGui::PopID();
            }
            for (const auto &n : f->directshow_only) {
                ImGui::PushID(id++);
                if (ImGui::Selectable((utf8(card_title(n)) + kDot + basic).c_str(), names_match(n, c.device))) c.device = n;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }

        // HDR only where the card has a 10-bit mode (a choice already made
        // stays, so it can be seen and changed).
        const bool no_hdr = device && !ten_bit_modes;
        const bool said_hdr = !c.hdr_auto && c.hdr;
        row(s, "Signal");
        static const char *const kSignal[] = {"Automatic", "SDR", "HDR"};
        const int signal = ui::segmented("##signal", kSignal, c.hdr_auto ? 0 : said_hdr ? 2 : 1, no_hdr ? 1u << 2 : 0);
        if (signal == 0) c.hdr_auto = true, c.hdr = false;
        if (signal == 1) c.hdr_auto = c.hdr = false;
        if (signal == 2) c.hdr_auto = false, c.hdr = true;
        after(c.hdr_auto ? (no_hdr ? "This card has no HDR: SDR." : "Follows what the console sends.")
              : said_hdr ? (no_hdr ? "This card cannot capture HDR." : "Console set to HDR")
                         : "Console set to SDR");

        const auto modes = modes_for(device, ten_bit);
        bool mode_ok = c.width == 0;
        for (const auto &m : modes)
            mode_ok = mode_ok || (m.width == c.width && m.height == c.height && std::abs(m.fps - c.fps) < 0.01);
        if (!mode_ok && device) c.width = c.height = 0, c.fps = 0;  // not offered here: back to automatic
        // What automatic picks: for the screen the app plays on.
        ModeRequest request;
        request.hdr = ten_bit;
        request.max_auto_height = s.system.display.found ? UINT32(s.system.display.height) : 0;
        const auto picked = device ? choose_mode(device->modes, request) : std::nullopt;
        const std::string auto_mode =
            picked ? std::string("Automatic") + kDot + mode_text(picked->width, picked->height, picked->fps)
                   : std::string("Automatic");
        row(s, "Capture mode");
        fill();
        const std::string mode_label = c.width ? mode_text(c.width, c.height, c.fps) : auto_mode;
        if (ui::begin_combo("##mode", mode_label)) {
            const bool auto_hdr = !ten_bit && picked && has_hdr(device, picked->width, picked->height, picked->fps);
            if (mode_item(auto_mode, c.width == 0, auto_hdr)) c.width = c.height = 0, c.fps = 0;
            int id = 0;
            for (const auto &m : modes) {
                const bool on = m.width == c.width && m.height == c.height && std::abs(m.fps - c.fps) < 0.01;
                ImGui::PushID(id++);
                if (mode_item(mode_text(m.width, m.height, m.fps), on, !ten_bit && has_hdr(device, m.width, m.height, m.fps))) {
                    c.width = m.width;
                    c.height = m.height;
                    c.fps = m.fps;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (device && is_magewell(device->symbolic_link)) {
            row(s, "Low latency");
            ui::toggle("##low_latency", &c.low_latency);
            after("The card passes frames on a few ms sooner.");
            ui::tooltip("The card passes each frame on while it is still coming in: a few milliseconds sooner, and more "
                        "evenly timed. The card's driver keeps the setting, also for other programs.");
        }
    }

    section(s, "Screen");
    play_on_row(s);
    if (s.system_known && s.system.display.found) sub(s, utf8(screen_details(s.system.display)));

    section(s, "Extras", int(AdvicePlace::Extras), "DLSS 5");
    row(s, "Frame generation");
    ui::toggle("##framegen", &c.framegen);
    ImGui::SameLine();
    if (!c.framegen) ImGui::BeginDisabled();
    static const char *const kUpTo[] = {"x2", "x3", "x4"};
    const int up_to = ui::segmented("##up_to", kUpTo, c.framegen_multiplier - 2, unusable_multipliers(s));
    if (up_to >= 0) c.framegen_multiplier = up_to + 2;
    after("up to");
    if (!c.framegen) ImGui::EndDisabled();

    // DLSS 5 is judged for the GPU mpv draws on.
    const GpuInfo &gpu = s.system.gpu.gpu;
    const bool nr_gpu = s.system_known && dlss5_gpu(gpu);
    const bool forced = s.original.dlss5 == Settings::Dlss5::On;  // the settings file: try on any GPU
    const bool can = s.dlss5_installed && (nr_gpu || forced);
    row(s, "DLSS 5");
    if (!can) ImGui::BeginDisabled();
    bool dlss5 = c.dlss5 != Settings::Dlss5::Off && can;
    if (ui::toggle("##dlss5", &dlss5))
        // Back on: as the settings file had it, else automatic (on: for a GPU that is only tried).
        c.dlss5 = !dlss5 ? Settings::Dlss5::Off
                  : s.original.dlss5 != Settings::Dlss5::Off ? s.original.dlss5
                  : nr_gpu ? Settings::Dlss5::Auto : Settings::Dlss5::On;
    if (!can) ImGui::EndDisabled();
    if (s.system_known && !nr_gpu && !forced) {
        after("Needs an NVIDIA RTX 30, 40 or 50 series GPU.");
    } else if (s.system_known && !s.dlss5_installed) {
        ImGui::SameLine();
        if (ui::action_button("Download##dlss5")) setup(s, LauncherAction::Setup);
        after("Not downloaded yet (about 250 MB).");
    } else {
        after("Neural rendering. Changes how the game looks.");
    }

    // The input program (input_program.h): separate from the app, started
    // with the picture.
    row(s, "Mouse and keyboard");
    if (!s.input_found) ImGui::BeginDisabled();
    bool input = c.input && s.input_found;
    if (ui::toggle("##input", &input)) c.input = input;
    if (!s.input_found) ImGui::EndDisabled();
    after(s.input_found ? "Play the console with this PC's mouse and keyboard." : "No input program is set.");
    ui::tooltip("Starts the input program with the picture and ends it with the picture. Only the mouse and keyboard "
                "go to the console, over its Remote Play on your own network; the picture stays the capture card's. "
                "The input program's own key hands the mouse and keyboard to the game and back.");
}

// How a capture smaller than the screen is scaled up.
void scaling_rows(Launcher &s) {
    Settings &c = s.now;
    const GpuInfo &gpu = s.system.gpu.gpu;
    const std::wstring vsr = s.system_known ? vsr_kind(gpu) : L"none";
    const char *vsr_label = vsr == L"intel" ? "Video super resolution (Intel)" : "Video super resolution (NVIDIA RTX)";
    struct Option {
        Settings::Scaling value;
        const char *label;
    };
    const Option options[] = {
        {Settings::Scaling::Mpv, "mpv (Lanczos)"},
        {Settings::Scaling::Fsr, "FSR 1 (sharper)"},
        {Settings::Scaling::Vsr, vsr_label},
        {Settings::Scaling::Cnn, "ArtCNN (neural, the most detail)"},
        {Settings::Scaling::Anime4k, "Anime4K (cel-shaded and drawn styles)"},
    };
    row(s, "Scaling");
    fill();
    const char *current = options[0].label;
    for (const Option &o : options)
        if (o.value == c.scaling) current = o.label;
    if (ui::begin_combo("##scaling", current)) {
        for (const Option &o : options) {
            const bool off = o.value == Settings::Scaling::Vsr && vsr == L"none";
            if (off) ImGui::BeginDisabled();
            if (ImGui::Selectable(o.label, o.value == c.scaling)) c.scaling = o.value;
            if (off) ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    // A capture as tall as its place on the chosen screen is not scaled at all.
    const Outcome &o = s.advice.outcome;
    if (s.advice_valid && o.capture_known && o.picture_height > 0 && !o.upscaled)
        sub(s, "Not used now: the capture already fills the screen.");
    else if (c.scaling == Settings::Scaling::Vsr && s.system_known && gpu.found && vsr == L"none")
        sub(s, "Needs an NVIDIA RTX or Intel GPU: FSR 1 is used on this one.");
    else if (c.scaling == Settings::Scaling::Vsr)
        sub(s, "Uses the GPU's video engine; FSR 1 if it cannot run.");
    else if (c.scaling == Settings::Scaling::Cnn || c.scaling == Settings::Scaling::Anime4k)
        sub(s, "Takes more GPU time, which DLSS 5 and frame generation also need.");
}

// What is done with the picture: an HDR signal, an SDR one, and scaling up.
void tab_picture(Launcher &s) {
    Settings &c = s.now;
    section(s, "An HDR signal", int(AdvicePlace::Picture), "the picture");
    // Signal: Automatic can turn out either way, so both halves apply.
    const bool hdr = c.hdr || c.hdr_auto;
    const bool sdr = !c.hdr || c.hdr_auto;
    if (!hdr) ImGui::BeginDisabled();
    Settings as_hdr = c;
    as_hdr.hdr = true;
    const HdrPlan plan = hdr_plan(as_hdr, s.system.display);
    const bool to_sdr = plan.output == HdrPlan::Output::Sdr;

    row(s, "Send it as");
    fill();
    const std::string automatic = std::string("Automatic") + kDot + "HDR when the screen has it on";
    const std::string always_sdr = std::string("Always SDR") + kDot + "tone mapped";
    const char *const outputs[] = {automatic.c_str(), "Always HDR", always_sdr.c_str()};
    const Settings::HdrOutput values[] = {Settings::HdrOutput::Auto, Settings::HdrOutput::Hdr, Settings::HdrOutput::Sdr};
    int current = c.hdr_output == Settings::HdrOutput::Hdr ? 1 : c.hdr_output == Settings::HdrOutput::Sdr ? 2 : 0;
    if (ui::begin_combo("##hdr_output", outputs[current])) {
        for (int i = 0; i < 3; ++i)
            if (ImGui::Selectable(outputs[i], i == current)) c.hdr_output = values[i];
        ImGui::EndCombo();
    }
    if (!hdr)
        sub(s, "The signal is set to SDR (Play tab): these are for an HDR signal.");
    else if (s.system_known)
        sub(s, "Now: " + utf8(hdr_plan_text(plan)) + ".");

    // Fitting: only for HDR output; SDR output is always tone mapped.
    row(s, "Fit highlights");
    if (to_sdr) ImGui::BeginDisabled();
    ui::toggle("##fit", &c.hdr_fit_to_screen);
    if (to_sdr) ImGui::EndDisabled();
    after(hdr && to_sdr          ? "Not used: tone mapping to SDR fits it already."
          : c.hdr_fit_to_screen ? "On: highlights come down to the screen's peak."
                                : "Off: the console's own HDR calibration fits it.");
    ui::tooltip("Off: the picture goes out as the console sends it, and the console's HDR calibration, run once while "
                "playing here, fits it to the screen. On: for a console that cannot be calibrated to this screen.");
    const bool fitting = c.hdr_fit_to_screen && !to_sdr;
    if (!fitting) ImGui::BeginDisabled();
    row(s, "Screen peak");
    fill();
    const int reported = s.system.display.peak_nits >= 80 ? int(std::lround(s.system.display.peak_nits)) : 0;
    const std::string as_reported =
        reported ? std::format("What the screen reports ({} nits)", reported) : std::string("What the screen reports (nothing)");
    const std::string peak_label = c.display_peak_nits > 0 ? std::format("{} nits", c.display_peak_nits) : as_reported;
    if (ui::begin_combo("##peak", peak_label)) {
        if (ImGui::Selectable(as_reported.c_str(), c.display_peak_nits == 0)) c.display_peak_nits = 0;
        for (int nits : {400, 500, 600, 800, 1000, 1200, 1500, 2000, 3000, 4000}) {
            const std::string label = std::format("{} nits", nits);
            if (ImGui::Selectable(label.c_str(), c.display_peak_nits == nits)) c.display_peak_nits = nits;
        }
        ImGui::EndCombo();
    }
    ui::tooltip("Screens often report less than they reach: pick the real peak if you know it.");
    if (!fitting) ImGui::EndDisabled();

    // The curve and the per-frame peak: for fitting and for SDR output.
    const bool mapping = fitting || to_sdr;
    if (!mapping) ImGui::BeginDisabled();
    row(s, "Curve");
    const auto curve_label = [&](const std::wstring &name) {
        if (name != L"auto") return utf8(name);
        return std::string(to_sdr ? "Automatic (mpv's spline)" : "Automatic (BT.2390)");
    };
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.50f);
    if (ui::begin_combo("##curve", curve_label(c.tone_mapping))) {
        for (const std::wstring &name : tone_mapping_names())
            if (ImGui::Selectable(curve_label(name).c_str(), name == c.tone_mapping)) c.tone_mapping = name;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ui::toggle("##dynamic_peak", &c.hdr_dynamic_peak);
    after("Measure each frame");
    ui::tooltip("Measuring each frame's peak fits games that never reach their peak better, for some GPU time. Off: "
                "the console's peak is where the curve starts.");
    if (!mapping) ImGui::EndDisabled();

    row(s, "Console's peak");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.38f);
    const std::string source_label = std::format("{} nits", c.hdr_peak_nits);
    if (ui::begin_combo("##source_peak", source_label)) {
        bool listed = false;
        for (int nits : {400, 600, 800, 1000, 1500, 2000, 4000, 10000}) {
            listed = listed || nits == c.hdr_peak_nits;
            const std::string label = std::format("{} nits", nits);
            if (ImGui::Selectable(label.c_str(), c.hdr_peak_nits == nits)) c.hdr_peak_nits = nits;
        }
        if (!listed) ImGui::Selectable(source_label.c_str(), true);  // the settings file's own value stays
        ImGui::EndCombo();
    }
    after("As set in its HDR calibration.");
    ui::tooltip("The brightest the console's picture gets. It goes into the HDR metadata sent to the screen.");

    // The in-game menu's white in HDR.
    row(s, "Menu brightness");
    fill();
    const std::string as_windows = "As Windows' SDR brightness";
    const std::string white_label = c.paper_white_nits > 0 ? std::format("{} nits", c.paper_white_nits) : as_windows;
    if (ui::begin_combo("##paper_white", white_label)) {
        if (ImGui::Selectable(as_windows.c_str(), c.paper_white_nits == 0)) c.paper_white_nits = 0;
        bool listed = c.paper_white_nits == 0;
        for (int nits : {100, 150, 203, 250, 300, 400}) {
            listed = listed || nits == c.paper_white_nits;
            const std::string label = std::format("{} nits", nits);
            if (ImGui::Selectable(label.c_str(), c.paper_white_nits == nits)) c.paper_white_nits = nits;
        }
        if (!listed) ImGui::Selectable(white_label.c_str(), true);
        ImGui::EndCombo();
    }
    ui::tooltip("How bright the menu's white is drawn over an HDR picture.");
    if (!hdr) ImGui::EndDisabled();

    section(s, "An SDR signal, and scaling up");
    row(s, "SDR range");
    if (!sdr) ImGui::BeginDisabled();
    static const char *const kRange[] = {"Limited", "Full"};
    const int range = ui::segmented("##range", kRange, c.sdr_full_range ? 1 : 0);
    if (range >= 0) c.sdr_full_range = range == 1;
    if (!sdr) ImGui::EndDisabled();
    after(!sdr ? "Not used: the signal is HDR." : "Limited suits most consoles.");
    ui::tooltip("As the console's RGB range is set. Wrong: blacks look grey, or dark detail is crushed.");
    scaling_rows(s);
}

// A small picture with the areas frame generation leaves alone on it.
void areas_picture(const Launcher &s, const std::vector<KeepRegion> &areas) {
    const float w = 96 * s.scale, h = 54 * s.scale;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *d = ImGui::GetWindowDrawList();
    d->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ui::u32(ui::kControl), 4 * s.scale);
    for (const KeepRegion &r : areas) {
        const ImVec2 a(p.x + w * r.x * 0.01f, p.y + h * r.y * 0.01f);
        const ImVec2 b(p.x + w * (r.x + r.w) * 0.01f, p.y + h * (r.y + r.h) * 0.01f);
        const float line = std::max(1.0f, 1.5f * s.scale);
        if (r.ellipse)
            d->AddEllipse(ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), ImVec2((b.x - a.x) * 0.5f, (b.y - a.y) * 0.5f),
                          ui::u32(ui::kText), 0.0f, 0, line);
        else
            d->AddRect(a, b, ui::u32(ui::kText), 0.0f, 0, line);
    }
    ImGui::Dummy(ImVec2(w, h));
}

void tab_frames(Launcher &s) {
    Settings &c = s.now;
    section(s, "Frame generation", int(AdvicePlace::Frames), "frame generation");
    row(s, "Frame generation");
    ui::toggle("##framegen", &c.framegen);
    const Outcome &o = s.advice.outcome;
    if (c.framegen && s.advice_valid && !o.framegen)
        after("It cannot run as things are: see Worth a look.", ui::kWarn);
    else
        after("Adds frames between the game's own.");
    if (!c.framegen) ImGui::BeginDisabled();

    // Up to x2, x3 or x4: a choice not usable on this screen is greyed out
    // and says why (one already chosen stays, with what it comes to).
    row(s, "Up to");
    const char *const titles[] = {"x2", "x3", "x4"};
    std::string notes[3];
    const MultiplierChoice *chosen = nullptr;
    for (int k = 2; k <= 4; ++k) {
        notes[k - 2] = std::format("30 \xE2\x86\x92 {} fps", 30 * k);
        for (const MultiplierChoice &m : s.multipliers)
            if (m.multiplier == k) {
                notes[k - 2] = utf8(m.note);
                if (c.framegen_multiplier == k) chosen = &m;
            }
    }
    const char *const note_texts[] = {notes[0].c_str(), notes[1].c_str(), notes[2].c_str()};
    const int up_to = ui::cells("##up_to", titles, note_texts, 3, c.framegen_multiplier - 2, unusable_multipliers(s),
                                ImGui::GetContentRegionAvail().x);
    if (up_to >= 0) c.framegen_multiplier = up_to + 2;
    {
        const int added = c.framegen_multiplier - 1;
        const double delay = chosen ? chosen->delay_ms : 1000.0 / 30.0 * added / c.framegen_multiplier;
        if (c.framegen && chosen && !chosen->usable && !chosen->why.empty())
            sub(s, std::format("x{} changes nothing here: {}", c.framegen_multiplier, utf8(chosen->why)));
        else
            sub(s, std::format("{} added frame{} per game frame, each a full pass on the GPU. A 30 fps game's own "
                               "frame shows {:.0f} ms later.",
                               added, added == 1 ? "" : "s", delay));
    }

    // What makes the frames: the app's own generator on any GPU, or NVIDIA's
    // where the GPU has it.
    const GpuInfo &gpu = s.system.gpu.gpu;
    const bool dlss_gpu = !s.system_known || dlss_fg_gpu(gpu);
    row(s, "Frames by");
    // "ANR", not "Apex ANR": four choices have to fit the row (the tooltip names it in full).
    static const char *const kBy[] = {APP_NAME, "NVIDIA DLSS", "NVIDIA Video", "ANR"};
    const bool video_there = dlss_gpu && s.video_installed;
    const int by = ui::segmented("##by", kBy, c.framegen_anr ? 3 : c.framegen_video ? 2 : c.framegen_dlss ? 1 : 0,
                                 (dlss_gpu ? 0 : 1u << 1) | (video_there ? 0 : 1u << 2) | (s.anr_installed ? 0 : 1u << 3));
    if (by >= 0) c.framegen_dlss = by == 1, c.framegen_video = by == 2, c.framegen_anr = by == 3;
    ui::tooltip("NVIDIA's generators need an NVIDIA RTX 40 or 50 series GPU. NVIDIA Video (a download on the About "
                "tab) is made for video: it works out the motion from the two pictures. NVIDIA DLSS is made for "
                "games, which tell it the motion; here it has to do without. Apex ANR (a beta, a download on the "
                "About tab, any GPU) is made for games too, and gets the motion " APP_NAME " finds. Quality and "
                "Method are for " APP_NAME "'s own, which steps in if another cannot run.");

    row(s, "Quality");
    static const char *const kQuality[] = {"High", "Fast"};
    const int quality = ui::segmented("##quality", kQuality, c.framegen_fast ? 1 : 0);
    if (quality >= 0) c.framegen_fast = quality == 1;
    after("Fast: about half the GPU time.");

    row(s, "Method");
    static const char *const kMethod[] = {"Motion", "Blend"};
    const int method = ui::segmented("##method", kMethod, c.framegen_blend ? 1 : 0);
    if (method >= 0) c.framegen_blend = method == 1;
    after("Blend: lightest, shows double.");
    ui::tooltip("Motion: the added frame follows what moves. Blend: a plain mix of two frames, the lightest, but moving "
                "things show double.");

    row(s, "Limit");
    fill();
    const double refresh = s.system.display.found ? s.system.display.refresh_hz : 0;
    const std::string screen_rate = refresh > 0 ? std::format("The screen's rate ({:.0f} Hz)", refresh)
                                                : std::string("The screen's rate");
    const std::string cap_label = c.framegen_max_hz > 0 ? std::format("{} fps", c.framegen_max_hz) : screen_rate;
    if (ui::begin_combo("##cap", cap_label)) {
        if (ImGui::Selectable(screen_rate.c_str(), c.framegen_max_hz == 0)) c.framegen_max_hz = 0;
        bool listed = c.framegen_max_hz == 0;
        for (int hz : {48, 60, 72, 90, 100, 120, 144, 165, 200, 240, 360}) {
            if (refresh > 0 && hz >= std::round(refresh)) break;  // not below the screen's rate: it would change nothing
            listed = listed || hz == c.framegen_max_hz;
            const std::string label = std::format("{} fps", hz);
            if (ImGui::Selectable(label.c_str(), c.framegen_max_hz == hz)) c.framegen_max_hz = hz;
        }
        if (!listed) ImGui::Selectable(cap_label.c_str(), true);  // the settings file's own value stays
        ImGui::EndCombo();
    }
    ui::tooltip("Frames are added only while the result stays at or under this. With variable refresh, a little under "
                "the screen's rate keeps it inside the range.");

    // The areas left alone are drawn over the picture, in the menu: here
    // they are only shown.
    std::vector<KeepRegion> areas;
    parse_keep_regions(narrow(c.framegen_keep), areas);
    row(s, "Areas left alone");
    areas_picture(s, areas);
    after(areas.empty() ? std::string("None. Draw them over the picture: Tab while playing.")
                        : std::format("{} area{}. Edit them over the picture: Tab while playing.", areas.size(),
                                      areas.size() == 1 ? "" : "s"));
    row(s, "Find the HUD");
    ui::toggle("##hud", &c.framegen_hud);
    after("Everywhere, not only in the areas.");
    ui::tooltip("Added frames show the game's own frame wherever a HUD is found, so its text stays sharp. Off: only in "
                "the areas left alone that are set to find the HUD.");
    if (!c.framegen) ImGui::EndDisabled();

    row(s, "Frame rates");
    ui::toggle("##rates", &c.framegen_show_rates);
    after("Show them in a corner while playing.");
}

// The screen, its variable refresh, the GPU, and the window.
void tab_screen(Launcher &s) {
    Settings &c = s.now;
    section(s, "Screen", int(AdvicePlace::Screen), "this screen");
    play_on_row(s);
    const DisplayInfo &d = s.system.display;
    if (s.system_known && d.found) sub(s, utf8(screen_hdr_text(d)));

    // Variable refresh: what was found, and the user's word when the app
    // cannot read it (AMD and Intel GPUs) or reads it wrong.
    row(s, "Variable refresh");
    const std::string as_found = "As found: " + (d.found ? utf8(vrr_found_word(d)) : std::string("not known"));
    const char *const labels[] = {as_found.c_str(), "On", "Off"};
    const int vrr = ui::segmented("##vrr", labels, c.vrr == Settings::Vrr::On ? 1 : c.vrr == Settings::Vrr::Off ? 2 : 0);
    if (vrr >= 0) c.vrr = vrr == 1 ? Settings::Vrr::On : vrr == 2 ? Settings::Vrr::Off : Settings::Vrr::Auto;
    const std::string brand = d.found ? utf8(vrr_brand_text(d)) : std::string();
    sub(s, (brand.empty() ? std::string() : brand + ". ") +
               "Switched in the GPU's control panel and the monitor's menu; this only tells " APP_NAME " what to pace by.");

    // The GPU: a choice only on a PC with more than one.
    const GpuChoice &gpu = s.system.gpu;
    if (s.system_known && s.system.gpus.size() > 1) {
        row(s, "GPU");
        fill();
        const std::string own = "The screen's own" + (gpu.screen_gpu.found ? kDot + utf8(gpu.screen_gpu.name) : std::string());
        const std::string current = c.gpu.empty() || !gpu.gpu.found ? own : utf8(gpu.gpu.name);
        if (ui::begin_combo("##gpu", current)) {
            if (ImGui::Selectable(own.c_str(), c.gpu.empty())) c.gpu.clear();
            int id = 0;
            for (const GpuInfo &g : s.system.gpus) {
                ImGui::PushID(id++);
                if (ImGui::Selectable(utf8(gpu_title(g)).c_str(), !c.gpu.empty() && gpu.gpu.found && g.name == gpu.gpu.name))
                    c.gpu = g.name;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    } else if (s.system_known && gpu.gpu.found) {
        row(s, "GPU");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(utf8(gpu_title(gpu.gpu)).c_str());
    }

    section(s, "Window");
    row(s, "Show as");
    static const char *const kShow[] = {"Full screen", "Borderless", "Window"};
    const int show = ui::segmented("##show", kShow, c.fullscreen ? 0 : c.borderless ? 1 : 2);
    if (show == 0) c.borderless = c.fullscreen = true;
    if (show == 1) c.borderless = true, c.fullscreen = false;
    if (show == 2) c.borderless = c.fullscreen = false;
    ui::tooltip("Full screen is mpv's own, as F or a double-click switches it. Borderless fills the screen without "
                "being it.");
    const bool bars = c.borderless && !c.fullscreen;
    row(s, "Skip black bars");
    if (!bars) ImGui::BeginDisabled();
    ui::toggle("##bars", &c.skip_black_bars);
    if (!bars) ImGui::EndDisabled();
    after(!bars ? "For Borderless." : "For 16:9 on an ultrawide: a lighter load.");
    ui::tooltip("For a picture of another shape than the screen: DLSS 5 and frame generation work on the picture "
                "only. On some PCs Windows then composes the window and frame generation falls short: check the frame "
                "rates in the menu.");
    row(s, "Stream window");
    ui::toggle("##stream", &c.stream_window);
    after("A second window in SDR, for sharing.");
    ui::tooltip("A 2560 x 1440 window in SDR with a live copy of the picture, on a screen other than the one you play "
                "on. Share that window in Discord or OBS: your own picture stays as it is, HDR included.");
}

void tab_sound(Launcher &s) {
    Settings &c = s.now;
    const Found *f = s.found;
    const float value_room = -62 * s.scale;  // for the figure after a slider
    section(s, "Sound");
    row(s, "Card's sound");
    ui::toggle("##audio", &c.audio);
    if (!c.audio) ImGui::BeginDisabled();
    row(s, "Input");
    fill();
    const std::string own = "The card's own (automatic)";
    const std::string current = c.audio_device.empty() ? own : utf8(c.audio_device);
    if (ui::begin_combo("##audio_input", current)) {
        if (ImGui::Selectable(own.c_str(), c.audio_device.empty())) c.audio_device.clear();
        int id = 0;
        if (f)
            for (const auto &a : f->audio) {
                ImGui::PushID(id++);
                if (ImGui::Selectable(utf8(a.name).c_str(), a.name == c.audio_device)) c.audio_device = a.name;
                ImGui::PopID();
            }
        ImGui::EndCombo();
    }
    row(s, "Volume");
    ui::slider_int("##volume", &c.volume, 0, 100, 2, value_room);
    value_text(s, std::format("{} %", c.volume));
    row(s, "Boost");
    ui::slider_int("##boost", &c.boost, 100, 400, 10, value_room);
    value_text(s, std::format("{} %", c.boost));
    row(s, "Audio delay");
    ui::slider_int("##delay", &c.audio_delay_ms, 0, std::max(500, c.audio_delay_ms), 5, value_room);
    value_text(s, std::format("{} ms", c.audio_delay_ms));
    sub(s, "Holds the sound back to match the picture.");
    row(s, "Timing");
    static const char *const kTiming[] = {"Lowest latency", "Steadier"};
    const int timing = ui::segmented("##timing", kTiming, c.audio_min_period ? 0 : 1);
    if (timing >= 0) c.audio_min_period = timing == 0;
    sub(s, "Steadier if the sound crackles.");
    if (!c.audio) ImGui::EndDisabled();
}

// The settings few people need, in a small window of their own.
void advanced_popup(Launcher &s) {
    Settings &c = s.now;
    const float sc = s.scale, column = 170 * sc;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * sc, 14 * sc));
    ImGui::SetNextWindowSize(ImVec2(520 * sc, 0));
    if (ImGui::BeginPopup("advanced")) {
        const auto label = [column](const char *text) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(text);
            ImGui::SameLine(column + ImGui::GetStyle().WindowPadding.x);
        };
        ui::section_label("Advanced", 0);
        label("Card opened in");
        static const char *const kPath[] = {"Automatic", "Full mode", "Basic mode"};
        const int path = ui::segmented("##path", kPath,
                                       c.path == Settings::CapturePath::MediaFoundation ? 1
                                       : c.path == Settings::CapturePath::DirectShow    ? 2
                                                                                        : 0);
        if (path >= 0)
            c.path = path == 1 ? Settings::CapturePath::MediaFoundation
                     : path == 2 ? Settings::CapturePath::DirectShow
                                 : Settings::CapturePath::Auto;
        ui::hint("Basic mode: mpv reads the card. No frame generation, and repeated frames are not dropped.");
        label("Repeated frames");
        static const char *const kDedup[] = {"Dropped", "Kept"};
        const int dedup = ui::segmented("##dedup", kDedup, c.dedup ? 0 : 1);
        if (dedup >= 0) c.dedup = dedup == 0;
        ui::hint("A console sends a 30 fps game as 60 frames, each twice. Kept: no frame generation.");
        label("mpv's log");
        ui::toggle("##mpv_log", &c.mpv_log);
        ImGui::SameLine();
        ui::row_hint("Written to the logs folder.");
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

// A web address without its scheme, as a link is shown.
std::string bare_address(const char *url) {
    const std::string text = url;
    const size_t at = text.find("://");
    return at == std::string::npos ? text : text.substr(at + 3);
}

// The parts the app downloads, help, how it starts, and who made what.
void tab_about(Launcher &s) {
    Settings &c = s.now;
    section(s, "Downloaded parts");
    row(s, "Installed");
    ui::row_hint(s.versions.empty() ? "Nothing yet: mpv and ReShade are downloaded when you press Play." : s.versions.c_str(),
                 ui::kText);
    if (!s.versions.empty()) {
        if (s.other_versions) {
            if (ui::notice("versions", ui::Status::Warn, "Warning",
                           "This " APP_NAME " was tested with other versions of these.", "Download tested versions"))
                setup(s, LauncherAction::Setup);
        } else {
            row(s, "Repair");
            if (ui::action_button("Download again")) setup(s, LauncherAction::Setup);
            after("Fetches the tested versions once more.");
        }
    }
    if (!s.dlss5_installed && s.system_known && dlss5_gpu(s.system.gpu.gpu)) {
        row(s, "DLSS 5");
        if (ui::action_button("Download##dlss5")) setup(s, LauncherAction::Setup);
        after("Not downloaded yet (about 250 MB).");
    }
    if (!s.dlssfg_installed && (!s.system_known || dlss_fg_gpu(s.system.gpu.gpu))) {
        row(s, "DLSS Frame Gen.");
        if (ui::action_button("Download##dlssfg")) setup(s, LauncherAction::SetupDlssFg);
        after("Not downloaded (7 MB, from NVIDIA).");
    }
    if (!s.video_installed && (!s.system_known || dlss_fg_gpu(s.system.gpu.gpu))) {
        row(s, "NVIDIA Video");
        if (ui::action_button("Download##video")) setup(s, LauncherAction::SetupVideo);
        after("Not downloaded (440 MB, NVIDIA's licences).");
    }
    if (!s.anr_installed) {
        row(s, "Apex ANR");
        if (ui::action_button("Download##anr")) setup(s, LauncherAction::SetupAnr);
        after("Not downloaded (4 MB, Apex Compute's beta).");
    }

    section(s, "Help");
    if (s.extras && s.extras->bug_report && s.bug) {
        const int state = s.bug->state.load();
        row(s, "Bug report");
        if (state == BugReport::Saving) {
            ui::row_hint("Saving a bug report...");
        } else {
            if (ui::action_button("Save a bug report")) s.bug->start(s.extras->bug_report);
            if (state == BugReport::Saved) {
                after("Saved in the Bug reports folder: logs, settings and the PC, no user name.");
            } else if (state == BugReport::Failed) {
                after("Could not save it: " + narrow(s.bug->error), ui::kBad);
            } else {
                ImGui::SameLine();
                ui::key_cap("F9", ImGui::GetFrameHeight());
                after("while playing adds the frames on screen.");
            }
        }
    }
    row(s, "Files");
    if (ui::action_button("Open the settings file")) open_in_windows(s.settings_path);
    ImGui::SameLine();
    if (ui::action_button("Open the logs folder"))
        open_in_windows(s.engine_dir.substr(0, s.engine_dir.find_last_of(L'\\')) + L"\\logs");

    section(s, "Starting");
    row(s, "This window");
    ui::toggle("##show_launcher", &c.show_launcher);
    after("Show it when " APP_NAME " starts. Off: hold Shift to see it.");
    row(s, "Advanced");
    if (ui::action_button("Change")) ImGui::OpenPopup("advanced");
    after("The card's mode, repeated frames, mpv's log.");
    advanced_popup(s);

    // Who made what: this app and where it lives, the project it is based
    // on, and NVIDIA for DLSS.
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6 * s.scale);
    const ImVec2 line = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(line, ImVec2(line.x + ImGui::GetContentRegionAvail().x, line.y), ui::u32(ui::kLine));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 7 * s.scale);
    ui::hint(APP_NAME " " APP_VERSION);
    ImGui::SameLine(0, 12 * s.scale);
    if (ui::link((bare_address(APP_HOMEPAGE) + "##home").c_str())) open_in_windows(widen(APP_HOMEPAGE));
    ui::hint("Based on " APP_BASED_ON);
    ImGui::SameLine(0, 12 * s.scale);
    if (ui::link((bare_address(APP_BASED_ON_URL) + "##based").c_str())) open_in_windows(widen(APP_BASED_ON_URL));
    ui::hint("DLSS 5 and DLSS Frame Generation use NVIDIA DLSS, a technology of NVIDIA Corporation.");
}

// ---------------------------------------------------------------- the frame around them

// The name, the version and the tabs: plain words, the open one with a line
// of the accent under it.
void top_bar(Launcher &s) {
    const float sc = s.scale, h = std::round(kTopBar * sc);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList *d = ImGui::GetWindowDrawList();
    float x = origin.x + kGutter * sc;

    ImGui::PushFont(ui::bold_font(), ui::base_font() * 1.24f);
    const float name_size = ImGui::GetFontSize();
    const float name_y = origin.y + (h - name_size) * 0.5f;
    d->AddText(ImVec2(x, name_y), ui::u32(ui::kText), APP_NAME);
    x += ImGui::CalcTextSize(APP_NAME).x + 8 * sc;
    ImGui::PopFont();
    ImGui::PushFont(nullptr, ui::base_font() * 0.8f);
    d->AddText(ImVec2(x, name_y + (name_size - ImGui::GetFontSize()) * 0.74f), ui::u32(ui::kTextDim), APP_VERSION);
    x += ImGui::CalcTextSize(APP_VERSION).x + kGutter * sc;
    ImGui::PopFont();

    // "##tab": the first has the same word on it as the Play button.
    const char *const names[kTabCount] = {"Play##tab",   "Picture##tab", "Frame generation##tab",
                                          "Screen##tab", "Sound##tab",   "About##tab"};
    const float fs = ImGui::GetFontSize();
    for (int i = 0; i < kTabCount; ++i) {
        ImGui::PushFont(ui::bold_font(), 0.0f);
        const float bold_w = ImGui::CalcTextSize(names[i], nullptr, true).x;
        ImGui::PopFont();
        const float w = bold_w + 24 * sc;
        ImGui::SetCursorScreenPos(ImVec2(x, origin.y));
        if (ImGui::InvisibleButton(names[i], ImVec2(w, h), ImGuiButtonFlags_EnableNav)) s.tab = i;
        const bool on = s.tab == i;
        const bool lit = on || ImGui::IsItemHovered();
        if (on) ImGui::PushFont(ui::bold_font(), 0.0f);
        const float text_w = ImGui::CalcTextSize(names[i], nullptr, true).x;
        d->AddText(ImVec2(x + (w - text_w) * 0.5f, origin.y + (h - fs) * 0.5f), ui::u32(lit ? ui::kText : ui::kTextDim), names[i],
                   std::strstr(names[i], "##"));
        if (on) ImGui::PopFont();
        if (on)
            d->AddRectFilled(ImVec2(x, origin.y + h - std::max(2.0f, 2 * sc)), ImVec2(x + w, origin.y + h), ui::u32(ui::kAccent));
        x += w + 2 * sc;
    }
    d->AddLine(ImVec2(origin.x, origin.y + h), ImVec2(origin.x + ImGui::GetWindowWidth(), origin.y + h), ui::u32(ui::kLine));
}

// "What you will get", in the order the picture travels: the console, the
// capture card, the frames, the screen; then what is worth a look.
void result_column(Launcher &s) {
    const float sc = s.scale;
    ui::section_label("What you will get");
    if (!s.advice_valid) {
        ui::hint(s.found ? "Looking at the screens..." : "Looking at this PC...");
        return;
    }
    ImDrawList *d = ImGui::GetWindowDrawList();
    const float indent = 22 * sc;
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    float last_dot = -1;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2 * sc);
    for (const ResultStep &step : s.steps) {
        // The step's dot, joined to the one before by a thin line.
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 dot(p.x + 6 * sc, p.y + 8 * sc);
        if (last_dot >= 0) d->AddLine(ImVec2(dot.x, last_dot + 9 * sc), ImVec2(dot.x, dot.y - 9 * sc), ui::u32(ui::kLine), 1.5f * sc);
        d->AddCircleFilled(dot, 4.5f * sc, ui::u32(ui::kAccent));
        last_dot = dot.y;

        ImGui::Indent(indent);
        ImGui::PushFont(ui::bold_font(), ui::base_font() * 0.74f);
        ui::spaced_text(utf8(step.title).c_str(), ui::kTextDim, ImGui::GetFontSize() * 0.09f);
        ImGui::PopFont();
        if (!step.chips.empty()) {
            ImGui::PushFont(nullptr, ui::base_font() * 0.9f);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(5 * sc, 5 * sc));
            for (size_t i = 0; i < step.chips.size(); ++i) {
                const std::string label = utf8(step.chips[i]);
                if (i && ImGui::GetItemRectMax().x + 5 * sc + ui::chip_width(label.c_str()) <= right) ImGui::SameLine();
                ui::chip(label.c_str());
                // What the setting is called on the console, and why.
                for (const AdviceItem &item : s.advice.items)
                    if (item.kind == AdviceKind::Console && item.brief == step.chips[i]) ui::tooltip(utf8(item.text).c_str());
            }
            // The console's HDR calibration, where it is what fits the
            // picture to the screen: one more thing to do there, once.
            const std::string calibration = utf8(console_hint(s.advice));
            if (!calibration.empty()) {
                const char *const label = "Calibrate HDR once";
                if (ImGui::GetItemRectMax().x + 5 * sc + ui::chip_width(label) <= right) ImGui::SameLine();
                ui::chip(label);
                ui::tooltip(calibration.c_str());
            }
            ImGui::PopStyleVar();
            ImGui::PopFont();
        }
        // A step that is not as asked is in the warning's colour, one that
        // stops the play in the problem's; its words say which.
        const ImVec4 &status = step.problem ? ui::kBad : ui::kWarn;
        if (!step.big.empty()) {
            ImGui::PushFont(ui::bold_font(), ui::base_font() * 1.16f);
            ui::text(utf8(step.big).c_str(), step.matters ? status : ui::kText);
            ImGui::PopFont();
        }
        if (step.kind == ResultStep::Kind::Frames) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2 * sc);
            ui::cadence_strip(step.multiplier, right - ImGui::GetCursorScreenPos().x, 11 * sc);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 1 * sc);
        }
        if (!step.detail.empty()) ui::hint(utf8(step.detail).c_str(), step.matters ? status : ui::kTextDim);
        ImGui::Unindent(indent);
        ImGui::Dummy(ImVec2(0, 3 * sc));  // room before the next step
    }

    // Problems, warnings and what is recommended.
    int notices = 0;
    for (const AdviceItem &item : s.advice.items) notices += advice_shown(item) == AdviceShown::Notice;
    if (!notices) return;
    ui::section_label("Worth a look", 8);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * sc, 6 * sc));
    for (const AdviceItem &item : s.advice.items)
        if (advice_shown(item) == AdviceShown::Notice) advice_notice(s, item);
    ImGui::PopStyleVar();
}

// Along the bottom, whatever tab is open: whether it can play, in a word;
// what will be seen, in a sentence; and Play.
void play_bar(Launcher &s, float top, float height) {
    const float sc = s.scale, width = ImGui::GetWindowWidth();
    const ImVec2 origin = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(origin.x, origin.y + top), ImVec2(origin.x + width, origin.y + top),
                                        ui::u32(ui::kLine));
    const float frame = ImGui::GetFrameHeight(), middle = top + height * 0.5f;
    const ImVec2 play(190 * sc, 44 * sc);
    const float play_x = width - kGutter * sc - play.x;
    ImGui::SetCursorPos(ImVec2(play_x, middle - play.y * 0.5f));
    if (ui::primary_button("Play##go", play)) {
        s.action = LauncherAction::Play;
        s.finished = true;
    }
    ImGui::SetItemDefaultFocus();  // the keyboard starts here: Enter plays, the arrows go to the settings

    ImGui::SetCursorPos(ImVec2(kGutter * sc, middle - frame * 0.5f));
    if (!s.advice_valid) {
        ui::row_hint(s.found ? "Looking at the screens..." : "Looking at this PC...");
        return;
    }
    using State = PlaySummary::State;
    ui::pill(s.summary.state == State::Problem   ? ui::Status::Bad
             : s.summary.state == State::Warning ? ui::Status::Warn
                                                 : ui::Status::Ok,
             utf8(s.summary.word).c_str());
    ImGui::SameLine(0, 12 * sc);

    // The sentence, and if there is room after it, how the menu is opened.
    const float text_x = ImGui::GetCursorPosX(), end = play_x - 16 * sc;
    const char *const tip = "opens the menu while playing";
    ImGui::PushFont(nullptr, ui::base_font() * 0.84f);
    const float tip_w = ImGui::CalcTextSize("Tab").x + 30 * sc + ImGui::CalcTextSize(tip).x;
    ImGui::PopFont();
    ImGui::PushFont(nullptr, ui::base_font() * 0.94f);
    const std::string sentence = utf8(s.summary.text);
    const float sentence_w = ImGui::CalcTextSize(sentence.c_str()).x;
    const bool room = text_x + sentence_w + 16 * sc + tip_w <= end;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(ui::fit_text(sentence, end - text_x).c_str());
    ImGui::PopFont();
    if (!room) return;
    ImGui::PushFont(nullptr, ui::base_font() * 0.84f);
    ImGui::SetCursorPos(ImVec2(end - tip_w, middle - frame * 0.5f));
    ui::key_cap("Tab", frame);
    ImGui::SameLine(0, 7 * sc);
    ImGui::AlignTextToFramePadding();
    ui::text(tip, ui::kTextDim);
    ImGui::PopFont();
}

void draw(Launcher &s) {
    const ImGuiIO &io = ImGui::GetIO();
    const float sc = s.scale;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    top_bar(s);

    const float top = std::round(kTopBar * sc) + 1, bar = std::round(kPlayBar * sc);
    const float body = io.DisplaySize.y - top - bar;
    const float result = std::round(kResult * sc), main = io.DisplaySize.x - result;

    // The open tab. It is made to fit; on a screen too small for the window
    // it scrolls and Play stays in view.
    ImGui::SetCursorPos(ImVec2(0, top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kGutter * sc, 6 * sc));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10 * sc, 4 * sc));
    ImGui::BeginChild("tab", ImVec2(main, body), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding);
    switch (s.tab) {
    case kTabPicture: tab_picture(s); break;
    case kTabFrames: tab_frames(s); break;
    case kTabScreen: tab_screen(s); break;
    case kTabSound: tab_sound(s); break;
    case kTabAbout: tab_about(s); break;
    default: tab_play(s); break;
    }
    const float over = ImGui::GetCursorPosY() + ImGui::GetScrollY() - body;
    if (over > 0 && !s.overflow_logged[s.tab]) {
        s.overflow_logged[s.tab] = true;
        log(L"launcher: tab {} is {:.0f} pixels taller than its place (scale {:.2f})", s.tab, over, sc);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);

    ImGui::SetCursorPos(ImVec2(main, top));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::kSurface);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20 * sc, 6 * sc));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * sc, 3 * sc));
    ImGui::BeginChild("result", ImVec2(result, body), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding);
    result_column(s);
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    const ImVec2 origin = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(origin.x + main, origin.y + top), ImVec2(origin.x + main, origin.y + top + body),
                                        ui::u32(ui::kLine));

    play_bar(s, top + body, bar);
    ImGui::End();
}

// What changed goes into the settings file; what did not stays as the user wrote it.
void save(const std::wstring &path, const Settings &now, const Settings &original) {
    const std::vector<SettingValue> v = changed_settings(original, now);
    if (v.empty()) return;
    save_settings(path, v);
    log(L"launcher: {} setting(s) saved to {}", v.size(), path);
}

// What engine\components.ini says is installed, in a line.
std::string versions_line(const std::wstring &engine_dir) {
    const std::wstring ini = engine_dir + L"\\components.ini";
    auto get = [&](const wchar_t *section, const wchar_t *key) {
        wchar_t value[128] = {};
        GetPrivateProfileStringW(section, key, L"", value, 128, ini.c_str());
        return std::wstring(value);
    };
    std::string line;
    auto add = [&](const std::wstring &part) { line += (line.empty() ? "" : kDot) + narrow(part); };
    std::wstring mpv = get(L"mpv", L"version");
    if (!mpv.empty()) add(L"mpv " + mpv.substr(0, mpv.find(L"-git")));
    if (const std::wstring r = get(L"reshade", L"version"); !r.empty()) add(L"ReShade " + r);
    std::wstring renodx = get(L"dlss5", L"renodx");
    if (!renodx.empty()) add(L"DLSS 5 " + renodx.substr(renodx.find_last_of(L'-') + 1));
    std::wstring fg = get(L"dlssfg", L"version");
    if (!fg.empty()) add(L"DLSS Frame Generation " + fg.substr(fg[0] == L'v' ? 1 : 0));
    return line;
}

std::string windows_font(const char *file) {
    char dir[MAX_PATH] = {};
    GetWindowsDirectoryA(dir, MAX_PATH);
    const std::string path = std::string(dir) + "\\Fonts\\" + file;
    return GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES ? std::string() : path;
}

}  // namespace

LauncherAction run_launcher(const std::wstring &settings_path, const std::wstring &engine_dir,
                            const LauncherExtras &extras) {
    BugReport bug;  // joined last, after the window is gone
    Settings settings;
    std::wstring error;
    if (!load_settings(settings_path, settings, error)) log(L"launcher: {}", error);  // shown again on Play

    Window w;
    g_window = &w;
    if (!open_window(w)) {
        g_window = nullptr;
        return LauncherAction::Play;  // no window: play as before
    }
    Survey survey;
    survey.start();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
#ifdef NDEBUG
    io.ConfigDebugHighlightIdConflicts = false;  // a developer's warning, not for players
#endif
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Segoe UI, which every Windows has (not shipped with the app); Dear
    // ImGui's own small font where it is missing.
    const std::string font = windows_font("segoeui.ttf");
    if (font.empty() || !io.Fonts->AddFontFromFileTTF(font.c_str())) io.Fonts->AddFontDefault();
    const std::string bold = windows_font("seguisb.ttf");
    if (!bold.empty()) io.Fonts->AddFontFromFileTTF(bold.c_str());
    ImGui_ImplWin32_Init(w.hwnd);
    ImGui_ImplDX11_Init(w.device.Get(), w.context.Get());

    Launcher launcher{settings, settings};
    launcher.settings_path = settings_path;
    launcher.engine_dir = engine_dir;
    launcher.dlss5_installed = component_installed(engine_dir, Component::Dlss5);
    launcher.dlssfg_installed = component_installed(engine_dir, Component::DlssFg);
    launcher.video_installed = component_installed(engine_dir, Component::Video);
    launcher.anr_installed = component_installed(engine_dir, Component::Anr);
    // The app's folder is the engine folder's parent.
    launcher.input_found = input_program_found(settings, engine_dir.substr(0, engine_dir.find_last_of(L"\\/")));
    launcher.versions = versions_line(engine_dir);
    for (Component c : {Component::Mpv, Component::ReShade, Component::Dlss5, Component::DlssFg, Component::Video,
                        Component::Anr})
        launcher.other_versions =
            launcher.other_versions || (component_installed(engine_dir, c) && !component_as_tested(engine_dir, c));
    launcher.extras = &extras;
    launcher.bug = &bug;
    // env UI_TAB, a test switch: the tab that is open at the start (0 to 5).
    launcher.tab = std::clamp(_wtoi(env(L"UI_TAB").c_str()), 0, int(kTabCount) - 1);
    float style_scale = 0;
    ULONGLONG looked = GetTickCount64();

    while (!launcher.finished && !w.closed) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (w.closed) break;
        if (IsIconic(w.hwnd)) {
            Sleep(30);
            continue;
        }
        if (w.resize_w && w.resize_h) {
            w.rtv.Reset();
            w.swapchain->ResizeBuffers(0, w.resize_w, w.resize_h, DXGI_FORMAT_UNKNOWN, 0);
            w.resize_w = w.resize_h = 0;
            make_target(w);
        }
        if (style_scale != w.scale) {
            ui::apply_style(w.scale, 1.0f, ui::kLauncherFont);
            style_scale = w.scale;
        }
        launcher.scale = w.scale;
        if (survey.cards_done() && !launcher.found) launcher.found = &survey.found();
        // The screens again when they may have changed, at most once a second.
        if (w.look_again && GetTickCount64() - looked > 1000) {
            w.look_again = false;
            looked = GetTickCount64();
            survey.look_again();
        }
        if (survey.take_system(launcher.system.displays, launcher.system.gpus)) {
            launcher.system_known = true;
            launcher.advice_valid = false;
        }
        refresh(launcher);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        draw(launcher);
        ImGui::Render();
        const float clear[4] = {ui::kBg.x, ui::kBg.y, ui::kBg.z, 1.0f};
        ID3D11RenderTargetView *target = w.rtv.Get();
        w.context->OMSetRenderTargets(1, &target, nullptr);
        w.context->ClearRenderTargetView(target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        w.swapchain->Present(1, 0);
    }

    // Kept for Play, and before setup (the window comes back after it).
    if (launcher.finished) save(settings_path, launcher.now, launcher.original);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    w.rtv.Reset();
    w.swapchain.Reset();
    w.context.Reset();
    w.device.Reset();
    DestroyWindow(w.hwnd);
    g_window = nullptr;
    return launcher.finished ? launcher.action : LauncherAction::Quit;
}

}  // namespace app
