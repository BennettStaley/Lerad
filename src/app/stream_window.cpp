// SPDX-License-Identifier: MIT
#include "stream_window.h"

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <vector>

#include "app_version.h"
#include "log.h"
#include "stream_link.h"

namespace app {
namespace {

using Microsoft::WRL::ComPtr;

const wchar_t kClass[] = APP_ID_W L"-stream";

// mpv's own window: visible, top-level and unowned, of that process.
HWND find_mpv(DWORD pid) {
    struct Find {
        DWORD pid;
        HWND found;
    } f{pid, nullptr};
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto *f = reinterpret_cast<Find *>(lp);
            DWORD owner = 0;
            GetWindowThreadProcessId(hwnd, &owner);
            if (owner != f->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
            RECT r{};
            if (!GetWindowRect(hwnd, &r) || r.right - r.left < 64 || r.bottom - r.top < 64) return TRUE;
            f->found = hwnd;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&f));
    return f.found;
}

struct Screen {
    HMONITOR monitor;
    RECT work;
    bool primary;
    std::wstring name;
};

std::vector<Screen> screens() {
    std::vector<Screen> all;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(m, &info))
                reinterpret_cast<std::vector<Screen> *>(lp)->push_back(
                    {m, info.rcWork, (info.dwFlags & MONITORINFOF_PRIMARY) != 0, info.szDevice});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&all));
    return all;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_CLOSE) {
        // The loop that owns the window takes it down.
        if (auto *closed = reinterpret_cast<bool *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) *closed = true;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

// The window, and the picture in it.
struct Show {
    HWND hwnd = nullptr;
    bool closed = false;  // its close box was pressed
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> chain;
    ComPtr<ID3D11Texture2D> shared;
    ComPtr<IDXGIKeyedMutex> mutex;
    ULONGLONG last_frame = 0, last_try = 0;
    unsigned long long frames = 0;
    bool said_waiting = false;

    void drop_picture() {
        mutex.Reset();
        shared.Reset();
        chain.Reset();
        if (context) context->ClearState();
        context.Reset();
        device.Reset();
    }

    // Opens the add-on's texture: on the GPU that made it, which is found
    // by trying each. False while there is none yet.
    bool open_picture() {
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            ComPtr<ID3D11Device> d;
            ComPtr<ID3D11DeviceContext> c;
            if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                         nullptr, 0, D3D11_SDK_VERSION, &d, nullptr, &c)))
                continue;
            ComPtr<ID3D11Device1> d1;
            ComPtr<ID3D11Texture2D> texture;
            if (FAILED(d.As(&d1)) ||
                FAILED(d1->OpenSharedResourceByName(stream::kTexture, DXGI_SHARED_RESOURCE_READ, IID_PPV_ARGS(&texture))))
                continue;
            ComPtr<IDXGIKeyedMutex> m;
            if (FAILED(texture.As(&m))) continue;
            DXGI_SWAP_CHAIN_DESC1 sc{};
            sc.Width = stream::kWidth;
            sc.Height = stream::kHeight;
            sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sc.SampleDesc.Count = 1;
            sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sc.BufferCount = 2;
            sc.Scaling = DXGI_SCALING_STRETCH;  // the window may be smaller than the picture
            sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            sc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            ComPtr<IDXGISwapChain1> swap;
            const HRESULT hr = factory->CreateSwapChainForHwnd(d.Get(), hwnd, &sc, nullptr, nullptr, &swap);
            if (FAILED(hr)) {
                log(L"stream window: no swap chain on {} (0x{:08X})", desc.Description, static_cast<unsigned>(hr));
                return false;
            }
            factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
            // The picture's own drawing goes first on the GPU: this window's
            // copy can wait a moment, a frame of the game's cannot.
            ComPtr<IDXGIDevice> dxgi;
            if (SUCCEEDED(d.As(&dxgi))) dxgi->SetGPUThreadPriority(-7);
            device = d;
            context = c;
            chain = swap;
            shared = texture;
            mutex = m;
            log(L"stream window: showing the picture, drawn on {}", desc.Description);
            return true;
        }
        return false;
    }

    // A new frame is there: into the window.
    void show() {
        const ULONGLONG now = GetTickCount64();
        if (!shared) {
            if (last_try && now - last_try < 250) return;
            last_try = now;
            if (!open_picture()) {
                if (!said_waiting) log(L"stream window: waiting for the picture");
                said_waiting = true;
                return;
            }
            said_waiting = false;
            last_frame = now;
        }
        const HRESULT held = mutex->AcquireSync(0, 30);
        if (held != S_OK) {
            // Abandoned: the mpv that made the texture is gone.
            if (held != static_cast<HRESULT>(WAIT_TIMEOUT)) drop_picture();
            return;
        }
        ComPtr<ID3D11Texture2D> back;
        const bool ok = SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&back)));
        if (ok) context->CopyResource(back.Get(), shared.Get());
        mutex->ReleaseSync(0);
        if (!ok || FAILED(chain->Present(0, 0))) {
            log(L"stream window: the picture could not be shown; opening it again");
            drop_picture();
            return;
        }
        last_frame = now;
        ++frames;
    }
};

// The window on a screen the picture is not on (while there is another),
// its client area the copy's size or the largest of its shape that fits.
HWND open_window(HWND mpv, bool *closed) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);  // again for a second window: fails, harmless

    const std::vector<Screen> all = screens();
    if (all.empty()) return nullptr;
    const HMONITOR playing = MonitorFromWindow(mpv, MONITOR_DEFAULTTONEAREST);
    int play = -1, primary = -1;
    for (int i = 0; i < int(all.size()); ++i) {
        if (all[i].monitor == playing) play = i;
        if (all[i].primary) primary = i;
    }
    const int at = stream::pick_monitor(int(all.size()), play, primary);
    const Screen &screen = all[at];

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const HWND hwnd = CreateWindowExW(0, kClass, APP_NAME_W L" stream", style, screen.work.left, screen.work.top, 640, 400,
                                      nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        log(L"stream window: could not be opened (error {})", GetLastError());
        return nullptr;
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(closed));
    // Its frame's size on that screen (the screen's own scaling), then the
    // client area that fits beside it.
    RECT window{}, client{};
    GetWindowRect(hwnd, &window);
    GetClientRect(hwnd, &client);
    const int frame_w = (window.right - window.left) - client.right, frame_h = (window.bottom - window.top) - client.bottom;
    const int work_w = screen.work.right - screen.work.left, work_h = screen.work.bottom - screen.work.top;
    int w = 0, h = 0;
    stream::fit(work_w - frame_w, work_h - frame_h, w, h);
    const int x = screen.work.left + (work_w - (w + frame_w)) / 2, y = screen.work.top + (work_h - (h + frame_h)) / 2;
    SetWindowPos(hwnd, nullptr, x, y, w + frame_w, h + frame_h, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    log(L"stream window: opened {} x {} on {}{}", w, h, screen.name,
        at == play ? L" (the screen the picture is on: there is no other)"
                   : screen.primary ? L" (the main screen; the picture is on another)" : L" (not the picture's screen)");
    if (w != int(stream::kWidth))
        log(L"stream window: {} x {} does not fit that screen beside the window's frame and the taskbar; what is "
            L"shared is the window's size",
            stream::kWidth, stream::kHeight);
    return hwnd;
}

}  // namespace

bool StreamWindow::offer(bool on) {
    if (on_) return true;
    on_ = CreateEventW(nullptr, TRUE, on ? TRUE : FALSE, stream::kOn);
    if (!on_) {
        log(L"stream window: not offered (no switch, error {})", GetLastError());
        return false;
    }
    // Made by this app: as it says, not as another left it.
    if (on) SetEvent(on_);
    else ResetEvent(on_);
    frame_ = CreateEventW(nullptr, FALSE, FALSE, stream::kFrame);
    if (!frame_) {
        log(L"stream window: not offered (no event for its frames, error {})", GetLastError());
        CloseHandle(on_);
        on_ = nullptr;
        return false;
    }
    return true;
}

void StreamWindow::start(DWORD mpv_pid, std::function<void()> closed) {
    if (!on_ || thread_.joinable()) return;
    pid_ = mpv_pid;
    closed_ = std::move(closed);
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void StreamWindow::stop() {
    stopping_ = true;
    if (thread_.joinable()) thread_.join();
    if (on_) CloseHandle(on_);
    if (frame_) CloseHandle(frame_);
    on_ = frame_ = nullptr;
}

void StreamWindow::run() {
    Show show;
    auto close = [&](const wchar_t *why) {
        if (!show.hwnd) return;
        log(L"stream window: closed ({}) after {} frames", why, show.frames);
        show.drop_picture();
        DestroyWindow(show.hwnd);
        show = Show{};
    };
    while (!stopping_) {
        const bool wanted = WaitForSingleObject(on_, 0) == WAIT_OBJECT_0;
        if (!wanted) {
            close(L"switched off");
            WaitForSingleObject(on_, 200);
            continue;
        }
        if (!show.hwnd) {
            const HWND mpv = find_mpv(pid_);
            if (!mpv) {  // the picture's window is not up yet
                Sleep(200);
                continue;
            }
            show.hwnd = open_window(mpv, &show.closed);
            if (!show.hwnd) {
                ResetEvent(on_);
                continue;
            }
        }
        const DWORD woke = MsgWaitForMultipleObjects(1, &frame_, FALSE, 100, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (show.closed) {
            ResetEvent(on_);
            close(L"its close box");
            if (closed_) closed_();
            continue;
        }
        if (woke == WAIT_OBJECT_0) show.show();
        // No frame for a while: the mpv that drew them is gone, or draws
        // them anew (another size of picture never needs this). Its texture
        // is let go, so that the next one can take the name.
        if (show.shared && GetTickCount64() - show.last_frame > 2000) {
            log(L"stream window: no new picture for two seconds; waiting for it again");
            show.drop_picture();
        }
    }
    close(L"the app is done");
}

}  // namespace app
