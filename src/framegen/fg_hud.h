// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// fg_hud.h - finds the HUD by itself, and keeps it out of the frames in
// between.
//
// A frame generator moves what it sees; a HUD does not move, and what is drawn
// over moving scenery comes out faded, doubled or in pieces. The areas left
// alone (keep_regions.h) cure that where the user draws a box, at the price of
// everything in the box standing still, the scenery seen beside and through
// the HUD included. Here the HUD's own pixels are found (hud.hlsl says how):
// what has stood still for a few pairs of real frames while the motion found
// for the scenery says it should have changed. A frame in between then shows
// the nearer real frame on those pixels only - after either generator, the
// app's own (fg_interp.h) or a GPU vendor's (fg_bridge.h).
//
// Shared by the add-on (framegen_addon.cpp) and the evaluator
// (eval/fg_eval.cpp), like fg_interp.h. The caller owns the device context and
// its state: update() and apply() change pipeline state freely, within the
// slots StateBackup in framegen_addon.cpp covers.
//
// Two rules (hud.hlsl):
//   motion  with the interpolator's block motion from a to b
//           (fg::BlockMotion): what is measured in docs/FRAMEGEN-LEARNINGS.md.
//   plain   without any: detail that does not change while the frame as a
//           whole does. For when no motion is to be had.
//
// Thresholds are in steps of 8-bit SDR luma. For HDR10 (PQ) a step counts
// half as much of the code range (set_pq): around a HUD's brightness, 10 to
// 200 nits, PQ spends about half the code range an SDR signal does on the same
// contrast, and the PS5's noise in a 10-bit signal is about half as large.
// That is arithmetic, not measurement: every clip the rule was tuned and
// scored on is SDR.

#pragma once
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "fg_interp.h"
#include "hud_apply_ps.h"
#include "hud_apply_vs.h"
#include "hud_bg_cs.h"
#include "hud_clean_ps.h"
#include "hud_count_cs.h"
#include "hud_fill_cs.h"
#include "hud_luma_cs.h"
#include "hud_mask_cs.h"
#include "hud_mean_cs.h"
#include "hud_plain_cs.h"
#include "hud_range_cs.h"
#include "hud_update_cs.h"
#include "hud_ways_cs.h"

namespace fg {

// One GPU time, measured now and then with timestamp queries and read back
// without waiting (the add-on's log).
class GpuTimer {
public:
    bool create(ID3D11Device *device) {
        D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        if (FAILED(device->CreateQuery(&d, &disjoint_))) return false;
        d.Query = D3D11_QUERY_TIMESTAMP;
        return SUCCEEDED(device->CreateQuery(&d, &start_)) && SUCCEEDED(device->CreateQuery(&d, &end_));
    }
    void release() {
        disjoint_.Reset();
        start_.Reset();
        end_.Reset();
        open_ = waiting_ = false;
        sum_ = 0;
        n_ = 0;
    }
    // Around the work, on one call in `every`. Nothing while the last
    // measurement is still on its way back.
    void begin(ID3D11DeviceContext *ctx, unsigned every = 8) {
        if (!disjoint_ || waiting_ || ++calls_ % every != 0) return;
        ctx->Begin(disjoint_.Get());
        ctx->End(start_.Get());
        open_ = true;
    }
    void end(ID3D11DeviceContext *ctx) {
        if (!open_) return;
        ctx->End(end_.Get());
        ctx->End(disjoint_.Get());
        open_ = false;
        waiting_ = true;
    }
    void poll(ID3D11DeviceContext *ctx) {
        if (!waiting_) return;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 t0 = 0, t1 = 0;
        const UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
        if (ctx->GetData(disjoint_.Get(), &dj, sizeof(dj), flags) != S_OK ||
            ctx->GetData(start_.Get(), &t0, sizeof(t0), flags) != S_OK ||
            ctx->GetData(end_.Get(), &t1, sizeof(t1), flags) != S_OK)
            return;
        waiting_ = false;
        if (dj.Disjoint || dj.Frequency == 0 || t1 < t0) return;
        sum_ += double(t1 - t0) * 1000.0 / double(dj.Frequency);
        ++n_;
    }
    // The mean of what was measured since the last call, in ms; under 0: nothing.
    double take_ms() {
        const double ms = n_ ? sum_ / n_ : -1.0;
        sum_ = 0;
        n_ = 0;
        return ms;
    }

private:
    Microsoft::WRL::ComPtr<ID3D11Query> disjoint_, start_, end_;
    bool open_ = false, waiting_ = false;
    unsigned calls_ = 0;
    double sum_ = 0;
    unsigned n_ = 0;
};

class HudMask {
public:
    enum class Rule { motion, plain };

    // A part of the target apply() draws: a box, or the ellipse inside it.
    struct Area {
        UINT left = 0, top = 0, right = 0, bottom = 0;  // right and bottom are one past the last pixel
        bool ellipse = false;
    };

    // width x height: the frames given to update() and apply().
    bool create(ID3D11Device *device, UINT width, UINT height, std::string &error) {
        release();
        device_ = device;
        width_ = width;
        height_ = height;
        HRESULT hr = S_OK;
        auto check = [&](HRESULT r, const char *what) {
            if (FAILED(r) && SUCCEEDED(hr)) {
                hr = r;
                char text[64];
                snprintf(text, sizeof(text), "%s: 0x%08lX", what, static_cast<unsigned long>(r));
                error = text;
            }
        };
        check(device->CreateComputeShader(g_hud_luma_cs, sizeof(g_hud_luma_cs), nullptr, &luma_cs_), "HUD luma shader");
        check(device->CreateComputeShader(g_hud_bg_cs, sizeof(g_hud_bg_cs), nullptr, &bg_cs_), "HUD background shader");
        check(device->CreateComputeShader(g_hud_fill_cs, sizeof(g_hud_fill_cs), nullptr, &fill_cs_), "HUD fill shader");
        check(device->CreateComputeShader(g_hud_ways_cs, sizeof(g_hud_ways_cs), nullptr, &ways_cs_), "HUD motion shader");
        check(device->CreateComputeShader(g_hud_update_cs, sizeof(g_hud_update_cs), nullptr, &update_cs_), "HUD update shader");
        check(device->CreateComputeShader(g_hud_range_cs, sizeof(g_hud_range_cs), nullptr, &range_cs_), "HUD range shader");
        check(device->CreateComputeShader(g_hud_mean_cs, sizeof(g_hud_mean_cs), nullptr, &mean_cs_), "HUD mean shader");
        check(device->CreateComputeShader(g_hud_plain_cs, sizeof(g_hud_plain_cs), nullptr, &plain_cs_), "HUD plain shader");
        check(device->CreateComputeShader(g_hud_mask_cs, sizeof(g_hud_mask_cs), nullptr, &mask_cs_), "HUD mask shader");
        check(device->CreateComputeShader(g_hud_count_cs, sizeof(g_hud_count_cs), nullptr, &count_cs_), "HUD count shader");
        check(device->CreateVertexShader(g_hud_apply_vs, sizeof(g_hud_apply_vs), nullptr, &apply_vs_), "HUD vertex shader");
        check(device->CreatePixelShader(g_hud_apply_ps, sizeof(g_hud_apply_ps), nullptr, &apply_ps_), "HUD apply shader");
        check(device->CreatePixelShader(g_hud_clean_ps, sizeof(g_hud_clean_ps), nullptr, &clean_ps_), "HUD clean shader");
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = sizeof(Params);
        cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&cbd, nullptr, &cb_), "HUD constant buffer");
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sd, &linear_), "HUD sampler");

        // The small formats where the GPU can write them from a compute
        // shader (every Direct3D 11 GPU should); else ones the interpolator
        // writes too (flow.hlsl), which hold the same values in more bytes.
        const auto writable = [&](DXGI_FORMAT wanted, DXGI_FORMAT other) {
            UINT support = 0;
            return SUCCEEDED(device->CheckFormatSupport(wanted, &support)) &&
                           (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)
                       ? wanted
                       : other;
        };
        pair_format_ = writable(DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_FLOAT);
        check(make(width, height, pair_format_, luma_), "HUD luma");
        for (Surface &s : count_)
            check(make(width, height, writable(DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R32G32_UINT), s), "HUD counters");
        check(make(width, height, writable(DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R16_FLOAT), mask_), "HUD mask");
        check(make(1, 1, DXGI_FORMAT_R32_FLOAT, mean_), "HUD mean");
        check(make(1, 1, DXGI_FORMAT_R32_UINT, total_), "HUD count");
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = 1;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_UINT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&td, nullptr, &total_read_), "HUD count readback");
        if (FAILED(hr)) {
            release();
            return false;
        }
        update_timer.create(device);
        apply_timer.create(device);
        clean_timer.create(device);
        reset();
        return true;
    }

    void release() {
        luma_cs_.Reset(), bg_cs_.Reset(), fill_cs_.Reset(), ways_cs_.Reset(), update_cs_.Reset();
        range_cs_.Reset(), mean_cs_.Reset(), plain_cs_.Reset(), mask_cs_.Reset(), count_cs_.Reset();
        apply_vs_.Reset();
        apply_ps_.Reset();
        clean_ps_.Reset();
        for (Drawn &c : clean_) c = Drawn{};
        cb_.Reset();
        linear_.Reset();
        luma_ = range_ = mask_ = mean_ = total_ = ways_ = Surface{};
        for (Surface &s : count_) s = Surface{};
        for (Surface &s : bg_) s = Surface{};
        total_read_.Reset();
        update_timer.release();
        apply_timer.release();
        clean_timer.release();
        device_.Reset();
        width_ = height_ = 0;
        grid_w_ = grid_h_ = 0;
        pairs_ = 0;
        count_asked_ = false;
    }

    bool ready() const { return device_ != nullptr; }
    UINT width() const { return width_; }
    UINT height() const { return height_; }

    // What was counted is forgotten: a new picture size or place, a switch
    // between SDR and HDR, a long break. The mask is empty until two pairs on.
    void reset() { fresh_ = true; }

    // The frames hold HDR10 (PQ) values, not SDR ones: what a level is.
    void set_pq(bool pq) {
        const float now = (pq ? pq_step : 1.0f) / 255.0f;
        if (now != unit_) reset();
        unit_ = now;
    }

    // How many pairs the counters have seen since they started over. The
    // mask can hold something from the second on.
    unsigned pairs() const { return pairs_; }
    Rule rule() const { return rule_; }
    const Picture &picture() const { return picture_; }
    ID3D11Texture2D *mask_texture() const { return mask_.tex.Get(); }
    // clean_frames()' last pictures: a (0) and b (1) without their HUD (null: none yet). For the evaluator.
    ID3D11Texture2D *clean_texture(int which) const { return clean_[which & 1].tex.Get(); }

    // One pair of real frames on: a then b, width x height shader resources,
    // b the frame after a. Once per pair, before apply() for its frames in
    // between. motion: the block motion from a to b (the interpolator's, of
    // its draw of this pair or its find_flow); without one (null, or no
    // field) the plain rule is used. picture: the part of the frames that is
    // looked at when there is no motion to say (all 0: the whole frame).
    void update(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *a, ID3D11ShaderResourceView *b,
                const BlockMotion *motion, const Picture &picture = {}) {
        if (!ready()) return;
        const bool with_motion = motion && motion->field && motion->grid_w && motion->grid_h;
        const Rule rule = with_motion ? Rule::motion : Rule::plain;
        Picture place = with_motion ? motion->picture : picture;
        if (place.x1 > width_ || place.y1 > height_ || place.x0 >= place.x1 || place.y0 >= place.y1) place = Picture{};
        if (rule != rule_ || !(place == picture_)) reset();
        rule_ = rule;
        picture_ = place;
        if (rule == Rule::plain && !range_.tex && FAILED(make(width_, height_, pair_format_, range_))) return;
        if (with_motion && !ensure_grid(motion->grid_w, motion->grid_h)) return;
        if (fresh_) {
            const UINT zero[4] = {0, 0, 0, 0};
            const FLOAT none[4] = {0, 0, 0, 0};
            // Both: outside the picture neither is written again.
            for (Surface &s : count_) ctx->ClearUnorderedAccessViewUint(s.uav.Get(), zero);
            ctx->ClearUnorderedAccessViewFloat(mask_.uav.Get(), none);
            fresh_ = false;
            pairs_ = 0;
        }

        Params p = params();
        if (with_motion) {
            p.flow_scale = motion->to_frame;
            p.has_stats = motion->stats ? 1 : 0;
            p.cut_fraction = motion->cut_fraction;
            p.cut_detailed = motion->cut_detailed;
        }
        ctx->UpdateSubresource(cb_.Get(), 0, nullptr, &p, 0, 0);
        ctx->CSSetConstantBuffers(0, 1, cb_.GetAddressOf());
        ctx->CSSetSamplers(0, 1, linear_.GetAddressOf());
        const UINT gx = groups(UINT(p.pic_w)), gy = groups(UINT(p.pic_h));
        dispatch(ctx, luma_cs_.Get(), {a, b}, luma_.uav.Get(), gx, gy);
        if (with_motion) {
            // The scenery's motion: per block, filled from the blocks around, then per cell.
            dispatch(ctx, bg_cs_.Get(), {motion->field}, bg_[0].uav.Get(), UINT(p.block_w), UINT(p.block_h));
            for (int i = 0; i < 3; ++i)
                dispatch(ctx, fill_cs_.Get(), {bg_[i & 1].srv.Get()}, bg_[(i & 1) ^ 1].uav.Get(), groups(UINT(p.block_w)),
                         groups(UINT(p.block_h)));
            dispatch(ctx, ways_cs_.Get(), {motion->field, bg_[1].srv.Get()}, ways_.uav.Get(), groups(grid_w_), groups(grid_h_));
            dispatch(ctx, update_cs_.Get(), {luma_.srv.Get(), ways_.srv.Get(), count_[cur_].srv.Get(), motion->stats},
                     count_[cur_ ^ 1].uav.Get(), gx, gy);
        } else {
            dispatch(ctx, range_cs_.Get(), {luma_.srv.Get()}, range_.uav.Get(), gx, gy);
            dispatch(ctx, mean_cs_.Get(), {luma_.srv.Get()}, mean_.uav.Get(), 1, 1);
            dispatch(ctx, plain_cs_.Get(), {luma_.srv.Get(), range_.srv.Get(), count_[cur_].srv.Get(), mean_.srv.Get()},
                     count_[cur_ ^ 1].uav.Get(), gx, gy);
        }
        cur_ ^= 1;
        dispatch(ctx, mask_cs_.Get(), {count_[cur_].srv.Get()}, mask_.uav.Get(), gx, gy);
        unbind(ctx);
        ++pairs_;
    }

    // Draws real (the nearer real frame, a shader resource) into target (the
    // frame in between, a render target) where the mask is set: over the
    // whole picture (no areas), or inside each area only. t: where the frame
    // in between sits (0 a, 1 b), for a frame the generator drew from
    // clean_frames()' pictures: a pixel that had only HUD to be drawn from
    // gets the real frame too. Under 0: the mask alone.
    void apply(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *real, ID3D11RenderTargetView *target,
               const Area *areas = nullptr, size_t count = 0, float t = -1.0f, ID3D11ShaderResourceView *a = nullptr,
               ID3D11ShaderResourceView *b = nullptr) {
        if (!ready() || pairs_ < 2) return;
        begin_draw(ctx, apply_ps_.Get());
        ctx->OMSetRenderTargets(1, &target, nullptr);
        ID3D11ShaderResourceView *srvs[6] = {real, mask_.srv.Get(), ways_.srv.Get(), a, b, count_[cur_].srv.Get()};
        ctx->PSSetShaderResources(0, 6, srvs);
        Params p = params();
        p.carry_t = t;
        p.carry_rule = t >= 0.0f && a && b && rule_ == Rule::motion && ways_.srv ? carry : 0;
        draw_areas(ctx, p, areas, count, 0, 0, width_, height_);
        end_draw(ctx);
    }

    // The frames without their HUD, for the generator to draw the frame in
    // between from (fg::Interpolator::after_motion): under each HUD pixel of
    // one frame, what the scenery shows there in the other, a step along the
    // motion (hud.hlsl CleanPS). So the HUD is not carried along with the
    // scenery, as a faint copy beside itself; apply() then puts the HUD
    // itself back. After update() for the same pair. a and b: the frames, or
    // the picture's part of them (as the interpolator works on); changed for
    // the pictures to draw from. Over the whole picture (no areas), or inside
    // each area only - the same as apply() gets, or a HUD is taken out and
    // not put back. False (a and b as they were) when there is nothing to do.
    bool clean_frames(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *&a, ID3D11ShaderResourceView *&b,
                      const Area *areas = nullptr, size_t count = 0) {
        if (!ready() || clean < 1 || pairs_ < 2 || rule_ != Rule::motion || !ways_.srv) return false;
        Microsoft::WRL::ComPtr<ID3D11Resource> ra, rb;
        a->GetResource(&ra);
        b->GetResource(&rb);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> ta;
        if (FAILED(ra.As(&ta))) return false;
        D3D11_TEXTURE2D_DESC desc{};
        ta->GetDesc(&desc);
        // The frames, or the picture's part: where its first pixel is in the frame.
        UINT ox = 0, oy = 0;
        if (desc.Width != width_ || desc.Height != height_) {
            if (picture_.x1 == 0 || desc.Width != picture_.x1 - picture_.x0 || desc.Height != picture_.y1 - picture_.y0)
                return false;
            ox = picture_.x0;
            oy = picture_.y0;
        }
        if (!clean_[0].tex || clean_format_ != desc.Format || clean_w_ != desc.Width || clean_h_ != desc.Height) {
            D3D11_TEXTURE2D_DESC cd{};
            cd.Width = desc.Width;
            cd.Height = desc.Height;
            cd.MipLevels = cd.ArraySize = 1;
            cd.Format = desc.Format;
            cd.SampleDesc.Count = 1;
            cd.Usage = D3D11_USAGE_DEFAULT;
            cd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            for (Drawn &c : clean_) {
                c = Drawn{};
                if (FAILED(device_->CreateTexture2D(&cd, nullptr, &c.tex)) ||
                    FAILED(device_->CreateRenderTargetView(c.tex.Get(), nullptr, &c.rtv)) ||
                    FAILED(device_->CreateShaderResourceView(c.tex.Get(), nullptr, &c.srv))) {
                    clean_[0] = clean_[1] = Drawn{};
                    return false;
                }
            }
            clean_format_ = desc.Format;
            clean_w_ = desc.Width;
            clean_h_ = desc.Height;
        }
        ctx->CopyResource(clean_[0].tex.Get(), ra.Get());
        ctx->CopyResource(clean_[1].tex.Get(), rb.Get());
        begin_draw(ctx, clean_ps_.Get());
        Params p = params();
        p.clean_x = INT(ox);
        p.clean_y = INT(oy);
        p.clean_w = float(desc.Width);
        p.clean_h = float(desc.Height);
        ID3D11ShaderResourceView *const other[2] = {b, a};
        for (int f = 0; f < 2; ++f) {
            ID3D11RenderTargetView *target = clean_[f].rtv.Get();
            ctx->OMSetRenderTargets(1, &target, nullptr);
            ID3D11ShaderResourceView *srvs[4] = {other[f], mask_.srv.Get(), ways_.srv.Get(), a};
            ctx->PSSetShaderResources(0, 4, srvs);
            p.clean_sign = f == 0 ? 1.0f : -1.0f;
            draw_areas(ctx, p, areas, count, ox, oy, desc.Width, desc.Height);
        }
        end_draw(ctx);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);  // they are read from next
        a = clean_[0].srv.Get();
        b = clean_[1].srv.Get();
        return true;
    }

    // How many pixels the mask holds: asked for with count_pixels() (after
    // an update), had some frames later from read_count() - false until then.
    void count_pixels(ID3D11DeviceContext *ctx) {
        if (!ready() || count_asked_) return;
        const Params p = params();
        ctx->UpdateSubresource(cb_.Get(), 0, nullptr, &p, 0, 0);
        ctx->CSSetConstantBuffers(0, 1, cb_.GetAddressOf());
        dispatch(ctx, count_cs_.Get(), {mask_.srv.Get()}, total_.uav.Get(), 1, 1);
        unbind(ctx);
        ctx->CopyResource(total_read_.Get(), total_.tex.Get());
        count_asked_ = true;
    }
    bool read_count(ID3D11DeviceContext *ctx, UINT &pixels) {
        if (!count_asked_) return false;
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(ctx->Map(total_read_.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map))) return false;
        pixels = *static_cast<const UINT *>(map.pData);
        ctx->Unmap(total_read_.Get(), 0);
        count_asked_ = false;
        return true;
    }

    // For the add-on's log: around update() (and the motion search made for
    // it alone, if any), around apply() and around clean_frames().
    GpuTimer update_timer, apply_timer, clean_timer;

    // Tuning, for the evaluator. Levels are steps of 8-bit SDR luma (hud.hlsl).
    float pq_step = 0.5f;        // what a level is in a PQ frame, in 8-bit steps of its code range (not measured)
    float min_motion = 1.5f;     // a motion under this many pixels says nothing
    float still_level = 4.0f;    // still: the pixel's 3x3 mean change is at most this
    float strict_level = 8.0f;   // ... and no pixel within 2 px of it changes by more than this
    float moved_level = 10.0f;   // ... and the motion does not explain it: a here and b there differ by more than this
    float changed_level = 16.0f; // a pixel that changes by this is let go
    float edge_level = 10.0f;    // edge: a luma step over 2 px of this, in a and in b
    float edge_same = 0.5f;      // ... the same edge: the gradients differ by at most this share of the weaker one
    float edge_across = 0.5f;    // ... lying across the motion (one along it slides on itself)
    // The counters, in fiftieths: a pair with evidence gains 17 (on from two
    // pairs: 34), a still pixel that is no longer still loses 17 a pair, an
    // edge that is gone 25.
    int count_up = 17, count_max = 50, on_from = 34;
    int still_down = 17, edge_down = 25;
    // 1 = a still pixel that is on is let go by its own change, not its 3x3's: a stroke of text stays while
    // the scenery behind it changes (0: as the rule was first measured).
    int hold_own = 0;
    // A pair the interpolator took for a cut (more of it unmatched than its
    // cut shares; it shows a again there): 0 = counted like any other,
    // 1 = no evidence is taken from it, 2 = the counters start over.
    // 0, by measurement: fast action is taken for a cut pair after pair
    // (three of the seven clips), and the counters need no telling - across a
    // real cut every pixel changes and is let go at once, and what does not
    // change there is a HUD that is still on. HUD error on the clips: 1.16
    // levels with 0, 1.23 with 1, 1.49 with 2 (docs/FRAMEGEN-LEARNINGS.md).
    int cut_rule = 0;
    // The plain rule: detail is a 9x9 contrast of this; the frame as a whole
    // moves when its mean change is over that; on after this many still pairs.
    float plain_detail = 30.0f, plain_moves = 1.0f;
    int plain_pairs = 4;
    // The HUD is not carried along with the scenery (docs/FRAMEGEN-LEARNINGS.md, 14, has the numbers).
    // clean_frames(): 0 = off; 1 = the generator draws from the frames without their HUD.
    int clean = 1;
    float clean_grow = 0.0f;     // ... a pixel this near the mask counts as HUD there too (pixels; 0 = the mask)
    float clean_reach = 256.0f;  // ... for a HUD pixel whose scenery is hidden in the other frame too, or where no
                                 // motion is known, a's scenery from the nearest place the HUD ends along the
                                 // motion's line, at most this far (pixels; 0 = left as it is)
    // What apply() does about a pixel that is not HUD but was drawn from one - the place its scenery came
    // from in a, or went to in b, along the scenery's motion, is HUD (given t and both frames): 0 = nothing;
    // 1 = where both places are HUD, the real frame; 2 = and where one is, the real frame if the pixel stands
    // still itself, else the scenery from the other frame; 3 = the same, with a pixel that gave evidence in
    // this pair counting as HUD there too, whether the mask holds it or not. 0, by measurement: 1 changed
    // nothing, 2 and 3 made the scenery and the whole frame worse on the clips.
    int carry = 0;

private:
    // hud.hlsl's constant buffer.
    struct Params {
        INT frame_w, frame_h, pic_x, pic_y;
        INT pic_w, pic_h, grid_w, grid_h;
        INT block_w, block_h;
        FLOAT unit, flow_scale;
        FLOAT still_level, strict_level, moved_level, changed_level;
        FLOAT edge_level, edge_same, edge_across, min_motion;
        INT count_up, count_max, still_down, edge_down;
        INT on_still, on_edge, has_stats, cut_rule;
        FLOAT cut_fraction, cut_detailed, plain_detail, plain_moves;
        FLOAT ellipse_cx, ellipse_cy, ellipse_ix, ellipse_iy;
        INT clean_x, clean_y;
        FLOAT clean_sign, carry_t;
        FLOAT clean_w, clean_h, clean_grow, clean_reach;
        INT hold_own, carry_rule, spare_[2];
    };
    static_assert(sizeof(Params) % 16 == 0, "constant buffers come in 16-byte registers");

    struct Surface {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    };

    HRESULT make(UINT w, UINT h, DXGI_FORMAT format, Surface &s) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, s.tex.ReleaseAndGetAddressOf());
        if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(s.tex.Get(), nullptr, s.srv.ReleaseAndGetAddressOf());
        if (SUCCEEDED(hr)) hr = device_->CreateUnorderedAccessView(s.tex.Get(), nullptr, s.uav.ReleaseAndGetAddressOf());
        if (FAILED(hr)) s = Surface{};
        return hr;
    }

    // The textures that follow the motion field's grid.
    bool ensure_grid(UINT gw, UINT gh) {
        if (gw == grid_w_ && gh == grid_h_ && ways_.tex) return true;
        grid_w_ = grid_h_ = 0;
        if (FAILED(make(gw, gh, DXGI_FORMAT_R32G32B32A32_FLOAT, ways_))) return false;
        for (Surface &s : bg_)
            if (FAILED(make((gw + 7) / 8, (gh + 7) / 8, DXGI_FORMAT_R32G32B32A32_FLOAT, s))) return false;
        grid_w_ = gw;
        grid_h_ = gh;
        return true;
    }

    Params params() const {
        Params p{};
        p.frame_w = INT(width_);
        p.frame_h = INT(height_);
        p.pic_x = INT(picture_.x0);
        p.pic_y = INT(picture_.y0);
        p.pic_w = INT(picture_.x1 ? picture_.x1 - picture_.x0 : width_);
        p.pic_h = INT(picture_.x1 ? picture_.y1 - picture_.y0 : height_);
        p.grid_w = INT(grid_w_);
        p.grid_h = INT(grid_h_);
        p.block_w = INT((grid_w_ + 7) / 8);
        p.block_h = INT((grid_h_ + 7) / 8);
        p.unit = unit_;
        p.flow_scale = 2.0f;
        p.still_level = still_level;
        p.strict_level = strict_level;
        p.moved_level = moved_level;
        p.changed_level = changed_level;
        p.edge_level = edge_level;
        p.edge_same = edge_same;
        p.edge_across = edge_across;
        p.min_motion = min_motion;
        p.count_up = rule_ == Rule::plain ? 1 : count_up;
        p.count_max = rule_ == Rule::plain ? plain_pairs : count_max;
        p.still_down = still_down;
        p.edge_down = edge_down;
        p.on_still = rule_ == Rule::plain ? plain_pairs : on_from;
        p.on_edge = rule_ == Rule::plain ? 0x7fffffff : on_from;
        p.cut_rule = cut_rule;
        p.plain_detail = plain_detail;
        p.plain_moves = plain_moves;
        p.carry_t = -1.0f;
        p.clean_grow = clean_grow;
        p.clean_reach = clean_reach;
        p.hold_own = hold_own;
        return p;
    }

    static UINT groups(UINT n) { return (n + 7) / 8; }

    // The state the two drawing passes share: a triangle over the viewport, their constants and sampler.
    void begin_draw(ID3D11DeviceContext *ctx, ID3D11PixelShader *ps) {
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(apply_vs_.Get(), nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->RSSetState(nullptr);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
        ctx->PSSetSamplers(0, 1, linear_.GetAddressOf());
    }
    void end_draw(ID3D11DeviceContext *ctx) {
        ID3D11ShaderResourceView *none[6] = {};
        ctx->PSSetShaderResources(0, 6, none);
    }

    // One draw per area (in frame pixels; none: the whole picture), each inside the picture - the mask holds
    // nothing elsewhere - and inside the target, whose first pixel is the frame's (ox, oy) and which is w x h.
    void draw_areas(ID3D11DeviceContext *ctx, Params &p, const Area *areas, size_t count, UINT ox, UINT oy, UINT w, UINT h) {
        const Area whole{picture_.x1 ? picture_.x0 : 0, picture_.x1 ? picture_.y0 : 0, picture_.x1 ? picture_.x1 : width_,
                         picture_.x1 ? picture_.y1 : height_, false};
        if (!areas) {
            areas = &whole;
            count = 1;
        }
        for (size_t i = 0; i < count; ++i) {
            Area r = areas[i];
            const float cx = (r.left + r.right) * 0.5f, cy = (r.top + r.bottom) * 0.5f;
            const float rx = (r.right - r.left) * 0.5f, ry = (r.bottom - r.top) * 0.5f;
            r.left = std::max({r.left, whole.left, ox});
            r.top = std::max({r.top, whole.top, oy});
            r.right = std::min({r.right, whole.right, ox + w});
            r.bottom = std::min({r.bottom, whole.bottom, oy + h});
            if (r.left >= r.right || r.top >= r.bottom) continue;
            p.ellipse_cx = cx;
            p.ellipse_cy = cy;
            p.ellipse_ix = r.ellipse && rx > 0 ? 1.0f / rx : 0.0f;
            p.ellipse_iy = r.ellipse && ry > 0 ? 1.0f / ry : 0.0f;
            ctx->UpdateSubresource(cb_.Get(), 0, nullptr, &p, 0, 0);
            const D3D11_VIEWPORT vp{float(r.left - ox), float(r.top - oy), float(r.right - r.left), float(r.bottom - r.top), 0, 1};
            ctx->RSSetViewports(1, &vp);
            ctx->Draw(3, 0);
        }
    }

    // One compute pass. The output is unbound first and the inputs bound
    // before it, so no texture is an input and an output at once.
    void dispatch(ID3D11DeviceContext *ctx, ID3D11ComputeShader *cs, std::initializer_list<ID3D11ShaderResourceView *> in,
                  ID3D11UnorderedAccessView *out, UINT gx, UINT gy) {
        ID3D11UnorderedAccessView *no_uav = nullptr;
        ctx->CSSetUnorderedAccessViews(0, 1, &no_uav, nullptr);
        ID3D11ShaderResourceView *srvs[kInputs] = {};
        UINT n = 0;
        for (auto *v : in) srvs[n++] = v;
        ctx->CSSetShaderResources(0, kInputs, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, &out, nullptr);
        ctx->CSSetShader(cs, nullptr, 0);
        ctx->Dispatch(gx, gy, 1);
    }

    void unbind(ID3D11DeviceContext *ctx) {
        ID3D11UnorderedAccessView *no_uav = nullptr;
        ctx->CSSetUnorderedAccessViews(0, 1, &no_uav, nullptr);
        ID3D11ShaderResourceView *none[kInputs] = {};
        ctx->CSSetShaderResources(0, kInputs, none);
    }

    static constexpr UINT kInputs = 4;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    UINT width_ = 0, height_ = 0;
    DXGI_FORMAT pair_format_ = DXGI_FORMAT_R16G16_UNORM;  // two lumas a pixel (luma_, range_)
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> luma_cs_, bg_cs_, fill_cs_, ways_cs_, update_cs_, range_cs_, mean_cs_,
        plain_cs_, mask_cs_, count_cs_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> apply_vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> apply_ps_, clean_ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linear_;
    Surface luma_;      // both frames' luma (a, b), full size
    Surface count_[2];  // the counters (still, edge), ping-pong
    int cur_ = 0;       // which of them is the latest
    Surface mask_;      // 1 where a frame in between shows the real frame
    Surface range_;     // the plain rule: b's lowest and highest luma over 9 px across
    Surface mean_;      // ... the frame's mean change (1x1)
    Surface bg_[2];     // per block of 8x8 cells: the scenery's motion (and whether it has one)
    Surface ways_;      // per cell: its own motion and the scenery's, in frame pixels
    Surface total_;     // the mask's pixel count (1x1)
    struct Drawn {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    };
    Drawn clean_[2];    // a and b without their HUD (clean_frames), in the frames' own format
    DXGI_FORMAT clean_format_ = DXGI_FORMAT_UNKNOWN;
    UINT clean_w_ = 0, clean_h_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> total_read_;
    bool count_asked_ = false;
    UINT grid_w_ = 0, grid_h_ = 0;
    Rule rule_ = Rule::motion;
    Picture picture_;
    float unit_ = 1.0f / 255.0f;
    bool fresh_ = true;   // the counters start from nothing at the next update
    unsigned pairs_ = 0;
};

}  // namespace fg
