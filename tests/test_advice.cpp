// SPDX-License-Identifier: MIT
// Checks the advice: what will happen, the limits and the recommendations
// for a screen, a GPU, a card and settings.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "advice.h"

using namespace glasir;

namespace {

int g_failures = 0;

void expect(bool ok, const char *what) {
    std::printf("%-76s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_failures;
}

VideoMode mode(const wchar_t *subtype, UINT32 w, UINT32 h, double fps) {
    VideoMode m;
    m.subtype = subtype;
    m.width = w;
    m.height = h;
    m.fps = fps;
    for (PixelFormat c : {PixelFormat::NV12, PixelFormat::P010, PixelFormat::YUY2, PixelFormat::UYVY})
        if (m.subtype == format_name(c)) {
            m.format = c;
            m.supported = true;
        }
    return m;
}

// A card like the Elgato 4K X: SDR up to 4K144, HDR up to 1440p60 / 4K30 / 1080p120.
std::vector<VideoMode> elgato() {
    return {mode(L"NV12", 3840, 2160, 144), mode(L"NV12", 3840, 2160, 120), mode(L"NV12", 3840, 2160, 60),
            mode(L"NV12", 3840, 2160, 30),  mode(L"NV12", 2560, 1440, 144), mode(L"NV12", 2560, 1440, 120),
            mode(L"NV12", 2560, 1440, 60),  mode(L"NV12", 1920, 1080, 120), mode(L"NV12", 1920, 1080, 60),
            mode(L"P010", 3840, 2160, 30),  mode(L"P010", 2560, 1440, 60),  mode(L"P010", 1920, 1080, 120),
            mode(L"P010", 1920, 1080, 60),  mode(L"MJPG", 3840, 2160, 60),  mode(L"I420", 1920, 1080, 60)};
}

DisplayInfo screen(int w, int h, double hz) {
    DisplayInfo d;
    d.found = true;
    d.number = 1;
    d.device = L"\\\\.\\DISPLAY1";
    d.width = w;
    d.height = h;
    d.refresh_hz = hz;
    d.gpu_vendor = kVendorNvidia;
    d.hdr_supported = d.hdr_on = true;
    d.vrr = DisplayInfo::Vrr::On;
    d.vrr_windowed = DisplayInfo::Tri::Yes;
    return d;
}

GpuInfo gpu(const wchar_t *name, unsigned vendor, unsigned mb, bool with_screen = true) {
    GpuInfo g;
    g.name = name;
    g.vendor = vendor;
    g.found = true;
    g.device_id = vendor + mb;
    g.dedicated_mb = mb;
    if (with_screen) g.screens = {L"\\\\.\\DISPLAY1"};
    return g;
}

// A desktop with one RTX 5090, the Elgato and default settings.
AdviceInput desktop(const DisplayInfo &d) {
    AdviceInput in;
    in.display = d;
    in.gpus = {gpu(L"NVIDIA GeForce RTX 5090", kVendorNvidia, 32000)};
    in.gpu = choose_gpu(in.gpus, d.device, L"");
    in.card_found = true;
    in.card = L"Elgato 4K X";
    in.modes = elgato();
    return in;
}

bool is(const VideoMode &m, const wchar_t *subtype, UINT32 w, UINT32 h, double fps) {
    return m.subtype == subtype && m.width == w && m.height == h && std::fabs(m.fps - fps) < 0.01;
}

const ShownRate *shown(const Advice &a, double game) {
    for (const ShownRate &r : a.outcome.shown)
        if (std::fabs(r.game_fps - game) < 0.5) return &r;
    return nullptr;
}

bool suggests(const Advice &a, const char *id, const wchar_t *setting) {
    const AdviceItem *i = find_advice(a, id);
    if (!i) return false;
    for (const SettingValue &v : i->suggest)
        if (setting_text(v) == setting) return true;
    return false;
}

bool has(const Advice &a, const char *id) { return find_advice(a, id) != nullptr; }

}  // namespace

int main() {
    using Sev = AdviceSeverity;
    std::wstring error;
    {
        // An ultrawide 5120x1440 at 240 Hz, SDR, everything automatic.
        AdviceInput in = desktop(screen(5120, 1440, 240));
        const Advice a = advise(in);
        expect(a.outcome.capture_known && a.outcome.media_foundation && is(a.outcome.capture, L"NV12", 2560, 1440, 60),
               "automatic on a 1440-line screen: NV12 2560x1440 at 60");
        expect(a.outcome.picture_width == 2560 && a.outcome.picture_height == 1440 && !a.outcome.upscaled,
               "the picture's place on an ultrawide: 2560x1440, nothing to scale");
        expect(shown(a, 30) && shown(a, 30)->doubled && shown(a, 30)->shown_fps == 60 && shown(a, 60) &&
                   shown(a, 60)->doubled && shown(a, 60)->shown_fps == 120 && !shown(a, 120),
               "240 Hz: 30 -> 60 and 60 -> 120; no 120 fps games through a 60 fps capture");
        expect(a.outcome.framegen && a.outcome.max_shown_hz == 240 && a.outcome.vrr == L"on" && a.outcome.dlss5 &&
                   a.outcome.hdr == Outcome::Hdr::SdrCapture && a.outcome.gpu_class == GpuClass::High,
               "frame generation, VRR on, DLSS 5 on, an SDR capture, a high-class GPU");
        expect(std::fabs(a.outcome.raw_gb_per_s - 2560 * 1440 * 1.5 * 60 / 1e9) < 0.001 && !has(a, "raw-data-rate"),
               "raw NV12 1440p60 is 0.33 GB/s: not worth a line");
        const AdviceItem *rec = find_advice(a, "rec-capture");
        expect(rec && rec->applied && rec->kind == AdviceKind::Recommendation && rec->suggest.empty(),
               "the capture recommendation is what automatic already does");
        expect(suggests(a, "rec-scaling", L"video.scaling=mpv") && find_advice(a, "rec-scaling")->applied,
               "scaling: mpv's own when nothing is scaled up (already set)");
        expect(suggests(a, "rec-framegen-quality", L"framegen.quality=high") &&
                   suggests(a, "rec-framegen-method", L"framegen.method=motion"),
               "a high-class GPU: frame generation high, motion");
        expect(suggests(a, "capture-120", L"capture.fps=120") && suggests(a, "capture-120", L"capture.height=1440"),
               "a 120 Hz-capable screen and card: 1440p120 is offered for 120 Hz consoles");
        const AdviceItem *hdr = find_advice(a, "card-hdr-limit");
        expect(hdr && hdr->severity == Sev::Info && hdr->text.find(L"2560x1440 at 60 fps") != std::wstring::npos &&
                   hdr->text.find(L"3840x2160 at 30 fps") != std::wstring::npos &&
                   hdr->text.find(L"3840x2160 at 144 fps") != std::wstring::npos,
               "the card's HDR limit: 1440p60 (4K30 too), SDR up to 4K144");
        expect(!has(a, "vrr-off") && !has(a, "vrr-unknown") && !has(a, "screen-60hz") && !has(a, "dlss5-gpu"),
               "nothing to warn about");
        for (size_t i = 1; i < a.items.size(); ++i) {
            const auto rank = [](const AdviceItem &x) {
                return x.kind == AdviceKind::Recommendation && x.severity == Sev::Info ? 3 : 2 - int(x.severity);
            };
            if (rank(a.items[i - 1]) > rank(a.items[i])) expect(false, "items are sorted: problems first");
        }
        const std::wstring text = advice_text(a);
        expect(text.find(L"Capture: NV12 2560x1440 at 60 fps, Media Foundation") != std::wstring::npos &&
                   text.find(L"a 60 fps game at 120 fps (doubled)") != std::wstring::npos &&
                   text.find(L"(rec-scaling)") != std::wstring::npos,
               "the advice as text");
    }
    {
        // A 4K 120 Hz screen, HDR, PS5 at 4K30 through the card's only 4K HDR mode.
        AdviceInput in = desktop(screen(3840, 2160, 120));
        apply_override(in.settings, L"video.mode=hdr", error);
        apply_override(in.settings, L"capture.width=3840", error);
        apply_override(in.settings, L"capture.height=2160", error);
        apply_override(in.settings, L"capture.fps=30", error);
        Advice a = advise(in);
        expect(is(a.outcome.capture, L"P010", 3840, 2160, 30) && a.outcome.hdr == Outcome::Hdr::Hdr,
               "4K30 HDR as asked: P010 3840x2160 at 30, sent as HDR");
        expect(a.outcome.shown.size() == 1 && shown(a, 30)->doubled && shown(a, 30)->shown_fps == 60,
               "a 30 fps capture: 30 -> 60, and no faster game gets through");
        expect(std::fabs(a.outcome.raw_gb_per_s - 0.746) < 0.01 && !a.outcome.upscaled, "0.75 GB/s raw, not scaled");
        expect(suggests(a, "capture-30fps", L"capture.height=1440") && suggests(a, "capture-30fps", L"capture.fps=60") &&
                   find_advice(a, "capture-30fps")->severity == Sev::Info,
               "said once: games above 30 fps lose frames; 1440p60 HDR is there");
        in.display.hdr_on = false;
        a = advise(in);
        expect(a.outcome.hdr == Outcome::Hdr::ToneMapped && has(a, "hdr-off-in-windows") &&
                   find_advice(a, "hdr-off-in-windows")->severity == Sev::Warning,
               "HDR off in Windows: tone mapped, with a warning");
        apply_override(in.settings, L"hdr.output=hdr", error);
        a = advise(in);
        expect(a.outcome.hdr == Outcome::Hdr::Hdr && has(a, "hdr-forced") && !has(a, "hdr-off-in-windows"),
               "hdr.output = hdr with HDR off in Windows: sent anyway, with a warning");
        apply_override(in.settings, L"hdr.output=sdr", error);
        in.display.hdr_on = true;
        a = advise(in);
        expect(a.outcome.hdr == Outcome::Hdr::ToneMapped && !has(a, "hdr-forced"), "hdr.output = sdr: tone mapped");
        in.display.hdr_on = in.display.hdr_supported = false;
        apply_override(in.settings, L"hdr.output=auto", error);
        a = advise(in);
        expect(has(a, "hdr-no-screen") && find_advice(a, "hdr-no-screen")->severity == Sev::Info,
               "a screen without HDR: tone mapped, for information");

        // The same screen, HDR, automatic: 60 fps first.
        in = desktop(screen(3840, 2160, 120));
        apply_override(in.settings, L"video.mode=hdr", error);
        a = advise(in);
        expect(is(a.outcome.capture, L"P010", 2560, 1440, 60) && a.outcome.upscaled && a.outcome.picture_height == 2160,
               "automatic HDR on a 4K screen: 1440p60 (4K HDR is 30 fps only), scaled up");
        expect(suggests(a, "capture-sharper-30", L"capture.width=3840") &&
                   suggests(a, "capture-sharper-30", L"capture.fps=30"),
               "and 4K30 HDR is offered for 30 fps games");
        expect(suggests(a, "rec-scaling", L"video.scaling=cnn") && !find_advice(a, "rec-scaling")->applied,
               "scaled up on a high-class GPU: ArtCNN, not set yet");
        expect(shown(a, 60) && shown(a, 60)->doubled && shown(a, 60)->shown_fps == 120, "120 Hz: 60 -> 120 fits");

        // SDR 4K60 on a 1440-line screen: more than the screen has.
        in = desktop(screen(2560, 1440, 144));
        apply_override(in.settings, L"capture.width=3840", error);
        apply_override(in.settings, L"capture.height=2160", error);
        a = advise(in);
        expect(has(a, "capture-larger-than-screen") &&
                   find_advice(a, "capture-larger-than-screen")->severity == Sev::Warning &&
                   suggests(a, "capture-larger-than-screen", L"capture.width=2560"),
               "a 4K capture on a 1440p screen: a warning with the mode that fits");
        apply_override(in.settings, L"capture.fps=144", error);
        in.display = screen(3840, 2160, 144);
        a = advise(in);
        expect(has(a, "raw-data-rate") && a.outcome.raw_gb_per_s > 1.7, "raw NV12 4K144 is 1.8 GB/s: said");
        expect(shown(a, 120) && !shown(a, 120)->doubled && shown(a, 120)->shown_fps == 120 && shown(a, 60)->doubled &&
                   !has(a, "screen-60hz"),
               "a 144 fps capture on 144 Hz: 120 fps games pass as they are, 60 is doubled");
    }
    {
        // A 60 Hz screen.
        AdviceInput in = desktop(screen(1920, 1080, 59.94));
        Advice a = advise(in);
        expect(shown(a, 30)->doubled && !shown(a, 60)->doubled && shown(a, 60)->shown_fps == 59.94 &&
                   has(a, "screen-60hz") && find_advice(a, "screen-60hz")->text.find(L"60 Hz") != std::wstring::npos,
               "60 Hz: 30 fps is doubled, 60 fps is not, and why");
        apply_override(in.settings, L"framegen.max_output_hz=30", error);
        a = advise(in);
        expect(has(a, "framegen-nothing-doubled") && !shown(a, 30)->doubled, "a limit of 30 Hz: nothing is doubled");
        in = desktop(screen(2560, 1440, 144));
        apply_override(in.settings, L"framegen.max_output_hz=100", error);
        a = advise(in);
        expect(has(a, "screen-60hz") && find_advice(a, "screen-60hz")->text.find(L"max_output_hz") != std::wstring::npos,
               "framegen.max_output_hz below 120: 60 fps not doubled, the setting named");
        in = desktop(screen(2560, 1440, 144));
        apply_override(in.settings, L"frames.dedup=0", error);
        a = advise(in);
        expect(!a.outcome.framegen && suggests(a, "framegen-needs-dedup", L"frames.dedup=1") && !shown(a, 30)->doubled,
               "frame generation without dedup: off, with the fix");
        in = desktop(screen(2560, 1440, 144));
        in.reshade_installed = false;
        a = advise(in);
        expect(!a.outcome.framegen && !a.outcome.dlss5 && has(a, "framegen-no-reshade"), "no ReShade: neither");
    }
    {
        // Variable refresh.
        AdviceInput in = desktop(screen(2560, 1440, 144));
        in.display.vrr = DisplayInfo::Vrr::Off;
        in.display.vrr_kind = L"FreeSync";
        in.display.vrr_min_hz = 48;
        in.display.vrr_max_hz = 144;
        Advice a = advise(in);
        expect(a.outcome.vrr == L"off" && has(a, "vrr-off") && find_advice(a, "vrr-off")->severity == Sev::Warning &&
                   find_advice(a, "vrr-off")->text.find(L"FreeSync (48-144 Hz)") != std::wstring::npos &&
                   find_advice(a, "vrr-off")->text.find(L"NVIDIA Control Panel") != std::wstring::npos,
               "VRR switched off on an NVIDIA screen: a warning that says where to turn it on");
        apply_override(in.settings, L"display.vrr=on", error);
        a = advise(in);
        expect(a.outcome.vrr == L"on" && !has(a, "vrr-off") && suggests(a, "vrr-override", L"display.vrr=auto"),
               "display.vrr = on against what was found: on, and said");
        in.settings.vrr = Settings::Vrr::Auto;
        in.display.vrr = DisplayInfo::Vrr::Supported;
        in.display.gpu_vendor = kVendorAmd;
        a = advise(in);
        expect(a.outcome.vrr == L"unknown" && has(a, "vrr-not-confirmed") &&
                   find_advice(a, "vrr-not-confirmed")->suggest.empty(),
               "the screen can, the GPU's state is not read (AMD): unknown, nothing suggested blindly");
        in.display.vrr = DisplayInfo::Vrr::Unknown;
        a = advise(in);
        expect(a.outcome.vrr == L"unknown" && has(a, "vrr-unknown"), "nothing known: unknown, said");
        in.display.vrr = DisplayInfo::Vrr::Unsupported;
        a = advise(in);
        expect(a.outcome.vrr == L"off" && has(a, "vrr-none") && find_advice(a, "vrr-none")->severity == Sev::Info,
               "no VRR on the screen: pacing is less even, for information");
        in.display.vrr = DisplayInfo::Vrr::On;
        in.display.vrr_windowed = DisplayInfo::Tri::No;
        a = advise(in);
        expect(a.outcome.vrr == L"on" && has(a, "vrr-fullscreen-only"), "G-SYNC for full screen only: a warning");
        in.display.found = false;
        a = advise(in);
        expect(a.outcome.vrr == L"unknown" && !has(a, "vrr-unknown") && a.outcome.picture_height == 0,
               "no screen found: unknown, and nothing claimed about it");
    }
    {
        // GPUs.
        AdviceInput in = desktop(screen(2560, 1440, 144));
        in.gpus = {gpu(L"Intel(R) Iris(R) Xe Graphics", kVendorIntel, 128),
                   gpu(L"NVIDIA GeForce RTX 4060 Laptop GPU", kVendorNvidia, 8000, false)};
        in.gpu = choose_gpu(in.gpus, in.display.device, L"");
        in.display.gpu_vendor = kVendorIntel;
        apply_override(in.settings, L"capture.height=1080", error);
        apply_override(in.settings, L"capture.width=1920", error);
        Advice a = advise(in);
        expect(!a.outcome.dlss5 && has(a, "dlss5-gpu") && find_advice(a, "dlss5-gpu")->severity == Sev::Info &&
                   suggests(a, "gpu-has-rtx", L"display.gpu=NVIDIA GeForce RTX 4060 Laptop GPU"),
               "hybrid laptop: no DLSS 5 on the integrated GPU; the RTX is offered, not taken");
        expect(a.outcome.gpu_class == GpuClass::Low && suggests(a, "rec-framegen-quality", L"framegen.quality=fast") &&
                   suggests(a, "rec-scaling", L"video.scaling=fsr") &&
                   suggests(a, "rec-framegen-method", L"framegen.method=motion"),
               "a low-class GPU: frame generation fast, FSR 1");
        in.settings.gpu = L"RTX";
        in.gpu = choose_gpu(in.gpus, in.display.device, in.settings.gpu);
        a = advise(in);
        expect(a.outcome.dlss5 && has(a, "gpu-other-than-screen") && !has(a, "gpu-has-rtx") &&
                   find_advice(a, "gpu-other-than-screen")->severity == Sev::Warning &&
                   suggests(a, "rec-scaling", L"video.scaling=vsr") &&
                   suggests(a, "rec-framegen-quality", L"framegen.quality=fast"),
               "display.gpu = RTX: DLSS 5, the copy between GPUs warned; mid-class with DLSS 5: VSR, fast");
        in.settings.gpu = L"Voodoo";
        in.gpu = choose_gpu(in.gpus, in.display.device, in.settings.gpu);
        a = advise(in);
        expect(has(a, "gpu-setting-not-found"), "display.gpu names no GPU: said");

        in = desktop(screen(3840, 2160, 60));
        in.gpus = {gpu(L"AMD Radeon(TM) Graphics", kVendorAmd, 512)};
        in.gpu = choose_gpu(in.gpus, in.display.device, L"");
        apply_override(in.settings, L"video.scaling=vsr", error);
        apply_override(in.settings, L"dlss5.enabled=on", error);
        a = advise(in);
        expect(suggests(a, "vsr-unavailable", L"video.scaling=mpv") &&
                   find_advice(a, "dlss5-gpu")->severity == Sev::Warning && a.outcome.dlss5,
               "VSR on AMD cannot run (a 4K capture needs no scaling); dlss5 = on off an RTX is tried, warned");
        expect(suggests(a, "rec-framegen-method", L"framegen.method=blend"),
               "an integrated GPU drawing 4K: blend is the one that may keep up");
        in.gpus = {gpu(L"NVIDIA GeForce RTX 5070", kVendorNvidia, 12000)};
        in.gpu = choose_gpu(in.gpus, in.display.device, L"");
        in.settings.dlss5 = Settings::Dlss5::Auto;
        in.dlss5_installed = false;
        a = advise(in);
        expect(!a.outcome.dlss5 && has(a, "dlss5-not-downloaded") && !has(a, "dlss5-gpu"),
               "an RTX without the DLSS 5 download: off, and where to get it");
        in.gpus.clear();
        in.gpu = choose_gpu(in.gpus, in.display.device, L"");
        a = advise(in);
        expect(a.outcome.gpu_class == GpuClass::Unknown && suggests(a, "rec-framegen-quality", L"framegen.quality=fast") &&
                   !has(a, "dlss5-gpu"),
               "no GPU found: the careful settings, nothing claimed");
    }
    {
        // Cards.
        AdviceInput in = desktop(screen(2560, 1440, 144));
        in.modes = {mode(L"NV12", 1920, 1080, 60), mode(L"YUY2", 1920, 1080, 60)};
        Advice a = advise(in);
        expect(has(a, "card-no-hdr") && a.outcome.upscaled && is(a.outcome.capture, L"NV12", 1920, 1080, 60),
               "a 1080p SDR card: no HDR, scaled up");
        apply_override(in.settings, L"video.mode=hdr", error);
        a = advise(in);
        expect(!a.outcome.capture_known && suggests(a, "capture-no-hdr-mode", L"video.mode=sdr") &&
                   a.items.front().id == "capture-no-hdr-mode" && a.items.front().severity == Sev::Problem,
               "HDR asked of it: a problem, first in the list, with the way out");
        in = desktop(screen(2560, 1440, 144));
        apply_override(in.settings, L"capture.width=1280", error);
        apply_override(in.settings, L"capture.height=720", error);
        a = advise(in);
        expect(!a.outcome.capture_known && suggests(a, "capture-no-mode", L"capture.width=0"),
               "a size the card does not have: a problem; automatic is the way out");
        in = desktop(screen(2560, 1440, 144));
        in.modes = {mode(L"MJPG", 1920, 1080, 60)};
        a = advise(in);
        expect(a.outcome.capture_known && !a.outcome.media_foundation && !a.outcome.framegen &&
                   has(a, "capture-directshow") && !shown(a, 30)->doubled,
               "a card with MJPEG only: DirectShow, no frame generation");
        in.modes.clear();
        a = advise(in);
        expect(!a.outcome.capture_known && !a.outcome.media_foundation && has(a, "capture-directshow"),
               "a card only DirectShow sees: the same, mode unknown");
        in.card_found = false;
        a = advise(in);
        expect(a.items.front().id == "no-card" && a.items.front().severity == Sev::Problem, "no card: a problem");
        in = desktop(screen(2560, 1440, 144));
        in.monitor_matched = false;
        in.settings.monitor = L"7";
        a = advise(in);
        expect(has(a, "monitor-not-found"), "display.monitor names no screen: said");
    }

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
