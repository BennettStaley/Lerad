// SPDX-License-Identifier: MIT
// The PC's GPUs, which one mpv draws on, and what that one can do (the
// NVIDIA-only features, and a rough class for the recommendations).
#pragma once
#include <string>
#include <vector>

namespace glasir {

struct GpuInfo {
    std::wstring name;  // e.g. "NVIDIA GeForce RTX 5070 Ti" (Windows' description of the adapter)
    unsigned vendor = 0;
    bool found = false;
    unsigned device_id = 0;
    unsigned dedicated_mb = 0;          // video memory of its own; an integrated GPU has little or none
    std::vector<std::wstring> screens;  // the outputs it drives ("\\.\DISPLAY1"), as DisplayInfo::device
};

constexpr unsigned kVendorNvidia = 0x10DE;
constexpr unsigned kVendorIntel = 0x8086;
constexpr unsigned kVendorAmd = 0x1002;

// The hardware GPUs, in Windows' order. Left out: software adapters
// (Microsoft Basic Render Driver) and a second adapter with the same name
// and device as an earlier one that drives no screen. Virtual display
// drivers show up as such a twin of the real GPU; so would a second
// identical card, which mpv could not be told apart from the first by name
// anyway.
std::vector<GpuInfo> list_gpus();

// Which GPU mpv draws on.
struct GpuChoice {
    GpuInfo gpu;         // the one mpv draws on; found = false: no GPU was found (mpv picks)
    GpuInfo screen_gpu;  // the one the screen is plugged into; found = false: unknown
    bool by_setting = false;         // glasir.ini's display.gpu named it
    bool setting_not_found = false;  // display.gpu names no GPU of this PC: the screen's is used
    // gpu is not the screen's: every frame is copied to the screen's GPU
    // (a hybrid laptop with display.gpu set, or a desktop with the screen on
    // the wrong card). It works, with some latency, and VRR may not hold.
    bool other_than_screen = false;
};

// The GPU for a screen (DisplayInfo::device) and glasir.ini's display.gpu
// (a part of the name, any case; empty: the screen's own GPU, as drawing
// where the screen is plugged in is the direct path: no copy between GPUs).
// Without a GPU for the screen, the first one. Glasir never moves to a
// faster GPU by itself: on a hybrid laptop the advice (advice.h) says when
// display.gpu would get DLSS 5, and what it costs.
GpuChoice choose_gpu(const std::vector<GpuInfo> &gpus, const std::wstring &screen_device, const std::wstring &wanted);

// choose_gpu for the primary screen without a setting: the GPU of a PC that
// has not been set up.
GpuInfo display_gpu();

// DLSS 5 neural rendering runs on RTX 30, 40 and 50 series GPUs (NVIDIA's
// nvngx_dlssnr.dll 310.9.16 and newer; 310.8 ran on RTX 50 only).
bool dlss5_gpu(const GpuInfo &gpu);

// The video super resolution the GPU's video processor may have, as mpv's
// d3d11vpp scaling-mode names it: "nvidia" (RTX), "intel", or "none". The
// driver has the last word: glasir_scaling.lua falls back to FSR 1 if it
// turns it down.
const wchar_t *vsr_kind(const GpuInfo &gpu);

// How much GPU there is, for the recommendations. A guess from the name and
// the video memory, nothing measured: Low is an integrated or small GPU,
// High one with room for the heaviest settings at once.
enum class GpuClass { Unknown, Low, Mid, High };
GpuClass gpu_class(const GpuInfo &gpu);
const wchar_t *gpu_class_name(GpuClass c);  // "unknown", "low", "mid", "high"

// One line for the log and --system: name, memory, class, its screens.
std::wstring gpu_text(const GpuInfo &gpu);

}  // namespace glasir
