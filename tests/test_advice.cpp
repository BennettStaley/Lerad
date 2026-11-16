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

// A desktop with one RTX 5090, the Elgato and default settings, but SDR said
// (video.mode is auto by default; the tests of it say so).
AdviceInput desktop(const DisplayInfo &d) {
    AdviceInput in;
    in.settings.dlss5 = Settings::Dlss5::Auto;  // asked for (it is off by default)
    in.settings.hdr_auto = false;
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
        expect(shown(a, 30) && shown(a, 30)->multiplier == 2 && shown(a, 30)->shown_fps == 60 && shown(a, 60) &&
                   shown(a, 60)->multiplier == 2 && shown(a, 60)->shown_fps == 120 && !shown(a, 120),
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
                if (x.kind == AdviceKind::Console) return 4;
                return x.kind == AdviceKind::Recommendation && x.severity == Sev::Info ? 3 : 2 - int(x.severity);
            };
            if (rank(a.items[i - 1]) > rank(a.items[i])) expect(false, "items are sorted: problems first");
        }
        const std::wstring text = advice_text(a);
        expect(text.find(L"Capture: NV12 2560x1440 at 60 fps, Media Foundation") != std::wstring::npos &&
                   text.find(L"a 60 fps game at 120 fps (x2)") != std::wstring::npos &&
                   text.find(L"(rec-scaling)") != std::wstring::npos,
               "the advice as text");
    }
    {
        // video.mode = auto: the card's 10-bit mode for what is asked, shown
        // as the signal was last time; SDR on a card without one.
        AdviceInput in = desktop(screen(3840, 2160, 120));
        for (const wchar_t *o : {L"video.mode=auto", L"capture.width=3840", L"capture.height=2160", L"capture.fps=30"})
            apply_override(in.settings, o, error);
        Advice a = advise(in);
        expect(a.outcome.video.ten_bit && a.outcome.video.hdr && is(a.outcome.capture, L"P010", 3840, 2160, 30) &&
                   a.outcome.hdr == Outcome::Hdr::Hdr,
               "auto, HDR found last time: P010 3840x2160 at 30, sent as HDR");
        apply_override(in.settings, L"video.auto_found=sdr", error);
        a = advise(in);
        expect(a.outcome.video.ten_bit && !a.outcome.video.hdr && is(a.outcome.capture, L"P010", 3840, 2160, 30) &&
                   a.outcome.hdr == Outcome::Hdr::SdrCapture,
               "auto, SDR found last time: still the 10-bit mode, shown as SDR");
        in.modes = {mode(L"NV12", 3840, 2160, 30), mode(L"NV12", 1920, 1080, 60)};
        a = advise(in);
        expect(!a.outcome.video.ten_bit && !a.outcome.video.hdr && is(a.outcome.capture, L"NV12", 3840, 2160, 30),
               "auto on a card without a 10-bit mode: SDR in NV12");
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
        expect(a.outcome.shown.size() == 1 && shown(a, 30)->multiplier == 2 && shown(a, 30)->shown_fps == 60,
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
        expect(shown(a, 60) && shown(a, 60)->multiplier == 2 && shown(a, 60)->shown_fps == 120, "120 Hz: 60 -> 120 fits");

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
        expect(shown(a, 120) && shown(a, 120)->multiplier == 1 && shown(a, 120)->shown_fps == 120 && shown(a, 60)->multiplier == 2 &&
                   !has(a, "screen-60hz"),
               "a 144 fps capture on 144 Hz: 120 fps games pass as they are, 60 is doubled");
    }
    {
        // A 60 Hz screen.
        AdviceInput in = desktop(screen(1920, 1080, 59.94));
        Advice a = advise(in);
        expect(shown(a, 30)->multiplier == 2 && shown(a, 60)->multiplier == 1 && shown(a, 60)->shown_fps == 59.94 &&
                   has(a, "screen-60hz") && find_advice(a, "screen-60hz")->text.find(L"60 Hz") != std::wstring::npos,
               "60 Hz: 30 fps is doubled, 60 fps is not, and why");
        apply_override(in.settings, L"framegen.max_output_hz=30", error);
        a = advise(in);
        expect(has(a, "framegen-nothing-doubled") && shown(a, 30)->multiplier == 1, "a limit of 30 Hz: nothing is doubled");
        in = desktop(screen(2560, 1440, 144));
        apply_override(in.settings, L"framegen.max_output_hz=100", error);
        a = advise(in);
        expect(has(a, "screen-60hz") && find_advice(a, "screen-60hz")->text.find(L"max_output_hz") != std::wstring::npos,
               "framegen.max_output_hz below 120: 60 fps not doubled, the setting named");
        in = desktop(screen(2560, 1440, 144));
        apply_override(in.settings, L"frames.dedup=0", error);
        a = advise(in);
        expect(!a.outcome.framegen && suggests(a, "framegen-needs-dedup", L"frames.dedup=1") && shown(a, 30)->multiplier == 1,
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
                   has(a, "capture-directshow") && shown(a, 30)->multiplier == 1,
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
    {
        // More than x2: the most that fits, as the add-on's own rules say.
        // A 4K 120 Hz screen with VRR on, 4K30 HDR.
        AdviceInput in = desktop(screen(3840, 2160, 120));
        for (const wchar_t *o : {L"video.mode=hdr", L"capture.width=3840", L"capture.height=2160", L"capture.fps=30"})
            apply_override(in.settings, o, error);
        Advice a = advise(in);
        expect(a.outcome.multiplier_wanted == 2 && shown(a, 30)->multiplier == 2 && !a.outcome.capped &&
                   a.outcome.max_shown_hz == 120 && shown(a, 30)->cadence == FramegenStatus::Cadence::VrrEven &&
                   !has(a, "framegen-fewer-fit") && !has(a, "framegen-uneven") && !has(a, "framegen-vrr-ceiling"),
               "x2 by default: 30 -> 60 on 120 Hz with VRR, even, nothing to say");
        apply_override(in.settings, L"framegen.multiplier=3", error);
        a = advise(in);
        expect(shown(a, 30)->multiplier == 3 && shown(a, 30)->shown_fps == 90 &&
                   shown(a, 30)->cadence == FramegenStatus::Cadence::VrrEven,
               "up to x3 with VRR: 30 -> 90");
        apply_override(in.settings, L"framegen.multiplier=4", error);
        a = advise(in);
        expect(shown(a, 30)->multiplier == 4 && shown(a, 30)->shown_fps == 120 &&
                   shown(a, 30)->cadence == FramegenStatus::Cadence::VrrCeiling && has(a, "framegen-vrr-ceiling"),
               "up to x4 with VRR: 30 -> 120, which fills the screen (said)");
        expect(advice_text(a).find(L"a 30 fps game at 120 fps (x4); frame generation up to x4, at most 120 frames a "
                                   L"second (the screen's rate)") != std::wstring::npos,
               "the text names the multiplier and the limit");
        // max_output_hz below the screen's rate is kept exactly: 100 leaves x3.
        apply_override(in.settings, L"framegen.max_output_hz=100", error);
        a = advise(in);
        expect(a.outcome.capped && a.outcome.max_shown_hz == 100 && shown(a, 30)->multiplier == 3 &&
                   !shown(a, 30)->kept_even && has(a, "framegen-fewer-fit") &&
                   find_advice(a, "framegen-fewer-fit")->text.find(L"max_output_hz = 100") != std::wstring::npos,
               "a limit of 100 on 120 Hz: x3 (90), and why not x4");
        apply_override(in.settings, L"framegen.max_output_hz=90", error);
        a = advise(in);
        expect(shown(a, 30)->multiplier == 3, "a limit of 90: 90 fps still fits (the limit itself)");
        apply_override(in.settings, L"framegen.max_output_hz=89", error);
        a = advise(in);
        expect(shown(a, 30)->multiplier == 2, "a limit of 89: x2, no tolerance on a limit the user set");
        // At or above the screen's rate it changes nothing, and that is said.
        apply_override(in.settings, L"framegen.max_output_hz=240", error);
        a = advise(in);
        expect(!a.outcome.capped && a.outcome.max_shown_hz == 120 && shown(a, 30)->multiplier == 4 &&
                   suggests(a, "framegen-cap-no-effect", L"framegen.max_output_hz=0"),
               "a limit of 240 on a 120 Hz screen: no limit at all");

        // The same screen with VRR off: a fixed 120 Hz. x3 would be 90 fps,
        // which does not fall on whole refreshes: x2 is kept.
        in.display.vrr = DisplayInfo::Vrr::Off;
        apply_override(in.settings, L"framegen.max_output_hz=0", error);
        apply_override(in.settings, L"framegen.multiplier=3", error);
        a = advise(in);
        expect(a.outcome.vrr == L"off" && shown(a, 30)->multiplier == 2 && shown(a, 30)->kept_even &&
                   shown(a, 30)->cadence == FramegenStatus::Cadence::FixedEven && has(a, "framegen-fewer-fit") &&
                   find_advice(a, "framegen-fewer-fit")->text.find(L"whole refreshes") != std::wstring::npos &&
                   !has(a, "framegen-uneven"),
               "fixed 120 Hz, up to x3: x2 (2 refreshes each), and why");
        apply_override(in.settings, L"framegen.multiplier=4", error);
        a = advise(in);
        expect(shown(a, 30)->multiplier == 4 && shown(a, 30)->cadence == FramegenStatus::Cadence::FixedEven &&
                   !has(a, "framegen-fewer-fit"),
               "fixed 120 Hz, up to x4: 30 -> 120, a frame per refresh");
        // The user's word for VRR counts: display.vrr = on.
        apply_override(in.settings, L"framegen.multiplier=3", error);
        apply_override(in.settings, L"display.vrr=on", error);
        a = advise(in);
        expect(a.outcome.vrr == L"on" && shown(a, 30)->multiplier == 3, "display.vrr = on: x3 again");

        // A fixed 144 Hz screen: 60 and 120 fps do not divide it.
        in = desktop(screen(2560, 1440, 144));
        in.display.vrr = DisplayInfo::Vrr::Unsupported;
        a = advise(in);
        const AdviceItem *uneven = find_advice(a, "framegen-uneven");
        expect(shown(a, 30)->cadence == FramegenStatus::Cadence::FixedUneven && shown(a, 60)->better_hz == 120 &&
                   uneven && uneven->text.find(L"144 Hz") != std::wstring::npos,
               "fixed 144 Hz: 30 -> 60 and 60 -> 120 are uneven, said");
        // 60 Hz: nothing above x2 for 30 fps whatever is asked.
        in = desktop(screen(1920, 1080, 60));
        apply_override(in.settings, L"framegen.multiplier=4", error);
        a = advise(in);
        expect(shown(a, 30)->multiplier == 2 && shown(a, 60)->multiplier == 1 && has(a, "screen-60hz"),
               "60 Hz, up to x4: 30 -> 60 and no more");
        // A small GPU asked for x4: x2 is recommended; a large one is left alone.
        in.gpus = {gpu(L"Intel(R) UHD Graphics 770", kVendorIntel, 128)};
        in.gpu = choose_gpu(in.gpus, in.display.device, L"");
        a = advise(in);
        expect(suggests(a, "rec-framegen-multiplier", L"framegen.multiplier=2"), "a low-class GPU and x4: x2 advised");
        in = desktop(screen(3840, 2160, 120));
        apply_override(in.settings, L"framegen.multiplier=4", error);
        expect(!has(advise(in), "rec-framegen-multiplier"), "a high-class GPU and x4: its own choice");
    }
    {
        // HDR as hdr.h plans it: fitting to the screen.
        AdviceInput in = desktop(screen(3840, 2160, 120));
        in.display.peak_nits = 1000;
        apply_override(in.settings, L"video.mode=hdr", error);
        Advice a = advise(in);
        expect(a.outcome.hdr_plan.output == HdrPlan::Output::Hdr && !a.outcome.hdr_plan.fit &&
                   advice_text(a).find(L"HDR: HDR10 to the screen, as the console sends it") != std::wstring::npos &&
                   has(a, "console-hdr-calibration"),
               "HDR passed through: said as hdr.h says it, with the calibration hint");
        apply_override(in.settings, L"hdr.fit_to_screen=1", error);
        a = advise(in);
        expect(a.outcome.hdr_plan.fit && a.outcome.hdr_plan.peak_nits == 1000 && has(a, "hdr-fit-reported-peak") &&
                   advice_text(a).find(L"fitted to its 1000 nits") != std::wstring::npos &&
                   !has(a, "console-hdr-calibration"),
               "fit_to_screen: fitted to the peak the screen reports, which is said");
        apply_override(in.settings, L"hdr.display_peak_nits=800", error);
        a = advise(in);
        expect(a.outcome.hdr_plan.peak_nits == 800 && !has(a, "hdr-fit-reported-peak"), "a peak from glasir.ini: used");
        apply_override(in.settings, L"hdr.display_peak_nits=0", error);
        in.display.peak_nits = 0;
        a = advise(in);
        expect(!a.outcome.hdr_plan.fit && has(a, "hdr-fit-no-peak") &&
                   find_advice(a, "hdr-fit-no-peak")->severity == Sev::Warning,
               "fit_to_screen without any peak: not fitted, a warning");
    }
    {
        // What to set on the console.
        AdviceInput in = desktop(screen(3840, 2160, 120));
        for (const wchar_t *o : {L"video.mode=hdr", L"capture.width=3840", L"capture.height=2160", L"capture.fps=30"})
            apply_override(in.settings, o, error);
        Advice a = advise(in);
        const auto brief = [&a](const char *id) {
            const AdviceItem *i = find_advice(a, id);
            return i && i->kind == AdviceKind::Console ? i->brief : std::wstring(L"?");
        };
        expect(brief("console-resolution") == L"2160p" && brief("console-hdr") == L"HDR on" &&
                   brief("console-120hz") == L"120 Hz output off" && brief("console-vrr") == L"VRR off" &&
                   brief("console-hdr-calibration").empty(),
               "4K30 HDR: 2160p, HDR on, 120 Hz output off, VRR off, calibrate once");
        expect(a.items.back().kind == AdviceKind::Console &&
                   advice_text(a).find(L"Set your console to:\n  Video output resolution: 2160p") != std::wstring::npos,
               "the console's settings come last, under their own heading");
        in = desktop(screen(5120, 1440, 240));
        apply_override(in.settings, L"capture.width=2560", error);
        apply_override(in.settings, L"capture.height=1440", error);
        apply_override(in.settings, L"capture.fps=120", error);
        a = advise(in);
        expect(brief("console-resolution") == L"1440p" && brief("console-hdr") == L"HDR off" &&
                   brief("console-120hz") == L"120 Hz output on" && !has(a, "console-hdr-calibration"),
               "1440p120 SDR: 1440p, HDR off, 120 Hz output on");
        in.display.hdr_on = false;
        apply_override(in.settings, L"video.mode=hdr", error);
        apply_override(in.settings, L"capture.fps=60", error);
        a = advise(in);
        expect(brief("console-hdr") == L"HDR on" && !has(a, "console-hdr-calibration"),
               "HDR tone mapped to SDR: HDR on at the console, no calibration hint");
        in.card_found = false;
        a = advise(in);
        expect(!has(a, "console-resolution") && has(a, "console-vrr"), "no card: no size to name");
    }

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
