// SPDX-License-Identifier: MIT
// The picture, scaled into the stream window's texture as SDR sRGB
// (stream_out.h). The frame comes in the back buffer's encoding:
//   0  SDR: as it is
//   1  HDR10: BT.2020 primaries, PQ
//   2  scRGB: linear BT.709, 1.0 = 80 nits
// HDR is tone mapped as the screenshot's PNG is (src/common/tone_map.h): white
// at white_nits, colours outside BT.709 brought in toward grey, the brightest
// channel rolled off above the knee.

Texture2D<float4> frame : register(t0);
SamplerState smooth : register(s0);

cbuffer Params : register(b0) {
    uint mode;
    float white_nits;
    float2 pad;
    float4 picture;  // the picture's part of the frame: left, top, right, bottom as 0-1
};

void VSMain(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    // One triangle over the whole target.
    float2 t = float2((id << 1) & 2, id & 2);
    pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    uv = lerp(picture.xy, picture.zw, t);
}

float3 pq_to_nits(float3 e) {
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow(saturate(e), 1.0 / m2);
    return 10000.0 * pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

static const float3x3 kBt2020To709 = {
    1.660491, -0.587641, -0.072850,
    -0.124550, 1.132900, -0.008349,
    -0.018151, -0.100579, 1.118730,
};

float3 srgb_encode(float3 v) {
    v = saturate(v);
    return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

// Linear BT.709 light, 1.0 = the SDR white, to sRGB.
float3 to_sdr(float3 c) {
    const float knee = 0.75;
    float y = dot(c, float3(0.2126, 0.7152, 0.0722));
    if (!(y > 0.0)) return 0.0;
    float low = min(c.r, min(c.g, c.b));
    if (low < 0.0) c = y + (c - y) * (y / (y - low));
    float high = max(c.r, max(c.g, c.b));
    if (high > knee) c *= (knee + (1.0 - knee) * tanh((high - knee) / (1.0 - knee))) / high;
    return srgb_encode(c);
}

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 c = frame.SampleLevel(smooth, uv, 0).rgb;
    if (mode == 1)
        c = to_sdr(mul(kBt2020To709, pq_to_nits(c)) / white_nits);
    else if (mode == 2)
        c = to_sdr(c * (80.0 / white_nits));
    return float4(c, 1.0);
}
