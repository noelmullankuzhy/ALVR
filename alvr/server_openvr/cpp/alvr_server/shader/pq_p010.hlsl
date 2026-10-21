// PQ only: linear BT.709 composition/correction/FFR -> BT.2020/PQ limited P010.
cbuffer PqP010Params {
    float yOffset;
    float yScale;
    float chromaScale;
    float referenceWhiteScale;
    float m1; float m2; float c1; float c2;
    float c3; float padding0; float padding1; float padding2;
    float4 matrixRow0; float4 matrixRow1; float4 matrixRow2;
    float4 lumaCoeffs;
};

Texture2D<float4> sourceTexture : register(t0);

// Textually parallel to Hdr10.h; ordered comparison maps NaN/negative to zero.
// max() wrappers only guard fxc's pow-base analysis; valid inputs are unchanged.
float EncodePqNormalized(float normalized) {
    normalized = normalized > 0.0 ? (normalized < 1.0 ? normalized : 1.0) : 0.0;
    if (normalized == 0.0) return 0.0;
    float p = pow(max(normalized, 0.0), m1);
    return pow(max((c1 + c2 * p) / (1.0 + c3 * p), 0.0), m2);
}

float3 ReadEncoded(int2 pos) {
    float3 linearRgb = sourceTexture.Load(int3(pos, 0)).rgb;
    float3 normalized = float3(dot(matrixRow0.xyz, linearRgb),
        dot(matrixRow1.xyz, linearRgb), dot(matrixRow2.xyz, linearRgb)) * referenceWhiteScale;
    return float3(EncodePqNormalized(normalized.r), EncodePqNormalized(normalized.g),
        EncodePqNormalized(normalized.b));
}

float PackP010(float code) {
    // R16_UNORM stores round(value * 65535). Explicitly round the 10-bit
    // code first, then place it in bits 15:6, with bits 5:0 exactly zero.
    return floor(clamp(code, 0.0, 1023.0) + 0.5) * (64.0 / 65535.0);
}

struct PS_OUTPUT {
    float4 plane_Y : SV_Target0;
    float4 plane_UV : SV_Target1;
};

PS_OUTPUT main(float4 position : SV_POSITION) {
    int2 pos = int2(position.xy);
    float y = dot(ReadEncoded(pos), lumaCoeffs.xyz);

    // Each UV texel covers a centered 2x2 block. Average in nonlinear RGB
    // before the linear YCbCr matrix (equivalent to averaging chroma).
    // HEVC VUI signals Annex E type 1: (0.5, 0.5) relative to top-left luma.
    int2 p = pos * 2;
    float3 rgb = (ReadEncoded(p) + ReadEncoded(p + int2(1, 0))
        + ReadEncoded(p + int2(0, 1)) + ReadEncoded(p + int2(1, 1))) * 0.25;
    float uvY = dot(rgb, lumaCoeffs.xyz);
    float u = 512.0 + chromaScale * (rgb.b - uvY) / (2.0 * (1.0 - lumaCoeffs.b));
    float v = 512.0 + chromaScale * (rgb.r - uvY) / (2.0 * (1.0 - lumaCoeffs.r));

    PS_OUTPUT output;
    output.plane_Y = float4(PackP010(yOffset + yScale * y), 0, 0, 1);
    output.plane_UV = float4(PackP010(u), PackP010(v), 0, 1);
    return output;
}
