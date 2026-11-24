// SPDX-License-Identifier: MIT
#include "view_shifter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <reshade.hpp>

#include "app_version.h"
#include "shift_ps.h"
#include "shift_vs.h"

namespace app::overlay {
namespace {

constexpr double kMaxAge = 0.1;    // seconds: the input program writes every few milliseconds
constexpr float kLimit = 0.10f;    // of the picture's width: further than this the edge shows too much
constexpr double kRetry = 1.0;     // seconds between looks for the mapping

int64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

int64_t qpc_per_second() {
    static const int64_t f = [] {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        return q.QuadPart;
    }();
    return f;
}

}  // namespace

bool ViewShifter::read(ViewShift &out) {
    if (!shared_) {
        const int64_t now = qpc();
        if (tried_ && now - tried_ < int64_t(kRetry * double(qpc_per_second()))) return false;
        tried_ = now;
        mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, name_.c_str());
        if (!mapping_) return false;
        shared_ = static_cast<const ViewShift *>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, sizeof(ViewShift)));
        if (!shared_) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
            return false;
        }
        reshade::log::message(reshade::log::level::info, APP_NAME " view shift: the input program's values are read");
    }
    // The writer makes seq odd, writes, makes it even: a copy between two
    // equal even values is whole.
    for (int tries = 0; tries < 4; ++tries) {
        const uint32_t before = shared_->seq;
        std::memcpy(&out, shared_, sizeof(out));
        if (!(before & 1) && shared_->seq == before) return true;
    }
    return false;
}

bool ViewShifter::ready(ID3D11Device *device, const D3D11_TEXTURE2D_DESC &bb) {
    if (failed_) return false;
    if (!vs_) {
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = 48;
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_SAMPLER_DESC s{};
        s.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_BLEND_DESC b{};
        b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_RASTERIZER_DESC r{};
        r.FillMode = D3D11_FILL_SOLID;
        r.CullMode = D3D11_CULL_NONE;
        r.DepthClipEnable = TRUE;
        D3D11_DEPTH_STENCIL_DESC d{};
        if (FAILED(device->CreateVertexShader(g_shift_vs, sizeof(g_shift_vs), nullptr, &vs_)) ||
            FAILED(device->CreatePixelShader(g_shift_ps, sizeof(g_shift_ps), nullptr, &ps_)) ||
            FAILED(device->CreateBuffer(&cb, nullptr, &params_)) || FAILED(device->CreateSamplerState(&s, &sampler_)) ||
            FAILED(device->CreateBlendState(&b, &blend_)) || FAILED(device->CreateRasterizerState(&r, &raster_)) ||
            FAILED(device->CreateDepthStencilState(&d, &depth_))) {
            failed_ = true;
            reshade::log::message(reshade::log::level::warning, APP_NAME " view shift: could not be set up; it stays off");
            return false;
        }
    }
    if (!copy_ || copy_desc_.Width != bb.Width || copy_desc_.Height != bb.Height || copy_desc_.Format != bb.Format) {
        copy_srv_.Reset();
        copy_.Reset();
        D3D11_TEXTURE2D_DESC t = bb;
        t.MipLevels = 1;
        t.ArraySize = 1;
        t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        t.CPUAccessFlags = 0;
        t.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&t, nullptr, &copy_)) ||
            FAILED(device->CreateShaderResourceView(copy_.Get(), nullptr, &copy_srv_))) {
            copy_srv_.Reset();
            copy_.Reset();
            return false;
        }
        copy_desc_ = t;
    }
    return true;
}

void ViewShifter::apply(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *back_buffer,
                        const D3D11_BOX &picture, Encoding encoding, float paper_nits) {
    if (name_.empty()) return;
    ViewShift asked{};
    if (!read(asked)) return;
    const float width = float(picture.right - picture.left), height = float(picture.bottom - picture.top);
    const ShiftNow now = shift_pixels(asked, qpc(), qpc_per_second(), kMaxAge, width, height, kLimit);
    if (!now.any()) return;
    D3D11_TEXTURE2D_DESC bb{};
    back_buffer->GetDesc(&bb);
    if (bb.SampleDesc.Count != 1 || !ready(device, bb)) return;
    StateBackup backup(context);
    // Made for each draw and let go at once: a view kept on the back buffer
    // would hold it, and the swap chain could not be resized.
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(device->CreateRenderTargetView(back_buffer, nullptr, &rtv))) return;
    context->CopyResource(copy_.Get(), back_buffer);

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(context->Map(params_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    // The dot's white: the menu's white, in the frame's encoding.
    float white = 1.0f;
    if (encoding == Encoding::Pq) {
        const double y = std::pow(std::clamp(double(paper_nits), 80.0, 1000.0) / 10000.0, 0.1593017578125);
        white = float(std::pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375));
    } else if (encoding == Encoding::ScRgb) {
        white = paper_nits / 80.0f;
    }
    struct {
        float shift[2];
        float size[2];
        float box[4];
        float dot_radius, dot_white;
        float pad[2];
    } p{{now.x, now.y},
        {float(bb.Width), float(bb.Height)},
        {float(picture.left), float(picture.top), float(picture.right), float(picture.bottom)},
        now.dot ? std::max(1.5f, height / 1080.0f * 2.0f) : 0.0f,
        white,
        {}};
    static_assert(sizeof(p) == 48);
    std::memcpy(m.pData, &p, sizeof(p));
    context->Unmap(params_.Get(), 0);

    ID3D11RenderTargetView *target = rtv.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    const FLOAT factor[4] = {};
    context->OMSetBlendState(blend_.Get(), factor, 0xFFFFFFFF);
    context->OMSetDepthStencilState(depth_.Get(), 0);
    context->RSSetState(raster_.Get());
    D3D11_VIEWPORT vp{0, 0, float(bb.Width), float(bb.Height), 0, 1};
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
    context->PSSetConstantBuffers(0, 1, &cb);
    context->Draw(3, 0);
    ID3D11ShaderResourceView *none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
}

void ViewShifter::release() {
    copy_srv_.Reset();
    copy_.Reset();
    depth_.Reset();
    raster_.Reset();
    blend_.Reset();
    sampler_.Reset();
    params_.Reset();
    ps_.Reset();
    vs_.Reset();
    if (shared_) UnmapViewOfFile(shared_);
    if (mapping_) CloseHandle(mapping_);
    shared_ = nullptr;
    mapping_ = nullptr;
    tried_ = 0;
}

}  // namespace app::overlay
