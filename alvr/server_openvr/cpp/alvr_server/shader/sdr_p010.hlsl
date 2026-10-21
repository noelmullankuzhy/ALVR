// SDR only. Source SRVs expose linear RGB, including the sRGB legacy target.
// Keep composition/correction/FFR linear; apply sRGB transfer exactly once here.
cbuffer SdrP010Params {
    float yOffset;
    float yScale;
    float chromaScale;
    float padding;
};

Texture2D<float4> sourceTexture : register(t0);

float3 ReadSrgb(int2 pos) {
    float3 rgbLinear = saturate(sourceTexture.Load(int3(pos, 0)).rgb);
    return float3(
        rgbLinear.r <= 0.0031308 ? 12.92 * rgbLinear.r : 1.055 * pow(rgbLinear.r, 1.0 / 2.4) - 0.055,
        rgbLinear.g <= 0.0031308 ? 12.92 * rgbLinear.g : 1.055 * pow(rgbLinear.g, 1.0 / 2.4) - 0.055,
        rgbLinear.b <= 0.0031308 ? 12.92 * rgbLinear.b : 1.055 * pow(rgbLinear.b, 1.0 / 2.4) - 0.055
    );
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
    const float3 bt709 = float3(0.2126, 0.7152, 0.0722);
    float y = dot(ReadSrgb(pos), bt709);

    // Each UV texel covers a centered 2x2 block. Average in nonlinear RGB
    // before the linear YCbCr matrix (equivalent to averaging chroma).
    // HEVC VUI signals Annex E type 1: (0.5, 0.5) relative to top-left luma.
    int2 p = pos * 2;
    float3 rgb = (ReadSrgb(p) + ReadSrgb(p + int2(1, 0))
        + ReadSrgb(p + int2(0, 1)) + ReadSrgb(p + int2(1, 1))) * 0.25;
    float uvY = dot(rgb, bt709);
    float u = 512.0 + chromaScale * (rgb.b - uvY) / (2.0 * (1.0 - bt709.b));
    float v = 512.0 + chromaScale * (rgb.r - uvY) / (2.0 * (1.0 - bt709.r));

    PS_OUTPUT output;
    output.plane_Y = float4(PackP010(yOffset + yScale * y), 0, 0, 1);
    output.plane_UV = float4(PackP010(u), PackP010(v), 0, 1);
    return output;
}
