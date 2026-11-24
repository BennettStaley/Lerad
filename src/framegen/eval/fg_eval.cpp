// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (c) 2026 Abhito
// fg_eval - scores the add-on's in-between frames against the true ones.
//
// Reads the clips make_sequences.sh writes (seq/<clip>/f01.png ... f09.png,
// odd = real frames, even = true in-between frames) - or a folder the add-on's
// capture key saved (f01.png ... f05.png, five real frames in a row: the frame
// made between f01 and f03 is scored against f02) - makes each in-between
// frame on the GPU with fg::Interpolator - the same code the add-on runs - and
// compares it with the true one (PSNR over RGB; higher is better, identical
// is "inf"). "repeat" (show the earlier real frame again) is the baseline any
// method has to beat. GPU time is measured with timestamp queries.
//
//   fg_eval.exe [--clip NAME] [--out DIR] [--methods repeat,blend,motion,dlss,fsr,anr] [--hdr] SEQ_DIR
//
// "dlss" is NVIDIA's DLSS Frame Generation (fg_dlss.h) and "fsr" AMD's FSR
// frame generation (fg_fsr.h), on 10-bit copies of the frames as the add-on's
// back buffer has them; --hdr tells them they are HDR (PQ). Their libraries
// are looked for in FG_LIBRARY_DIR, else beside this program. "anr" is Apex
// Neural Rendering's frame generation (fg_anr.h), from the folder "anr" there.
//
// --picture X0 Y0 X1 Y1: the picture's place in frames with black bars (the
// add-on's PictureOnly): the generator works on that part alone. --adjacent
// and --set NAME VALUE: below.
//
// With --out, the first in-between frame of each clip and method is saved as
// <clip>_<method>.png for looking at.
//
// --hud BOXES.json: the HUD mask (fg_hud.h), along longer clips (33 frames:
// pair k is frames 2k-1 and 2k+1, the true frame between them 2k). The mask
// needs a few pairs of history, so every pair is run in order, and each
// pair's frame is scored four ways: as the generator made it, with the hand-
// marked HUD boxes of BOXES.json copied from the later real frame (the areas
// left alone), with the mask applied inside those boxes, and with the mask
// applied everywhere. Errors are mean absolute differences from the true
// frame in 8-bit levels on three sets of pixels - the HUD's own (inside the
// boxes, unchanged over the whole clip and with detail), the rest of the
// boxes (scenery beside and through the HUD) and everything outside - plus
// the whole frame's PSNR; over pairs 5 on, when every history has had its
// time. BOXES.json: {"clip": [{"name": "...", "box": [left, top, width,
// height]}, ...], ...}. Methods: motion, dlss. --hud-rules motion,plain: the
// mask's rule (for dlss, "motion" is the interpolator's search run for the
// mask alone). --hud-set NAME VALUE: one of fg::HudMask's settings.
// --hud-save 8,12: with --out, those pairs' frames and masks as PNG.
// --hud-late N (a clip of any length): the first N scored pairs and the
// clip's last N are scored apart - a mask that has just begun, and one that
// has had the clip's length.
// --resize W H: the frames scaled to that size first (for timing at another
// size; the boxes are scaled along). --as-pq NITS: the clips' SDR frames are
// given to the generator and the mask as HDR10 (PQ) ones, white at NITS, and
// the frames that come back are scored as SDR again (what the mask does on a
// PQ picture, for want of HDR clips).

#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <memory>

#include "fg_anr.h"
#include "fg_dlss.h"
#include "fg_fsr.h"
#include "fg_hud.h"
#include "fg_interp.h"
#include "json.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

// 16 bits a channel whatever the file had, so 10-bit captures keep theirs.
struct Image {
    UINT w = 0, h = 0;
    std::vector<UINT16> rgba;
};

// to_w x to_h: scaled to that size (0: as it is).
bool load_png(IWICImagingFactory *wic, const fs::path &path, Image &img, UINT to_w = 0, UINT to_h = 0) {
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                              &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&converter)))
        return false;
    // Scaled in the file's own format, then made 16 bits a channel.
    ComPtr<IWICBitmapSource> source = frame;
    if (to_w && to_h) {
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(frame.Get(), to_w, to_h, WICBitmapInterpolationModeFant)))
            return false;
        source = scaler;
    }
    if (FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat64bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&img.w, &img.h)))
        return false;
    img.rgba.resize(size_t(img.w) * img.h * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, img.w * 8, static_cast<UINT>(img.rgba.size() * 2),
                                           reinterpret_cast<BYTE *>(img.rgba.data())));
}

// An 8-bit preview, as BGRA (the order WIC's PNG encoder takes).
bool save_png(IWICImagingFactory *wic, const fs::path &path, const Image &img) {
    std::vector<BYTE> bgra(img.rgba.size());
    for (size_t i = 0; i < bgra.size(); i += 4) {
        bgra[i] = BYTE(img.rgba[i + 2] >> 8);
        bgra[i + 1] = BYTE(img.rgba[i + 1] >> 8);
        bgra[i + 2] = BYTE(img.rgba[i] >> 8);
        bgra[i + 3] = BYTE(img.rgba[i + 3] >> 8);
    }
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(wic->CreateStream(&stream)) &&
           SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
           SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
           SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
           SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
           SUCCEEDED(frame->SetSize(img.w, img.h)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
           IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) &&
           SUCCEEDED(frame->WritePixels(img.h, img.w * 4, static_cast<UINT>(bgra.size()), bgra.data())) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

double psnr(const Image &a, const Image &b) {
    double sum = 0;
    const size_t n = size_t(a.w) * a.h;
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c) {
            const double d = double(a.rgba[i * 4 + c]) - double(b.rgba[i * 4 + c]);
            sum += d * d;
        }
    const double mse = sum / (double(n) * 3.0);
    return mse <= 0 ? INFINITY : 10.0 * std::log10(65535.0 * 65535.0 / mse);
}

struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Query> disjoint, start, end;
};

bool make_texture(Gpu &gpu, const Image &img, ComPtr<ID3D11Texture2D> &tex, ComPtr<ID3D11ShaderResourceView> &srv) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = img.w;
    desc.Height = img.h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{img.rgba.data(), img.w * 8, 0};
    return SUCCEEDED(gpu.device->CreateTexture2D(&desc, &data, &tex)) &&
           SUCCEEDED(gpu.device->CreateShaderResourceView(tex.Get(), nullptr, &srv));
}

// Times one draw on the GPU, in milliseconds.
double timed(Gpu &gpu, const std::function<void()> &work) {
    gpu.ctx->Begin(gpu.disjoint.Get());
    gpu.ctx->End(gpu.start.Get());
    work();
    gpu.ctx->End(gpu.end.Get());
    gpu.ctx->End(gpu.disjoint.Get());
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    while (gpu.ctx->GetData(gpu.disjoint.Get(), &dj, sizeof(dj), 0) == S_FALSE) YieldProcessor();
    UINT64 t0 = 0, t1 = 0;
    while (gpu.ctx->GetData(gpu.start.Get(), &t0, sizeof(t0), 0) == S_FALSE) YieldProcessor();
    while (gpu.ctx->GetData(gpu.end.Get(), &t1, sizeof(t1), 0) == S_FALSE) YieldProcessor();
    return dj.Disjoint || dj.Frequency == 0 ? -1.0 : double(t1 - t0) * 1000.0 / double(dj.Frequency);
}

// A GPU vendor's frame generation between two frames, on R10G10B10A2 copies.
struct Vendor {
    std::unique_ptr<fg::VendorGenerator> made;
    fg::VendorGenerator &fg;
    explicit Vendor(std::unique_ptr<fg::VendorGenerator> generator) : made(std::move(generator)), fg(*made) {}
    ComPtr<ID3D11Texture2D> in[2], staging;
    ComPtr<ID3D11ShaderResourceView> in_srv[2];
    std::vector<UINT32> packed;
    UINT w = 0, h = 0;
    bool tried = false, failed = false;

    bool prepare(Gpu &gpu, UINT width, UINT height, bool hdr, std::string &error) {
        if (failed) return false;
        if (!tried) {
            tried = true;
            wchar_t temp[MAX_PATH] = {};
            GetTempPathW(MAX_PATH, temp);
            // Its libraries: in FG_LIBRARY_DIR, else beside this program.
            wchar_t model[MAX_PATH] = {};
            if (!GetEnvironmentVariableW(L"FG_LIBRARY_DIR", model, MAX_PATH)) {
                GetModuleFileNameW(nullptr, model, MAX_PATH);
                if (wchar_t *slash = wcsrchr(model, L'\\')) *slash = 0;
            }
            if (!fg.start(gpu.device.Get(), temp, model, error)) return failed = true, false;
            printf("%s: up to %d frames between two\n", fg.name(), fg.max_between());
        }
        if (width == w && height == h) return true;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = fg::DlssFg::kSharedMisc;
        for (int i = 0; i < 2; ++i)
            if (FAILED(gpu.device->CreateTexture2D(&desc, nullptr, in[i].ReleaseAndGetAddressOf())) ||
                FAILED(gpu.device->CreateShaderResourceView(in[i].Get(), nullptr, in_srv[i].ReleaseAndGetAddressOf())))
                return error = "shared textures", failed = true, false;
        desc.MiscFlags = 0;
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(gpu.device->CreateTexture2D(&desc, nullptr, staging.ReleaseAndGetAddressOf())))
            return error = "staging texture", failed = true, false;
        if (!fg.set_frames(in[0].Get(), in[1].Get(), hdr, error)) return failed = true, false;
        w = width;
        h = height;
        packed.resize(size_t(w) * h);
        return true;
    }
    void put(Gpu &gpu, int slot, const Image &img) {
        for (size_t i = 0; i < packed.size(); ++i) {
            const UINT16 *c = img.rgba.data() + i * 4;
            const auto ten = [](UINT16 v) { return (UINT32(v) * 1023 + 32767) / 65535; };
            packed[i] = ten(c[0]) | ten(c[1]) << 10 | ten(c[2]) << 20 | 3u << 30;
        }
        gpu.ctx->UpdateSubresource(in[slot].Get(), 0, nullptr, packed.data(), w * 4, 0);
    }
    // The interpolator's motion from the later frame back to the earlier one
    // (find_flow with back), as the generator's motion input: a vector per
    // pixel, in pixels (fg_bridge.h set_motion), drawn as the add-on draws
    // it. use false: no motion input again.
    ComPtr<ID3D11Texture2D> motion;
    ComPtr<ID3D11RenderTargetView> motion_target;
    bool put_motion(Gpu &gpu, fg::Interpolator &interp, bool use, std::string &error) {
        if (!use) return motion.Reset(), motion_target.Reset(), fg.set_motion(nullptr, error);
        if (!motion) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = w;
            desc.Height = h;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R16G16_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = fg::VendorGenerator::kSharedMisc;
            if (FAILED(gpu.device->CreateTexture2D(&desc, nullptr, &motion)) ||
                FAILED(gpu.device->CreateRenderTargetView(motion.Get(), nullptr, &motion_target)))
                return motion.Reset(), error = "motion texture", false;
            if (!fg.set_motion(motion.Get(), error)) return motion.Reset(), motion_target.Reset(), false;
        }
        if (!interp.draw_motion_back(gpu.ctx.Get(), motion_target.Get())) return error = "no motion found", false;
        return true;
    }
    void get(Gpu &gpu, int index, Image &img) { read(gpu, fg.between(index), img); }
    // Any texture of the frames' size and format.
    void read(Gpu &gpu, ID3D11Texture2D *texture, Image &img) {
        gpu.ctx->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE map{};
        gpu.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map);
        img.w = w;
        img.h = h;
        img.rgba.resize(size_t(w) * h * 4);
        for (UINT y = 0; y < h; ++y) {
            const UINT32 *row = reinterpret_cast<const UINT32 *>(static_cast<BYTE *>(map.pData) + size_t(y) * map.RowPitch);
            UINT16 *out = img.rgba.data() + size_t(y) * w * 4;
            for (UINT x = 0; x < w; ++x) {
                const auto sixteen = [](UINT32 v) { return UINT16((v * 65535 + 511) / 1023); };
                out[x * 4 + 0] = sixteen(row[x] & 1023);
                out[x * 4 + 1] = sixteen(row[x] >> 10 & 1023);
                out[x * 4 + 2] = sixteen(row[x] >> 20 & 1023);
                out[x * 4 + 3] = 65535;
            }
        }
        gpu.ctx->Unmap(staging.Get(), 0);
    }
};

struct Result {
    double psnr_sum = 0;
    int psnr_n = 0;
    bool any_inf = false;
    std::vector<double> ms;
};

// ---- --hud: the HUD mask along a clip ----------------------------------------------

struct HudBox {
    std::string name;
    int x = 0, y = 0, w = 0, h = 0;
};

// The 8 bits of a 16-bit sample that save_png writes.
inline int eight(UINT16 v) { return v >> 8; }

// Rec. 709 luma of a pixel's 8-bit values, to a whole level.
inline float luma8(const UINT16 *c) {
    return std::nearbyint(0.2126f * float(eight(c[0])) + 0.7152f * float(eight(c[1])) + 0.0722f * float(eight(c[2])));
}

// In place: each value becomes the lowest (lo) and highest (hi) within
// `radius` of it, across and down; what is off the picture does not count.
void spread(std::vector<float> &lo, std::vector<float> &hi, UINT w, UINT h, int radius) {
    std::vector<float> a(lo.size()), b(hi.size());
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x) {
            const int x0 = std::max(0, int(x) - radius), x1 = std::min(int(w) - 1, int(x) + radius);
            float l = 1e30f, m = -1e30f;
            for (int q = x0; q <= x1; ++q) {
                l = std::min(l, lo[size_t(y) * w + q]);
                m = std::max(m, hi[size_t(y) * w + q]);
            }
            a[size_t(y) * w + x] = l;
            b[size_t(y) * w + x] = m;
        }
    for (UINT y = 0; y < h; ++y) {
        const int y0 = std::max(0, int(y) - radius), y1 = std::min(int(h) - 1, int(y) + radius);
        for (UINT x = 0; x < w; ++x) {
            float l = 1e30f, m = -1e30f;
            for (int q = y0; q <= y1; ++q) {
                l = std::min(l, a[size_t(q) * w + x]);
                m = std::max(m, b[size_t(q) * w + x]);
            }
            lo[size_t(y) * w + x] = l;
            hi[size_t(y) * w + x] = m;
        }
    }
}

constexpr BYTE kInBox = 1, kHudPixel = 2, kBoxRest = 4;

// Per pixel, which set it is scored in: inside a hand-marked box (kInBox);
// the HUD's own - inside a box, the same in every frame of the clip (its luma
// moves by less than 8 levels) and with detail (a 9x9 contrast over 40 in
// the clip's mean) - (kHudPixel); the rest of the boxes, 3 px clear of those
// (kBoxRest). counts: HUD pixels, rest, outside the boxes.
std::vector<BYTE> hud_regions(const std::vector<Image> &frames, const std::vector<HudBox> &boxes, size_t counts[3]) {
    const UINT w = frames[0].w, h = frames[0].h;
    const size_t n = size_t(w) * h;
    std::vector<float> lo(n, 1e30f), hi(n, -1e30f), mean(n, 0.0f);
    for (const Image &f : frames)
        for (size_t i = 0; i < n; ++i) {
            const float y = luma8(f.rgba.data() + i * 4);
            lo[i] = std::min(lo[i], y);
            hi[i] = std::max(hi[i], y);
            mean[i] += y;
        }
    for (float &m : mean) m /= float(frames.size());
    std::vector<float> dlo = mean, dhi = mean;
    spread(dlo, dhi, w, h, 4);
    std::vector<BYTE> region(n, 0);
    for (const HudBox &b : boxes)
        for (int y = std::max(0, b.y); y < std::min(int(h), b.y + b.h); ++y)
            for (int x = std::max(0, b.x); x < std::min(int(w), b.x + b.w); ++x) region[size_t(y) * w + x] = kInBox;
    std::vector<float> near_lo(n), near_hi(n);
    for (size_t i = 0; i < n; ++i) {
        const bool own = (region[i] & kInBox) && hi[i] - lo[i] < 8.0f && dhi[i] - dlo[i] > 40.0f;
        if (own) region[i] |= kHudPixel;
        near_lo[i] = near_hi[i] = own ? 1.0f : 0.0f;
    }
    spread(near_lo, near_hi, w, h, 3);
    counts[0] = counts[1] = counts[2] = 0;
    for (size_t i = 0; i < n; ++i) {
        if ((region[i] & kInBox) && near_hi[i] < 0.5f) region[i] |= kBoxRest;
        if (region[i] & kHudPixel)
            ++counts[0];
        else if (region[i] & kBoxRest)
            ++counts[1];
        else if (!(region[i] & kInBox))
            ++counts[2];
    }
    return region;
}

enum HudTreatment { kAsIs, kHardBox, kMaskInBoxes, kMaskEverywhere, kHudTreatments };
const wchar_t *const kHudTreatmentNames[kHudTreatments] = {L"as is", L"hard box", L"mask in boxes", L"mask everywhere"};

struct HudScore {
    std::wstring clip, method, rule;
    size_t counts[3] = {};
    double err[kHudTreatments][3] = {};  // summed over pairs and pixels: HUD pixels, box rest, outside
    double mse[kHudTreatments] = {};     // summed over pairs
    int pairs = 0;
    double mask_px = 0, mask_in_box = 0, mask_on_hud = 0;  // summed over the scored pairs
    std::vector<double> covered;                           // per pair from the first: share of the HUD's pixels in the mask
    std::vector<double> ms_frame, ms_flow, ms_detect, ms_apply, ms_apply_boxes;
    std::vector<double> ms_clean;  // the frame drawn from the frames without their HUD (ms_frame: as they are)
    size_t wrong = 0;  // pixels where the apply pass did not draw what the mask says

    double error(int treatment, int set) const { return err[treatment][set] / std::max(1, pairs) / double(std::max<size_t>(1, counts[set])); }
    double psnr(int treatment) const {
        const double m = mse[treatment] / std::max(1, pairs);
        return m > 0 ? 10.0 * std::log10(255.0 * 255.0 / m) : 99.0;
    }
    static double median(std::vector<double> v) {
        v.erase(std::remove_if(v.begin(), v.end(), [](double x) { return x < 0; }), v.end());
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }
};

bool set_hud_value(fg::HudMask &hud, const std::wstring &name, double value) {
    if (name == L"pq_step") hud.pq_step = float(value);
    else if (name == L"min_motion") hud.min_motion = float(value);
    else if (name == L"still_level") hud.still_level = float(value);
    else if (name == L"strict_level") hud.strict_level = float(value);
    else if (name == L"moved_level") hud.moved_level = float(value);
    else if (name == L"changed_level") hud.changed_level = float(value);
    else if (name == L"edge_level") hud.edge_level = float(value);
    else if (name == L"edge_same") hud.edge_same = float(value);
    else if (name == L"edge_across") hud.edge_across = float(value);
    else if (name == L"count_up") hud.count_up = int(value);
    else if (name == L"count_max") hud.count_max = int(value);
    else if (name == L"on_from") hud.on_from = int(value);
    else if (name == L"still_down") hud.still_down = int(value);
    else if (name == L"edge_down") hud.edge_down = int(value);
    else if (name == L"cut_rule") hud.cut_rule = int(value);
    else if (name == L"plain_detail") hud.plain_detail = float(value);
    else if (name == L"plain_moves") hud.plain_moves = float(value);
    else if (name == L"plain_pairs") hud.plain_pairs = int(value);
    else if (name == L"clean") hud.clean = int(value);
    else if (name == L"clean_grow") hud.clean_grow = float(value);
    else if (name == L"clean_reach") hud.clean_reach = float(value);
    else if (name == L"hold_own") hud.hold_own = int(value);
    else if (name == L"carry") hud.carry = int(value);
    else return false;
    return true;
}

struct HudOptions {
    fs::path boxes_file;
    std::vector<std::wstring> methods, rules;
    std::vector<std::pair<std::wstring, double>> tuning;
    std::vector<int> save_pairs;
    fs::path out_dir;
    UINT to_w = 0, to_h = 0;  // --resize
    bool hdr = false;
    // --as-pq NITS: the clips' SDR frames are given to the generator and the mask as HDR10 (PQ) ones, white at
    // this many nits, and what comes back is scored as SDR again.
    double pq_nits = 0;
    int first_pair = 5;       // scores are over this pair on
    // --hud-late N: two scores, over the first N of those pairs (a mask that has just begun) and over the
    // clip's last N (one that has had the clip's length).
    int late = 0;
};

void hud_table(const std::vector<HudScore> &scores, const std::wstring &method, const std::wstring &rule, const wchar_t *title,
               const std::function<double(const HudScore &, int)> &value) {
    wprintf(L"\n%s, %s frames, rule %s: %s\n%-10s", method.c_str(), method == L"dlss" ? L"DLSS FG's" : L"the own generator's",
            rule.c_str(), title, L"clip");
    for (const wchar_t *t : kHudTreatmentNames) wprintf(L"%17s", t);
    wprintf(L"\n");
    double total[kHudTreatments] = {};
    int n = 0;
    for (const HudScore &s : scores) {
        if (s.method != method || s.rule != rule) continue;
        wprintf(L"%-10s", s.clip.c_str());
        for (int t = 0; t < kHudTreatments; ++t) {
            const double v = value(s, t);
            total[t] += v;
            wprintf(L"%17.2f", v);
        }
        wprintf(L"\n");
        ++n;
    }
    wprintf(L"%-10s", L"mean");
    for (int t = 0; t < kHudTreatments; ++t) wprintf(L"%17.2f", total[t] / std::max(1, n));
    wprintf(L"\n");
}

int run_hud(Gpu &gpu, IWICImagingFactory *wic, const std::vector<fs::path> &clips, fg::Interpolator &interp,
            const HudOptions &opt) {
    std::string text;
    if (FILE *f = _wfopen(opt.boxes_file.c_str(), L"rb")) {
        char buffer[4096];
        for (size_t got; (got = fread(buffer, 1, sizeof(buffer), f)) > 0;) text.append(buffer, got);
        fclose(f);
    }
    const std::optional<app::Json> boxes_json = app::Json::parse(text);
    if (!boxes_json || !boxes_json->is(app::Json::Type::Object)) {
        fwprintf(stderr, L"--hud: %s is not a JSON object of clips\n", opt.boxes_file.c_str());
        return 2;
    }
    for (const std::wstring &m : opt.methods)
        if (m != L"motion" && m != L"dlss") {
            fwprintf(stderr, L"--hud: the methods are motion and dlss, not %s\n", m.c_str());
            return 2;
        }
    for (const std::wstring &r : opt.rules)
        if (r != L"motion" && r != L"plain") {
            fwprintf(stderr, L"--hud-rules: motion or plain, not %s\n", r.c_str());
            return 2;
        }

    fg::HudMask hud;
    for (const auto &[name, value] : opt.tuning)
        if (!set_hud_value(hud, name, value)) {
            fwprintf(stderr, L"unknown HUD setting %s\n", name.c_str());
            return 2;
        }
    Vendor dlss(std::make_unique<fg::DlssFg>());
    std::vector<HudScore> scores;

    // --as-pq: SDR samples (gamma 2.2, white at pq_nits) as PQ ones, and back to the nearest 8-bit level.
    const bool as_hdr = opt.hdr || opt.pq_nits > 0;
    std::vector<UINT16> to_pq, from_pq;
    if (opt.pq_nits > 0) {
        const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
        to_pq.resize(65536);
        from_pq.resize(65536);
        for (int i = 0; i < 65536; ++i) {
            const double light = std::pow(std::pow(i / 65535.0, 2.2) * opt.pq_nits / 10000.0, m1);
            to_pq[size_t(i)] = UINT16(std::lround(std::pow((c1 + c2 * light) / (1.0 + c3 * light), m2) * 65535.0));
            const double code = std::pow(i / 65535.0, 1.0 / m2);
            const double nits = 10000.0 * std::pow(std::max(code - c1, 0.0) / (c2 - c3 * code), 1.0 / m1);
            from_pq[size_t(i)] = UINT16(std::lround(std::min(1.0, std::pow(nits / opt.pq_nits, 1.0 / 2.2)) * 255.0) * 257);
        }
    }
    const auto encoded = [&](const Image &img) {
        Image out = img;
        if (!to_pq.empty())
            for (size_t i = 0; i < out.rgba.size(); ++i)
                if (i % 4 != 3) out.rgba[i] = to_pq[out.rgba[i]];
        return out;
    };
    const auto decode = [&](Image &img) {
        if (from_pq.empty()) return;
        for (size_t i = 0; i < img.rgba.size(); ++i)
            if (i % 4 != 3) img.rgba[i] = from_pq[img.rgba[i]];
    };

    for (const fs::path &clip : clips) {
        const app::Json *list = boxes_json->get(clip.filename().string());
        if (!list) continue;
        std::vector<Image> frames;
        for (int i = 1;; ++i) {
            wchar_t name[16];
            swprintf(name, 16, L"f%02d.png", i);
            Image img;
            if (!fs::exists(clip / name) || !load_png(wic, clip / name, img, opt.to_w, opt.to_h)) break;
            frames.push_back(std::move(img));
        }
        if (frames.size() < 5) continue;
        const UINT w = frames[0].w, h = frames[0].h;
        const size_t pixels = size_t(w) * h;
        const int pairs = int(frames.size() - 1) / 2;

        // The hand-marked boxes, scaled along with the frames.
        Image original;
        const bool scaled = opt.to_w != 0 && load_png(wic, clip / L"f01.png", original);
        const double sx = scaled ? double(w) / original.w : 1.0, sy = scaled ? double(h) / original.h : 1.0;
        std::vector<HudBox> boxes;
        std::vector<fg::HudMask::Area> areas;
        for (const app::Json &item : list->items()) {
            const app::Json *box = item.get("box");
            if (!box || box->items().size() != 4) continue;
            HudBox b;
            b.name = item.str("name");
            b.x = int(std::lround(box->items()[0].number() * sx));
            b.y = int(std::lround(box->items()[1].number() * sy));
            b.w = int(std::lround(box->items()[2].number() * sx));
            b.h = int(std::lround(box->items()[3].number() * sy));
            boxes.push_back(b);
            fg::HudMask::Area a;
            a.left = UINT(std::clamp(b.x, 0, int(w)));
            a.top = UINT(std::clamp(b.y, 0, int(h)));
            a.right = UINT(std::clamp(b.x + b.w, 0, int(w)));
            a.bottom = UINT(std::clamp(b.y + b.h, 0, int(h)));
            if (a.left < a.right && a.top < a.bottom) areas.push_back(a);
        }
        size_t counts[3];
        const std::vector<BYTE> region = hud_regions(frames, boxes, counts);
        wprintf(L"%s: %zu frames of %ux%u, %zu boxes; HUD pixels %zu, rest of the boxes %zu, outside %zu\n",
                clip.filename().c_str(), frames.size(), w, h, boxes.size(), counts[0], counts[1], counts[2]);

        std::string error;
        if ((interp.width() != w || interp.height() != h || interp.picture().x1 != 0) &&
            !interp.create(gpu.device.Get(), w, h, error)) {
            fprintf(stderr, "interpolator: %s\n", error.c_str());
            return 1;
        }
        if ((hud.width() != w || hud.height() != h) && !hud.create(gpu.device.Get(), w, h, error)) {
            fprintf(stderr, "HUD mask: %s\n", error.c_str());
            return 1;
        }
        hud.set_pq(as_hdr);

        // Where frames are drawn (16 bits a channel for the own generator, 10
        // for DLSS, as its copies are), and what is read back.
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.SampleDesc.Count = 1;
        ComPtr<ID3D11Texture2D> target, work, work10, staging, mask_read;
        ComPtr<ID3D11RenderTargetView> target_rtv, work_rtv, work10_rtv;
        td.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        gpu.device->CreateTexture2D(&td, nullptr, &target);
        gpu.device->CreateTexture2D(&td, nullptr, &work);
        gpu.device->CreateRenderTargetView(target.Get(), nullptr, &target_rtv);
        gpu.device->CreateRenderTargetView(work.Get(), nullptr, &work_rtv);
        td.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        gpu.device->CreateTexture2D(&td, nullptr, &work10);
        gpu.device->CreateRenderTargetView(work10.Get(), nullptr, &work10_rtv);
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
        gpu.device->CreateTexture2D(&td, nullptr, &staging);
        td.Format = DXGI_FORMAT_R8_UNORM;
        gpu.device->CreateTexture2D(&td, nullptr, &mask_read);
        const auto read16 = [&](ID3D11Texture2D *texture, Image &img) {
            gpu.ctx->CopyResource(staging.Get(), texture);
            D3D11_MAPPED_SUBRESOURCE map{};
            gpu.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map);
            img.w = w;
            img.h = h;
            img.rgba.resize(pixels * 4);
            for (UINT y = 0; y < h; ++y)
                memcpy(img.rgba.data() + size_t(y) * w * 4, static_cast<BYTE *>(map.pData) + size_t(y) * map.RowPitch,
                       size_t(w) * 8);
            gpu.ctx->Unmap(staging.Get(), 0);
        };

        for (const std::wstring &method : opt.methods) {
            const bool by_dlss = method == L"dlss";
            if (by_dlss) {
                std::string vendor_error;
                if (!dlss.prepare(gpu, w, h, as_hdr, vendor_error)) {
                    fprintf(stderr, "dlss: %s\n", vendor_error.c_str());
                    continue;
                }
            }
            for (const std::wstring &rule : opt.rules) {
                const bool plain = rule == L"plain";
                HudScore s;
                s.clip = clip.filename().wstring();
                s.method = method;
                s.rule = rule;
                std::copy(counts, counts + 3, s.counts);
                HudScore late = s;  // --hud-late: the clip's last pairs (s: its first)
                if (opt.late > 0) {
                    wchar_t label[96];
                    swprintf(label, 96, L"%s, pairs %d to %d", rule.c_str(), opt.first_pair, opt.first_pair + opt.late - 1);
                    s.rule = label;
                    swprintf(label, 96, L"%s, pairs %d to %d", rule.c_str(), pairs - opt.late + 1, pairs);
                    late.rule = label;
                }
                hud.reset();
                interp.forget_history();
                ComPtr<ID3D11Texture2D> tex[2];
                ComPtr<ID3D11ShaderResourceView> srv[2];
                if (!by_dlss) make_texture(gpu, encoded(frames[0]), tex[1], srv[1]);
                Image made, in_boxes, everywhere;
                std::vector<BYTE> mask(pixels);
                for (int pair = 1; pair <= pairs; ++pair) {
                    const Image &earlier = frames[size_t(pair) * 2 - 2], &truth = frames[size_t(pair) * 2 - 1],
                                &later = frames[size_t(pair) * 2];
                    ID3D11ShaderResourceView *a = nullptr, *b = nullptr;
                    ID3D11Texture2D *between = nullptr, *draw_on = nullptr;
                    ID3D11RenderTargetView *draw_rtv = nullptr;
                    // The frame in between, as the generator makes it.
                    if (by_dlss) {
                        std::string vendor_error;
                        dlss.put(gpu, 0, encoded(earlier));
                        dlss.put(gpu, 1, encoded(later));
                        // As in the scoring of single pairs above: a running sequence.
                        bool ok = dlss.fg.generate(0, 1, 1, false, vendor_error) && dlss.fg.generate(1, 0, 1, true, vendor_error);
                        if (ok) s.ms_frame.push_back(timed(gpu, [&] { ok = dlss.fg.generate(0, 1, 1, true, vendor_error); }));
                        if (!ok) {
                            fprintf(stderr, "dlss: %s\n", vendor_error.c_str());
                            break;
                        }
                        a = dlss.in_srv[0].Get();
                        b = dlss.in_srv[1].Get();
                        between = dlss.fg.between(1);
                        draw_on = work10.Get();
                        draw_rtv = work10_rtv.Get();
                        dlss.read(gpu, between, made);
                        decode(made);
                    } else {
                        tex[0] = tex[1];
                        srv[0] = srv[1];
                        tex[1].Reset();
                        srv[1].Reset();
                        make_texture(gpu, encoded(later), tex[1], srv[1]);
                        a = srv[0].Get();
                        b = srv[1].Get();
                        fg::DrawOptions o;
                        o.follows = pair > 1;
                        interp.draw(gpu.ctx.Get(), a, b, target_rtv.Get(), o);
                        s.ms_frame.push_back(timed(gpu, [&] { interp.draw(gpu.ctx.Get(), a, b, target_rtv.Get(), o); }));
                        between = target.Get();
                        draw_on = work.Get();
                        draw_rtv = work_rtv.Get();
                        read16(between, made);
                        decode(made);
                    }
                    // The mask, one pair on.
                    fg::BlockMotion motion;
                    if (!plain) {
                        if (by_dlss) s.ms_flow.push_back(timed(gpu, [&] { interp.find_flow(gpu.ctx.Get(), a, b); }));
                        motion = interp.block_motion();
                    }
                    s.ms_detect.push_back(timed(gpu, [&] { hud.update(gpu.ctx.Get(), a, b, plain ? nullptr : &motion); }));
                    {
                        gpu.ctx->CopyResource(mask_read.Get(), hud.mask_texture());
                        D3D11_MAPPED_SUBRESOURCE map{};
                        gpu.ctx->Map(mask_read.Get(), 0, D3D11_MAP_READ, 0, &map);
                        for (UINT y = 0; y < h; ++y)
                            memcpy(mask.data() + size_t(y) * w, static_cast<BYTE *>(map.pData) + size_t(y) * map.RowPitch, w);
                        gpu.ctx->Unmap(mask_read.Get(), 0);
                    }
                    // The later real frame where the mask is set: everywhere, and inside the boxes only.
                    const auto read = [&](Image &img) {
                        if (by_dlss)
                            dlss.read(gpu, draw_on, img);
                        else
                            read16(draw_on, img);
                    };
                    // With the own generator the masked frames are drawn again, from the frames without their HUD
                    // (fg::HudMask::clean): where the mask will be applied, so once for each of the two.
                    const bool cleaning = !by_dlss && !plain && hud.clean >= 1;
                    const bool changing = cleaning || (!by_dlss && !plain && hud.carry >= 1);  // more than the mask's pixels
                    const bool saving = !opt.out_dir.empty() &&
                                        std::find(opt.save_pairs.begin(), opt.save_pairs.end(), pair) != opt.save_pairs.end();
                    const float at = by_dlss || plain ? -1.0f : 0.5f;
                    const auto draw_clean = [&](const fg::HudMask::Area *list, size_t n) {
                        interp.after_motion = [&](ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *&fa,
                                                  ID3D11ShaderResourceView *&fb) { hud.clean_frames(ctx, fa, fb, list, n); };
                        fg::DrawOptions o;
                        o.follows = pair > 1;
                        interp.draw(gpu.ctx.Get(), a, b, target_rtv.Get(), o);
                        interp.after_motion = nullptr;
                    };
                    if (cleaning) s.ms_clean.push_back(timed(gpu, [&] { draw_clean(nullptr, 0); }));
                    Image clean_a, clean_b;  // for --hud-save: what the generator drew from
                    if (cleaning && saving && hud.clean_texture(0)) {
                        read16(hud.clean_texture(0), clean_a);
                        read16(hud.clean_texture(1), clean_b);
                        decode(clean_a);
                        decode(clean_b);
                    }
                    gpu.ctx->CopyResource(draw_on, between);
                    s.ms_apply.push_back(timed(gpu, [&] { hud.apply(gpu.ctx.Get(), b, draw_rtv, nullptr, 0, at, a, b); }));
                    gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
                    read(everywhere);
                    decode(everywhere);
                    if (cleaning) draw_clean(areas.data(), areas.size());
                    gpu.ctx->CopyResource(draw_on, between);
                    s.ms_apply_boxes.push_back(
                        timed(gpu, [&] { hud.apply(gpu.ctx.Get(), b, draw_rtv, areas.data(), areas.size(), at, a, b); }));
                    gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
                    read(in_boxes);
                    decode(in_boxes);
                    if (!by_dlss) interp.keep_history(gpu.ctx.Get());

                    // What the true frame says of each.
                    const bool early = pair >= opt.first_pair && (opt.late <= 0 || pair < opt.first_pair + opt.late);
                    const bool last = opt.late > 0 && pair > pairs - opt.late;
                    const bool scored = early || last;
                    const int off = !by_dlss ? 0 : as_hdr ? 2 : 1;
                    double err[kHudTreatments][3] = {}, sq[kHudTreatments] = {};
                    size_t mask_px = 0, mask_in_box = 0, mask_on_hud = 0;
                    for (size_t i = 0; i < pixels; ++i) {
                        const BYTE flags = region[i];
                        const bool on = mask[i] > 127;
                        const bool in_box = (flags & kInBox) != 0;
                        mask_px += on;
                        mask_in_box += on && in_box;
                        mask_on_hud += on && (flags & kHudPixel);
                        const UINT16 *t = truth.rgba.data() + i * 4, *g = made.rgba.data() + i * 4, *l = later.rgba.data() + i * 4,
                                     *e = everywhere.rgba.data() + i * 4, *x = in_boxes.rgba.data() + i * 4;
                        // The apply pass against the mask: where it is set the later frame (as the generator
                        // has it: 10 bits for DLSS), elsewhere the frame as made.
                        const bool e_real = on, x_real = on && in_box;
                        bool wrong = false;
                        int abs_sum[kHudTreatments] = {}, sq_sum[kHudTreatments] = {};
                        for (int c = 0; c < 3; ++c) {
                            const int tv = eight(t[c]), gv = eight(g[c]), ev = eight(e[c]), xv = eight(x[c]);
                            const int lv = eight(l[c]);
                            const int d[kHudTreatments] = {gv - tv, (in_box ? lv : gv) - tv, xv - tv, ev - tv};
                            for (int k = 0; k < kHudTreatments; ++k) {
                                abs_sum[k] += std::abs(d[k]);
                                sq_sum[k] += d[k] * d[k];
                            }
                            // (DLSS's copies are 10 bits: a real frame's sample there may be a level off l's,
                            // two as PQ.)
                            // (Drawn from the frames without their HUD, the rest is not the frame as made.)
                            if (!changing && !e_real && ev != gv) wrong = true;
                            if (!changing && !x_real && xv != gv) wrong = true;
                            if (e_real && std::abs(ev - lv) > off) wrong = true;
                            if (x_real && std::abs(xv - lv) > off) wrong = true;
                        }
                        s.wrong += wrong;
                        if (!scored) continue;
                        const int set = (flags & kHudPixel) ? 0 : (flags & kBoxRest) ? 1 : !in_box ? 2 : -1;
                        for (int k = 0; k < kHudTreatments; ++k) {
                            if (set >= 0) err[k][set] += abs_sum[k] / 3.0;
                            sq[k] += sq_sum[k] / 3.0;
                        }
                    }
                    s.covered.push_back(double(mask_on_hud) / double(std::max<size_t>(1, counts[0])));
                    for (HudScore *to : {early ? &s : nullptr, last ? &late : nullptr}) {
                        if (!to) continue;
                        for (int k = 0; k < kHudTreatments; ++k) {
                            for (int set = 0; set < 3; ++set) to->err[k][set] += err[k][set];
                            to->mse[k] += sq[k] / double(pixels);
                        }
                        to->mask_px += double(mask_px);
                        to->mask_in_box += double(mask_in_box);
                        to->mask_on_hud += double(mask_on_hud);
                        ++to->pairs;
                    }
                    if (saving) {
                        wchar_t stem[160];
                        swprintf(stem, 160, L"%s_%s_%s_p%02d", s.clip.c_str(), method.c_str(), rule.c_str(), pair);
                        Image picture;
                        picture.w = w;
                        picture.h = h;
                        picture.rgba.resize(pixels * 4);
                        for (size_t i = 0; i < pixels; ++i)
                            picture.rgba[i * 4] = picture.rgba[i * 4 + 1] = picture.rgba[i * 4 + 2] = mask[i] > 127 ? 65535 : 0,
                                             picture.rgba[i * 4 + 3] = 65535;
                        save_png(wic, opt.out_dir / (std::wstring(stem) + L"_mask.png"), picture);
                        save_png(wic, opt.out_dir / (std::wstring(stem) + L"_asis.png"), made);
                        save_png(wic, opt.out_dir / (std::wstring(stem) + L"_everywhere.png"), everywhere);
                        save_png(wic, opt.out_dir / (std::wstring(stem) + L"_boxes.png"), in_boxes);
                        if (!clean_a.rgba.empty()) {
                            save_png(wic, opt.out_dir / (std::wstring(stem) + L"_clean_a.png"), clean_a);
                            save_png(wic, opt.out_dir / (std::wstring(stem) + L"_clean_b.png"), clean_b);
                        }
                    }
                }
                const double n = std::max(1, s.pairs);
                wprintf(L"  %-6s %-6s HUD px %5.2f %5.2f %5.2f %5.2f | rest %5.2f %5.2f %5.2f %5.2f | PSNR %5.2f %5.2f %5.2f %5.2f | "
                        L"mask %7.0f px, %3.0f%% in boxes, %3.0f%% of the HUD | ms: frame %.2f",
                        method.c_str(), rule.c_str(), s.error(0, 0), s.error(1, 0), s.error(2, 0), s.error(3, 0), s.error(0, 1),
                        s.error(1, 1), s.error(2, 1), s.error(3, 1), s.psnr(0), s.psnr(1), s.psnr(2), s.psnr(3), s.mask_px / n,
                        100.0 * s.mask_in_box / std::max(1.0, s.mask_px), 100.0 * s.mask_on_hud / n / double(std::max<size_t>(1, counts[0])),
                        HudScore::median(s.ms_frame));
                if (!s.ms_flow.empty()) wprintf(L", search %.2f", HudScore::median(s.ms_flow));
                wprintf(L", detector %.2f, apply %.3f (boxes %.3f)", HudScore::median(s.ms_detect), HudScore::median(s.ms_apply),
                        HudScore::median(s.ms_apply_boxes));
                if (!s.ms_clean.empty()) wprintf(L", frame without the HUD %.2f", HudScore::median(s.ms_clean));
                if (s.wrong) wprintf(L" | %zu PIXELS NOT AS THE MASK SAYS", s.wrong);
                wprintf(L"\n");
                if (opt.late > 0) {
                    const double m = std::max(1, late.pairs);
                    wprintf(L"  %-6s the last %d pairs: HUD px %5.2f %5.2f %5.2f %5.2f | rest %5.2f %5.2f %5.2f %5.2f | PSNR %5.2f %5.2f "
                            L"%5.2f %5.2f | mask %7.0f px, %3.0f%% in boxes, %3.0f%% of the HUD\n",
                            method.c_str(), opt.late, late.error(0, 0), late.error(1, 0), late.error(2, 0), late.error(3, 0),
                            late.error(0, 1), late.error(1, 1), late.error(2, 1), late.error(3, 1), late.psnr(0), late.psnr(1),
                            late.psnr(2), late.psnr(3), late.mask_px / m, 100.0 * late.mask_in_box / std::max(1.0, late.mask_px),
                            100.0 * late.mask_on_hud / m / double(std::max<size_t>(1, counts[0])));
                }
                fflush(stdout);
                scores.push_back(std::move(s));
                if (opt.late > 0) scores.push_back(std::move(late));
            }
        }
    }

    for (const std::wstring &method : opt.methods) {
        // The rules as they were scored (with --hud-late, each twice).
        std::vector<std::wstring> labels;
        for (const HudScore &s : scores)
            if (s.method == method && std::find(labels.begin(), labels.end(), s.rule) == labels.end()) labels.push_back(s.rule);
        for (const std::wstring &rule : labels) {
            wchar_t title[160];
            swprintf(title, 160, L"error on the HUD's own pixels (levels of 255), pairs %d on", opt.first_pair);
            hud_table(scores, method, rule, title, [](const HudScore &s, int t) { return s.error(t, 0); });
            hud_table(scores, method, rule, L"error on the rest of the boxes (scenery beside and through the HUD)",
                      [](const HudScore &s, int t) { return s.error(t, 1); });
            hud_table(scores, method, rule, L"error outside the boxes", [](const HudScore &s, int t) { return s.error(t, 2); });
            hud_table(scores, method, rule, L"whole frame, PSNR dB", [](const HudScore &s, int t) { return s.psnr(t); });
            wprintf(L"\n%s, rule %s: the mask (pixels set, share of them inside the boxes, pixels outside, share of the HUD's "
                    L"pixels covered) and GPU ms (the frame, the motion search for the mask alone, the detector, the apply "
                    L"pass everywhere, in the boxes, the frame drawn from the frames without their HUD)\n",
                    method.c_str(), rule.c_str());
            for (const HudScore &s : scores) {
                if (s.method != method || s.rule != rule) continue;
                const double n = std::max(1, s.pairs);
                wprintf(L"%-10s %8.0f %4.0f%% %8.0f %4.0f%%   %6.2f %6.2f %6.2f %6.3f %6.3f %6.2f   covered by pair:", s.clip.c_str(),
                        s.mask_px / n, 100.0 * s.mask_in_box / std::max(1.0, s.mask_px), (s.mask_px - s.mask_in_box) / n,
                        100.0 * s.mask_on_hud / n / double(std::max<size_t>(1, s.counts[0])), HudScore::median(s.ms_frame),
                        HudScore::median(s.ms_flow), HudScore::median(s.ms_detect), HudScore::median(s.ms_apply),
                        HudScore::median(s.ms_apply_boxes), HudScore::median(s.ms_clean));
                for (double c : s.covered) wprintf(L" %.0f", 100.0 * c);
                wprintf(L"\n");
            }
        }
    }
    if (!opt.out_dir.empty()) {
        // The same for other tools.
        if (FILE *f = _wfopen((opt.out_dir / L"hud_scores.json").c_str(), L"wb")) {
            fprintf(f, "[\n");
            for (size_t i = 0; i < scores.size(); ++i) {
                const HudScore &s = scores[i];
                const double n = std::max(1, s.pairs);
                fprintf(f, " {\"clip\": \"%ls\", \"method\": \"%ls\", \"rule\": \"%ls\", \"pairs\": %d, \"pixels\": [%zu, %zu, %zu],\n",
                        s.clip.c_str(), s.method.c_str(), s.rule.c_str(), s.pairs, s.counts[0], s.counts[1], s.counts[2]);
                const char *const names[kHudTreatments] = {"as is", "hard box", "mask in boxes", "mask everywhere"};
                for (int t = 0; t < kHudTreatments; ++t)
                    fprintf(f, "  \"%s\": {\"hud\": %.4f, \"rest\": %.4f, \"outside\": %.4f, \"psnr\": %.4f},\n", names[t],
                            s.error(t, 0), s.error(t, 1), s.error(t, 2), s.psnr(t));
                fprintf(f, "  \"mask\": {\"px\": %.1f, \"in_box\": %.4f, \"hud_covered\": %.4f},\n", s.mask_px / n,
                        s.mask_in_box / std::max(1.0, s.mask_px), s.mask_on_hud / n / double(std::max<size_t>(1, s.counts[0])));
                fprintf(f, "  \"ms\": {\"frame\": %.3f, \"search\": %.3f, \"detector\": %.3f, \"apply\": %.4f, \"apply_boxes\": %.4f, "
                           "\"frame_clean\": %.3f},\n",
                        HudScore::median(s.ms_frame), HudScore::median(s.ms_flow), HudScore::median(s.ms_detect),
                        HudScore::median(s.ms_apply), HudScore::median(s.ms_apply_boxes), HudScore::median(s.ms_clean));
                fprintf(f, "  \"covered\": [");
                for (size_t k = 0; k < s.covered.size(); ++k) fprintf(f, "%s%.4f", k ? ", " : "", s.covered[k]);
                fprintf(f, "], \"wrong\": %zu}%s\n", s.wrong, i + 1 < scores.size() ? "," : "");
            }
            fprintf(f, "]\n");
            fclose(f);
        }
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
    std::wstring only_clip, out_dir, methods = L"repeat,blend,motion";
    bool profile = false, show_stats = false, show_layers = false, interp_profile_layers = false;
    // --adjacent: make the frame between each two frames in a row (f02 and f03
    // of a capture are what the add-on worked on) and only save it - there is
    // no true frame between them to score against.
    bool adjacent = false;
    bool hdr = false;  // --hdr: the frames are PQ (for dlss)
    // --picture X0 Y0 X1 Y1: where the picture is in frames with black bars (the add-on has it from mpv);
    // clips it does not fit in are made whole.
    fg::Picture picture;
    std::vector<std::pair<std::wstring, double>> tuning;  // --set NAME VALUE
    HudOptions hud;                                       // --hud and its options
    std::wstring hud_rules = L"motion", hud_save;
    bool methods_given = false;
    const auto split = [](const std::wstring &list) {
        std::vector<std::wstring> out;
        for (size_t p = 0; p <= list.size();) {
            size_t q = list.find(L',', p);
            if (q == std::wstring::npos) q = list.size();
            if (q > p) out.push_back(list.substr(p, q - p));
            p = q + 1;
        }
        return out;
    };
    fs::path seq_dir;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--clip" && i + 1 < argc) only_clip = argv[++i];
        else if (a == L"--out" && i + 1 < argc) out_dir = argv[++i];
        else if (a == L"--methods" && i + 1 < argc) methods = argv[++i], methods_given = true;
        else if (a == L"--hud" && i + 1 < argc) hud.boxes_file = argv[++i];
        else if (a == L"--hud-rules" && i + 1 < argc) hud_rules = argv[++i];
        else if (a == L"--hud-save" && i + 1 < argc) hud_save = argv[++i];
        else if (a == L"--hud-first" && i + 1 < argc) hud.first_pair = _wtoi(argv[++i]);
        else if (a == L"--hud-late" && i + 1 < argc) hud.late = _wtoi(argv[++i]);
        else if (a == L"--as-pq" && i + 1 < argc) hud.pq_nits = _wtof(argv[++i]);
        else if (a == L"--hud-set" && i + 2 < argc) {
            hud.tuning.emplace_back(argv[i + 1], _wtof(argv[i + 2]));
            i += 2;
        }
        else if (a == L"--resize" && i + 2 < argc) {
            hud.to_w = UINT(_wtoi(argv[i + 1]));
            hud.to_h = UINT(_wtoi(argv[i + 2]));
            i += 2;
        }
        else if (a == L"--profile") profile = true;
        else if (a == L"--profile-layers") profile = true, interp_profile_layers = true;
        else if (a == L"--stats") show_stats = true;
        else if (a == L"--layers") show_layers = true;
        else if (a == L"--adjacent") adjacent = true;
        else if (a == L"--hdr") hdr = true;
        else if (a == L"--picture" && i + 4 < argc) {
            picture.x0 = UINT(_wtoi(argv[i + 1]));
            picture.y0 = UINT(_wtoi(argv[i + 2]));
            picture.x1 = UINT(_wtoi(argv[i + 3]));
            picture.y1 = UINT(_wtoi(argv[i + 4]));
            i += 4;
        }
        else if (a == L"--set" && i + 2 < argc) {
            tuning.emplace_back(argv[i + 1], _wtof(argv[i + 2]));
            i += 2;
        }
        else seq_dir = a;
    }
    if (seq_dir.empty()) {
        fwprintf(stderr, L"usage: fg_eval.exe [--clip NAME] [--out DIR] [--methods repeat,blend,motion] SEQ_DIR\n");
        return 2;
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        fwprintf(stderr, L"no WIC\n");
        return 1;
    }

    Gpu gpu;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const UINT dev_flags = GetEnvironmentVariableW(L"FG_D3D_DEBUG", nullptr, 0) ? D3D11_CREATE_DEVICE_DEBUG : 0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, dev_flags, levels, 3, D3D11_SDK_VERSION,
                                 &gpu.device, nullptr, &gpu.ctx))) {
        fwprintf(stderr, L"no D3D11 device\n");
        return 1;
    }
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    gpu.device->CreateQuery(&qd, &gpu.disjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    gpu.device->CreateQuery(&qd, &gpu.start);
    gpu.device->CreateQuery(&qd, &gpu.end);
    {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC ad{};
        if (SUCCEEDED(gpu.device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&ad)))
            wprintf(L"GPU: %s\n", ad.Description);
    }

    const bool hud_mode = !hud.boxes_file.empty();
    if (hud_mode && !methods_given) methods = L"motion";
    const std::vector<std::wstring> method_names = split(methods);
    if (!out_dir.empty()) fs::create_directories(out_dir);

    std::vector<fs::path> clips;
    for (auto &e : fs::directory_iterator(seq_dir))
        if (e.is_directory() && (only_clip.empty() || e.path().filename() == only_clip)) clips.push_back(e.path());
    std::sort(clips.begin(), clips.end());

    if (!hud_mode) {
        wprintf(L"\n%-12s", L"clip");
        for (auto &m : method_names) wprintf(L"  %10s dB %8s", m.c_str(), L"ms");
        wprintf(L"\n");
    }

    fg::Interpolator interp;
    interp.layer_profile = interp_profile_layers;
    for (auto &[name, value] : tuning) {
        if (name == L"radius_fine") interp.search_radius_fine = int(value);
        else if (name == L"radius_coarse") interp.search_radius_coarse = int(value);
        else if (name == L"search_lambda") interp.search_lambda = float(value);
        else if (name == L"mid_lambda") interp.mid_lambda = float(value);
        else if (name == L"occlusion_cost") interp.occlusion_cost = float(value);
        else if (name == L"flow_weight") interp.flow_weight = float(value);
        else if (name == L"bad_cost") interp.bad_cost = float(value);
        else if (name == L"cut_fraction") interp.cut_fraction = float(value);
        else if (name == L"cut_detailed") interp.cut_detailed = float(value);
        else if (name == L"layer_cost") interp.layer_cost = float(value);
        else if (name == L"slow_cost") interp.slow_cost = float(value);
        else if (name == L"static_eps") interp.static_eps = float(value);
        else if (name == L"gain_deadzone") interp.gain_deadzone = float(value);
        else if (name == L"gain_deadzone_matched") interp.gain_deadzone_matched = float(value);
        else if (name == L"soft_temp") interp.soft_temp = float(value);
        else if (name == L"soft_floor") interp.soft_floor = float(value);
        else if (name == L"soft_scale") interp.soft_scale = float(value);
        else if (name == L"debug_view") interp.debug_view = int(value);
        else if (name == L"still_check") interp.still_check = float(value);
        else if (name == L"reach_plateau") interp.reach_plateau = float(value);
        else if (name == L"soft_wide") interp.soft_wide = float(value);
        else if (name == L"guess_cost") interp.guess_cost = float(value);
        else if (name == L"guess_ratio") interp.guess_ratio = float(value);
        else if (name == L"hist_search") interp.hist_search = int(value);
        else if (name == L"hist_cost") interp.hist_cost = float(value);
        else if (name == L"hist_ratio") interp.hist_ratio = float(value);
        else if (name == L"edge_cost") interp.edge_cost = float(value);
        else if (name == L"edge_tol") interp.edge_tol = float(value);
        else if (name == L"edge_fit") interp.edge_fit = float(value);
        else if (name == L"edge_still") interp.edge_still = float(value);
        else if (name == L"edge_explains") interp.edge_explains = float(value);
        else if (name == L"edge_search") interp.edge_search = int(value);
        else if (name == L"hud_detail") interp.hud_detail = float(value);
        else if (name == L"hud_same") interp.hud_same = float(value);
        else if (name == L"hud_across") interp.hud_across = float(value);
        else if (name == L"hud_pixels") interp.hud_pixels = float(value);
        else if (name == L"hud_spread") interp.hud_spread = int(value);
        else if (name == L"hud_slack") interp.hud_slack = float(value);
        else if (name == L"persist_max") interp.persist_max = float(value);
        else if (name == L"warp_taps") interp.warp_taps = int(value);
        else if (name == L"warp_ring") interp.warp_ring = int(value);
        else if (name == L"layer_enable") interp.layer_enable = int(value);
        else if (name == L"layer_margin") interp.layer_margin = float(value);
        else if (name == L"layer_min_bin") interp.layer_min_bin = float(value);
        else if (name == L"layer_detail") interp.layer_detail = float(value);
        else if (name == L"layer_vdiff") interp.layer_vdiff = float(value);
        else if (name == L"layer_link") interp.layer_link = float(value);
        else if (name == L"layer_min_area") interp.layer_min_area = float(value);
        else if (name == L"layer_max_area") interp.layer_max_area = float(value);
        else if (name == L"layer_fill") interp.layer_fill = float(value);
        else if (name == L"layer_band") interp.layer_band = float(value);
        else if (name == L"layer_colour") interp.layer_colour = float(value);
        else if (name == L"layer_bgfail") interp.layer_bgfail = float(value);
        else if (name == L"layer_sup") interp.layer_sup = float(value);
        else if (name == L"layer_both") interp.layer_both = int(value);
        else if (name == L"layer_kill") interp.layer_kill = float(value);
        else if (name == L"layer_void") interp.layer_void = float(value);
        else if (name == L"layer_dark") interp.layer_dark = float(value);
        else if (name == L"layer_edge") interp.layer_edge = float(value);
        else if (name == L"layer_grow_iters") interp.layer_grow_iters = int(value);
        else if (name == L"layer_grow_passes") interp.layer_grow_passes = int(value);
        else if (name == L"layer_refine") interp.layer_refine = int(value);
        else if (name == L"layer_gate") interp.layer_gate = int(value);
        else if (name == L"layer_thin") interp.layer_thin = float(value);
        else if (name == L"layer_fit") interp.layer_fit = float(value);
        else if (name == L"layer_attach") interp.layer_attach = float(value);
        else if (name == L"layer_bgslack") interp.layer_bgslack = float(value);
        else if (name == L"layer_vmax") interp.layer_vmax = float(value);
        else if (name == L"cap_fit") interp.cap_fit = float(value);
        else if (name == L"cap_cell") interp.cap_cell = float(value);
        else if (name == L"layer_wmax") interp.layer_wmax = float(value);
        else if (name == L"layer_nbmotion") interp.layer_nbmotion = float(value);
        else if (name == L"layer_inner_colour") interp.layer_inner_colour = float(value);
        else if (name == L"layer_holes") interp.layer_holes = float(value);
        else if (name == L"layer_bgnear") interp.layer_bgnear = float(value);
        else if (name == L"layer_select_gates") interp.layer_select_gates = float(value);
        else if (name == L"layer_ghost") interp.layer_ghost = float(value);
        else if (name == L"layer_bg_flat") interp.layer_bg_flat = float(value);
        else if (name == L"layer_nb_own") interp.layer_nb_own = float(value);
        else if (name == L"layer_still") interp.layer_still = float(value);
        else if (name == L"layer_select_dom") interp.layer_select_dom = float(value);
        else if (name == L"layer_merge_core") interp.layer_merge_core = float(value);
        else if (name == L"layer_hcentre") interp.layer_hcentre = float(value);
        else if (name == L"layer_merge") interp.layer_merge = float(value);
        else if (name == L"layer_refine_edge") interp.layer_refine_edge = float(value);
        else if (name == L"layer_still_pow") interp.layer_still_pow = float(value);
        else if (name == L"layer_still_dx") interp.layer_still_dx = float(value);
        else {
            fwprintf(stderr, L"unknown setting %s\n", name.c_str());
            return 2;
        }
    }
    if (hud_mode) {
        hud.methods = method_names;
        hud.rules = split(hud_rules);
        for (const std::wstring &pair : split(hud_save)) hud.save_pairs.push_back(_wtoi(pair.c_str()));
        hud.out_dir = out_dir;
        hud.hdr = hdr;
        return run_hud(gpu, wic.Get(), clips, interp, hud);
    }
    std::vector<double> totals(method_names.size(), 0.0);
    Vendor dlss(std::make_unique<fg::DlssFg>()), fsr(std::make_unique<fg::FsrFg>()), anr(std::make_unique<fg::AnrFg>());
    for (auto &clip : clips) {
        std::vector<Image> frames;
        for (int i = 1;; ++i) {
            wchar_t name[16];
            swprintf(name, 16, L"f%02d.png", i);
            Image img;
            if (!fs::exists(clip / name) || !load_png(wic.Get(), clip / name, img)) break;
            frames.push_back(std::move(img));
        }
        if (frames.size() < 3) continue;
        const UINT w = frames[0].w, h = frames[0].h;

        std::string error;
        const fg::Picture place = picture.x1 <= w && picture.y1 <= h ? picture : fg::Picture{};
        if ((interp.width() != w || interp.height() != h || !(interp.picture() == place)) &&
            !interp.create(gpu.device.Get(), w, h, error, place)) {
            fprintf(stderr, "interpolator: %s\n", error.c_str());
            return 1;
        }
        std::vector<ComPtr<ID3D11Texture2D>> tex(frames.size());
        std::vector<ComPtr<ID3D11ShaderResourceView>> srv(frames.size());
        for (size_t i = 0; i < frames.size(); ++i) make_texture(gpu, frames[i], tex[i], srv[i]);

        D3D11_TEXTURE2D_DESC td{};
        tex[0]->GetDesc(&td);
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> target, staging;
        ComPtr<ID3D11RenderTargetView> rtv;
        gpu.device->CreateTexture2D(&td, nullptr, &target);
        gpu.device->CreateRenderTargetView(target.Get(), nullptr, &rtv);
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        gpu.device->CreateTexture2D(&td, nullptr, &staging);

        wprintf(L"%-12s", clip.filename().c_str());
        for (size_t m = 0; m < method_names.size(); ++m) {
            const std::wstring &method = method_names[m];
            Result r;
            const size_t step = adjacent ? 1 : 2;
            interp.forget_history();
            for (size_t i = 0; i + step < frames.size(); i += step) {
                const Image &truth = frames[i + (adjacent ? 0 : 1)];
                const size_t next = i + step;
                Image out;
                out.w = w;
                out.h = h;
                if (method == L"repeat") {
                    out.rgba = frames[i].rgba;
                } else if (method == L"dlss" || method == L"fsr" || method == L"anr" || method == L"anrflow") {
                    const bool with_motion = method == L"anrflow";
                    Vendor &vendor = method == L"dlss" ? dlss : method == L"fsr" ? fsr : anr;
                    std::string vendor_error;
                    if (!vendor.prepare(gpu, w, h, hdr, vendor_error)) {
                        if (!vendor_error.empty()) fprintf(stderr, "%ls: %s\n", method.c_str(), vendor_error.c_str());
                        out.rgba = frames[i].rgba;  // scores as "repeat"
                    } else {
                        vendor.put(gpu, 0, frames[i]);
                        vendor.put(gpu, 1, frames[next]);
                        bool made = true;
                        // As a running sequence gives them: a start (A, B), back
                        // to A, and the frame scored is the one made with B then
                        // (FG_EVAL_RESTART=1: the frame of a fresh start).
                        const bool fresh = getenv("FG_EVAL_RESTART") != nullptr;
                        if (&vendor == &anr) {
                            // "anrflow": the own search's motion back from the later frame as its motion
                            // vectors, and the frame of a fresh start (the motion is this pair's alone).
                            if (with_motion) interp.find_flow(gpu.ctx.Get(), srv[i].Get(), srv[next].Get(), true);
                            made = vendor.put_motion(gpu, interp, with_motion, vendor_error);
                        }
                        if (made && with_motion) {
                            r.ms.push_back(timed(gpu, [&] { made = vendor.fg.generate(0, 1, 1, false, vendor_error); }));
                        } else if (made) {
                        made = vendor.fg.generate(0, 1, 1, false, vendor_error);
                        if (made) made = vendor.fg.generate(1, 0, 1, !fresh, vendor_error);
                        if (made)
                            r.ms.push_back(timed(gpu, [&] { made = vendor.fg.generate(0, 1, 1, !fresh, vendor_error); }));
                        }
                        if (made) {
                            vendor.get(gpu, 1, out);
                        } else {
                            fprintf(stderr, "%ls: %s\n", method.c_str(), vendor_error.c_str());
                            out.rgba = frames[i].rgba;
                        }
                    }
                } else {
                    fg::DrawOptions o;
                    o.method = method == L"motion" ? fg::Method::motion : fg::Method::blend;
                    o.follows = i > 0;  // each pair starts on the frame the last one ended on
                    // A few untimed runs first, then the timed one.
                    for (int warm = 0; warm < 3; ++warm)
                        interp.draw(gpu.ctx.Get(), srv[i].Get(), srv[next].Get(), rtv.Get(), o);
                    r.ms.push_back(timed(gpu, [&] {
                        interp.draw(gpu.ctx.Get(), srv[i].Get(), srv[next].Get(), rtv.Get(), o);
                    }));
                    if (o.method == fg::Method::motion) interp.keep_history(gpu.ctx.Get());
                    if (show_stats && o.method == fg::Method::motion && interp.stats_texture()) {
                        D3D11_TEXTURE2D_DESC sd{};
                        interp.stats_texture()->GetDesc(&sd);
                        sd.BindFlags = 0;
                        sd.Usage = D3D11_USAGE_STAGING;
                        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        ComPtr<ID3D11Texture2D> st;
                        gpu.device->CreateTexture2D(&sd, nullptr, &st);
                        gpu.ctx->CopyResource(st.Get(), interp.stats_texture());
                        D3D11_MAPPED_SUBRESOURCE sm{};
                        gpu.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &sm);
                        const float *v = static_cast<const float *>(sm.pData);
                        wprintf(L" [pair %zu: gain %.3f, unmatched %.3f, of detailed %.3f, detailed %.3f]", i / 2 + 1, v[0], v[1],
                                v[2], v[3]);
                        gpu.ctx->Unmap(st.Get(), 0);
                    }
                    if (show_layers && o.method == fg::Method::motion && interp.layer_texture()) {
                        D3D11_TEXTURE2D_DESC sd{};
                        interp.layer_texture()->GetDesc(&sd);
                        sd.BindFlags = 0;
                        sd.Usage = D3D11_USAGE_STAGING;
                        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        ComPtr<ID3D11Texture2D> st;
                        gpu.device->CreateTexture2D(&sd, nullptr, &st);
                        gpu.ctx->CopyResource(st.Get(), interp.layer_texture());
                        D3D11_MAPPED_SUBRESOURCE sm{};
                        gpu.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &sm);
                        const float *v = static_cast<const float *>(sm.pData);
                        wprintf(L"\n  [layer %zu: active %.0f dom (%.1f,%.1f) peak2 %.0f other %.0f | root %.0f v (%.1f,%.1f) "
                                L"cells %.0f..%.0f x %.0f..%.0f area %.0f fill %.2f margin %.3f luma %.2f bg (%.1f,%.1f) | alive %.0f f %.3f e %.3f px %.0f | %.0f %.0f %.0f %.0f %.0f %.0f | cells k0 %.0f k1 %.0f bg %.0f fgpx %.0f bgpx %.0f | seedA %.0f candA %.0f seedB %.0f candB %.0f | vcost %.3f at v0 %.3f v0 (%.1f,%.1f) | cands %.0f members %.0f]",
                                i + 1, v[0], v[1], v[2], v[3], v[14], v[4], v[5], v[6], v[7], v[9], v[8], v[10], v[11], v[12],
                                v[13], v[23], v[24], v[25], v[15], v[20], v[21], v[22], v[26], v[27], v[28], v[29], v[30], v[31], v[32], v[33], v[34], v[35], v[36], v[37], v[38], v[39], v[40], v[41], v[42], v[43], v[44], v[46], v[45]);
                        gpu.ctx->Unmap(st.Get(), 0);
                        if (getenv("FG_LAYER_COMPS")) {
                            // Every component in the running (area >= 12): area, box, black cells, mean margin, motion.
                            D3D11_BUFFER_DESC bd{};
                            interp.layer_counters()->GetDesc(&bd);
                            bd.BindFlags = 0;
                            bd.MiscFlags = 0;
                            bd.Usage = D3D11_USAGE_STAGING;
                            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                            ComPtr<ID3D11Buffer> sb;
                            gpu.device->CreateBuffer(&bd, nullptr, &sb);
                            gpu.ctx->CopyResource(sb.Get(), interp.layer_counters());
                            D3D11_MAPPED_SUBRESOURCE cm{};
                            gpu.ctx->Map(sb.Get(), 0, D3D11_MAP_READ, 0, &cm);
                            const UINT *u = static_cast<const UINT *>(cm.pData);
                            const UINT cells = interp.grid_w() * interp.grid_h();
                            const UINT min_area = std::max(1, atoi(getenv("FG_LAYER_COMPS")));
                            for (UINT c = 0; c < cells; ++c) {
                                const UINT *a = u + size_t(c) * 9;
                                if (a[0] < (min_area > 1 ? min_area : 12u)) continue;
                                const int x0 = int(0xffff - a[1]), y0 = int(0xffff - a[2]), x1 = int(a[3]), y1 = int(a[4]);
                                const float vx = float(int(a[7])) / (16.0f * a[0]) * 2.0f, vy = float(int(a[8])) / (16.0f * a[0]) * 2.0f;
                                const float fill = float(a[0]) / float((x1 - x0 + 1) * (y1 - y0 + 1));
                                wprintf(L"\n    comp %u: area %u cells %d..%d x %d..%d fill %.2f black %u margin %.3f v (%.1f,%.1f)", c, a[0], x0, x1,
                                        y0, y1, fill, a[5], a[6] / 1000.0f / a[0], vx, vy);
                            }
                            gpu.ctx->Unmap(sb.Get(), 0);
                        }
                        if (getenv("FG_LAYER_VCOST")) {
                            // L_REFINE's cost per offset around the cells' mean (every second row and column).
                            D3D11_TEXTURE2D_DESC vd{};
                            interp.layer_vcost()->GetDesc(&vd);
                            vd.BindFlags = 0;
                            vd.Usage = D3D11_USAGE_STAGING;
                            vd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                            ComPtr<ID3D11Texture2D> vt;
                            gpu.device->CreateTexture2D(&vd, nullptr, &vt);
                            gpu.ctx->CopyResource(vt.Get(), interp.layer_vcost());
                            D3D11_MAPPED_SUBRESOURCE vm{};
                            gpu.ctx->Map(vt.Get(), 0, D3D11_MAP_READ, 0, &vm);
                            for (UINT y = 0; y < vd.Height; y += 2) {
                                wprintf(L"\n    vc %+3d:", int(y) - 16);
                                const float *row = reinterpret_cast<const float *>(static_cast<BYTE *>(vm.pData) + y * vm.RowPitch);
                                for (UINT x = 0; x < vd.Width; x += 2) wprintf(L" %3.0f", row[x] * 1000.0f);
                            }
                            gpu.ctx->Unmap(vt.Get(), 0);
                        }
                        if (getenv("FG_LAYER_ARGS")) {
                            D3D11_BUFFER_DESC bd{};
                            interp.layer_args2()->GetDesc(&bd);
                            bd.BindFlags = 0;
                            bd.MiscFlags = 0;
                            bd.Usage = D3D11_USAGE_STAGING;
                            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                            ComPtr<ID3D11Buffer> sb;
                            gpu.device->CreateBuffer(&bd, nullptr, &sb);
                            gpu.ctx->CopyResource(sb.Get(), interp.layer_args2());
                            gpu.ctx->Map(sb.Get(), 0, D3D11_MAP_READ, 0, &sm);
                            const UINT *u = static_cast<const UINT *>(sm.pData);
                            wprintf(L" args");
                            for (int k = 0; k < 12; ++k) wprintf(L" %u", u[k]);
                            gpu.ctx->Unmap(sb.Get(), 0);
                        }
                    }
                    gpu.ctx->CopyResource(staging.Get(), target.Get());
                    D3D11_MAPPED_SUBRESOURCE map{};
                    gpu.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map);
                    out.rgba.resize(size_t(w) * h * 4);
                    for (UINT y = 0; y < h; ++y)
                        memcpy(out.rgba.data() + size_t(y) * w * 4, static_cast<BYTE *>(map.pData) + size_t(y) * map.RowPitch,
                               size_t(w) * 8);
                    gpu.ctx->Unmap(staging.Get(), 0);
                }
                const double p = psnr(out, truth);
                if (std::isinf(p))
                    r.any_inf = true;
                else
                    r.psnr_sum += p, ++r.psnr_n;
                if (!out_dir.empty()) {
                    // The first pair as <clip>_<method>.png, the others with their number.
                    std::wstring name = clip.filename().wstring() + L"_" + method;
                    if (adjacent)
                        name += L"_adj" + std::to_wstring(i + 1) + std::to_wstring(i + 2);
                    else if (i > 0)
                        name += L"_" + std::to_wstring(i / 2 + 1);
                    save_png(wic.Get(), fs::path(out_dir) / (name + L".png"), out);
                }
            }
            const double avg = r.psnr_n ? r.psnr_sum / r.psnr_n : INFINITY;
            totals[m] += std::isinf(avg) ? 99.0 : avg;
            std::sort(r.ms.begin(), r.ms.end());
            const double ms = r.ms.empty() ? 0.0 : r.ms[r.ms.size() / 2];
            wprintf(L"  %10.2f dB %8.3f", avg, ms);
        }
        wprintf(L"\n");
        if (profile) {
            // Each stage of one motion draw, from a timestamp after each.
            std::vector<ComPtr<ID3D11Query>> marks;
            std::vector<std::string> names;
            D3D11_QUERY_DESC tq{D3D11_QUERY_TIMESTAMP, 0};
            interp.on_stage = [&](const char *name) {
                ComPtr<ID3D11Query> q;
                gpu.device->CreateQuery(&tq, &q);
                gpu.ctx->End(q.Get());
                marks.push_back(q);
                names.push_back(name);
            };
            fg::DrawOptions o;
            gpu.ctx->Begin(gpu.disjoint.Get());
            gpu.ctx->End(gpu.start.Get());
            interp.draw(gpu.ctx.Get(), srv[0].Get(), srv[2].Get(), rtv.Get(), o);
            gpu.ctx->End(gpu.disjoint.Get());
            interp.on_stage = nullptr;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            while (gpu.ctx->GetData(gpu.disjoint.Get(), &dj, sizeof(dj), 0) == S_FALSE) YieldProcessor();
            UINT64 prev = 0;
            while (gpu.ctx->GetData(gpu.start.Get(), &prev, sizeof(prev), 0) == S_FALSE) YieldProcessor();
            wprintf(L"%-12s", L"");
            for (size_t i = 0; i < marks.size(); ++i) {
                UINT64 stamp = 0;
                while (gpu.ctx->GetData(marks[i].Get(), &stamp, sizeof(stamp), 0) == S_FALSE) YieldProcessor();
                wprintf(L"  %S %.3f", names[i].c_str(), double(stamp - prev) * 1000.0 / double(dj.Frequency));
                prev = stamp;
            }
            wprintf(L"\n");
        }
    }
    {
        ComPtr<ID3D11InfoQueue> iq;
        if (dev_flags && SUCCEEDED(gpu.device.As(&iq))) {
            const UINT64 nm = iq->GetNumStoredMessages();
            for (UINT64 k = 0; k < nm && k < 20; ++k) {
                SIZE_T len = 0;
                iq->GetMessage(k, nullptr, &len);
                std::vector<char> buf(len);
                auto *m = reinterpret_cast<D3D11_MESSAGE *>(buf.data());
                if (SUCCEEDED(iq->GetMessage(k, m, &len))) printf("D3D: %.*s\n", int(m->DescriptionByteLength), m->pDescription);
            }
        }
    }
    wprintf(L"%-12s", L"mean");
    for (size_t m = 0; m < method_names.size(); ++m) wprintf(L"  %10.2f dB %8s", totals[m] / std::max<size_t>(1, clips.size()), L"");
    wprintf(L"\n");
    return 0;
}
