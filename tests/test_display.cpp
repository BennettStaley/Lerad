// SPDX-License-Identifier: MIT
// Checks picking the screen and the GPU, reading variable refresh out of an
// EDID, and putting it together with what NVIDIA's driver says.
#include <cstdio>
#include <string>
#include <vector>

#include "app_version.h"
#include "display.h"
#include "gpu.h"
#include "mpv_launch.h"
#include "settings.h"
#include "stream_link.h"
#include "vrr.h"

using namespace app;

namespace {

int g_failures = 0;

void expect(bool ok, const char *what) {
    std::printf("%-74s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_failures;
}

DisplayInfo screen(int number, const wchar_t *name, bool primary) {
    DisplayInfo d;
    d.found = true;
    d.number = number;
    d.device = L"\\\\.\\DISPLAY" + std::to_wstring(number);
    d.name = name;
    d.primary = primary;
    return d;
}

GpuInfo gpu(const wchar_t *name, unsigned vendor, unsigned mb, std::vector<std::wstring> screens = {}) {
    GpuInfo g;
    g.name = name;
    g.vendor = vendor;
    g.found = true;
    g.device_id = 1;
    g.dedicated_mb = mb;
    g.screens = std::move(screens);
    return g;
}

// An EDID's first block: version 1.<revision>, the feature byte, and a range
// limits descriptor (min-max Hz) in its second descriptor slot.
std::vector<unsigned char> edid_base(int revision, unsigned char features, unsigned char min, unsigned char max) {
    std::vector<unsigned char> e(128, 0);
    const unsigned char header[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    for (int i = 0; i < 8; ++i) e[i] = header[i];
    e[18] = 1;
    e[19] = static_cast<unsigned char>(revision);
    e[24] = features;
    e[72 + 3] = 0xFD;
    e[72 + 5] = min;
    e[72 + 6] = max;
    return e;
}

// Adds a CTA-861 extension block with these data blocks (header bytes included).
void add_cta(std::vector<unsigned char> &e, const std::vector<unsigned char> &blocks) {
    std::vector<unsigned char> b(128, 0);
    b[0] = 0x02;
    b[1] = 0x03;
    b[2] = static_cast<unsigned char>(4 + blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i) b[4 + i] = blocks[i];
    e.insert(e.end(), b.begin(), b.end());
    ++e[126];
}

bool has(const std::wstring &text, const wchar_t *part) { return text.find(part) != std::wstring::npos; }

}  // namespace

int main() {
    using Vrr = DisplayInfo::Vrr;
    using Tri = DisplayInfo::Tri;
    {
        // The screen the settings file asks for.
        const std::vector<DisplayInfo> two = {screen(2, L"Odyssey G95SC", true), screen(1, L"PA279CDV", false)};
        bool matched = false;
        expect(choose_display(two, L"", &matched)->number == 2 && matched, "no display.monitor: the primary");
        expect(choose_display(two, L"1", &matched)->name == L"PA279CDV" && matched,
               "display.monitor = 1: \\\\.\\DISPLAY1, not the first in the list");
        expect(choose_display(two, L"2", &matched)->number == 2 && matched, "display.monitor = 2");
        expect(choose_display(two, L"pa279", &matched)->number == 1 && matched, "a part of the name, any case");
        expect(choose_display(two, L"\\\\.\\display1", &matched)->number == 1 && matched, "Windows' own name for it");
        expect(choose_display(two, L"3", &matched)->number == 2 && !matched, "no screen 3: the primary, and said so");
        expect(choose_display(two, L"LG", &matched)->primary && !matched, "no such name: the primary, and said so");
        expect(choose_display({}, L"", &matched) == nullptr && !matched, "no screens at all");
        const std::vector<DisplayInfo> unnamed = {screen(1, L"", true), screen(2, L"", false)};
        expect(choose_display(unnamed, L"x", &matched)->number == 1 && !matched,
               "a name never matches a screen without one");
    }
    {
        // What the add-ons are told.
        expect(std::wstring(vrr_for_addons(Vrr::On, -1)) == L"on", "VRR found on: on");
        expect(std::wstring(vrr_for_addons(Vrr::Off, -1)) == L"off" &&
                   std::wstring(vrr_for_addons(Vrr::Unsupported, -1)) == L"off",
               "VRR switched off, or none: off");
        expect(std::wstring(vrr_for_addons(Vrr::Supported, -1)) == L"unknown" &&
                   std::wstring(vrr_for_addons(Vrr::Unknown, -1)) == L"unknown",
               "the screen can but nothing says it is on: unknown, not a guess");
        expect(std::wstring(vrr_for_addons(Vrr::Off, 1)) == L"on" && std::wstring(vrr_for_addons(Vrr::On, 0)) == L"off",
               "display.vrr = on / off wins over what was found");
        // Every screen, for frame generation, which goes by the one the
        // window is on (src/framegen/fg_screen.h reads this).
        std::vector<DisplayInfo> two = {screen(2, L"Odyssey G95SC", true), screen(1, L"PA279CDV", false)};
        two[0].refresh_hz = 239.76, two[0].vrr = Vrr::Supported;
        two[1].refresh_hz = 119.88, two[1].vrr = Vrr::Off;
        expect(screens_for_addons(two, L"\\\\.\\DISPLAY1", -1) ==
                   L"\\\\.\\DISPLAY2|unknown|239.760||2 (Odyssey G95SC);\\\\.\\DISPLAY1|off|119.880||1 (PA279CDV)",
               "every screen with its own VRR state and rate");
        expect(screens_for_addons(two, L"\\\\.\\DISPLAY1", 1) ==
                   L"\\\\.\\DISPLAY2|unknown|239.760||2 (Odyssey G95SC);\\\\.\\DISPLAY1|on|119.880||1 (PA279CDV)",
               "display.vrr = on speaks for the screen " APP_NAME " plays on, not the other");
        two[1].name = L"A|B;C";
        two[1].monitor_path = L"\\\\?\\DISPLAY#AUS2704#5&1a2b3c&0&UID4352#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";
        expect(screens_for_addons(two, L"", -1).ends_with(
                   L"|\\\\?\\DISPLAY#AUS2704#5&1a2b3c&0&UID4352#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}|1 (A B C)"),
               "the monitor's own path goes along; a name cannot break the list apart");
        expect(screens_for_addons({}, L"", -1).empty(), "no screens: nothing told");
    }
    {
        // The GPU mpv draws on.
        const std::vector<GpuInfo> desktop = {
            gpu(L"NVIDIA GeForce RTX 5090", kVendorNvidia, 32000, {L"\\\\.\\DISPLAY2", L"\\\\.\\DISPLAY1"}),
            gpu(L"AMD Radeon(TM) Graphics", kVendorAmd, 2000)};
        GpuChoice c = choose_gpu(desktop, L"\\\\.\\DISPLAY1", L"");
        expect(c.gpu.name == L"NVIDIA GeForce RTX 5090" && !c.by_setting && !c.other_than_screen,
               "no display.gpu: the GPU the screen is plugged into");
        c = choose_gpu(desktop, L"\\\\.\\DISPLAY1", L"radeon");
        expect(c.gpu.vendor == kVendorAmd && c.by_setting && c.other_than_screen &&
                   c.screen_gpu.vendor == kVendorNvidia,
               "display.gpu = radeon: that one, and frames cross to the screen's GPU");
        c = choose_gpu(desktop, L"\\\\.\\DISPLAY1", L"Arc");
        expect(c.gpu.vendor == kVendorNvidia && c.setting_not_found && !c.by_setting,
               "display.gpu names no GPU: the screen's, and said so");
        const std::vector<GpuInfo> laptop = {
            gpu(L"Intel(R) Iris(R) Xe Graphics", kVendorIntel, 128, {L"\\\\.\\DISPLAY1"}),
            gpu(L"NVIDIA GeForce RTX 4060 Laptop GPU", kVendorNvidia, 8000)};
        c = choose_gpu(laptop, L"\\\\.\\DISPLAY1", L"");
        expect(c.gpu.vendor == kVendorIntel && !dlss5_gpu(c.gpu),
               "hybrid laptop: the screen's integrated GPU, never the other one unasked");
        c = choose_gpu(laptop, L"\\\\.\\DISPLAY1", L"RTX");
        expect(dlss5_gpu(c.gpu) && c.other_than_screen, "hybrid laptop, display.gpu = RTX: DLSS 5, across GPUs");
        c = choose_gpu(desktop, L"", L"");
        expect(c.gpu.vendor == kVendorNvidia && !c.screen_gpu.found, "screen unknown: the first GPU");
        expect(!choose_gpu({}, L"\\\\.\\DISPLAY1", L"RTX").gpu.found, "no GPU found: none, mpv picks");

        // The class, a guess from the name and the memory.
        expect(gpu_class(gpu(L"NVIDIA GeForce RTX 5090", kVendorNvidia, 32000)) == GpuClass::High &&
                   gpu_class(gpu(L"NVIDIA GeForce RTX 3080", kVendorNvidia, 10000)) == GpuClass::High &&
                   gpu_class(gpu(L"NVIDIA GeForce RTX 5070 Ti", kVendorNvidia, 16000)) == GpuClass::High,
               "GPU class: RTX x070 and up are high (a 3080 despite its 10 GB)");
        expect(gpu_class(gpu(L"NVIDIA GeForce RTX 3060", kVendorNvidia, 12000)) == GpuClass::Mid &&
                   gpu_class(gpu(L"NVIDIA GeForce RTX 4070 Laptop GPU", kVendorNvidia, 8000)) == GpuClass::Mid &&
                   gpu_class(gpu(L"NVIDIA GeForce RTX 2080 Ti", kVendorNvidia, 11000)) == GpuClass::Mid,
               "GPU class: x060, laptop x070 and RTX 20 are mid");
        expect(gpu_class(gpu(L"AMD Radeon RX 7900 XTX", kVendorAmd, 24000)) == GpuClass::High &&
                   gpu_class(gpu(L"AMD Radeon RX 7600", kVendorAmd, 8000)) == GpuClass::Mid &&
                   gpu_class(gpu(L"Intel(R) Arc(TM) A770 Graphics", kVendorIntel, 16000)) == GpuClass::Mid,
               "GPU class: other vendors by memory; Intel Arc no higher than mid");
        expect(gpu_class(gpu(L"AMD Radeon(TM) Graphics", kVendorAmd, 2000)) == GpuClass::Low &&
                   gpu_class(gpu(L"Intel(R) UHD Graphics 770", kVendorIntel, 128)) == GpuClass::Low &&
                   gpu_class(gpu(L"NVIDIA GeForce GTX 1050", kVendorNvidia, 2000)) == GpuClass::Low,
               "GPU class: integrated and small GPUs are low");
        expect(gpu_class(gpu(L"Some New GPU", 0x1234, 0)) == GpuClass::Unknown && gpu_class(GpuInfo{}) == GpuClass::Unknown,
               "GPU class: no memory reported, or no GPU: unknown");
    }
    {
        // The EDID.
        expect(!parse_edid_vrr(nullptr, 0).valid, "no EDID");
        std::vector<unsigned char> junk(128, 0x55);
        expect(!parse_edid_vrr(junk.data(), junk.size()).valid, "not an EDID");

        // A monitor like the Odyssey G95SC on HDMI: the HDMI Forum block
        // (VRR 48-240) and AMD's (version 2, 48-240).
        std::vector<unsigned char> e = edid_base(3, 0x2A, 48, 240);
        add_cta(e, {0x6D, 0xD8, 0x5D, 0xC4, 0x01, 0x78, 0x80, 0x5B, 0x02, 0x30, 0xF0, 0xC3, 0x54, 0x1D,  // HDMI Forum
                    0x6D, 0x1A, 0x00, 0x00, 0x02, 0x07, 0x30, 0xF0, 0x00, 0x04, 0x60, 0x02, 0x4B, 0x02});  // AMD
        EdidVrr v = parse_edid_vrr(e.data(), e.size());
        expect(v.valid && v.hdmi_vrr && v.hdmi_min_hz == 48 && v.hdmi_max_hz == 240, "HDMI Forum block: VRR 48-240 Hz");
        expect(v.freesync && v.freesync_min_hz == 48 && v.freesync_max_hz == 240, "AMD block, version 2: 48-240 Hz");
        expect(!v.range, "EDID 1.3 range limits are not a VRR range");

        // A DisplayPort monitor: EDID 1.4, continuous frequency, range 48-120,
        // AMD block version 1.
        e = edid_base(4, 0x3B, 48, 120);
        add_cta(e, {0x68, 0x1A, 0x00, 0x00, 0x01, 0x01, 0x30, 0x78, 0x00});
        v = parse_edid_vrr(e.data(), e.size());
        expect(v.freesync && v.freesync_min_hz == 48 && v.freesync_max_hz == 120 && !v.hdmi_vrr,
               "AMD block, version 1: FreeSync 48-120 Hz, no HDMI VRR");
        expect(v.range && v.range_min_hz == 48 && v.range_max_hz == 120, "EDID 1.4 continuous range 48-120 Hz");

        // AMD version 3: the highest rate is 10 bits wide (360 Hz).
        e = edid_base(4, 0x3A, 48, 255);
        add_cta(e, {0x6F, 0x1A, 0x00, 0x00, 0x03, 0x01, 0x30, 0x00, 0, 0, 0, 0, 0, 0, 0x68, 0x01});
        v = parse_edid_vrr(e.data(), e.size());
        expect(v.freesync && v.freesync_max_hz == 360, "AMD block, version 3: up to 360 Hz");

        // A monitor without VRR: AMD's block without the FreeSync flag, an
        // HDMI Forum block with VRRmin 0, and a block running past the end.
        e = edid_base(4, 0x3A, 56, 75);
        add_cta(e, {0x68, 0x1A, 0x00, 0x00, 0x01, 0x00, 0x30, 0x78, 0x00,
                    0x6A, 0xD8, 0x5D, 0xC4, 0x01, 0x78, 0x80, 0x5B, 0x02, 0x00, 0x00, 0x7F, 0x1A, 0x00});
        v = parse_edid_vrr(e.data(), e.size());
        expect(v.valid && !v.freesync && !v.hdmi_vrr && !v.range, "no FreeSync flag, VRRmin 0, a broken block: no VRR");

        // The HDMI Forum "sink capability" block (extended tag 0x79).
        e = edid_base(3, 0x2A, 48, 120);
        add_cta(e, {0xEB, 0x79, 0x00, 0x00, 0x01, 0x78, 0x80, 0x5B, 0x02, 0x28, 0x78, 0x00});
        v = parse_edid_vrr(e.data(), e.size());
        expect(v.hdmi_vrr && v.hdmi_min_hz == 40 && v.hdmi_max_hz == 120, "HDMI Forum sink capability block: 40-120 Hz");

        // Only the first block came through (an extension is announced).
        e = edid_base(4, 0x3B, 48, 144);
        e[126] = 1;
        v = parse_edid_vrr(e.data(), e.size());
        expect(v.valid && !v.freesync && v.range, "a missing extension block is not read");
    }
    {
        // The EDID and NVIDIA's driver together.
        EdidVrr hdmi;
        hdmi.valid = hdmi.hdmi_vrr = hdmi.freesync = true;
        hdmi.hdmi_min_hz = hdmi.freesync_min_hz = 48;
        hdmi.hdmi_max_hz = hdmi.freesync_max_hz = 240;
        EdidVrr dp;
        dp.valid = dp.freesync = true;
        dp.freesync_min_hz = 48;
        dp.freesync_max_hz = 120;
        EdidVrr plain;
        plain.valid = true;
        NvidiaVrr on;
        on.read = on.supported = on.capable_now = true;
        on.mode = 2;

        DisplayInfo d;
        apply_vrr(d, nullptr, nullptr, false);
        expect(d.vrr == Vrr::Unknown && d.vrr_source.empty(), "nothing read: unknown");
        apply_vrr(d, &plain, nullptr, false);
        expect(d.vrr == Vrr::Unknown, "an EDID without VRR blocks proves nothing (G-SYNC modules have none): unknown");
        apply_vrr(d, &dp, nullptr, false);
        expect(d.vrr == Vrr::Supported && d.vrr_kind == L"FreeSync" && d.vrr_min_hz == 48 && d.vrr_max_hz == 120 &&
                   has(d.vrr_source, L"EDID"),
               "the EDID alone (AMD, Intel): supported, with the range, never \"on\"");
        apply_vrr(d, &hdmi, &on, true);
        expect(d.vrr == Vrr::On && d.vrr_windowed == Tri::Yes && d.vrr_kind == L"HDMI VRR" && d.vrr_max_hz == 240 &&
                   has(d.vrr_source, L"NVIDIA driver") && has(d.vrr_source, L"EDID"),
               "NVIDIA, G-SYNC on for windowed and full screen: on, both sources named");
        NvidiaVrr n = on;
        n.mode = 1;
        apply_vrr(d, &dp, &n, false);
        expect(d.vrr == Vrr::On && d.vrr_windowed == Tri::No, "G-SYNC for full screen only: on, not for windows");
        n.mode = 0;
        apply_vrr(d, &dp, &n, false);
        expect(d.vrr == Vrr::Off && d.vrr_windowed == Tri::Unknown, "G-SYNC switched off in the driver: off");
        n.mode = -1;
        apply_vrr(d, &dp, &n, false);
        expect(d.vrr == Vrr::Supported, "the G-SYNC setting could not be read: supported, not a guess");
        n = on;
        n.capable_now = false;
        apply_vrr(d, &dp, &n, false);
        expect(d.vrr == Vrr::Off, "not at the display mode set now: off");
        n = NvidiaVrr{};
        n.read = true;
        apply_vrr(d, &dp, &n, false);
        expect(d.vrr == Vrr::Off && has(d.vrr_source, L"not offered"),
               "the screen can (EDID) but the driver does not offer it: off");
        apply_vrr(d, &plain, &n, false);
        expect(d.vrr == Vrr::Unsupported, "the driver says none and the EDID has none: unsupported");
        n = on;
        n.true_gsync = true;
        n.min_hz = 30;
        apply_vrr(d, &plain, &n, false);
        expect(d.vrr == Vrr::On && d.vrr_kind == L"G-SYNC" && d.vrr_min_hz == 30 && d.vrr_max_hz == 0,
               "a G-SYNC module: on, the lowest rate from the driver");
        apply_vrr(d, nullptr, nullptr, false);
        expect(d.vrr == Vrr::Unknown && d.vrr_kind.empty() && d.vrr_min_hz == 0 && d.vrr_windowed == Tri::Unknown,
               "reading again starts from nothing");
    }
    {
        DisplayInfo d = screen(1, L"PA279CDV", false);
        d.width = 3840;
        d.height = 2160;
        d.refresh_hz = 120;
        d.vrr = Vrr::On;
        d.vrr_kind = L"FreeSync";
        d.vrr_min_hz = 48;
        d.vrr_max_hz = 120;
        d.vrr_source = L"NVIDIA driver";
        const std::wstring t = display_text(d);
        expect(has(t, L"PA279CDV, 3840x2160 @ 120.000 Hz") && has(t, L"VRR on (FreeSync) 48-120 Hz [NVIDIA driver]"),
               "the screen in one line");
        expect(display_text(DisplayInfo{}) == L"unknown", "no screen: unknown");

        // mpv is told the screen and the GPU.
        auto has_arg = [](const std::vector<std::wstring> &args, const wchar_t *arg) {
            for (const auto &a : args)
                if (a == arg) return true;
            return false;
        };
        auto starts = [](const std::vector<std::wstring> &args, const wchar_t *prefix) {
            for (const auto &a : args)
                if (a.rfind(prefix, 0) == 0) return true;
            return false;
        };
        MpvPlan plan;
        plan.display = d;
        plan.display.enum_index = 1;
        Settings s;
        std::vector<std::wstring> args = mpv_arguments(plan, s);
        expect(!starts(args, L"--screen=") && !starts(args, L"--fs-screen") && !starts(args, L"--d3d11-adapter"),
               "no display.monitor, no GPU found: mpv is told neither (as before)");
        plan.on_display = true;
        plan.d3d11_adapter = L"NVIDIA GeForce RTX 5090";
        args = mpv_arguments(plan, s);
        expect(has_arg(args, L"--screen=1") && has_arg(args, L"--fs-screen=1") &&
                   has_arg(args, L"--d3d11-adapter=NVIDIA GeForce RTX 5090"),
               "display.monitor: mpv's --screen and --fs-screen by Windows' monitor order; the GPU by name");
        plan.display.enum_index = -1;
        expect(!starts(mpv_arguments(plan, s), L"--screen="), "the monitor's place unknown: mpv is not told a screen");
    }
    {
        // The stream window: which screen it opens on, and the size that fits.
        using app::stream::pick_monitor;
        expect(pick_monitor(2, 0, 1) == 1, "stream window: the main screen when the picture is on the other");
        expect(pick_monitor(2, 1, 1) == 0, "stream window: never the picture's screen, also when that is the main one");
        expect(pick_monitor(3, 0, 0) == 1, "stream window: the first other screen");
        expect(pick_monitor(3, 2, -1) == 0, "stream window: no main screen told: the first other");
        expect(pick_monitor(1, 0, 0) == 0, "stream window: one screen only: that one");
        expect(pick_monitor(2, -1, 1) == 1, "stream window: the picture's screen unknown: the main one");
        expect(pick_monitor(0, 0, 0) == -1, "stream window: no screen");
        int w = 0, h = 0;
        app::stream::fit(5120, 1440, w, h);
        expect(w == 2560 && h == 1440, "stream window: 2560 x 1440 where it fits");
        app::stream::fit(5104, 1361, w, h);
        expect(h == 1361 && w == 2419, "stream window: the same shape, smaller, under a title bar and a taskbar");
        app::stream::fit(1920, 1040, w, h);
        expect(w == 1848 && h == 1040, "stream window: fitted to a 1080-line screen by its height");
        app::stream::fit(1280, 1400, w, h);
        expect(w == 1280 && h == 720, "stream window: fitted by its width");
        Settings s;
        std::wstring error;
        expect(!s.stream_window && apply_override(s, L"stream.window=1", error) && s.stream_window,
               "stream.window: off unless asked for");
    }

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
