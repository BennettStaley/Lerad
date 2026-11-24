// SPDX-License-Identifier: MIT
// Moves the picture on the frame by a few pixels (view_shift.h): each point
// of the picture's box takes its colour from shift pixels away in a copy of
// the frame. At the box's edge the last column or row is repeated. Outside
// the box (black bars) the frame is left as it is.
// Optionally a small fixed dot at the box's centre: the point aimed at while
// the game's own crosshair moves with the picture.

Texture2D<float4> frame : register(t0);
SamplerState smooth : register(s0);

cbuffer Params : register(b0) {
    float2 shift;   // pixels
    float2 size;    // the frame's
    float4 box;     // left, top, right, bottom, in pixels
    float dot_radius;  // 0: no dot
    float dot_white;   // the dot's white in the frame's encoding
    float2 pad;
};

void VSMain(uint id : SV_VertexID, out float4 pos : SV_Position) {
    // One triangle over the whole target.
    float2 uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target {
    if (pos.x < box.x || pos.y < box.y || pos.x >= box.z || pos.y >= box.w) discard;
    if (dot_radius > 0) {
        const float d = distance(pos.xy, (box.xy + box.zw) * 0.5);
        if (d <= dot_radius) return float4(dot_white, dot_white, dot_white, 1);
        if (d <= dot_radius + 1.5) return float4(0, 0, 0, 1);
    }
    const float2 from = clamp(pos.xy + shift, box.xy + 0.5, box.zw - 0.5);
    return frame.SampleLevel(smooth, from / size, 0);
}
