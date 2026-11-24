// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// NVIDIA DLSS Frame Generation as a frame generator for video (fg_bridge.h
// says how a Direct3D 12 feature for games is made to work on mpv's Direct3D
// 11 pictures).
//
// With true motion vectors, zeros, or vectors of the wrong sign it made the
// same frame (RTX 5090, driver 616.92, 11-18): it finds the motion itself.
//
// NGX is the driver's (_nvngx.dll, found through the registry key the driver
// writes); nothing of NVIDIA's is built in. The few structures and names
// below are from NVIDIA's DLSS SDK headers (github.com/NVIDIA/DLSS:
// nvsdk_ngx_defs.h, nvsdk_ngx_params.h, nvsdk_ngx_defs_dlssg.h), written out
// here so the SDK is not needed to build. The model itself (nvngx_dlssg.dll,
// from github.com/NVIDIA/DLSS) has to be in the library folder: the driver
// carries one too, but NGX does not give it to a program that brings none
// ("feature not found", 0xBAD00004; driver 616.92 with its 310.2.1).
//
// NGX is ended with NVSDK_NGX_D3D12_Shutdown: Shutdown1, called on the
// driver's library directly, crashed.
//
// Every picture has to be given once for each frame in between (index 1 to
// count), also the first of a sequence: given index 1 alone, the next
// picture's frames came out as that picture itself.
#pragma once
#include "app_version.h"
#include "fg_bridge.h"

namespace fg {

namespace ngx {

using Result = unsigned;
inline bool ok(Result r) { return (r & 0xFFF00000u) != 0xBAD00000u; }

// NVSDK_NGX_Parameter: the same virtual functions in the same order, so the
// compiler lays the table out as NVIDIA's does.
struct Parameter {
    virtual void Set(const char *name, unsigned long long value) = 0;
    virtual void Set(const char *name, float value) = 0;
    virtual void Set(const char *name, double value) = 0;
    virtual void Set(const char *name, unsigned int value) = 0;
    virtual void Set(const char *name, int value) = 0;
    virtual void Set(const char *name, ID3D11Resource *value) = 0;
    virtual void Set(const char *name, ID3D12Resource *value) = 0;
    virtual void Set(const char *name, void *value) = 0;
    virtual Result Get(const char *name, unsigned long long *value) const = 0;
    virtual Result Get(const char *name, float *value) const = 0;
    virtual Result Get(const char *name, double *value) const = 0;
    virtual Result Get(const char *name, unsigned int *value) const = 0;
    virtual Result Get(const char *name, int *value) const = 0;
    virtual Result Get(const char *name, ID3D11Resource **value) const = 0;
    virtual Result Get(const char *name, ID3D12Resource **value) const = 0;
    virtual Result Get(const char *name, void **value) const = 0;
    virtual void Reset() = 0;
};
struct Handle;

struct FeatureCommonInfo {  // NVSDK_NGX_FeatureCommonInfo
    const wchar_t *const *paths;  // where else to look for the model
    unsigned path_count;
    void *internal;
    void *log_callback;
    int log_level;
    bool log_only_callback;
};

constexpr int kFrameGeneration = 11;  // NVSDK_NGX_Feature_FrameGeneration
constexpr int kApiVersion = 0x15;     // NVSDK_NGX_Version_API
constexpr int kCustomEngine = 0;      // NVSDK_NGX_ENGINE_TYPE_CUSTOM

using Init = Result (*)(const char *project, int engine, const char *engine_version, const wchar_t *data_path,
                        ID3D12Device *device, int sdk_version, const FeatureCommonInfo *info);
using GetParameters = Result (*)(Parameter **out);
using DestroyParameters = Result (*)(Parameter *parameters);
using CreateFeature = Result (*)(ID3D12GraphicsCommandList *list, int feature, Parameter *parameters, Handle **out);
using EvaluateFeature = Result (*)(ID3D12GraphicsCommandList *list, const Handle *feature,
                                   const Parameter *parameters, void *progress);
using ReleaseFeature = Result (*)(Handle *feature);
using Shutdown = Result (*)();

}  // namespace ngx

// A frame generation with NGX's Direct3D 12 calls and DLSS Frame Generation's
// parameter names: NVIDIA's own through the driver (DlssFg below), or another
// library that takes its place in games (fg_anr.h). The derived class loads
// it and fills in the functions and parameters_.
class NgxFg : public VendorGenerator {
protected:
    bool create_feature(std::string &error) override {
        ngx::Parameter &p = *parameters_;
        p.Set("CreationNodeMask", 1u);
        p.Set("VisibilityNodeMask", 1u);
        p.Set("Width", width_);  // the names before DLSSG.Width, for an older model
        p.Set("Height", height_);
        p.Set("DLSSG.Width", width_);
        p.Set("DLSSG.Height", height_);
        p.Set("DLSSG.BackbufferFormat", unsigned(format_));
        p.Set("DLSSG.InternalWidth", width_);
        p.Set("DLSSG.InternalHeight", height_);
        p.Set("DLSSG.DynamicResolution", 0u);
        const ngx::Result r = create_feature_(list_.Get(), ngx::kFrameGeneration, parameters_, &feature_);
        if (!ngx::ok(r) || !feature_) {
            feature_ = nullptr;
            return error = result(" did not start for this picture", r), false;
        }
        if (aux_state_ != kShaderRead) {
            transition(depth_.Get(), kShaderRead, aux_state_);
            transition(vectors_.Get(), kShaderRead, aux_state_);
        }
        // A still camera looking down +z, 90 degrees across.
        const float aspect = float(width_) / float(height_);
        const float x = 1.0f / aspect, a = kFar / (kFar - kNear), b = -kNear * kFar / (kFar - kNear);
        const float view_to_clip[4][4] = {{x, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, a, 1}, {0, 0, b, 0}};
        const float clip_to_view[4][4] = {{1 / x, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 0, 1 / b}, {0, 0, 1, -a / b}};
        const float identity[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
        memcpy(view_to_clip_, view_to_clip, sizeof(view_to_clip));
        memcpy(clip_to_view_, clip_to_view, sizeof(clip_to_view));
        memcpy(identity_, identity, sizeof(identity));
        return true;
    }

    bool record(ID3D12Resource *picture, int count, bool restart, std::string &error) override {
        if (picture_state_ != kShaderRead) transition(picture, kShaderRead, picture_state_);
        bool ok = true;
        for (int index = 1; index <= count && ok; ++index) {
            const ngx::Result r = evaluate(picture, out_[index - 1].Get(), count, index, restart);
            if (!ngx::ok(r)) error = result(" failed", r), ok = false;
        }
        if (picture_state_ != kShaderRead) transition(picture, picture_state_, kShaderRead);
        return ok;
    }

    void release_feature() override {
        if (feature_ && release_feature_) release_feature_(feature_);
        feature_ = nullptr;
    }

    D3D12_RESOURCE_STATES motion_state() const override { return aux_state_; }

    std::string result(const char *what, ngx::Result r) const {
        char text[240];
        snprintf(text, sizeof(text), "%s%s (NGX 0x%08X)", name(), what, r);
        return text;
    }

    ngx::CreateFeature create_feature_ = nullptr;
    ngx::EvaluateFeature evaluate_ = nullptr;
    ngx::ReleaseFeature release_feature_ = nullptr;
    ngx::Parameter *parameters_ = nullptr;
    ngx::Handle *feature_ = nullptr;
    // The states the library is given the picture in, and the depth and
    // motion vectors (the frames it makes are always UNORDERED_ACCESS).
    D3D12_RESOURCE_STATES picture_state_ = kShaderRead, aux_state_ = kShaderRead;

private:
    // Records one frame in between: `index` of `count`, between the picture
    // given before and `picture`.
    ngx::Result evaluate(ID3D12Resource *picture, ID3D12Resource *made, int count, int index, bool reset) {
        ngx::Parameter &p = *parameters_;
        p.Set("DLSSG.Backbuffer", picture);
        p.Set("DLSSG.MVecs", motion_ ? motion_.Get() : vectors_.Get());
        p.Set("DLSSG.Depth", depth_.Get());
        p.Set("DLSSG.HUDLess", static_cast<ID3D12Resource *>(nullptr));
        p.Set("DLSSG.UI", static_cast<ID3D12Resource *>(nullptr));
        p.Set("DLSSG.UIAlpha", static_cast<ID3D12Resource *>(nullptr));
        p.Set("DLSSG.BidirectionalDistortionField", static_cast<ID3D12Resource *>(nullptr));
        p.Set("DLSSG.OutputInterpolated", made);
        p.Set("DLSSG.OutputReal", static_cast<ID3D12Resource *>(nullptr));
        p.Set("DLSSG.OutputDisableInterpolation", static_cast<ID3D12Resource *>(nullptr));
        p.Set("DLSSG.MultiFrameCount", unsigned(count));
        p.Set("DLSSG.MultiFrameIndex", unsigned(index));
        p.Set("DLSSG.CameraViewToClip", static_cast<void *>(view_to_clip_));
        p.Set("DLSSG.ClipToCameraView", static_cast<void *>(clip_to_view_));
        p.Set("DLSSG.ClipToLensClip", static_cast<void *>(identity_));
        p.Set("DLSSG.ClipToPrevClip", static_cast<void *>(identity_));
        p.Set("DLSSG.PrevClipToClip", static_cast<void *>(identity_));
        p.Set("DLSSG.JitterOffsetX", 0.0f);
        p.Set("DLSSG.JitterOffsetY", 0.0f);
        // The vectors times this are pixels: set_motion()'s are in pixels.
        // (The zeros had 1 / size here from the start; kept as measured.)
        p.Set("DLSSG.MvecScaleX", motion_ ? 1.0f : 1.0f / float(width_));
        p.Set("DLSSG.MvecScaleY", motion_ ? 1.0f : 1.0f / float(height_));
        p.Set("DLSSG.CameraPinholeOffsetX", 0.0f);
        p.Set("DLSSG.CameraPinholeOffsetY", 0.0f);
        static const char *const kPosition[3] = {"DLSSG.CameraPosX", "DLSSG.CameraPosY", "DLSSG.CameraPosZ"};
        static const char *const kUp[3] = {"DLSSG.CameraUpX", "DLSSG.CameraUpY", "DLSSG.CameraUpZ"};
        static const char *const kRight[3] = {"DLSSG.CameraRightX", "DLSSG.CameraRightY", "DLSSG.CameraRightZ"};
        static const char *const kForward[3] = {"DLSSG.CameraFwdX", "DLSSG.CameraFwdY", "DLSSG.CameraFwdZ"};
        for (int i = 0; i < 3; ++i) {
            p.Set(kPosition[i], 0.0f);
            p.Set(kUp[i], i == 1 ? 1.0f : 0.0f);
            p.Set(kRight[i], i == 0 ? 1.0f : 0.0f);
            p.Set(kForward[i], i == 2 ? 1.0f : 0.0f);
        }
        p.Set("DLSSG.CameraNear", kNear);
        p.Set("DLSSG.CameraFar", kFar);
        p.Set("DLSSG.CameraFOV", kFovY);
        p.Set("DLSSG.CameraAspectRatio", float(width_) / float(height_));
        p.Set("DLSSG.ColorBuffersHDR", hdr_ ? 1u : 0u);
        p.Set("DLSSG.DepthInverted", 0u);
        p.Set("DLSSG.CameraMotionIncluded", 1u);
        p.Set("DLSSG.Reset", reset ? 1u : 0u);
        p.Set("DLSSG.AutomodeOverrideReset", 0u);
        p.Set("DLSSG.NotRenderingGameFrames", 0u);
        p.Set("DLSSG.OrthoProjection", 0u);
        p.Set("DLSSG.MvecInvalidValue", -32768.0f);
        p.Set("DLSSG.MvecDilated", 0u);
        p.Set("DLSSG.MenuDetectionEnabled", 0u);
        return evaluate_(list_.Get(), feature_, parameters_, nullptr);
    }
    float view_to_clip_[4][4] = {}, clip_to_view_[4][4] = {}, identity_[4][4] = {};
};

class DlssFg final : public NgxFg {
public:
    ~DlssFg() override { stop(); }
    const char *name() const override { return "DLSS Frame Generation"; }
    static constexpr const wchar_t *kModel = L"nvngx_dlssg.dll";

protected:
    bool adapter_ok(const DXGI_ADAPTER_DESC &adapter, std::string &error) override {
        if (adapter.VendorId == 0x10DE) return true;
        return error = "not an NVIDIA GPU", false;
    }

    bool start_library(const std::wstring &data_dir, const std::wstring &library_dir, std::string &error) override {
        wchar_t folder[MAX_PATH] = {};
        DWORD bytes = sizeof(folder) - sizeof(wchar_t);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore", L"NGXPath",
                         RRF_RT_REG_SZ, nullptr, folder, &bytes) != ERROR_SUCCESS)
            return error = "the NVIDIA driver has no NGX (too old)", false;
        ngx_ = LoadLibraryExW((std::wstring(folder) + L"\\_nvngx.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!ngx_) return error = "the NVIDIA driver's NGX did not load", false;
        const auto init = get<ngx::Init>("NVSDK_NGX_D3D12_Init_ProjectID");
        const auto capabilities = get<ngx::GetParameters>("NVSDK_NGX_D3D12_GetCapabilityParameters");
        destroy_parameters_ = get<ngx::DestroyParameters>("NVSDK_NGX_D3D12_DestroyParameters");
        create_feature_ = get<ngx::CreateFeature>("NVSDK_NGX_D3D12_CreateFeature");
        evaluate_ = get<ngx::EvaluateFeature>("NVSDK_NGX_D3D12_EvaluateFeature");
        release_feature_ = get<ngx::ReleaseFeature>("NVSDK_NGX_D3D12_ReleaseFeature");
        shutdown_ = get<ngx::Shutdown>("NVSDK_NGX_D3D12_Shutdown");
        if (!init || !capabilities || !destroy_parameters_ || !create_feature_ || !evaluate_ || !release_feature_ ||
            !shutdown_)
            return error = "the NVIDIA driver's NGX lacks a function (too old)", false;

        model_dir_ = library_dir;
        model_paths_[0] = model_dir_.c_str();
        ngx::FeatureCommonInfo info{};
        if (!model_dir_.empty()) {
            info.paths = model_paths_;
            info.path_count = 1;
        }
        // A GUID of the app's own: with a custom engine NVIDIA hands out none.
        ngx::Result r = init("6f0d6f0e-51a8-4f0b-9c58-0b5a5d1c7a21", ngx::kCustomEngine, "1", data_dir.c_str(),
                             device_.Get(), ngx::kApiVersion, &info);
        if (!ngx::ok(r)) return error = result(": NGX did not start", r), false;
        ngx_started_ = true;
        r = capabilities(&parameters_);
        if (!ngx::ok(r) || !parameters_) return error = result(": NGX gave no parameters", r), false;
        int available = 0, needs_driver = 0;
        unsigned most = 0;
        parameters_->Get("FrameGeneration.Available", &available);
        parameters_->Get("FrameGeneration.NeedsUpdatedDriver", &needs_driver);
        parameters_->Get("DLSSG.MultiFrameCountMax", &most);
        if (!available)
            return error = needs_driver ? "DLSS Frame Generation needs a newer NVIDIA driver"
                                        : "DLSS Frame Generation is not available: it needs a GeForce RTX 40 series "
                                          "or newer, and nvngx_dlssg.dll in " APP_NAME "'s engine folder",
                   false;
        max_between_ = most < 1 ? 1 : int(most);
        return true;
    }

    void stop_library() override {
        if (parameters_ && destroy_parameters_) destroy_parameters_(parameters_);
        parameters_ = nullptr;
        if (ngx_started_ && shutdown_) shutdown_();
        ngx_started_ = false;
        // The driver's NGX stays loaded: it leaves threads behind that a
        // FreeLibrary would pull the code from under.
        ngx_ = nullptr;
    }

private:
    template <class F>
    F get(const char *name) const {
        return reinterpret_cast<F>(GetProcAddress(ngx_, name));
    }
    HMODULE ngx_ = nullptr;
    ngx::DestroyParameters destroy_parameters_ = nullptr;
    ngx::Shutdown shutdown_ = nullptr;
    bool ngx_started_ = false;
    std::wstring model_dir_;
    const wchar_t *model_paths_[1] = {};
};

}  // namespace fg
