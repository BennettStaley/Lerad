// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Apex Neural Rendering's frame generation (ANR FG, github.com/apex-compute/ANR)
// as a frame generator for video.
//
// ANR FG is made to take DLSS Frame Generation's place in a game: its library
// is an nvngx_dlssg.dll with NGX's Direct3D 12 functions of its own, called
// directly (no NVIDIA driver, any GPU it knows). So it runs on fg_dlss.h's
// calls and parameter names, with three differences its replay tool's notes
// give (record/replay/README.md in its repository):
//  - it is started with NVSDK_NGX_D3D12_Init_Ext and brings no parameter
//    object: the caller keeps the map and the library fills in what it offers
//    (NVSDK_NGX_D3D12_PopulateParameters_Impl);
//  - the picture is handed over as NON_PIXEL_SHADER_RESOURCE, depth and
//    motion vectors as COPY_DEST;
//  - it is ended with NVSDK_NGX_D3D12_Shutdown1.
//
// Nothing of Apex's is built in or shipped: the library and the files beside
// it (kFolder under the library folder) are downloaded by setup. It is a
// beta without a licence text (11-21).
#pragma once
#include "fg_dlss.h"

#include <string>
#include <unordered_map>

namespace fg {

namespace ngx {

// The parameters by name, as a game's side keeps them for such a library:
// a number can be read as any number type, a pointer as its own type or as
// void *.
class ParameterMap final : public Parameter {
public:
    void Set(const char *name, unsigned long long value) override { number(name, double(value)); }
    void Set(const char *name, float value) override { number(name, value); }
    void Set(const char *name, double value) override { number(name, value); }
    void Set(const char *name, unsigned int value) override { number(name, value); }
    void Set(const char *name, int value) override { number(name, value); }
    void Set(const char *name, ID3D11Resource *value) override { pointer(name, value, Kind::D3D11); }
    void Set(const char *name, ID3D12Resource *value) override { pointer(name, value, Kind::D3D12); }
    void Set(const char *name, void *value) override { pointer(name, value, Kind::Any); }
    Result Get(const char *name, unsigned long long *value) const override { return number(name, value); }
    Result Get(const char *name, float *value) const override { return number(name, value); }
    Result Get(const char *name, double *value) const override { return number(name, value); }
    Result Get(const char *name, unsigned int *value) const override { return number(name, value); }
    Result Get(const char *name, int *value) const override { return number(name, value); }
    Result Get(const char *name, ID3D11Resource **value) const override { return pointer(name, value, Kind::D3D11); }
    Result Get(const char *name, ID3D12Resource **value) const override { return pointer(name, value, Kind::D3D12); }
    Result Get(const char *name, void **value) const override { return pointer(name, value, Kind::Any); }
    void Reset() override { values_.clear(); }

private:
    static constexpr Result kSuccess = 1, kFail = 0xBAD00000u;  // NVSDK_NGX_Result_Success, _Fail
    enum class Kind { Number, D3D11, D3D12, Any };
    struct Value {
        Kind kind = Kind::Number;
        double number = 0;  // holds every NGX number type exactly
        void *pointer = nullptr;
    };
    void number(const char *name, double value) {
        if (name) values_[name] = {Kind::Number, value, nullptr};
    }
    void pointer(const char *name, void *value, Kind kind) {
        if (name) values_[name] = {kind, 0, value};
    }
    template <class T>
    Result number(const char *name, T *out) const {
        const auto found = name ? values_.find(name) : values_.end();
        if (!out || found == values_.end() || found->second.kind != Kind::Number) return kFail;
        *out = static_cast<T>(found->second.number);
        return kSuccess;
    }
    template <class T>
    Result pointer(const char *name, T **out, Kind kind) const {
        const auto found = name ? values_.find(name) : values_.end();
        if (!out || found == values_.end() || found->second.kind == Kind::Number) return kFail;
        if (kind != Kind::Any && found->second.kind != Kind::Any && found->second.kind != kind) return kFail;
        *out = static_cast<T *>(found->second.pointer);
        return kSuccess;
    }
    std::unordered_map<std::string, Value> values_;
};

using InitExt = Result (*)(unsigned long long app, const wchar_t *data_path, ID3D12Device *device, int sdk_version,
                           const FeatureCommonInfo *info);
using Populate = Result (*)(Parameter *parameters);
using Shutdown1 = Result (*)(ID3D12Device *device);

}  // namespace ngx

class AnrFg final : public NgxFg {
public:
    AnrFg() {
        picture_state_ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        aux_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    ~AnrFg() override { stop(); }
    const char *name() const override { return "ANR Frame Generation"; }
    // Under the library folder: the library and what it loads from beside it.
    static constexpr const wchar_t *kFolder = L"anr";
    static constexpr const wchar_t *kLibrary = L"nvngx_dlssg.dll";

    static std::wstring library_path(const std::wstring &library_dir) {
        return library_dir + L"\\" + kFolder + L"\\" + kLibrary;
    }
    static bool present(const std::wstring &library_dir) {
        return GetFileAttributesW(library_path(library_dir).c_str()) != INVALID_FILE_ATTRIBUTES;
    }

protected:
    // Any GPU: the library picks its own way for the one it finds, and says
    // so where it has none.
    bool adapter_ok(const DXGI_ADAPTER_DESC &, std::string &) override { return true; }

    bool start_library(const std::wstring &data_dir, const std::wstring &library_dir, std::string &error) override {
        if (!library_) {
            const std::wstring path = library_path(library_dir);
            // Its own folder first for what it loads: NVIDIA's file of the
            // same name may be in the folder above.
            library_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!library_) {
                char text[200];
                snprintf(text, sizeof(text), "ANR Frame Generation is not downloaded or did not load (error %lu)",
                         GetLastError());
                return error = text, false;
            }
        }
        const auto init = get<ngx::InitExt>("NVSDK_NGX_D3D12_Init_Ext");
        const auto populate = get<ngx::Populate>("NVSDK_NGX_D3D12_PopulateParameters_Impl");
        create_feature_ = get<ngx::CreateFeature>("NVSDK_NGX_D3D12_CreateFeature");
        evaluate_ = get<ngx::EvaluateFeature>("NVSDK_NGX_D3D12_EvaluateFeature");
        release_feature_ = get<ngx::ReleaseFeature>("NVSDK_NGX_D3D12_ReleaseFeature");
        shutdown_ = get<ngx::Shutdown1>("NVSDK_NGX_D3D12_Shutdown1");
        if (!init || !create_feature_ || !evaluate_ || !release_feature_)
            return error = "ANR Frame Generation's library lacks a function", false;

        const ngx::Result r = init(kApp, data_dir.c_str(), device_.Get(), ngx::kApiVersion, nullptr);
        if (!ngx::ok(r)) return error = result(" did not start on this GPU", r), false;
        library_started_ = true;
        map_.Reset();
        parameters_ = &map_;
        if (populate) populate(parameters_);
        unsigned most = 0;
        parameters_->Get("DLSSG.MultiFrameCountMax", &most);
        max_between_ = most < 1 ? 1 : int(most);
        return true;
    }

    void stop_library() override {
        parameters_ = nullptr;
        if (library_started_ && shutdown_) shutdown_(device_.Get());
        library_started_ = false;
        // The library stays loaded: the next start uses it again.
    }

private:
    static constexpr unsigned long long kApp = 0x4C45524144ull;  // an application number of the app's own

    template <class F>
    F get(const char *name) const {
        return reinterpret_cast<F>(GetProcAddress(library_, name));
    }

    HMODULE library_ = nullptr;
    ngx::Shutdown1 shutdown_ = nullptr;
    bool library_started_ = false;
    ngx::ParameterMap map_;
};

}  // namespace fg
