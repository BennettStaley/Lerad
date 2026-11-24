// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// A GPU vendor's frame generation, made to work on a video.
//
// DLSS Frame Generation (fg_dlss.h) and FSR frame generation (fg_fsr.h) run
// on Direct3D 12 only, and mpv draws with Direct3D 11. So the frames stay
// where they are and are shared: the add-on's two frame copies and the
// textures the made frames land in are Direct3D 11 textures opened on a
// Direct3D 12 device of the same GPU (no copy), and a shared fence orders the
// two devices' work on the GPU, so the CPU never waits.
//
// Both are made for games, which hand them depth, motion vectors and a camera
// with every frame. A video has none of those: they get a flat depth, a
// camera that stands still and motion vectors of zero, and find the motion in
// the pictures themselves.
//
// Both keep the last picture they were given: each call gives the frames
// between that one and the new one. After a break in the sequence the earlier
// picture is given again first, as a new start.
#pragma once
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace fg {

class VendorGenerator {
public:
    static constexpr int kMaxBetween = 3;  // x4: fg_pacing.h's kMaxMultiplier - 1

    VendorGenerator() = default;
    VendorGenerator(const VendorGenerator &) = delete;
    VendorGenerator &operator=(const VendorGenerator &) = delete;
    virtual ~VendorGenerator() = default;  // the derived class calls stop()

    virtual const char *name() const = 0;  // "DLSS Frame Generation"

    // A Direct3D 12 device on device11's GPU and the vendor's library on
    // it. data_dir: a folder it may write its logs to. library_dir: where the
    // files it needs are (the app's engine folder). False (with why) when
    // this PC cannot.
    bool start(ID3D11Device *device11, const std::wstring &data_dir, const std::wstring &library_dir,
               std::string &error) {
        stop();
        Microsoft::WRL::ComPtr<ID3D11Device5> device5;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        device11->GetImmediateContext(&context);
        if (FAILED(device11->QueryInterface(IID_PPV_ARGS(&device5))) || FAILED(context.As(&context11_)))
            return error = "no Direct3D 11.4 fences", stop(), false;
        device11_ = device11;
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (FAILED(device11->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
            FAILED(adapter->GetDesc(&adapter_)))
            return error = "no DXGI adapter", stop(), false;
        if (!adapter_ok(adapter_, error)) return stop(), false;

        d3d12_ = LoadLibraryW(L"d3d12.dll");
        const auto create =
            d3d12_ ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12_, "D3D12CreateDevice")) : nullptr;
        if (!create || FAILED(create(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_))))
            return error = "no Direct3D 12 device on this GPU", stop(), false;
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        HRESULT hr = device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_));
        for (int i = 0; i < kAllocators && SUCCEEDED(hr); ++i)
            hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators_[i]));
        if (SUCCEEDED(hr))
            hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr,
                                            IID_PPV_ARGS(&list_));
        if (SUCCEEDED(hr)) hr = list_->Close();
        HANDLE shared = nullptr;
        if (SUCCEEDED(hr)) hr = device_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence_));
        if (SUCCEEDED(hr)) hr = device_->CreateSharedHandle(fence_.Get(), nullptr, GENERIC_ALL, nullptr, &shared);
        if (SUCCEEDED(hr)) hr = device5->OpenSharedFence(shared, IID_PPV_ARGS(&fence11_));
        if (shared) CloseHandle(shared);
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (FAILED(hr) || !event_) return error = hresult("Direct3D 12 queue and fence", hr), stop(), false;

        if (!start_library(data_dir, library_dir, error)) return stop(), false;
        started_ = true;
        return true;
    }

    bool started() const { return started_; }
    bool ready() const { return ready_; }
    // The most frames it makes between two pictures on this GPU (1 = x2).
    int max_between() const { return max_between_ < kMaxBetween ? max_between_ : kMaxBetween; }

    // The flags a Direct3D 11 texture needs to be given to set_frames().
    static constexpr UINT kSharedMisc = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    // The two frame copies the pictures are put in (Direct3D 11 textures made
    // with kSharedMisc, the same size and format) and how they are encoded.
    // Makes the textures the frames in between come out in, the same format.
    bool set_frames(ID3D11Texture2D *frame0, ID3D11Texture2D *frame1, bool hdr, std::string &error) {
        drop_frames();
        if (!started()) return error = "not started", false;
        D3D11_TEXTURE2D_DESC desc{};
        frame0->GetDesc(&desc);
        HRESULT hr = open(frame0, in_[0]);
        if (SUCCEEDED(hr)) hr = open(frame1, in_[1]);
        if (FAILED(hr)) return error = hresult("sharing the frame copies with Direct3D 12", hr), drop_frames(), false;

        D3D11_TEXTURE2D_DESC out = desc;
        out.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        out.MiscFlags = kSharedMisc;
        out.Usage = D3D11_USAGE_DEFAULT;
        out.CPUAccessFlags = 0;
        for (int i = 0; i < max_between() && SUCCEEDED(hr); ++i) {
            hr = device11_->CreateTexture2D(&out, nullptr, &out11_[i]);
            if (SUCCEEDED(hr)) hr = open(out11_[i].Get(), out_[i]);
        }
        if (FAILED(hr)) return error = hresult("textures for the made frames", hr), drop_frames(), false;

        // A flat depth and no motion, the size of the picture.
        hr = texture(desc.Width, desc.Height, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, depth_);
        if (SUCCEEDED(hr))
            hr = texture(desc.Width, desc.Height, DXGI_FORMAT_R16G16_FLOAT, kShaderRead, vectors_);  // zeros as made
        Microsoft::WRL::ComPtr<ID3D12Resource> staging;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed{};
        if (SUCCEEDED(hr)) {
            const D3D12_RESOURCE_DESC depth_desc = depth_->GetDesc();
            UINT rows = 0;
            UINT64 row_bytes = 0, total = 0;
            device_->GetCopyableFootprints(&depth_desc, 0, 1, 0, &placed, &rows, &row_bytes, &total);
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = total;
            buffer.Height = 1;
            buffer.DepthOrArraySize = 1;
            buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                  nullptr, IID_PPV_ARGS(&staging));
            void *mapped = nullptr;
            if (SUCCEEDED(hr)) hr = staging->Map(0, nullptr, &mapped);
            if (SUCCEEDED(hr)) {
                for (UINT y = 0; y < rows; ++y) {
                    float *row = reinterpret_cast<float *>(static_cast<BYTE *>(mapped) + placed.Offset +
                                                           size_t(y) * placed.Footprint.RowPitch);
                    for (UINT x = 0; x < desc.Width; ++x) row[x] = kDepth;
                }
                staging->Unmap(0, nullptr);
            }
        }
        if (FAILED(hr)) return error = hresult("depth and motion textures", hr), drop_frames(), false;

        width_ = desc.Width;
        height_ = desc.Height;
        format_ = desc.Format;
        hdr_ = hdr;
        // The depth filled, and the vendor's own set-up for this size and format.
        if (!begin()) return error = "Direct3D 12 command list", drop_frames(), false;
        D3D12_TEXTURE_COPY_LOCATION to{}, from{};
        to.pResource = depth_.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.pResource = staging.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = placed;
        list_->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        transition(depth_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kShaderRead);
        const bool made = create_feature(error);
        wait_cpu(submit());  // the staging buffer goes away with this call
        if (!made) return drop_frames(), false;
        ready_ = true;
        return true;
    }

    // The motion from the later picture back to the earlier one, for a
    // library that uses what it is given (fg_anr.h): a Direct3D 11 texture
    // made with kSharedMisc, R16G16_FLOAT, the frames' size, in pixels (where
    // a pixel of the later picture was in the earlier one, less where it is).
    // The caller fills it before each generate(); a start (not continuous)
    // gets it too, with the earlier picture. Null: none again (zeros).
    bool set_motion(ID3D11Texture2D *motion, std::string &error) {
        motion_.Reset();
        if (!motion) return true;
        if (!ready()) return error = "not ready", false;
        const HRESULT hr = open(motion, motion_);
        if (FAILED(hr)) return error = hresult("sharing the motion with Direct3D 12", hr), motion_.Reset(), false;
        return true;
    }
    bool has_motion() const { return motion_ != nullptr; }

    // Makes the `count` frames between the pictures in frame copies `earlier`
    // and `later` (0 or 1). continuous: `earlier` is the picture that was
    // `later` in the call before, with the same count. The Direct3D 11
    // context's work so far (the copy into `later`) comes first on the GPU,
    // and what it does next comes after the frames are made: nothing waits
    // on the CPU.
    bool generate(int earlier, int later, int count, bool continuous, std::string &error) {
        if (!ready()) return error = "not ready", false;
        if (count < 1 || count > max_between()) return error = "more frames than it makes", false;
        // What Direct3D 11 queued so far comes first.
        const UINT64 before = ++value_;
        if (FAILED(context11_->Signal(fence11_.Get(), before))) return error = "Direct3D 11 fence", false;
        context11_->Flush();
        queue_->Wait(fence_.Get(), before);

        if (!begin()) return error = "Direct3D 12 command list", false;
        for (auto &in : in_) transition(in.Get(), D3D12_RESOURCE_STATE_COMMON, kShaderRead);
        for (int i = 0; i < count; ++i)
            transition(out_[i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (motion_) transition(motion_.Get(), D3D12_RESOURCE_STATE_COMMON, motion_state());
        bool ok = true;
        if (!continuous) ok = record(in_[earlier].Get(), count, true, error);
        if (ok) ok = record(in_[later].Get(), count, false, error);
        if (motion_) transition(motion_.Get(), motion_state(), D3D12_RESOURCE_STATE_COMMON);
        for (auto &in : in_) transition(in.Get(), kShaderRead, D3D12_RESOURCE_STATE_COMMON);
        for (int i = 0; i < count; ++i)
            transition(out_[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        const UINT64 done = submit();
        // Direct3D 11 goes on once the frames are made.
        context11_->Wait(fence11_.Get(), done);
        return ok;
    }

    // The made frame `index` (1 to count) of the last generate(), to copy from.
    ID3D11Texture2D *between(int index) const { return out11_[index - 1].Get(); }

    // Lets go of the frame copies given to set_frames() (they are about to
    // be made anew) and of all that was made for them.
    void drop_frames() {
        // Nothing of these may still be in use on the GPU.
        if (fence_ && queue_) {
            const UINT64 idle = ++value_;
            if (SUCCEEDED(queue_->Signal(fence_.Get(), idle))) wait_cpu(idle);
        }
        if (ready_ || width_) release_feature();
        ready_ = false;
        for (auto &t : in_) t.Reset();
        motion_.Reset();
        for (auto &t : out_) t.Reset();
        for (auto &t : out11_) t.Reset();
        depth_.Reset();
        vectors_.Reset();
        width_ = height_ = 0;
    }

    void stop() {
        drop_frames();
        if (device_) stop_library();
        started_ = false;
        fence11_.Reset();
        fence_.Reset();
        list_.Reset();
        for (auto &a : allocators_) a.Reset();
        queue_.Reset();
        device_.Reset();
        context11_.Reset();
        device11_.Reset();
        if (event_) CloseHandle(event_);
        event_ = nullptr;
        max_between_ = 0;
    }

protected:
    static constexpr float kDepth = 0.9f, kNear = 0.1f, kFar = 1000.0f, kFovY = 1.5707963f;
    static constexpr D3D12_RESOURCE_STATES kShaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // What the vendor's side does.
    //  - adapter_ok: whether this GPU is one of theirs that can.
    //  - start_library: loads it for device_; sets max_between_.
    //  - create_feature: for width_ x height_, format_, hdr_; may record on
    //    list_ (it runs before the first frames are made).
    //  - record: on list_, the `count` frames between the picture given
    //    before and `picture`, into out_[0 .. count - 1]; restart: `picture`
    //    starts a new sequence (what comes out is not shown).
    virtual bool adapter_ok(const DXGI_ADAPTER_DESC &adapter, std::string &error) = 0;
    virtual bool start_library(const std::wstring &data_dir, const std::wstring &library_dir, std::string &error) = 0;
    virtual bool create_feature(std::string &error) = 0;
    virtual bool record(ID3D12Resource *picture, int count, bool restart, std::string &error) = 0;
    virtual void release_feature() = 0;
    virtual void stop_library() = 0;
    // The state the library is given set_motion()'s texture in.
    virtual D3D12_RESOURCE_STATES motion_state() const { return kShaderRead; }

    static std::string hresult(const char *what, HRESULT hr) {
        char text[160];
        snprintf(text, sizeof(text), "%s: 0x%08lX", what, static_cast<unsigned long>(hr));
        return text;
    }
    void wait_cpu(UINT64 value) {
        if (!fence_ || fence_->GetCompletedValue() >= value) return;
        if (SUCCEEDED(fence_->SetEventOnCompletion(value, event_))) WaitForSingleObject(event_, 2000);
    }

    void transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        list_->ResourceBarrier(1, &barrier);
    }
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
    Microsoft::WRL::ComPtr<ID3D12Resource> in_[2], out_[kMaxBetween], depth_, vectors_;
    Microsoft::WRL::ComPtr<ID3D12Resource> motion_;  // set_motion()'s, in place of vectors_
    DXGI_ADAPTER_DESC adapter_{};
    UINT width_ = 0, height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    bool hdr_ = false;
    int max_between_ = 0;

private:
    static constexpr int kAllocators = 4;

    // A Direct3D 11 texture, opened on the Direct3D 12 device.
    HRESULT open(ID3D11Texture2D *texture11, Microsoft::WRL::ComPtr<ID3D12Resource> &out) {
        Microsoft::WRL::ComPtr<IDXGIResource1> resource;
        HRESULT hr = texture11->QueryInterface(IID_PPV_ARGS(&resource));
        HANDLE shared = nullptr;
        if (SUCCEEDED(hr))
            hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                              &shared);
        if (SUCCEEDED(hr)) hr = device_->OpenSharedHandle(shared, IID_PPV_ARGS(&out));
        if (shared) CloseHandle(shared);
        return hr;
    }
    HRESULT texture(UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_STATES state,
                    Microsoft::WRL::ComPtr<ID3D12Resource> &out) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        return device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&out));
    }

    // The next command allocator, once the GPU is through with what it held.
    bool begin() {
        next_ = (next_ + 1) % kAllocators;
        wait_cpu(submitted_[next_]);
        return SUCCEEDED(allocators_[next_]->Reset()) && SUCCEEDED(list_->Reset(allocators_[next_].Get(), nullptr));
    }
    // Runs the list; the fence value that says it is done.
    UINT64 submit() {
        list_->Close();
        ID3D12CommandList *lists[1] = {list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        const UINT64 done = ++value_;
        queue_->Signal(fence_.Get(), done);
        submitted_[next_] = done;
        return done;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device11_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context11_;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence11_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> out11_[kMaxBetween];
    HMODULE d3d12_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocators_[kAllocators];
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    UINT64 value_ = 0, submitted_[kAllocators] = {};
    int next_ = 0;
    HANDLE event_ = nullptr;
    bool started_ = false, ready_ = false;
};

}  // namespace fg
