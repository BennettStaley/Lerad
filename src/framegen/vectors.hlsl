// SPDX-License-Identifier: MPL-2.0
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// The block motion as motion vectors of a game: one vector per pixel of the
// frame, in frame pixels, for a frame generation that takes them (fg_anr.h).
// Between the cells' centres the motion is their mix; outside the picture
// (black bars) nothing moves.

Texture2D<float2> field : register(t0);  // a vector per cell of the picture
SamplerState linear_clamp : register(s0);

cbuffer Params : register(b0)
{
    float2 picture_origin;  // the picture's top left corner in the frame, pixels
    float2 picture_size;    // ... and its size
    float2 field_size;      // the cells times a cell's size: what the field covers, frame pixels
    float to_frame;         // the field's units to frame pixels
    float pad;
};

float2 PSMain(float4 pos : SV_Position) : SV_Target
{
    const float2 p = pos.xy - picture_origin;
    if (any(p < 0.0) || any(p >= picture_size)) return float2(0.0, 0.0);
    return field.SampleLevel(linear_clamp, p / field_size, 0) * to_frame;
}
