// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// AMD FSR frame generation as a frame generator for video (fg_bridge.h says
// how a Direct3D 12 feature for games is made to work on mpv's Direct3D 11
// pictures). It runs on any vendor's GPU.
//
// The FidelityFX API (third_party/fidelityfx, MIT) is called through AMD's
// signed libraries, loaded from the library folder at run time:
// amd_fidelityfx_loader_dx12.dll, which finds
// amd_fidelityfx_framegeneration_dx12.dll beside it. Frame generation is
// used on its own, without FSR's swap chain
// (FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY): it writes the frames
// into textures given to it and the app presents them.
//
// Each picture is a frame to it, numbered: a number that is not the one
// before plus one makes it start over, as does "reset".
#pragma once
#include "app_version.h"
#include "fg_bridge.h"

#include "api/include/dx12/ffx_api_dx12.h"
#include "framegeneration/include/ffx_framegeneration.h"

namespace fg {

class FsrFg final : public VendorGenerator {
public:
    ~FsrFg() override { stop(); }
    const char *name() const override { return "FSR Frame Generation"; }
    static constexpr const wchar_t *kLoader = L"amd_fidelityfx_loader_dx12.dll";
    static constexpr const wchar_t *kLibrary = L"amd_fidelityfx_framegeneration_dx12.dll";

protected:
    bool adapter_ok(const DXGI_ADAPTER_DESC &, std::string &) override { return true; }

    bool start_library(const std::wstring &, const std::wstring &library_dir, std::string &error) override {
        if (!loader_)
            loader_ = LoadLibraryExW((library_dir + L"\\" + kLoader).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!loader_) return error = "amd_fidelityfx_loader_dx12.dll did not load from " APP_NAME "'s engine folder", false;
        create_ = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(loader_, "ffxCreateContext"));
        destroy_ = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(loader_, "ffxDestroyContext"));
        configure_ = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(loader_, "ffxConfigure"));
        dispatch_ = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(loader_, "ffxDispatch"));
        if (!create_ || !destroy_ || !configure_ || !dispatch_)
            return error = "amd_fidelityfx_loader_dx12.dll lacks a function", false;
        max_between_ = kMaxBetween;  // it makes up to four
        return true;
    }

    bool create_feature(std::string &error) override {
        ffxCreateContextDescFrameGenerationVersion version{};
        version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
        version.version = FFX_FRAMEGENERATION_VERSION;
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.header.pNext = &version.header;
        backend.device = device_.Get();
        ffxCreateContextDescFrameGeneration create{};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        create.header.pNext = &backend.header;
        create.flags = FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS |
                       (hdr_ ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0);
        create.displaySize = {width_, height_};
        create.maxRenderSize = {width_, height_};
        create.backBufferFormat = ffxApiGetSurfaceFormatDX12(format_);
        const ffxReturnCode_t r = create_(&context_, &create.header, nullptr);
        if (r != FFX_API_RETURN_OK || !context_) {
            context_ = nullptr;
            return error = result("FSR frame generation did not start for this picture", r), false;
        }
        frame_ = 0;
        return true;
    }

    bool record(ID3D12Resource *picture, int count, bool restart, std::string &error) override {
        // A gap in the numbers starts it over, as "reset" does.
        frame_ += restart ? 8 : 1;
        const FfxApiRect2D whole{0, 0, int32_t(width_), int32_t(height_)};

        ffxConfigureDescFrameGeneration configure{};
        configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        configure.frameGenerationEnabled = true;
        configure.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
        configure.generationRect = whole;
        configure.frameID = frame_;
        ffxReturnCode_t r = configure_(&context_, &configure.header);
        if (r != FFX_API_RETURN_OK) return error = result("FSR frame generation refused its settings", r), false;

        // A still camera, a flat depth, no motion.
        ffxDispatchDescFrameGenerationPrepareV2 prepare{};
        prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        prepare.frameID = frame_;
        prepare.commandList = list_.Get();
        prepare.renderSize = {width_, height_};
        prepare.motionVectorScale = {1.0f, 1.0f};
        prepare.frameTimeDelta = 33.3f;
        prepare.reset = restart;
        prepare.cameraNear = kNear;
        prepare.cameraFar = kFar;
        prepare.cameraFovAngleVertical = kFovY;
        prepare.viewSpaceToMetersFactor = 1.0f;
        prepare.depth = ffxApiGetResourceDX12(depth_.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        prepare.motionVectors = ffxApiGetResourceDX12(vectors_.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        prepare.cameraUp[1] = 1.0f;
        prepare.cameraRight[0] = 1.0f;
        prepare.cameraForward[2] = 1.0f;
        r = dispatch_(&context_, &prepare.header);
        if (r != FFX_API_RETURN_OK) return error = result("FSR frame generation failed (prepare)", r), false;

        ffxDispatchDescFrameGeneration generate{};
        generate.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
        generate.commandList = list_.Get();
        generate.presentColor = ffxApiGetResourceDX12(picture, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        for (int i = 0; i < count; ++i)
            generate.outputs[i] = ffxApiGetResourceDX12(out_[i].Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        generate.numGeneratedFrames = uint32_t(count);
        generate.reset = restart;
        generate.backbufferTransferFunction =
            hdr_ ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ : FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        generate.minMaxLuminance[0] = hdr_ ? 0.001f : 0.0f;
        generate.minMaxLuminance[1] = hdr_ ? 1000.0f : 200.0f;
        generate.generationRect = whole;
        generate.frameID = frame_;
        r = dispatch_(&context_, &generate.header);
        if (r != FFX_API_RETURN_OK) return error = result("FSR frame generation failed", r), false;
        return true;
    }

    void release_feature() override {
        if (context_ && destroy_) destroy_(&context_, nullptr);
        context_ = nullptr;
    }

    // The library stays loaded: the next start uses it again.
    void stop_library() override {}

private:
    static std::string result(const char *what, ffxReturnCode_t r) {
        char text[200];
        snprintf(text, sizeof(text), "%s (FidelityFX %u)", what, unsigned(r));
        return text;
    }

    HMODULE loader_ = nullptr;
    PfnFfxCreateContext create_ = nullptr;
    PfnFfxDestroyContext destroy_ = nullptr;
    PfnFfxConfigure configure_ = nullptr;
    PfnFfxDispatch dispatch_ = nullptr;
    ffxContext context_ = nullptr;
    uint64_t frame_ = 0;
};

}  // namespace fg
