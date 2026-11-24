// SPDX-License-Identifier: MIT
#include "stream_out.h"

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <reshade.hpp>

#include <cstdio>
#include <cstring>

#include "app_version.h"
#include "stream_link.h"
#include "stream_ps.h"
#include "stream_vs.h"
#include "tone_map.h"

namespace app::overlay {
namespace {

using Microsoft::WRL::ComPtr;

void say(reshade::log::level level, const char *format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    reshade::log::message(level, (std::string(APP_NAME " stream window: ") + text).c_str());
}

}  // namespace

bool StreamOut::open_events() {
    if (looked_) return on_ != nullptr;
    looked_ = true;
    // The app makes the switch before it starts mpv: without it (an older
    // app, mpv started by hand) there is no window to draw for.
    on_ = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, stream::kOn);
    if (!on_) return false;
    frame_ = CreateEventW(nullptr, FALSE, FALSE, stream::kFrame);
    if (!frame_) {
        say(reshade::log::level::warning, "no event for its frames (error %lu); it stays off", GetLastError());
        CloseHandle(on_);
        on_ = nullptr;
    }
    return on_ != nullptr;
}

bool StreamOut::offered() { return open_events(); }

bool StreamOut::wanted() { return open_events() && WaitForSingleObject(on_, 0) == WAIT_OBJECT_0; }

void StreamOut::set_wanted(bool on) {
    if (!open_events()) return;
    failed_ = false;  // asked again: tried again
    if (on) SetEvent(on_);
    else ResetEvent(on_);
}

void StreamOut::drop(const char *why) {
    if (shared_) say(reshade::log::level::info, "stopped (%s) after %llu frames, %llu left out while the window read", why,
                     frames_, busy_);
    shared_rtv_.Reset();
    mutex_.Reset();
    shared_.Reset();
    if (shared_handle_) CloseHandle(shared_handle_);
    shared_handle_ = nullptr;
    copy_srv_.Reset();
    copy_.Reset();
    vs_.Reset();
    ps_.Reset();
    params_.Reset();
    sampler_.Reset();
    raster_.Reset();
    depth_.Reset();
    blend_.Reset();
    device_.Reset();
    width_ = height_ = 0;
    format_ = DXGI_FORMAT_UNKNOWN;
    frames_ = busy_ = 0;
}

void StreamOut::release() { drop("the picture's device is gone"); }

bool StreamOut::make(ID3D11Device *device, const D3D11_TEXTURE2D_DESC &bb) {
    device_ = device;
    // The back buffer cannot be read by a shader: a copy that can.
    D3D11_TEXTURE2D_DESC c{};
    c.Width = bb.Width;
    c.Height = bb.Height;
    c.MipLevels = 1;
    c.ArraySize = 1;
    c.Format = bb.Format;
    c.SampleDesc.Count = 1;
    c.Usage = D3D11_USAGE_DEFAULT;
    c.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device->CreateTexture2D(&c, nullptr, &copy_);
    if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(copy_.Get(), nullptr, &copy_srv_);
    if (FAILED(hr)) {
        say(reshade::log::level::error, "no copy of the %ux%u picture (0x%08lX)", bb.Width, bb.Height, hr);
        return false;
    }
    D3D11_TEXTURE2D_DESC t{};
    t.Width = stream::kWidth;
    t.Height = stream::kHeight;
    t.MipLevels = 1;
    t.ArraySize = 1;
    t.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_DEFAULT;
    t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    t.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    hr = device->CreateTexture2D(&t, nullptr, &shared_);
    if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(shared_.Get(), nullptr, &shared_rtv_);
    if (SUCCEEDED(hr)) hr = shared_.As(&mutex_);
    ComPtr<IDXGIResource1> resource;
    if (SUCCEEDED(hr)) hr = shared_.As(&resource);
    if (SUCCEEDED(hr))
        hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, stream::kTexture, &shared_handle_);
    if (FAILED(hr)) {
        // The name is taken while the app's window still holds the texture of
        // an mpv before this one: it lets go within seconds.
        say(reshade::log::level::warning, "no shared %ux%u texture yet (0x%08lX)", stream::kWidth, stream::kHeight, hr);
        return false;
    }
    hr = device->CreateVertexShader(g_stream_vs, sizeof(g_stream_vs), nullptr, &vs_);
    if (SUCCEEDED(hr)) hr = device->CreatePixelShader(g_stream_ps, sizeof(g_stream_ps), nullptr, &ps_);
    D3D11_BUFFER_DESC b{};
    b.ByteWidth = 32;
    b.Usage = D3D11_USAGE_DYNAMIC;
    b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED(hr)) hr = device->CreateBuffer(&b, nullptr, &params_);
    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(hr)) hr = device->CreateSamplerState(&s, &sampler_);
    D3D11_RASTERIZER_DESC r{};
    r.FillMode = D3D11_FILL_SOLID;
    r.CullMode = D3D11_CULL_NONE;
    r.DepthClipEnable = TRUE;
    if (SUCCEEDED(hr)) hr = device->CreateRasterizerState(&r, &raster_);
    D3D11_DEPTH_STENCIL_DESC d{};
    if (SUCCEEDED(hr)) hr = device->CreateDepthStencilState(&d, &depth_);
    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) hr = device->CreateBlendState(&bl, &blend_);
    if (FAILED(hr)) {
        say(reshade::log::level::error, "its drawing could not be set up (0x%08lX)", hr);
        return false;
    }
    width_ = bb.Width;
    height_ = bb.Height;
    format_ = bb.Format;
    return true;
}

void StreamOut::frame(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *back_buffer,
                      Encoding encoding, const D3D11_BOX &box) {
    if (!wanted()) {
        if (shared_) drop("switched off");
        return;
    }
    if (failed_ || !back_buffer) return;
    D3D11_TEXTURE2D_DESC bb{};
    back_buffer->GetDesc(&bb);
    if (bb.SampleDesc.Count != 1 || box.right <= box.left || box.bottom <= box.top) return;
    if (shared_ && (device != device_.Get() || bb.Width != width_ || bb.Height != height_ || bb.Format != format_)) {
        // The picture's own size or format changed: only the copy it is read
        // from is made again, the window keeps its texture.
        copy_srv_.Reset();
        copy_.Reset();
        if (device != device_.Get()) drop("another device draws the picture");
    }
    if (!shared_) {
        static ULONGLONG tried = 0;
        const ULONGLONG now = GetTickCount64();
        if (tried && now - tried < 1000) return;  // a taken name: once a second
        tried = now;
        if (!make(device, bb)) {
            // Only the name in the way: tried again for a while.
            static int name_tries = 0;
            const bool again = !shared_handle_ && copy_ && ++name_tries < 15;
            drop("could not be made");
            failed_ = !again;
            if (failed_) name_tries = 0;
            if (failed_) say(reshade::log::level::error, "it stays off until it is switched on again");
            return;
        }
        tried = 0;
        encoding_ = encoding;
        say(reshade::log::level::info, "on: the %ux%u picture as %ux%u SDR, %s", bb.Width, bb.Height, stream::kWidth,
            stream::kHeight,
            encoding == Encoding::Pq ? "tone mapped from HDR10" : encoding == Encoding::ScRgb ? "tone mapped from scRGB"
                                                                                               : "as it is (SDR)");
    }
    if (!copy_) {
        D3D11_TEXTURE2D_DESC c{};
        c.Width = bb.Width;
        c.Height = bb.Height;
        c.MipLevels = 1;
        c.ArraySize = 1;
        c.Format = bb.Format;
        c.SampleDesc.Count = 1;
        c.Usage = D3D11_USAGE_DEFAULT;
        c.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&c, nullptr, &copy_)) ||
            FAILED(device->CreateShaderResourceView(copy_.Get(), nullptr, &copy_srv_))) {
            drop("no copy of the picture at its new size");
            failed_ = true;
            return;
        }
        width_ = bb.Width;
        height_ = bb.Height;
        format_ = bb.Format;
        say(reshade::log::level::info, "the picture is %ux%u now", bb.Width, bb.Height);
    }
    if (encoding != encoding_) {
        encoding_ = encoding;
        say(reshade::log::level::info, "the picture is %s now",
            encoding == Encoding::Pq ? "HDR10: tone mapped" : encoding == Encoding::ScRgb ? "scRGB: tone mapped" : "SDR");
    }
    // The window reads the texture for a moment at a time: a frame that
    // would have to wait for it is left out instead.
    if (mutex_->AcquireSync(0, 0) != S_OK) {
        ++busy_;
        return;
    }
    context->CopyResource(copy_.Get(), back_buffer);
    {
        StateBackup backup(context);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(context->Map(params_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            struct {
                UINT mode;
                float white;
                float pad[2];
                float picture[4];
            } p{static_cast<UINT>(encoding),
                tone::kReferenceWhiteNits * 100.0f / float(brightness_),
                {},
                {float(box.left) / bb.Width, float(box.top) / bb.Height, float(box.right) / bb.Width,
                 float(box.bottom) / bb.Height}};
            memcpy(m.pData, &p, sizeof(p));
            context->Unmap(params_.Get(), 0);
        }
        // The picture keeps its shape inside the texture, black around it.
        const float pw = float(box.right - box.left), ph = float(box.bottom - box.top);
        float w = float(stream::kWidth), h = w * ph / pw;
        if (h > float(stream::kHeight)) {
            h = float(stream::kHeight);
            w = h * pw / ph;
        }
        const FLOAT black[4] = {0, 0, 0, 1};
        context->ClearRenderTargetView(shared_rtv_.Get(), black);
        ID3D11RenderTargetView *target = shared_rtv_.Get();
        context->OMSetRenderTargets(1, &target, nullptr);
        const FLOAT factor[4] = {};
        context->OMSetBlendState(blend_.Get(), factor, 0xFFFFFFFF);
        context->OMSetDepthStencilState(depth_.Get(), 0);
        context->RSSetState(raster_.Get());
        const D3D11_VIEWPORT vp{(float(stream::kWidth) - w) / 2, (float(stream::kHeight) - h) / 2, w, h, 0, 1};
        context->RSSetViewports(1, &vp);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetInputLayout(nullptr);
        context->VSSetShader(vs_.Get(), nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        context->PSSetShader(ps_.Get(), nullptr, 0);
        ID3D11ShaderResourceView *srv = copy_srv_.Get();
        context->PSSetShaderResources(0, 1, &srv);
        ID3D11SamplerState *sampler = sampler_.Get();
        context->PSSetSamplers(0, 1, &sampler);
        ID3D11Buffer *cb = params_.Get();
        context->VSSetConstantBuffers(0, 1, &cb);
        context->PSSetConstantBuffers(0, 1, &cb);
        context->Draw(3, 0);
        ID3D11ShaderResourceView *none = nullptr;
        context->PSSetShaderResources(0, 1, &none);
        ID3D11RenderTargetView *no_target = nullptr;
        context->OMSetRenderTargets(1, &no_target, nullptr);
    }
    // Sent to the GPU before the window may read: it draws on another device.
    context->Flush();
    mutex_->ReleaseSync(0);
    SetEvent(frame_);
    ++frames_;
}

}  // namespace app::overlay
