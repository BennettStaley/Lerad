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
#include "audio_endpoint.h"
#include "components.h"
#include "devices.h"
#include "display.h"
#include "glasir_version.h"
#include "gpu.h"
#include "hdr.h"
#include "launcher_text.h"
#include "log.h"
#include "settings.h"
#include "system_report.h"
#include "ui_style.h"
#include "update.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using Microsoft::WRL::ComPtr;

namespace glasir {
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

std::string mode_text(UINT32 w, UINT32 h, double fps) { return std::format("{} x {}  \xC2\xB7  {} fps", w, h, fps_text(fps)); }

// The device's modes Glasir can read in this picture mode, largest first.
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

// ---------------------------------------------------------------- the window

struct Window {
    HWND hwnd = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapchain;
    ComPtr<ID3D11RenderTargetView> rtv;
    UINT resize_w = 0, resize_h = 0;
    float dpi_scale = 1;
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
            w->dpi_scale = HIWORD(wparam) / 96.0f;
            const RECT *r = reinterpret_cast<RECT *>(lparam);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
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
    wc.lpszClassName = L"GlasirLauncher";
    RegisterClassExW(&wc);

    // Sized for the monitor the mouse is on, in its DPI. (Where the launcher
    // opens says nothing about where Glasir plays: that is the screen chosen
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
    w.dpi_scale = dpi / 96.0f;
    const int width = int(580 * w.dpi_scale), height = int(760 * w.dpi_scale);
    const RECT &work = mi.rcWork;
    const int x = work.left + (work.right - work.left - width) / 2;
    const int y = work.top + std::max(0L, (work.bottom - work.top - height) / 2);
    w.hwnd = CreateWindowExW(0, wc.lpszClassName, L"Glasir", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             x, y, width, height, nullptr, nullptr, wc.hInstance, nullptr);
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

// ---------------------------------------------------------------- drawing

const ImVec4 kProblem(1.0f, 0.50f, 0.45f, 1.0f);

ImFont *bold_font() {
    ImGuiIO &io = ImGui::GetIO();
    return io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : nullptr;
}

void heading(const char *text) {
    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y * 0.5f));
    ImGui::PushFont(bold_font(), ImGui::GetStyle().FontSizeBase * 0.8f);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::kGold);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Separator();
}

void coloured(const std::string &text, const ImVec4 &colour) {
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

void dim(const std::string &text) { coloured(text, ui::kTextDim); }

// A label in the left column and the widget's start. wide: the column for
// the longer labels of the HDR and frame generation tabs.
void row(const char *label, bool wide = false) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * (wide ? 8.5f : 6.0f));
    ImGui::SetNextItemWidth(-1);
}

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

enum Tab { kTabPlay, kTabHdr, kTabFramegen, kTabSound, kTabCount };

struct Launcher {
    // glasir.ini as the window shows it, and as it was read: what differs is
    // saved when Play is pressed (settings.h, changed_settings).
    Settings now;
    const Settings original;
    std::wstring engine_dir;
    const Found *found = nullptr;  // the cards, once they are read
    // The screens and GPUs, once they are read; display and gpu follow the
    // choices in `now`.
    bool system_known = false;
    SystemInfo system;
    // What `now` comes to on this PC (advice.h), worked out again when a
    // choice or the PC changes. Only with both the cards and the screens.
    bool advice_valid = false;
    Settings advised;
    AdviceInput input;
    Advice advice;
    std::vector<SummaryLine> summary;
    std::vector<MultiplierChoice> multipliers;

    int tab = kTabPlay;
    bool dlss5_installed = false;
    std::string versions;  // "mpv 20261031  ·  ReShade 6.8.0  ·  DLSS 5 10.8.0", "" if none
    const LauncherExtras *extras = nullptr;
    Release release;  // the newer Glasir, once the update check found one
    BugReport *bug = nullptr;
    float body_height = 0;     // what the open tab needs, in pixels
    float footer_height = 0;   // what the part that is always shown needs
    float content_height = 0;  // what the window needs
    LauncherAction action = LauncherAction::Quit;
    bool finished = false;
};

// The capture card `now` names (or the automatic one), as the window lists
// them: one Glasir reads itself, or a name only DirectShow knows.
struct Card {
    const CaptureDevice *automatic = nullptr;
    const CaptureDevice *device = nullptr;
    bool directshow = false;
};

Card find_card(const Launcher &s) {
    Card c;
    if (!s.found) return c;
    ModeRequest request;
    request.hdr = s.now.hdr;
    request.max_auto_height = s.system.display.found ? UINT32(s.system.display.height) : 0;
    c.automatic = choose_device(s.found->devices, L"", request);
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
    s.summary = summary_lines(s.input, s.advice);
    s.multipliers = multiplier_choices(s.input);
    s.advised = s.now;
    s.advice_valid = true;
}

// One piece of advice: its text, in its severity's colour, and "Use" at the
// right when it suggests settings.
void advice_row(Launcher &s, const AdviceItem &item) {
    const ImGuiStyle &style = ImGui::GetStyle();
    const bool use = !item.suggest.empty() && !item.applied;
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float button = use ? ImGui::CalcTextSize("Use").x + style.FramePadding.x * 2 : 0;
    const ImVec4 colour = item.severity == AdviceSeverity::Problem   ? kProblem
                          : item.severity == AdviceSeverity::Warning ? ui::kGold
                          : item.kind == AdviceKind::Recommendation  ? ui::kText
                                                                     : ui::kTextDim;
    ImGui::PushTextWrapPos(use ? right - button - style.ItemSpacing.x : 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextUnformatted(utf8(item.text).c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    if (!use) return;
    ImGui::SameLine(right - button);
    ImGui::PushID(item.id.c_str());
    if (ImGui::SmallButton("Use")) apply_suggestion(s.now, item);
    if (ImGui::IsItemHovered()) {
        std::string sets;
        for (const SettingValue &v : item.suggest) sets += (sets.empty() ? "" : "\n") + utf8(setting_text(v));
        ImGui::SetTooltip("%s", sets.c_str());
    }
    ImGui::PopID();
}

// The limits that are for information, beside the controls they are about.
void advice_here(Launcher &s, AdvicePlace place) {
    if (!s.advice_valid) return;
    for (const AdviceItem &item : s.advice.items)
        if (advice_shown(item) == AdviceShown::Place && advice_place(item.id) == place) advice_row(s, item);
}

// How a capture smaller than the screen is scaled up.
void scaling_row(Launcher &s) {
    Settings &c = s.now;
    const GpuInfo &gpu = s.system.gpu.gpu;
    const std::wstring vsr = s.system_known ? vsr_kind(gpu) : L"none";
    const char *vsr_label = vsr == L"intel" ? "Intel Video Super Resolution" : "NVIDIA RTX Video Super Resolution";
    struct Option {
        Settings::Scaling value;
        const char *label;
    };
    const Option options[] = {
        {Settings::Scaling::Mpv, "mpv (Lanczos)"},
        {Settings::Scaling::Fsr, "AMD FSR 1 (sharper)"},
        {Settings::Scaling::Vsr, vsr_label},
        {Settings::Scaling::Cnn, "ArtCNN (neural, the most detail)"},
        {Settings::Scaling::Anime4k, "Anime4K (cel-shaded and drawn styles)"},
    };
    row("Scaling");
    const char *current = options[0].label;
    for (const Option &o : options)
        if (o.value == c.scaling) current = o.label;
    if (ImGui::BeginCombo("##scaling", current)) {
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
        dim("Not used: the capture is as large as its place on the screen.");
    else if (c.scaling == Settings::Scaling::Vsr && s.system_known && gpu.found && vsr == L"none")
        dim("Needs an NVIDIA RTX or Intel GPU: FSR 1 is used on this one.");
    else if (c.scaling == Settings::Scaling::Vsr)
        dim("Uses the GPU's video engine; FSR 1 if it cannot run.");
    else if (c.scaling == Settings::Scaling::Cnn || c.scaling == Settings::Scaling::Anime4k)
        dim("Takes more GPU time, which DLSS 5 and frame generation also need.");
}

// The screen Glasir plays on and the GPU mpv draws on.
void screen_rows(Launcher &s) {
    heading("SCREEN");
    if (!s.system_known) {
        dim("Looking at the screens...");
        return;
    }
    Settings &c = s.now;
    const SystemInfo &sys = s.system;
    const DisplayInfo &d = sys.display;
    if (!d.found) {
        dim("No screen could be read: Glasir plays on the main one.");
    } else if (sys.displays.size() > 1) {
        row("Screen");
        if (ImGui::BeginCombo("##screen", utf8(screen_title(d)).c_str(), ImGuiComboFlags_HeightLarge)) {
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
        dim(utf8(screen_details(d)));
    } else {
        row("Screen");
        ImGui::TextUnformatted(utf8(screen_title(d)).c_str());
        dim(utf8(screen_details(d)));
    }

    // The GPU: a choice only on a PC with more than one.
    const GpuChoice &gpu = sys.gpu;
    if (sys.gpus.size() > 1) {
        row("GPU");
        const std::string own = "The screen's own" +
                                (gpu.screen_gpu.found ? " (" + utf8(gpu.screen_gpu.name) + ")" : std::string());
        const std::string current = c.gpu.empty() || !gpu.gpu.found ? own : utf8(gpu.gpu.name);
        if (ImGui::BeginCombo("##gpu", current.c_str())) {
            if (ImGui::Selectable(own.c_str(), c.gpu.empty())) c.gpu.clear();
            int id = 0;
            for (const GpuInfo &g : sys.gpus) {
                ImGui::PushID(id++);
                if (ImGui::Selectable(utf8(gpu_title(g)).c_str(), !c.gpu.empty() && gpu.gpu.found && g.name == gpu.gpu.name))
                    c.gpu = g.name;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    } else if (gpu.gpu.found) {
        row("GPU");
        ImGui::TextUnformatted(utf8(gpu_title(gpu.gpu)).c_str());
    }
    advice_here(s, AdvicePlace::Screen);
}

void tab_play(Launcher &s) {
    Settings &c = s.now;
    const Found *f = s.found;
    heading("CAPTURE");
    if (!f) {
        dim("Looking for capture cards...");
    } else {
        const Card card = find_card(s);
        const CaptureDevice *device = card.device;
        row("Card");
        const std::string auto_label = card.automatic ? "Automatic (" + utf8(card.automatic->name) + ")"
                                                      : std::string("Automatic (none found yet)");
        const std::string current =
            c.device.empty() ? auto_label : utf8(c.device) + (card.directshow ? "  (DirectShow)" : "");
        if (ImGui::BeginCombo("##card", current.c_str())) {
            if (ImGui::Selectable(auto_label.c_str(), c.device.empty())) c.device.clear();
            // Items are told apart by their place, not their text: two cards
            // can share a name, and ImGui warns about matching IDs.
            int id = 0;
            for (const auto &d : f->devices) {
                ImGui::PushID(id++);
                if (ImGui::Selectable(utf8(d.name).c_str(), names_match(d.name, c.device))) c.device = d.name;
                ImGui::PopID();
            }
            for (const auto &n : f->directshow_only) {
                ImGui::PushID(id++);
                if (ImGui::Selectable((utf8(n) + "  (DirectShow)").c_str(), names_match(n, c.device))) c.device = n;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (f->devices.empty() && f->directshow_only.empty()) dim("No capture card found. Plug it in and start Glasir again.");

        // HDR only where the card has a 10-bit mode (a choice already made
        // stays, so it can be seen and changed).
        const bool no_hdr = device && modes_for(device, true).empty();
        row("Picture");
        if (ImGui::RadioButton("SDR", !c.hdr)) c.hdr = false;
        ImGui::SameLine();
        if (no_hdr && !c.hdr) ImGui::BeginDisabled();
        if (ImGui::RadioButton("HDR", c.hdr)) c.hdr = true;
        if (no_hdr && !c.hdr) ImGui::EndDisabled();
        dim(no_hdr   ? "HDR needs a card with a 10-bit mode: this one has none."
            : c.hdr  ? "HDR needs HDR on the console. Only the card's HDR (10-bit) modes are listed."
                     : "Modes marked HDR can also be played in HDR.");

        const auto modes = modes_for(device, c.hdr);
        bool mode_ok = c.width == 0;
        for (const auto &m : modes)
            mode_ok = mode_ok || (m.width == c.width && m.height == c.height && std::abs(m.fps - c.fps) < 0.01);
        if (!mode_ok && device) c.width = c.height = 0, c.fps = 0;  // not offered here: back to automatic
        // What automatic picks: for the screen Glasir plays on.
        ModeRequest request;
        request.hdr = c.hdr;
        request.max_auto_height = s.system.display.found ? UINT32(s.system.display.height) : 0;
        const auto picked = device ? choose_mode(device->modes, request) : std::nullopt;
        const std::string auto_mode =
            picked ? "Automatic (" + mode_text(picked->width, picked->height, picked->fps) + ")" : std::string("Automatic");
        row("Mode");
        const std::string mode_label = c.width ? mode_text(c.width, c.height, c.fps) : auto_mode;
        if (ImGui::BeginCombo("##mode", mode_label.c_str())) {
            const bool auto_hdr = !c.hdr && picked && has_hdr(device, picked->width, picked->height, picked->fps);
            if (mode_item(auto_mode, c.width == 0, auto_hdr)) c.width = c.height = 0, c.fps = 0;
            int id = 0;
            for (const auto &m : modes) {
                const bool on = m.width == c.width && m.height == c.height && std::abs(m.fps - c.fps) < 0.01;
                ImGui::PushID(id++);
                if (mode_item(mode_text(m.width, m.height, m.fps), on, !c.hdr && has_hdr(device, m.width, m.height, m.fps))) {
                    c.width = m.width;
                    c.height = m.height;
                    c.fps = m.fps;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        advice_here(s, AdvicePlace::Capture);
    }

    screen_rows(s);

    heading("ENHANCEMENTS");
    // DLSS 5 is judged for the GPU mpv draws on.
    const GpuInfo &gpu = s.system.gpu.gpu;
    const bool nr_gpu = s.system_known && dlss5_gpu(gpu);
    const bool forced = s.original.dlss5 == Settings::Dlss5::On;  // glasir.ini: try on any GPU
    const bool can = s.dlss5_installed && (nr_gpu || forced);
    if (!can) ImGui::BeginDisabled();
    bool dlss5 = c.dlss5 != Settings::Dlss5::Off && can;
    if (ImGui::Checkbox("DLSS 5 neural rendering", &dlss5))
        // Back on: as glasir.ini had it, else automatic (on: for a GPU that is only tried).
        c.dlss5 = !dlss5 ? Settings::Dlss5::Off
                  : s.original.dlss5 != Settings::Dlss5::Off ? s.original.dlss5
                  : nr_gpu ? Settings::Dlss5::Auto : Settings::Dlss5::On;
    if (!can) ImGui::EndDisabled();
    if (s.system_known && !nr_gpu && !forced)
        dim("Needs an NVIDIA RTX 30, 40 or 50 series GPU" +
            (gpu.found ? " (this PC: " + utf8(gpu.name) + ")." : std::string(".")));
    else if (s.system_known && !s.dlss5_installed) {
        dim("Not downloaded yet (about 250 MB).");
        if (ImGui::Button("Download DLSS 5")) {
            s.action = LauncherAction::Setup;
            s.finished = true;
        }
    }
    ImGui::Checkbox("Frame generation", &c.framegen);
    if (c.framegen) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        dim(std::format("up to x{}, {} quality", c.framegen_multiplier, c.framegen_fast ? "fast" : "high"));
    }
    scaling_row(s);
    advice_here(s, AdvicePlace::Enhancements);
}

void tab_hdr(Launcher &s) {
    Settings &c = s.now;
    heading("AN HDR CAPTURE");
    if (!c.hdr) dim("The capture is SDR (Picture, on the Play tab). These apply to an HDR capture.");
    if (!c.hdr) ImGui::BeginDisabled();
    const HdrPlan plan = hdr_plan(c, s.system.display);
    const bool to_sdr = plan.output == HdrPlan::Output::Sdr;

    row("Send it as", true);
    const char *const outputs[] = {"Automatic (HDR when the screen has it on)", "Always HDR", "Always SDR (tone mapped)"};
    const Settings::HdrOutput values[] = {Settings::HdrOutput::Auto, Settings::HdrOutput::Hdr, Settings::HdrOutput::Sdr};
    int current = c.hdr_output == Settings::HdrOutput::Hdr ? 1 : c.hdr_output == Settings::HdrOutput::Sdr ? 2 : 0;
    if (ImGui::BeginCombo("##hdr_output", outputs[current])) {
        for (int i = 0; i < 3; ++i)
            if (ImGui::Selectable(outputs[i], i == current)) c.hdr_output = values[i];
        ImGui::EndCombo();
    }
    if (c.hdr && s.system_known) dim("Now: " + utf8(hdr_plan_text(plan)) + ".");

    // Fitting: only for HDR output; SDR output is always tone mapped.
    if (to_sdr) ImGui::BeginDisabled();
    ImGui::Checkbox("Fit highlights to this screen", &c.hdr_fit_to_screen);
    if (to_sdr) ImGui::EndDisabled();
    if (c.hdr && to_sdr)
        dim("Not used: the picture is tone mapped to SDR, which fits it already.");
    else if (!c.hdr_fit_to_screen)
        dim("Off: the picture goes out as the console sends it; its own HDR calibration fits it to the screen. On: "
            "Glasir brings highlights down to the screen's peak, for a console that cannot be calibrated to it.");
    const bool fitting = c.hdr_fit_to_screen && !to_sdr;
    if (!fitting) ImGui::BeginDisabled();
    row("Screen peak", true);
    const int reported = s.system.display.peak_nits >= 80 ? int(std::lround(s.system.display.peak_nits)) : 0;
    const std::string as_reported =
        reported ? std::format("What the screen reports ({} nits)", reported) : std::string("What the screen reports (nothing)");
    const std::string peak_label = c.display_peak_nits > 0 ? std::format("{} nits", c.display_peak_nits) : as_reported;
    if (ImGui::BeginCombo("##peak", peak_label.c_str())) {
        if (ImGui::Selectable(as_reported.c_str(), c.display_peak_nits == 0)) c.display_peak_nits = 0;
        for (int nits : {400, 500, 600, 800, 1000, 1200, 1500, 2000, 3000, 4000}) {
            const std::string label = std::format("{} nits", nits);
            if (ImGui::Selectable(label.c_str(), c.display_peak_nits == nits)) c.display_peak_nits = nits;
        }
        ImGui::EndCombo();
    }
    if (fitting && c.display_peak_nits == 0)
        dim(reported ? "Screens often report less than they reach: pick the real peak if you know it."
                     : "This screen reports no peak: pick one, or nothing can be fitted.");
    if (!fitting) ImGui::EndDisabled();

    // The curve and the per-frame peak: for fitting and for SDR output.
    const bool mapping = fitting || to_sdr;
    if (!mapping) ImGui::BeginDisabled();
    row("Curve", true);
    const auto curve_label = [&](const std::wstring &name) {
        if (name != L"auto") return utf8(name);
        return std::string(to_sdr ? "Automatic (mpv's spline)" : "Automatic (BT.2390)");
    };
    if (ImGui::BeginCombo("##curve", curve_label(c.tone_mapping).c_str())) {
        for (const std::wstring &name : tone_mapping_names())
            if (ImGui::Selectable(curve_label(name).c_str(), name == c.tone_mapping)) c.tone_mapping = name;
        ImGui::EndCombo();
    }
    ImGui::Checkbox("Measure each frame's peak", &c.hdr_dynamic_peak);
    if (mapping)
        dim("Measuring fits games that never reach their peak better, for some GPU time. Off: the console's peak "
            "below is where the curve starts.");
    if (!mapping) ImGui::EndDisabled();

    row("Console's peak", true);
    const std::string source_label = std::format("{} nits", c.hdr_peak_nits);
    if (ImGui::BeginCombo("##source_peak", source_label.c_str())) {
        bool listed = false;
        for (int nits : {400, 600, 800, 1000, 1500, 2000, 4000, 10000}) {
            listed = listed || nits == c.hdr_peak_nits;
            const std::string label = std::format("{} nits", nits);
            if (ImGui::Selectable(label.c_str(), c.hdr_peak_nits == nits)) c.hdr_peak_nits = nits;
        }
        if (!listed) ImGui::Selectable(source_label.c_str(), true);  // glasir.ini's own value stays
        ImGui::EndCombo();
    }
    dim("The brightest the console's picture gets: what its HDR calibration is set to. It goes into the HDR "
        "metadata sent to the screen.");
    if (!c.hdr) ImGui::EndDisabled();
    advice_here(s, AdvicePlace::Hdr);

    heading("AN SDR CAPTURE");
    if (c.hdr) ImGui::BeginDisabled();
    row("Range", true);
    if (ImGui::RadioButton("Limited (most consoles)", !c.sdr_full_range)) c.sdr_full_range = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Full", c.sdr_full_range)) c.sdr_full_range = true;
    if (c.hdr) ImGui::EndDisabled();
    dim(c.hdr ? "Not used: the capture is HDR."
              : "As the console's RGB range is set. Wrong: blacks look grey, or dark detail is crushed.");
}

void tab_framegen(Launcher &s) {
    Settings &c = s.now;
    heading("FRAME GENERATION");
    ImGui::Checkbox("Add frames between the game's", &c.framegen);
    const Outcome &o = s.advice.outcome;
    if (c.framegen && s.advice_valid && !o.framegen)
        coloured("It cannot run as things are: the notes above Play say why.", ui::kGold);
    if (!c.framegen) ImGui::BeginDisabled();

    row("Quality", true);
    if (ImGui::RadioButton("High", !c.framegen_fast)) c.framegen_fast = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Fast (about half the GPU time)", c.framegen_fast)) c.framegen_fast = true;

    row("Method", true);
    if (ImGui::RadioButton("Motion", !c.framegen_blend)) c.framegen_blend = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Blend", c.framegen_blend)) c.framegen_blend = true;
    if (c.framegen_blend) dim("Blend is a plain mix of the two frames: the lightest, but moving things show double.");

    // Up to x2, x3 or x4: a choice not usable on this screen is greyed out
    // and says why (one already chosen stays, with what it comes to).
    row("Up to", true);
    const MultiplierChoice *chosen = nullptr;
    std::string unusable;
    for (int k = 2; k <= 4; ++k) {
        const MultiplierChoice *m = nullptr;
        for (const MultiplierChoice &choice : s.multipliers)
            if (choice.multiplier == k) m = &choice;
        const bool on = c.framegen_multiplier == k;
        const bool off = m && !m->usable && !on;
        if (k > 2) ImGui::SameLine();
        if (off) ImGui::BeginDisabled();
        if (ImGui::RadioButton(std::format("x{}", k).c_str(), on)) c.framegen_multiplier = k;
        if (off) ImGui::EndDisabled();
        if (on) chosen = m;
        if (m && !m->usable && !m->why.empty()) unusable += std::format("{}x{}: {}", unusable.empty() ? "" : " ", k, utf8(m->why));
    }
    if (c.framegen && chosen && !chosen->text.empty()) {
        const int added = c.framegen_multiplier - 1;
        std::string text = "On this screen: " + utf8(chosen->text) + " fps.";
        if (chosen->usable)
            text += std::format(" {} added frame{} per game frame, each a full pass on the GPU; a 30 fps game's own "
                                "frame is shown {:.0f} ms later than with frame generation off.",
                                added, added == 1 ? "" : "s", chosen->delay_ms);
        dim(text);
    }
    if (c.framegen && !unusable.empty()) dim(unusable);

    row("At most", true);
    const double refresh = s.system.display.found ? s.system.display.refresh_hz : 0;
    const std::string screen_rate = refresh > 0 ? std::format("The screen's rate ({:.0f} Hz)", refresh)
                                                : std::string("The screen's rate");
    const std::string cap_label = c.framegen_max_hz > 0 ? std::format("{} frames a second", c.framegen_max_hz) : screen_rate;
    if (ImGui::BeginCombo("##cap", cap_label.c_str())) {
        if (ImGui::Selectable(screen_rate.c_str(), c.framegen_max_hz == 0)) c.framegen_max_hz = 0;
        bool listed = c.framegen_max_hz == 0;
        for (int hz : {48, 60, 72, 90, 100, 120, 144, 165, 200, 240, 360}) {
            if (refresh > 0 && hz >= std::round(refresh)) break;  // not below the screen's rate: it would change nothing
            listed = listed || hz == c.framegen_max_hz;
            const std::string label = std::format("{} frames a second", hz);
            if (ImGui::Selectable(label.c_str(), c.framegen_max_hz == hz)) c.framegen_max_hz = hz;
        }
        if (!listed) ImGui::Selectable(cap_label.c_str(), true);  // glasir.ini's own value stays
        ImGui::EndCombo();
    }
    dim("Frames are added only while the result stays at or under this. With variable refresh, a little under the "
        "screen's rate keeps it inside the range.");
    if (!c.framegen) ImGui::EndDisabled();
    ImGui::Checkbox("Show the frame rates in a corner while playing", &c.framegen_show_rates);
    advice_here(s, AdvicePlace::Framegen);

    // Variable refresh: what was found, and the user's word when Glasir
    // cannot read it (AMD and Intel GPUs) or reads it wrong.
    heading("THE SCREEN'S VARIABLE REFRESH");
    row("VRR is", true);
    const std::string found = s.system.display.found ? utf8(screen_vrr_text(s.system.display)) : std::string("not known");
    const std::string as_found = "As found (" + found + ")";
    const char *const labels[] = {as_found.c_str(), "On", "Off"};
    const Settings::Vrr values[] = {Settings::Vrr::Auto, Settings::Vrr::On, Settings::Vrr::Off};
    const int current = c.vrr == Settings::Vrr::On ? 1 : c.vrr == Settings::Vrr::Off ? 2 : 0;
    if (ImGui::BeginCombo("##vrr", labels[current])) {
        for (int i = 0; i < 3; ++i)
            if (ImGui::Selectable(labels[i], i == current)) c.vrr = values[i];
        ImGui::EndCombo();
    }
    dim("G-SYNC, FreeSync or HDMI VRR on the PC's screen: frames are then shown the moment they are due. It is "
        "switched in the GPU's control panel and the monitor's menu, not here; this only tells Glasir.");
}

void tab_sound(Launcher &s) {
    Settings &c = s.now;
    const Found *f = s.found;
    heading("SOUND");
    ImGui::Checkbox("Play the card's sound", &c.audio);
    if (c.audio && f) {
        row("Input");
        const std::string own = "The card's own (automatic)";
        const std::string current = c.audio_device.empty() ? own : utf8(c.audio_device);
        if (ImGui::BeginCombo("##audio", current.c_str())) {
            if (ImGui::Selectable(own.c_str(), c.audio_device.empty())) c.audio_device.clear();
            int id = 0;
            for (const auto &a : f->audio) {
                ImGui::PushID(id++);
                if (ImGui::Selectable(utf8(a.name).c_str(), a.name == c.audio_device)) c.audio_device = a.name;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }

    heading("WINDOW");
    row("Show as");
    const char *shown = c.fullscreen ? "Full screen" : c.borderless ? "Borderless (fills the screen)" : "A window";
    if (ImGui::BeginCombo("##window", shown)) {
        if (ImGui::Selectable("Full screen", c.fullscreen)) c.borderless = c.fullscreen = true;
        if (ImGui::Selectable("Borderless (fills the screen)", c.borderless && !c.fullscreen))
            c.borderless = true, c.fullscreen = false;
        if (ImGui::Selectable("A window", !c.borderless)) c.borderless = c.fullscreen = false;
        ImGui::EndCombo();
    }
    if (c.fullscreen) dim("mpv's own full screen, as f or a double-click switches it.");
    if (c.borderless && !c.fullscreen) {
        ImGui::Checkbox("Skip the black bars", &c.skip_black_bars);
        dim(c.skip_black_bars
                ? "For a picture of another shape than the screen (16:9 on an ultrawide): DLSS 5 and frame "
                  "generation work on the picture only. On some PCs Windows then composes the window and "
                  "frame generation falls short: check the frame rates in the menu (Tab)."
                : "For a picture of another shape than the screen (16:9 on an ultrawide): DLSS 5 and frame "
                  "generation skip the bars, a lighter load.");
    }
    ImGui::Checkbox("Show this window when Glasir starts", &c.show_launcher);
    if (!c.show_launcher) dim("Glasir then plays at once. Hold Shift while starting it to see this window.");
}

// The tabs: a row of plain buttons, the open one in gold with a line under it.
void tab_bar(Launcher &s) {
    // "##tab": the first has the same word on it as the Play button below.
    const char *const names[kTabCount] = {"Play##tab", "HDR##tab", "Frame generation##tab", "Sound and window##tab"};
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    for (int i = 0; i < kTabCount; ++i) {
        if (i) ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 0.4f);
        const bool on = s.tab == i;
        ImGui::PushStyleColor(ImGuiCol_Text, on ? ui::kGold : ui::kTextDim);
        if (ImGui::Button(names[i])) s.tab = i;
        ImGui::PopStyleColor();
        if (on) {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x + 4, b.y), ImVec2(b.x - 4, b.y),
                                                ImGui::GetColorU32(ui::kGold), 2.0f);
        }
    }
    ImGui::PopStyleColor();
}

// What is always shown, whatever tab is open: what the choices come to, what
// to set on the console, what needs attention, and Play.
void footer(Launcher &s) {
    heading("WHAT YOU WILL GET");
    if (!s.advice_valid) {
        dim(s.found ? "Looking at the screens..." : "Looking at this PC...");
    } else {
        for (const SummaryLine &line : s.summary) coloured(utf8(line.text), line.matters ? ui::kGold : ui::kText);
        const std::string console = utf8(console_line(s.advice));
        if (!console.empty()) {
            heading("SET YOUR CONSOLE TO");
            coloured(console, ui::kText);
            const std::string hint = utf8(console_hint(s.advice));
            if (!hint.empty()) dim(hint);
        }
        // Problems, warnings and what is recommended: a few lines at most,
        // the rest by scrolling, so Play stays where it is.
        int notices = 0;
        for (const AdviceItem &item : s.advice.items) notices += advice_shown(item) == AdviceShown::Notice;
        if (notices) {
            heading("WORTH A LOOK");
            const float line = ImGui::GetTextLineHeightWithSpacing();
            ImGui::SetNextWindowSizeConstraints(ImVec2(0, line), ImVec2(FLT_MAX, line * 6.5f));
            ImGui::BeginChild("notices", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened);
            for (const AdviceItem &item : s.advice.items)
                if (advice_shown(item) == AdviceShown::Notice) advice_row(s, item);
            ImGui::EndChild();
        }
    }

    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y * 0.5f));
    const float button_h = ImGui::GetFrameHeight() * 1.8f;
    ImGui::PushStyleColor(ImGuiCol_Button, ui::kGold);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.80f, 0.36f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.75f, 0.58f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.07f, 0.05f, 1.0f));
    ImGui::PushFont(bold_font(), ImGui::GetStyle().FontSizeBase * 1.15f);
    if (ImGui::Button("Play", ImVec2(-1, button_h))) {
        s.action = LauncherAction::Play;
        s.finished = true;
    }
    ImGui::SetItemDefaultFocus();  // the keyboard starts here: Enter plays, the arrows go to the settings
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    dim("While playing, Tab opens the menu.");
    ImGui::AlignTextToFramePadding();
    dim(s.versions.empty() ? "mpv and ReShade are downloaded when you press Play." : s.versions);
    if (!s.versions.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Update")) {
            s.action = LauncherAction::Setup;
            s.finished = true;
        }
    }
    if (s.extras && s.extras->update && s.extras->update->state() == UpdateCheck::State::Available) {
        if (s.release.tag.empty()) s.release = s.extras->update->release();
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kGold);
        ImGui::TextUnformatted(("Glasir " + narrow(version_text(s.release.version)) + " is out.").c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("Update Glasir")) {
            s.action = LauncherAction::SelfUpdate;
            s.finished = true;
        }
        if (!s.release.page.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("What's new"))
                ShellExecuteW(nullptr, L"open", s.release.page.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    if (s.extras && s.extras->bug_report && s.bug) {
        const int state = s.bug->state.load();
        ImGui::AlignTextToFramePadding();
        if (state == BugReport::Saving) {
            dim("Saving a bug report...");
        } else {
            if (ImGui::SmallButton("Save a bug report")) s.bug->start(s.extras->bug_report);
            ImGui::SameLine();
            if (state == BugReport::Saved)
                dim("Saved in \"Bug reports\" (logs, settings and the PC; no user name).");
            else if (state == BugReport::Failed)
                dim("Could not save it: " + narrow(s.bug->error));
            else
                dim("F9 while playing adds the frames on screen.");
        }
    }
}

void draw(Launcher &s) {
    const ImGuiIO &io = ImGui::GetIO();
    const ImGuiStyle &style = ImGui::GetStyle();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::PushFont(bold_font(), style.FontSizeBase * 1.8f);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::kGold);
    ImGui::TextUnformatted("Glasir");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    dim(std::string("  ") + GLASIR_VERSION + "  \xC2\xB7  Modern PC graphics for your console, through a capture card.");
    tab_bar(s);

    // The open tab takes what the window has left above the footer; on a
    // screen too small for all of it, the tab scrolls and Play stays in view.
    const float top = ImGui::GetCursorPosY();
    const float available = ImGui::GetContentRegionAvail().y - s.footer_height - style.ItemSpacing.y;
    const float height = std::max(available, ImGui::GetFrameHeight() * 4);
    ImGui::BeginChild("tab", ImVec2(0, height), ImGuiChildFlags_NavFlattened,
                      s.body_height <= height + 2 ? ImGuiWindowFlags_NoScrollbar : 0);
    switch (s.tab) {
    case kTabHdr: tab_hdr(s); break;
    case kTabFramegen: tab_framegen(s); break;
    case kTabSound: tab_sound(s); break;
    default: tab_play(s); break;
    }
    s.body_height = ImGui::GetCursorPosY();
    ImGui::EndChild();

    const float footer_top = ImGui::GetCursorPosY();
    footer(s);
    s.footer_height = ImGui::GetCursorPosY() - footer_top;
    s.content_height = top + s.body_height + style.ItemSpacing.y + s.footer_height + style.WindowPadding.y;
    ImGui::End();
}

// What changed goes into glasir.ini; what did not stays as the user wrote it.
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
    std::wstring line;
    auto add = [&](const std::wstring &part) { line += (line.empty() ? L"" : L"  ·  ") + part; };
    std::wstring mpv = get(L"mpv", L"version");
    if (!mpv.empty()) add(L"mpv " + mpv.substr(0, mpv.find(L"-git")));
    if (const std::wstring r = get(L"reshade", L"version"); !r.empty()) add(L"ReShade " + r);
    std::wstring renodx = get(L"dlss5", L"renodx");
    if (!renodx.empty()) add(L"DLSS 5 " + renodx.substr(renodx.find_last_of(L'-') + 1));
    return narrow(line);
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
    const std::string font = windows_font("segoeui.ttf");
    if (font.empty() || !io.Fonts->AddFontFromFileTTF(font.c_str())) io.Fonts->AddFontDefault();
    const std::string bold = windows_font("seguisb.ttf");
    if (!bold.empty()) io.Fonts->AddFontFromFileTTF(bold.c_str());
    ImGui_ImplWin32_Init(w.hwnd);
    ImGui_ImplDX11_Init(w.device.Get(), w.context.Get());

    Launcher launcher{settings, settings};
    launcher.engine_dir = engine_dir;
    launcher.dlss5_installed = component_installed(engine_dir, Component::Dlss5);
    launcher.versions = versions_line(engine_dir);
    launcher.extras = &extras;
    launcher.bug = &bug;
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
        if (style_scale != w.dpi_scale) {
            ui::apply_style(w.dpi_scale * 0.9f, 1.0f);
            style_scale = w.dpi_scale;
        }
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
        // The window takes the height its contents need (up to the screen's).
        RECT client{};
        GetClientRect(w.hwnd, &client);
        const int want = int(std::ceil(launcher.content_height));
        if (want > 0 && std::abs(want - int(client.bottom)) > 2) {
            RECT r{0, 0, client.right, want};
            const UINT dpi = GetDpiForWindow(w.hwnd);
            AdjustWindowRectExForDpi(&r, DWORD(GetWindowLongPtrW(w.hwnd, GWL_STYLE)), FALSE, 0, dpi);
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(w.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            RECT now{};
            GetWindowRect(w.hwnd, &now);
            const int height = std::min(int(r.bottom - r.top), int(mi.rcWork.bottom - mi.rcWork.top));
            const int top = std::max(mi.rcWork.top, std::min(now.top, mi.rcWork.bottom - height));
            if (height != now.bottom - now.top || top != now.top)
                SetWindowPos(w.hwnd, nullptr, now.left, top, r.right - r.left, height, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        const float clear[4] = {0.06f, 0.06f, 0.07f, 1.0f};
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

}  // namespace glasir
