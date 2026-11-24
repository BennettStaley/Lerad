// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// hud.hlsl - which pixels are a HUD: what stands still on the screen while
// the scenery behind it moves. Each pass is its own entry point, compiled with
// its macro defined (shaders.cmake); fg_hud.h runs them.
//
// Per pixel of the frame two small counters are kept from one pair of real
// frames (a, b) to the next:
//   still  the pixel stayed as it was, with nothing changing within 2 px of
//          it, while the scenery's motion says it should have changed;
//   edge   an edge stayed the same edge (direction and strength; its level
//          may change, as under a see-through HUD) while the motion would
//          have moved it.
// A pair with such evidence counts a counter up, and it is on from two pairs.
// A pixel that changes lets go of "still" at once; one that is merely no
// longer still loses it over the next pairs; an edge that is gone loses
// "edge" in two. The mask is where either counter is on, grown by 2 px
// (glyph edges are anti-aliased). A frame in between shows the nearer real
// frame there (ApplyPS).
//
// "The scenery's motion" is the interpolator's block motion from a to b
// (flow.hlsl, 16 px cells of the frame), tried two ways for every pixel: its
// own cell's, and the background's around it - per block of 8x8 cells the
// median of the cells that move (BgCS). A HUD panel's own cells stand still;
// the second way is what moves behind and beside it.
//
// The plain rule (PlainCS) needs no motion: detail that does not change
// while the frame as a whole does, counted over four pairs - the "static
// map" of TV logo detection.
//
// Levels are steps of 8-bit SDR luma, times `unit` in the frame's own
// encoding (1/255 for SDR; fg_hud.h has what is used for PQ).

cbuffer HudParams : register(b0)
{
    int2 frame_size;    // the textures (the whole frame)
    int2 pic_origin;    // the picture's first pixel in them
    int2 pic_size;      // the picture: what is looked at
    int2 grid_size;     // the motion field's cells (16 px of the picture each)
    int2 block_size;    // blocks of 8x8 cells
    float unit;         // one level of 8-bit SDR luma, in the frame's luma
    float flow_scale;   // the motion field's units to frame pixels
    float still_level;    // still: a pixel stays: its 3x3 mean change is at most this (levels)
    float strict_level;   // ... and no pixel within 2 px changes by more than this
    float moved_level;    // ... and the motion does not explain it: a here and b there differ by more than this
    float changed_level;  // a pixel that changes by this is let go
    float edge_level;     // edge: a luma step over 2 px of this, in a and in b
    float edge_same;      // ... the same edge: the gradients differ by at most this share of the weaker one
    float edge_across;    // ... across the motion (one along it slides on itself)
    float min_motion;     // a motion under this many pixels says nothing
    int count_up;         // gained per pair with evidence
    int count_max;
    int still_down;       // still: lost per pair in which the pixel is no longer still
    int edge_down;        // edge: lost per pair in which the edge is gone
    int2 on_from;         // MaskCS: the mask is where a counter (x, y) is at least this
    int has_stats;        // UpdateCS: the interpolator's stats are bound (a cut)
    int cut_rule;         // UpdateCS: a cut: 0 nothing, 1 no evidence from this pair, 2 the counters start over
    float cut_fraction;   // flow.hlsl's, as WarpPS takes them
    float cut_detailed;
    float plain_detail;   // PlainCS: 9x9 contrast that counts as detail (levels)
    float plain_moves;    // PlainCS: the frame as a whole moves: its mean change is over this (levels)
    float2 ellipse_centre;  // ApplyPS: an ellipse, in frame pixels
    float2 ellipse_inv;     // ... 1 / its radii (0, 0: no ellipse, the whole viewport)
    int2 clean_offset;      // CleanPS: a pixel of what is drawn, plus this, is its pixel in the frame (the picture's place)
    float clean_sign;       // CleanPS: 1 the frame is a (the scenery went on along the motion), -1 it is b
    float carry_t;          // ApplyPS: where the frame in between sits, 0 a to 1 b
    float2 clean_size;      // CleanPS: the size of what is drawn (the frame, or the picture's part of it)
    float clean_grow;       // CleanPS, ApplyPS: a pixel this near the mask counts as in it (pixels)
    float clean_reach;      // CleanPS: how far the scenery is fetched from for a pixel hidden in both frames (pixels; 0 = not)
    int hold_own;           // UpdateCS: 1 = a pixel that is on is let go by its own change, not its 3x3's
    int carry_rule;         // ApplyPS: what is done about a pixel drawn from a HUD pixel (fg_hud.h, carry)
    int2 hud_spare;
};

float Luma(float3 c)
{
    // On the encoded values, like flow.hlsl's.
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// ---- Both frames' luma at full size -----------------------------------------------

#if defined(HUD_LUMA)
Texture2D<float4> frame_a : register(t0);
Texture2D<float4> frame_b : register(t1);
RWTexture2D<float2> luma_out : register(u0);

[numthreads(8, 8, 1)]
void LumaCS(uint3 id : SV_DispatchThreadID)
{
    const int2 local = int2(id.xy);
    if (any(local >= pic_size)) return;
    const int3 p = int3(local + pic_origin, 0);
    luma_out[p.xy] = float2(Luma(frame_a.Load(p).rgb), Luma(frame_b.Load(p).rgb));
}
#endif

// ---- The scenery's motion around each cell -----------------------------------------

#if defined(HUD_BG)
// Per block of 8x8 cells: the median (each way) of its cells that move, if a
// quarter of them do; z says whether it has one.
Texture2D<float2> flow : register(t0);
RWTexture2D<float4> bg_out : register(u0);

groupshared float2 g_v[64];
groupshared uint g_kind[64];  // 0 outside the grid, 1 a cell, 2 a cell that moves
groupshared float g_mid[4];   // the two middle values across, the two down

[numthreads(64, 1, 1)]
void BgCS(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    const int2 cell = int2(gid.xy) * 8 + int2(gi % 8u, gi / 8u);
    const bool inside = all(cell < grid_size);
    float2 v = 0;
    if (inside) v = flow.Load(int3(cell, 0)) * flow_scale;
    const bool moves = inside && dot(v, v) >= min_motion * min_motion;
    g_v[gi] = v;
    g_kind[gi] = inside ? (moves ? 2u : 1u) : 0u;
    GroupMemoryBarrierWithGroupSync();

    // Each moving cell's rank among them (ties by index: every rank is taken once).
    uint n = 0, cells = 0, rx = 0, ry = 0;
    [loop] for (uint j = 0; j < 64; ++j)
    {
        const uint kind = g_kind[j];
        cells += kind != 0u ? 1u : 0u;
        if (kind == 2u)
        {
            ++n;
            const float2 o = g_v[j];
            rx += (o.x < v.x || (o.x == v.x && j < gi)) ? 1u : 0u;
            ry += (o.y < v.y || (o.y == v.y && j < gi)) ? 1u : 0u;
        }
    }
    if (moves)
    {
        if (rx == (n - 1) / 2) g_mid[0] = v.x;
        if (rx == n / 2) g_mid[1] = v.x;
        if (ry == (n - 1) / 2) g_mid[2] = v.y;
        if (ry == n / 2) g_mid[3] = v.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi != 0) return;
    const bool valid = n > 0 && float(n) >= 0.25 * float(cells);
    bg_out[gid.xy] = valid ? float4(0.5 * (g_mid[0] + g_mid[1]), 0.5 * (g_mid[2] + g_mid[3]), 1, 0) : float4(0, 0, 0, 0);
}
#endif

#if defined(HUD_FILL)
// A block without a motion of its own takes the mean of the blocks around
// that have one. Run three times: up to three blocks away.
Texture2D<float4> bg_in : register(t0);
RWTexture2D<float4> bg_out : register(u0);

[numthreads(8, 8, 1)]
void FillCS(uint3 id : SV_DispatchThreadID)
{
    const int2 p = int2(id.xy);
    if (any(p >= block_size)) return;
    float4 c = bg_in.Load(int3(p, 0));
    if (c.z < 0.5)
    {
        float2 sum = 0;
        float n = 0;
        [unroll] for (int k = 0; k < 9; ++k)
        {
            const int2 q = p + int2(k % 3 - 1, k / 3 - 1);
            if (any(q < 0) || any(q >= block_size)) continue;
            const float4 o = bg_in.Load(int3(q, 0));
            if (o.z >= 0.5)
            {
                sum += o.xy;
                n += 1.0;
            }
        }
        if (n > 0.0) c = float4(sum / n, 1, 0);
    }
    bg_out[p] = c;
}
#endif

#if defined(HUD_WAYS)
// Per cell, in frame pixels: its own motion (xy) and the scenery's around it
// (zw), taken between the blocks.
Texture2D<float2> flow : register(t0);
Texture2D<float4> bg : register(t1);
SamplerState linear_clamp : register(s0);
RWTexture2D<float4> ways_out : register(u0);

[numthreads(8, 8, 1)]
void WaysCS(uint3 id : SV_DispatchThreadID)
{
    const int2 cell = int2(id.xy);
    if (any(cell >= grid_size)) return;
    const float2 own = flow.Load(int3(cell, 0)) * flow_scale;
    const float2 around = bg.SampleLevel(linear_clamp, (float2(cell) + 0.5) / float2(grid_size), 0).xy;
    ways_out[cell] = float4(own, around);
}
#endif

// ---- The counters, one pair on ---------------------------------------------------------

#if defined(HUD_UPDATE)
Texture2D<float2> lumas : register(t0);   // a, b
Texture2D<float4> ways : register(t1);    // WaysCS
Texture2D<uint2> count_in : register(t2); // still, edge
Texture2D<float> stats : register(t3);    // the interpolator's (flow.hlsl STATS_*), when has_stats
SamplerState linear_clamp : register(s0);
RWTexture2D<uint2> count_out : register(u0);

float2 At(int2 q) { return lumas.Load(int3(clamp(q, pic_origin, pic_origin + pic_size - 1), 0)); }

// At a place between pixels (frame pixels, a pixel's middle at +0.5).
float2 Between(float2 pos)
{
    pos = clamp(pos, float2(pic_origin) + 0.5, float2(pic_origin + pic_size) - 0.5);
    return lumas.SampleLevel(linear_clamp, pos / float2(frame_size), 0);
}

[numthreads(8, 8, 1)]
void UpdateCS(uint3 id : SV_DispatchThreadID)
{
    const int2 local = int2(id.xy);
    if (any(local >= pic_size)) return;
    const int2 p = local + pic_origin;
    int2 n = int2(count_in.Load(int3(p, 0)) & uint2(127u, 255u));  // (x's top bit: the last pair's evidence)
    // A pair across a cut matched nothing: its motion says nothing.
    bool cut = false;
    if (has_stats != 0 && cut_rule != 0)
        cut = stats.Load(int3(1, 0, 0)) > cut_fraction || stats.Load(int3(2, 0, 0)) > cut_detailed;
    if (cut && cut_rule == 2)
    {
        count_out[p] = uint2(0, 0);
        return;
    }

    // The 3x3 around the pixel: how much it changed, and its edge in a and in b.
    float2 y[9];
    float change = 0, most = 0;
    [unroll] for (int i = 0; i < 9; ++i)
    {
        y[i] = At(p + int2(i % 3 - 1, i / 3 - 1));
        const float d = abs(y[i].x - y[i].y);
        change += d;
        most = max(most, d);
    }
    change /= 9.0;
    const bool still = change <= still_level * unit;
    const bool changed = change > changed_level * unit;
    const float2 ga = float2(y[5].x - y[3].x, y[7].x - y[1].x), gb = float2(y[5].y - y[3].y, y[7].y - y[1].y);
    const float na = length(ga), nb = length(gb), strength = min(na, nb), apart = length(ga - gb);
    const bool edge = strength >= edge_level * unit && apart <= edge_same * strength;
    const bool edge_gone = !(strength >= 0.5 * edge_level * unit && apart <= 2.0 * edge_same * strength);

    const float4 w = ways.Load(int3(min(local >> 4, grid_size - 1), 0));
    const float2 way[2] = {w.xy, w.zw};
    const bool goes[2] = {dot(w.xy, w.xy) >= min_motion * min_motion, dot(w.zw, w.zw) >= min_motion * min_motion};
    bool is_still = false, is_edge = false;
    [branch] if ((goes[0] || goes[1]) && !cut)
    {
        // Nothing changes within 2 px: the ring around the 3x3 as well.
        bool strict = most <= strict_level * unit;
        [branch] if (strict)
        {
            [loop] for (int r = 0; r < 25; ++r)
            {
                const int2 o = int2(r % 5 - 2, r / 5 - 2);
                if (max(abs(o.x), abs(o.y)) < 2) continue;
                const float2 v = At(p + o);
                most = max(most, abs(v.x - v.y));
            }
            strict = most <= strict_level * unit;
        }
        // What is left to find out: does a motion explain the still pixel, or the lasting edge?
        bool explained = !strict, moved_edge = !edge;
        const float2 centre = float2(p) + 0.5;
        [loop] for (int k = 0; k < 2; ++k)
        {
            [branch] if (goes[k] && !(explained && moved_edge))
            {
                const float2 v = way[k];
                [branch] if (!explained)
                {
                    // a here against b where the motion takes it, and b here against a where it came from.
                    float to = 0, from = 0;
                    [unroll] for (int j = 0; j < 9; ++j)
                    {
                        const float2 q = centre + float2(j % 3 - 1, j / 3 - 1);
                        to += abs(y[j].x - Between(q + v).y);
                        from += abs(y[j].y - Between(q - v).x);
                    }
                    if (min(to, from) <= 9.0 * moved_level * unit) explained = true;
                }
                [branch] if (!moved_edge)
                {
                    const float2 both = ga + gb;
                    const bool across = abs(dot(both, v)) >= edge_across * length(both) * length(v);
                    // The same edge again where the motion takes it, or where it came from.
                    const float2 there = float2(Between(centre + v + float2(1, 0)).y - Between(centre + v - float2(1, 0)).y,
                                                Between(centre + v + float2(0, 1)).y - Between(centre + v - float2(0, 1)).y);
                    const float2 back = float2(Between(centre - v + float2(1, 0)).x - Between(centre - v - float2(1, 0)).x,
                                               Between(centre - v + float2(0, 1)).x - Between(centre - v - float2(0, 1)).x);
                    const bool same_there = length(there - ga) <= edge_same * min(na, length(there));
                    const bool same_back = length(back - gb) <= edge_same * min(nb, length(back));
                    if (!across || same_there || same_back) moved_edge = true;
                }
            }
        }
        is_still = strict && !explained;
        is_edge = edge && !moved_edge;
    }

    // Letting go. A stroke of text is a pixel or three wide: with the scenery changing behind it, its 3x3
    // changes while it does not. So a pixel that is on is judged by itself (hold_own); one that is not yet,
    // by its 3x3, which the frames' noise moves less.
    float gone = change;
    if (hold_own != 0 && n.x >= on_from.x) gone = abs(y[4].x - y[4].y);
    if (is_still)
        n.x = min(n.x + count_up, count_max);
    else if (gone > changed_level * unit)
        n.x = 0;
    else if (gone > still_level * unit)
        n.x = max(n.x - still_down, 0);
    if (is_edge)
        n.y = min(n.y + count_up, count_max);
    else if (edge_gone)
        n.y = max(n.y - edge_down, 0);
    count_out[p] = uint2(uint(n.x) | ((is_still || is_edge) ? 128u : 0u), uint(n.y));
}
#endif

// ---- The plain rule: no motion ---------------------------------------------------------

#if defined(HUD_RANGE)
// The later frame's lowest and highest luma over 9 px across (PlainCS takes
// them over 9 down: the contrast of the 9x9 around a pixel).
Texture2D<float2> lumas : register(t0);
RWTexture2D<float2> range_out : register(u0);

[numthreads(8, 8, 1)]
void RangeCS(uint3 id : SV_DispatchThreadID)
{
    const int2 local = int2(id.xy);
    if (any(local >= pic_size)) return;
    const int2 p = local + pic_origin;
    float lo = 1e9, hi = -1e9;
    [unroll] for (int i = -4; i <= 4; ++i)
    {
        const int x = p.x + i;
        if (x < pic_origin.x || x >= pic_origin.x + pic_size.x) continue;
        const float v = lumas.Load(int3(x, p.y, 0)).y;
        lo = min(lo, v);
        hi = max(hi, v);
    }
    range_out[p] = float2(lo, hi);
}
#endif

#if defined(HUD_MEAN)
// How much the frame as a whole changed from a to b: the mean over one pixel
// in sixteen (every fourth, across and down).
Texture2D<float2> lumas : register(t0);
RWTexture2D<float> mean_out : register(u0);

groupshared float g_sum[1024];

[numthreads(1024, 1, 1)]
void MeanCS(uint gi : SV_GroupIndex)
{
    const int2 samples = max(pic_size / 4, 1);
    float sum = 0;
    [loop] for (int row = int(gi); row < samples.y; row += 1024)
    {
        [loop] for (int x = 0; x < samples.x; ++x)
        {
            const float2 v = lumas.Load(int3(pic_origin + int2(x, row) * 4 + 1, 0));
            sum += abs(v.x - v.y);
        }
    }
    g_sum[gi] = sum;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint step = 512; step > 0; step >>= 1)
    {
        if (gi < step) g_sum[gi] += g_sum[gi + step];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0) mean_out[int2(0, 0)] = g_sum[0] / float(samples.x * samples.y);
}
#endif

#if defined(HUD_PLAIN)
Texture2D<float2> lumas : register(t0);
Texture2D<float2> ranges : register(t1);   // RangeCS
Texture2D<uint2> count_in : register(t2);
Texture2D<float> mean_in : register(t3);   // MeanCS
RWTexture2D<uint2> count_out : register(u0);

[numthreads(8, 8, 1)]
void PlainCS(uint3 id : SV_DispatchThreadID)
{
    const int2 local = int2(id.xy);
    if (any(local >= pic_size)) return;
    const int2 p = local + pic_origin;
    const int2 last = pic_origin + pic_size - 1;
    int n = int(count_in.Load(int3(p, 0)).x);
    float change = 0;
    [unroll] for (int i = 0; i < 9; ++i)
    {
        const float2 v = lumas.Load(int3(clamp(p + int2(i % 3 - 1, i / 3 - 1), pic_origin, last), 0));
        change += abs(v.x - v.y);
    }
    change /= 9.0;
    [branch] if (change <= still_level * unit && n < count_max && mean_in.Load(int3(0, 0, 0)) > plain_moves * unit)
    {
        float lo = 1e9, hi = -1e9;
        [unroll] for (int k = -4; k <= 4; ++k)
        {
            const int yy = p.y + k;
            if (yy < pic_origin.y || yy > last.y) continue;
            const float2 r = ranges.Load(int3(p.x, yy, 0));
            lo = min(lo, r.x);
            hi = max(hi, r.y);
        }
        if (hi - lo >= plain_detail * unit) n = min(n + count_up, count_max);
    }
    if (change > changed_level * unit) n = 0;
    count_out[p] = uint2(uint(n), 0u);
}
#endif

// ---- The mask ---------------------------------------------------------------------------

#if defined(HUD_MASK)
Texture2D<uint2> count_in : register(t0);
RWTexture2D<float> mask_out : register(u0);

[numthreads(8, 8, 1)]
void MaskCS(uint3 id : SV_DispatchThreadID)
{
    const int2 local = int2(id.xy);
    if (any(local >= pic_size)) return;
    const int2 p = local + pic_origin;
    bool on = false;
    [unroll] for (int i = 0; i < 25; ++i)
    {
        // Outside the texture a load gives 0, and outside the picture nothing is ever counted.
        const int2 n = int2(count_in.Load(int3(p + int2(i % 5 - 2, i / 5 - 2), 0)) & uint2(127u, 255u));
        on = on || n.x >= on_from.x || n.y >= on_from.y;
    }
    mask_out[p] = on ? 1.0 : 0.0;
}
#endif

#if defined(HUD_COUNT)
// How many pixels the mask holds (the add-on's log).
Texture2D<float> mask : register(t0);
RWTexture2D<uint> count_out : register(u0);

groupshared uint g_count[1024];

[numthreads(1024, 1, 1)]
void CountCS(uint gi : SV_GroupIndex)
{
    uint count = 0;
    [loop] for (int row = int(gi); row < pic_size.y; row += 1024)
    {
        [loop] for (int x = 0; x < pic_size.x; ++x)
            count += mask.Load(int3(pic_origin + int2(x, row), 0)) > 0.5 ? 1u : 0u;
    }
    g_count[gi] = count;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint step = 512; step > 0; step >>= 1)
    {
        if (gi < step) g_count[gi] += g_count[gi + step];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0) count_out[int2(0, 0)] = g_count[0];
}
#endif

// ---- A frame in between shows the nearer real frame where the mask is set ----------------

#if defined(HUD_APPLY_VS)
// A full-screen triangle from the vertex id alone; the viewport says which
// part of the target is drawn.
float4 ApplyVS(uint id : SV_VertexID) : SV_Position
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
#endif

#if defined(HUD_APPLY) || defined(HUD_CLEAN)
// Whether the place (frame pixels, a pixel's middle at +0.5) is on the mask,
// or within clean_grow of it.
bool OnMask(Texture2D<float> mask, SamplerState s, float2 at)
{
    const float2 inv = 1.0 / float2(frame_size);
    if (mask.SampleLevel(s, at * inv, 0) > 0.0) return true;
    if (clean_grow <= 0.0) return false;
    const float g = clean_grow;
    return mask.SampleLevel(s, (at + float2(g, 0)) * inv, 0) + mask.SampleLevel(s, (at - float2(g, 0)) * inv, 0) +
           mask.SampleLevel(s, (at + float2(0, g)) * inv, 0) + mask.SampleLevel(s, (at - float2(0, g)) * inv, 0) > 0.0;
}

// The scenery's motion from a to b at a frame pixel, in frame pixels: the
// background's around its cell, or the cell's own where that says nothing.
float2 SceneryMotion(Texture2D<float4> ways, int2 frame_pixel)
{
    const float4 w = ways.Load(int3(clamp((frame_pixel - pic_origin) >> 4, 0, grid_size - 1), 0));
    return dot(w.zw, w.zw) >= min_motion * min_motion ? w.zw : w.xy;
}
#endif

#if defined(HUD_APPLY)
Texture2D<float4> real_frame : register(t0);
Texture2D<float> mask : register(t1);
Texture2D<float4> ways : register(t2);     // WaysCS (carry_rule only)
Texture2D<float4> frame_a : register(t3);  // the two real frames (carry_rule only)
Texture2D<float4> frame_b : register(t4);
Texture2D<uint2> counters : register(t5);  // UpdateCS's, for this pair's evidence (carry_rule 3)
SamplerState linear_clamp : register(s0);

// Whether the place is HUD: on the mask - or, more freely (carry_rule 3), a
// pixel this pair gave evidence for, which the mask does not hold yet or not
// any more (text over a cloud it does not stand out from, pair after pair).
bool IsHud(float2 at)
{
    if (OnMask(mask, linear_clamp, at)) return true;
    if (carry_rule < 3) return false;
    const int2 q = int2(at);
    return ((counters.Load(int3(q, 0)).x | counters.Load(int3(q + int2(1, 0), 0)).x | counters.Load(int3(q - int2(1, 0), 0)).x |
             counters.Load(int3(q + int2(0, 1), 0)).x | counters.Load(int3(q - int2(0, 1), 0)).x) & 128u) != 0u;
}

float4 ApplyPS(float4 pos : SV_Position) : SV_Target
{
    const int3 p = int3(pos.xy, 0);
    const float2 d = (pos.xy - ellipse_centre) * ellipse_inv;
    if (dot(d, d) > 1.0) discard;
    if (mask.Load(p) >= 0.5) return real_frame.Load(p);
    // Not a HUD pixel. But the generator drew it from a where the scenery
    // came from and from b where it went, and a HUD pixel does not go along
    // with the scenery: where one of those places is HUD, the frame in between
    // shows a faint copy of the HUD here, half a step from the HUD itself.
    [branch] if (carry_rule <= 0) discard;
    const float2 v = SceneryMotion(ways, p.xy);
    [branch] if (dot(v, v) < min_motion * min_motion) discard;
    const float2 from = pos.xy - carry_t * v, to = pos.xy + (1.0 - carry_t) * v;
    const float2 first = float2(pic_origin) + 0.5, last = float2(pic_origin + pic_size) - 0.5;
    const bool from_hud = IsHud(from), to_hud = IsHud(to);
    if (!from_hud && !to_hud) discard;
    // Both are HUD (a gap between two icons in a fast turn): nothing of the
    // scenery was to be had here. The real frame, then.
    if (from_hud && to_hud) return real_frame.Load(p);
    [branch] if (carry_rule < 2) discard;
    // One is. A pixel that stands still here itself (something of a HUD the
    // mask does not hold, or plain sky) is the real frame's as it is; one
    // that moves is the scenery from the frame that shows it.
    const float3 here_a = frame_a.Load(p).rgb, here_b = frame_b.Load(p).rgb;
    const float3 apart = abs(here_a - here_b);
    if (max(apart.r, max(apart.g, apart.b)) <= strict_level * unit) return real_frame.Load(p);
    if (from_hud)
    {
        if (any(to < first) || any(to > last)) discard;
        return frame_b.SampleLevel(linear_clamp, to / float2(frame_size), 0);
    }
    if (any(from < first) || any(from > last)) discard;
    return frame_a.SampleLevel(linear_clamp, from / float2(frame_size), 0);
}
#endif

// ---- The real frames without their HUD, for the generator to draw from ------------------

#if defined(HUD_CLEAN)
// A generator carries what it sees along the scenery's motion, the HUD's
// pixels too: a frame in between then shows a faint copy of an icon half a
// step from the icon. The mask says which pixels of a and b are HUD, and so
// are not scenery to be carried. Under each of them, what the scenery shows
// there is in the other frame, a step along the motion - where that is clear
// of the HUD. A pixel of the frame in between whose sample in one frame lands
// on the HUD then reads the very pixel its sample in the other frame reads.
//
// Where that place is HUD as well, or no motion is known there (the generator
// tries the motions of the cells around a pixel, and one of those may carry a
// HUD pixel all the same), the scenery is in neither frame and what is drawn
// is a guess: frame a's scenery from the nearest place the HUD ends, looking
// both ways along the motion's line through the pixel - up and down where
// there is no motion - within clean_reach. The same guess for both frames
// (b's pixel takes it for where its scenery was in a), so along the motion the
// two still agree, and the generator draws that rather than pieces of HUD.
//
// Drawn over a copy of the frame; pixels that are left alone are discarded.
Texture2D<float4> other_frame : register(t0);
Texture2D<float> mask : register(t1);
Texture2D<float4> ways : register(t2);
Texture2D<float4> frame_a : register(t3);  // the earlier frame: the one drawn over, or the other
SamplerState linear_clamp : register(s0);

bool Inside(float2 at) { return all(at >= 0.5) && all(at <= clean_size - 0.5); }

float4 CleanPS(float4 pos : SV_Position) : SV_Target
{
    const float2 at = pos.xy + float2(clean_offset);  // in the frame
    const float2 d = (at - ellipse_centre) * ellipse_inv;
    if (dot(d, d) > 1.0) discard;
    if (!OnMask(mask, linear_clamp, at)) discard;
    float2 step = SceneryMotion(ways, int2(at));  // from a to b
    const bool moves = dot(step, step) >= min_motion * min_motion;
    if (!moves) step = 0;
    const float2 to = pos.xy + clean_sign * step;
    [branch] if (moves && Inside(to) && !OnMask(mask, linear_clamp, at + clean_sign * step))
        return other_frame.SampleLevel(linear_clamp, to / clean_size, 0);
    // The guess. In a: this pixel, or (drawing b) where its scenery was.
    const float2 along = moves ? normalize(step) : float2(0, 1);
    const float2 from = clean_sign < 0.0 ? to : pos.xy;
    float4 guess = 0;
    bool found = false;
    [loop] for (int k = 1; k <= 64 && !found; ++k)
    {
        const float reach = 4.0 * float(k);
        if (reach > clean_reach) break;
        [unroll] for (int side = 0; side < 2; ++side)
        {
            const float2 way = side == 0 ? -along : along;
            const float2 q = from + way * reach;
            if (!found && Inside(q) && !OnMask(mask, linear_clamp, q + float2(clean_offset)))
            {
                guess = frame_a.SampleLevel(linear_clamp, clamp(q + way * 3.0, 0.5, clean_size - 0.5) / clean_size, 0);
                found = true;
            }
        }
    }
    if (!found) discard;
    return guess;
}
#endif