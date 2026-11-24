// SPDX-License-Identifier: MIT
#include "overlay.h"

#include <windows.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include "app_env.h"
#include "compositor.h"
#include "menu.h"
#include "mpv_client.h"
#include "screenshot.h"
#include "stream_out.h"
#include "view_shifter.h"

using Microsoft::WRL::ComPtr;

namespace app::overlay {
namespace {

constexpr UINT kOpenKey = VK_TAB;
constexpr float kPaperNits = 203.0f;  // BT.2408's reference white: the menu in HDR when the app names no other
constexpr double kToastFade = 0.25;    // seconds a toast takes to fade out

struct Overlay {
    Host host;
    ID3D11Device *device = nullptr;  // mpv's, not held: release() comes before it goes
    ComPtr<ID3D11DeviceContext> context;
    ImGuiContext *imgui = nullptr;
    bool backend = false;
    Compositor compositor;
    MpvClient mpv;
    bool failed = false;
    std::atomic<bool> open{false};
    // mpv's effect runtime at this ReShade frame, while the menu is open
    // (for draw_again, on the same thread before the next frame).
    reshade::api::effect_runtime *runtime = nullptr;
    Encoding encoding = Encoding::Sdr;
    std::atomic<reshade::api::color_space> color_space{reshade::api::color_space::unknown};
    LARGE_INTEGER last{};
    UINT style_height = 0;
    std::wstring settings_path;
    bool source_hdr = false;
    bool source_auto = false;
    bool hdr_available = true;
    bool borderless = true;
    // The menu's and the toasts' white in HDR (env PAPER_WHITE_NITS:
    // the settings file's, else Windows' "SDR content brightness" for the screen),
    // so that they are as bright as the desktop's own windows.
    float paper_nits = kPaperNits;
    // What the app does with an HDR capture (its hdr.h), for the menu.
    std::string hdr_output, hdr_why;
    int hdr_peak_nits = 0;
    bool hdr_dynamic_peak = false;
    bool show_rates = false;  // the frame rate readout in the corner
    bool input_on = false;        // the app runs the input program (env INPUT)
    int mouse_sensitivity = 30;   // the settings file's input.sensitivity
    int mouse_aim = 100;          // and input.aim_sensitivity, percent
    bool mouse_forward = true;    // input.mouse_forward
    int mouse_lead = 140;         // input.lead_ms
    // The screen the exe plays on and its variable refresh, as told.
    std::string display_name, display_vrr;
    bool display_vrr_set = false;
    double display_hz = 0;
    std::string demo;  // env MENU_DEMO, a test switch (menu.h)
    // Frames the menu was drawn on since it opened: mpv's presents, and the
    // real frames frame generation presents itself.
    unsigned on_mpv = 0, on_real = 0;
    bool mpv_started = false;
    double closed_at = -100;  // when the menu last closed (its own changes still arrive then)
    // The toast showing (render thread), and toasts from this add-on.
    std::string toast_text;
    double toast_start = 0, toast_until = 0;
    unsigned mpv_toast_seen = 0;
    std::mutex local_m;
    std::string local_text;
    double local_seconds = 1.5;
    unsigned local_serial = 0, local_seen = 0;
    // Screenshots: the one being read back, and the requests seen.
    shot::Grabber shots;
    StreamOut stream;  // the stream window's frames (stream_out.h)
    ViewShifter shifter;  // the picture moved ahead of the console's answer (view_shift.h)
    unsigned shots_seen = 0;
};

Overlay g;

double now_seconds() {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return double(now.QuadPart) / double(freq.QuadPart);
}

double seconds_since(LARGE_INTEGER &last) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const double dt = last.QuadPart ? double(now.QuadPart - last.QuadPart) / double(freq.QuadPart) : 1.0 / 60.0;
    last = now;
    return dt > 0 ? dt : 1.0 / 60.0;
}

std::string windows_font(const char *file) {
    char dir[MAX_PATH] = {};
    GetWindowsDirectoryA(dir, MAX_PATH);
    const std::string path = std::string(dir) + "\\Fonts\\" + file;
    return GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES ? std::string() : path;
}

// An environment variable the app set before it started mpv; "" if unset.
std::string environment(const wchar_t *name) {
    std::string out;
    for (const wchar_t c : env(name)) out.push_back(c < 128 ? char(c) : '?');  // the app's own plain text
    return out;
}

bool init(ID3D11Device *device) {
    g.device = device;
    device->GetImmediateContext(&g.context);
    if (!g.compositor.init(device)) return false;
    IMGUI_CHECKVERSION();
    g.imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(g.imgui);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;  // nothing written into mpv's folder
    io.LogFilename = nullptr;
#ifdef NDEBUG
    io.ConfigDebugHighlightIdConflicts = false;  // a developer's warning, not for players
#endif
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Segoe UI, which every Windows has (not shipped with the app); Dear
    // ImGui's own small font where it is missing. The sizes are the style's
    // (ui_style.h): Dear ImGui makes each size of a font when it is first drawn.
    const std::string font = windows_font("segoeui.ttf");
    if (font.empty() || !io.Fonts->AddFontFromFileTTF(font.c_str())) io.Fonts->AddFontDefault();
    const std::string bold = windows_font("seguisb.ttf");
    if (!bold.empty()) io.Fonts->AddFontFromFileTTF(bold.c_str());
    g.backend = ImGui_ImplDX11_Init(device, g.context.Get());
    if (!g.backend) return false;

    g.settings_path = env(L"SETTINGS");
    g.source_hdr = _wcsicmp(env(L"VIDEO_MODE").c_str(), L"hdr") == 0;
    g.source_auto = env(L"VIDEO_AUTO") == L"1";
    // Unset (mpv started some other way): offered, as before.
    g.hdr_available = env(L"HDR_AVAILABLE") != L"0";
    g.borderless = _wcsicmp(env(L"WINDOW_MODE").c_str(), L"windowed") != 0;
    // The readout in the corner: the settings file's own value (the menu keeps it there).
    if (!g.settings_path.empty())
        g.show_rates = GetPrivateProfileIntW(L"framegen", L"show_rates", 0, g.settings_path.c_str()) != 0;
    // The mouse, when the app runs the input program: the settings file's
    // values, which the menu changes there.
    g.input_on = env(L"INPUT") == L"1";
    g.shifter.set_name(env(L"VIEW_SHIFT"));
    if (!g.settings_path.empty()) {
        g.mouse_sensitivity = int(GetPrivateProfileIntW(L"input", L"sensitivity", 30, g.settings_path.c_str()));
        g.mouse_aim = int(GetPrivateProfileIntW(L"input", L"aim_sensitivity", 100, g.settings_path.c_str()));
        g.mouse_forward = GetPrivateProfileIntW(L"input", L"mouse_forward", 1, g.settings_path.c_str()) != 0;
        g.mouse_lead = int(GetPrivateProfileIntW(L"input", L"lead_ms", 140, g.settings_path.c_str()));
    }
    const double paper = std::atof(environment(L"PAPER_WHITE_NITS").c_str());
    g.paper_nits = paper >= 80 && paper <= 1000 ? float(paper) : kPaperNits;
    g.hdr_output = environment(L"HDR_OUTPUT");
    if (const int brightness = std::atoi(environment(L"STREAM_BRIGHTNESS").c_str())) g.stream.set_brightness(brightness);
    g.hdr_why = environment(L"HDR_WHY");
    g.hdr_peak_nits = std::atoi(environment(L"HDR_PEAK_NITS").c_str());
    g.hdr_dynamic_peak = environment(L"HDR_DYNAMIC_PEAK") == "1";
    g.display_name = environment(L"DISPLAY_NAME");
    g.display_vrr = environment(L"DISPLAY_VRR");
    g.display_vrr_set = environment(L"DISPLAY_VRR_SET") == "1";
    g.display_hz = std::atof(environment(L"DISPLAY_HZ").c_str());
    g.demo = environment(L"MENU_DEMO");
    char ready[96];
    snprintf(ready, sizeof(ready), APP_NAME " menu: ready (white in HDR: %.0f nits)", g.paper_nits);
    reshade::log::message(reshade::log::level::info, ready);
    return true;
}

Encoding encoding_of(reshade::api::color_space cs) {
    switch (cs) {
    case reshade::api::color_space::hdr10_st2084: return Encoding::Pq;
    case reshade::api::color_space::extended_srgb_linear: return Encoding::ScRgb;
    default: return Encoding::Sdr;
    }
}

// Keys and the mouse, from ReShade's input (it sees them even while it keeps
// them from mpv).
void feed_input(reshade::api::effect_runtime *runtime, UINT width, UINT height, bool take_wheel) {
    ImGuiIO &io = ImGui::GetIO();
    static const std::pair<UINT, ImGuiKey> keys[] = {
        {VK_UP, ImGuiKey_UpArrow},     {VK_DOWN, ImGuiKey_DownArrow}, {VK_LEFT, ImGuiKey_LeftArrow},
        {VK_RIGHT, ImGuiKey_RightArrow}, {VK_RETURN, ImGuiKey_Enter}, {VK_SPACE, ImGuiKey_Space},
        {VK_HOME, ImGuiKey_Home},      {VK_END, ImGuiKey_End},        {VK_DELETE, ImGuiKey_Delete},
    };
    for (const auto &[vk, key] : keys) io.AddKeyEvent(key, runtime->is_key_down(vk));

    uint32_t x = 0, y = 0;
    int16_t wheel = 0;
    // In the window's client area (ReShade converts it, whatever its comment
    // says), scaled in case the back buffer is not the window's size.
    runtime->get_mouse_cursor_position(&x, &y, &wheel);
    RECT client{};
    const HWND hwnd = static_cast<HWND>(runtime->get_hwnd());
    if (hwnd && GetClientRect(hwnd, &client) && client.right > 0 && client.bottom > 0)
        io.AddMousePosEvent(float(x) * width / client.right, float(y) * height / client.bottom);
    else
        io.AddMousePosEvent(float(x), float(y));
    for (int b = 0; b < 3; ++b) io.AddMouseButtonEvent(b == 0 ? 0 : b == 2 ? 1 : 2, runtime->is_mouse_button_down(b));
    if (wheel && take_wheel) io.AddMouseWheelEvent(0, std::abs(wheel) >= WHEEL_DELTA ? float(wheel) / WHEEL_DELTA : float(wheel));
}

void set_open(bool open) {
    if (open == g.open) return;
    g.open = open;
    if (open) {
        reshade::log::message(reshade::log::level::info, APP_NAME " menu: open");
    } else {
        char text[128];
        snprintf(text, sizeof(text), APP_NAME " menu: closed; it was drawn on %u frames mpv presented, %u real frames",
                 g.on_mpv, g.on_real);
        reshade::log::message(reshade::log::level::info, text);
    }
    g.on_mpv = g.on_real = 0;
    if (!open) {
        g.closed_at = now_seconds();
        menu_closed(g.host, g.settings_path);
    }
    // mpv hides the cursor when the mouse rests; the menu needs it.
    g.mpv.command({"set", "cursor-autohide", open ? "no" : "100"});  // the app starts mpv with 100
    if (open) g.last.QuadPart = 0;
}

// Builds the menu for now and draws it on back_buffer. wheel: take the mouse
// wheel (once per ReShade frame: it is the movement since ReShade's last).
void build_and_draw(ID3D11DeviceContext *context, ID3D11Texture2D *back_buffer, bool wheel) {
    reshade::api::effect_runtime *runtime = g.runtime;
    D3D11_TEXTURE2D_DESC bb{};
    back_buffer->GetDesc(&bb);
    g.encoding = encoding_of(g.color_space.load());

    ImGui::SetCurrentContext(g.imgui);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(bb.Width), float(bb.Height));
    io.DeltaTime = float(seconds_since(g.last));
    const float scale = std::max(0.5f, bb.Height / 1080.0f);
    if (bb.Height != g.style_height) {
        apply_style(scale);
        g.style_height = bb.Height;
    }
    if (g.open) feed_input(runtime, bb.Width, bb.Height, wheel);  // a toast alone takes no input

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    bool keep = true;
    if (g.open) {
        MenuFrame frame;
        frame.runtime = runtime;
        frame.host = &g.host;
        frame.mpv = &g.mpv;
        frame.encoding = g.encoding;
        frame.settings_path = g.settings_path;
        frame.source_hdr = g.source_hdr;
        frame.source_auto = g.source_auto;
        frame.hdr_available = g.hdr_available;
        frame.hdr_output = g.hdr_output;
        frame.hdr_why = g.hdr_why;
        frame.hdr_peak_nits = g.hdr_peak_nits;
        frame.hdr_dynamic_peak = g.hdr_dynamic_peak;
        frame.borderless = g.borderless;
        frame.display_name = g.display_name;
        frame.display_vrr = g.display_vrr;
        frame.display_vrr_set = g.display_vrr_set;
        frame.display_hz = g.display_hz;
        frame.demo = g.demo;
        frame.stream_offered = g.stream.offered();
        frame.stream_on = g.stream.wanted();
        frame.set_stream = [](bool on) { g.stream.set_wanted(on); };
        frame.stream_brightness = g.stream.brightness();
        frame.set_stream_brightness = [](int percent) { g.stream.set_brightness(percent); };
        frame.input_on = g.input_on;
        frame.mouse_sensitivity = &g.mouse_sensitivity;
        frame.mouse_aim = &g.mouse_aim;
        frame.mouse_forward = &g.mouse_forward;
        frame.mouse_lead = &g.mouse_lead;
        frame.scale = scale;
        frame.show_rates = &g.show_rates;
        keep = draw_menu(frame);
    }
    if (g.show_rates) draw_rates(g.host, scale);
    const double now = now_seconds();
    if (now < g.toast_until) {
        const float alpha = float(std::clamp((g.toast_until - now) / kToastFade, 0.0, 1.0));
        draw_toast(g.toast_text, alpha, scale);
    }
    ImGui::Render();

    StateBackup backup(context);
    ID3D11RenderTargetView *target = g.compositor.begin_menu(context, bb.Width, bb.Height);
    static bool reported = false;
    if (target) {
        context->OMSetRenderTargets(1, &target, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (!g.compositor.composite(context, back_buffer, g.encoding, g.paper_nits) && !reported) {
            reported = true;
            reshade::log::message(reshade::log::level::warning, APP_NAME " menu: could not draw on this back buffer");
        }
    } else if (!reported) {
        reported = true;
        reshade::log::message(reshade::log::level::warning, APP_NAME " menu: could not make its texture");
    }
    if (!keep) set_open(false);
}

void show_toast(const std::string &text, double seconds, double now) {
    g.toast_text = text;
    g.toast_start = now;
    g.toast_until = now + seconds + kToastFade;
}

// New toasts from mpv and from this add-on. Changes of volume or mute while
// the menu is open (or just closed) are the menu's own: it shows them.
void poll_toasts(double now) {
    const Toast t = g.mpv.toast();
    if (t.serial != g.mpv_toast_seen) {
        g.mpv_toast_seen = t.serial;
        if (!(t.from_property && (g.open || now - g.closed_at < 0.6))) show_toast(t.text, t.seconds, now);
    }
    std::lock_guard lock(g.local_m);
    if (g.local_serial != g.local_seen) {
        g.local_seen = g.local_serial;
        show_toast(g.local_text, g.local_seconds, now);
    }
}

bool toast_showing() { return now_seconds() < g.toast_until; }

void shot_done(bool ok, const std::string &text) {
    reshade::log::message(ok ? reshade::log::level::info : reshade::log::level::error,
                          (APP_NAME " screenshot: " + std::string(ok ? "saved " : "failed: ") + text).c_str());
    toast(ok ? "Screenshot saved" : "Screenshot failed: the logs folder says why");
}

// The picture's part of a width x height back buffer: without mpv's black
// bars (osd-dimensions, scaled if the window is not the back buffer's size).
D3D11_BOX picture_box(const MpvState::Osd &osd, UINT width, UINT height) {
    D3D11_BOX box{0, 0, 0, width, height, 1};
    if (osd.w <= 0 || osd.h <= 0) return box;
    const double sx = double(width) / osd.w, sy = double(height) / osd.h;
    const auto at = [](double v, UINT limit) { return UINT(std::clamp(std::lround(v), 0L, long(limit))); };
    D3D11_BOX picture{at(osd.ml * sx, width), at(osd.mt * sy, height), 0, at(width - osd.mr * sx, width),
                      at(height - osd.mb * sy, height), 1};
    if (picture.right - picture.left < 16 || picture.bottom - picture.top < 16 || picture.right <= picture.left ||
        picture.bottom <= picture.top)
        return box;  // margins that make no sense: the whole frame
    return picture;
}

// Moves the picture's part of a frame about to be presented by what the
// input program asks for now.
void shift_view(ID3D11DeviceContext *context, ID3D11Texture2D *back_buffer) {
    D3D11_TEXTURE2D_DESC bb{};
    back_buffer->GetDesc(&bb);
    g.shifter.apply(g.device, context, back_buffer, picture_box(g.mpv.state().osd, bb.Width, bb.Height),
                    encoding_of(g.color_space.load()), g.paper_nits);
}

}  // namespace

void set_host(const Host &host) { g.host = host; }

D3D11_BOX picture_box(UINT width, UINT height) { return picture_box(g.mpv.state().osd, width, height); }

void toast(const std::string &text, double seconds) {
    std::lock_guard lock(g.local_m);
    g.local_text = text;
    g.local_seconds = seconds;
    ++g.local_serial;
}

void note_color_space(reshade::api::color_space color_space) { g.color_space = color_space; }

bool menu_open() { return g.open; }

void before_frame_generation(reshade::api::effect_runtime *runtime) {
    reshade::api::device *api_device = runtime->get_device();
    if (api_device->get_api() != reshade::api::device_api::d3d11) return;
    auto *device = reinterpret_cast<ID3D11Device *>(api_device->get_native());
    auto *swapchain = reinterpret_cast<IDXGISwapChain *>(runtime->get_native());
    if (!device || !swapchain || !g.device || device != g.device) return;  // the menu's device is mpv's
    g.shots.tick(g.context.Get(), shot_done);
    const unsigned asked = g.mpv.screenshot_requests();
    const bool shot = asked != g.shots_seen;
    // The stream window takes every game frame, as the screenshot would be.
    const bool stream = g.stream.wanted();
    if (!stream) g.stream.frame(device, g.context.Get(), nullptr, Encoding::Sdr, {});  // lets go of what it held
    if (!shot && !stream) return;
    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) return;
    D3D11_TEXTURE2D_DESC bb{};
    back_buffer->GetDesc(&bb);
    if (stream)
        g.stream.frame(device, g.context.Get(), back_buffer.Get(), encoding_of(g.color_space.load()),
                       picture_box(g.mpv.state().osd, bb.Width, bb.Height));
    if (!shot) return;
    g.shots_seen = asked;
    if (g.shots.busy()) {
        toast("Screenshot: still saving the last one");
        return;
    }
    std::string error;
    if (!g.shots.grab(device, g.context.Get(), back_buffer.Get(), encoding_of(g.color_space.load()),
                      picture_box(g.mpv.state().osd, bb.Width, bb.Height), error))
        shot_done(false, error);
}

void on_reshade_present(reshade::api::effect_runtime *runtime) {
    g.runtime = nullptr;
    reshade::api::device *api_device = runtime->get_device();
    if (g.failed || api_device->get_api() != reshade::api::device_api::d3d11) return;
    auto *device = reinterpret_cast<ID3D11Device *>(api_device->get_native());
    auto *swapchain = reinterpret_cast<IDXGISwapChain *>(runtime->get_native());
    if (!device || !swapchain) return;
    if (g.device && device != g.device) return;  // another device's swap chain
    static bool seen = false;
    if (!seen) {
        seen = true;
        reshade::log::message(reshade::log::level::info, APP_NAME " menu: Tab opens it");
    }

    // mpv's line (for the toasts) and the drawing are set up at once, not on
    // the first key: building the font then would be a hitch in the game.
    if (!g.mpv_started) {
        g.mpv_started = true;
        const std::string &p = g.host.mpv_pipe;
        g.mpv.start(L"\\\\.\\pipe\\" + std::wstring(p.begin(), p.end()));
    }
    if (!g.device && !init(device)) {
        g.failed = true;
        reshade::log::message(reshade::log::level::error, APP_NAME " menu: could not start; it stays off");
        return;
    }

    // The picture moved ahead of the console (view_shift.h), before anything
    // of the menu's is drawn on the frame. Not under the open menu.
    if (g.shifter.named() && !g.open) {
        ComPtr<ID3D11Texture2D> frame;
        if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&frame)))) shift_view(g.context.Get(), frame.Get());
    }

    if (runtime->is_key_pressed(kOpenKey) || (g.open && runtime->is_key_pressed(VK_ESCAPE))) set_open(!g.open);
    poll_toasts(now_seconds());
    if (!g.open && !toast_showing() && !g.show_rates) return;
    if (g.open) runtime->block_input_next_frame();
    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) return;
    g.runtime = runtime;
    build_and_draw(g.context.Get(), back_buffer.Get(), true);
    if (g.open) ++g.on_mpv;
}

void draw_again(ID3D11DeviceContext *context, ID3D11Texture2D *back_buffer) {
    if (g.device && g.shifter.named() && !g.open) shift_view(context, back_buffer);
    // Built again rather than drawn again: the menu then answers at the rate
    // frames reach the screen, not at mpv's.
    if ((!g.open && !toast_showing() && !g.show_rates) || !g.runtime || !g.imgui) return;
    build_and_draw(context, back_buffer, false);
    if (g.open) ++g.on_real;
}

void release() {
    g.shifter.release();
    g.shots.release();
    g.stream.release();
    g.mpv.stop();
    g.mpv_started = false;
    g.toast_until = 0;
    if (g.backend) {
        ImGui::SetCurrentContext(g.imgui);
        ImGui_ImplDX11_Shutdown();
        g.backend = false;
    }
    if (g.imgui) ImGui::DestroyContext(g.imgui);
    g.imgui = nullptr;
    g.compositor.release();
    g.context.Reset();
    g.device = nullptr;
    g.runtime = nullptr;
    g.open = false;
    g.style_height = 0;
}

}  // namespace app::overlay
