#include "AngleChartAxis.h"
#include "AngleChartColor.h"
cbuffer Chart { float4 tangents[2]; float4 size; float4 encode; }; // encode: mode, 1/gamma, stress, frame
float4 main(float2 uv : TEXCOORD0, float4 position : SV_Position) : SV_Target {
    uint eye = position.x >= size.x ? 1 : 0;
    float4 t = tangents[eye]; // left, right, down, up
    float x = lerp(t.x, t.y, (position.x - eye * size.x) / size.x);
    float y = lerp(t.w, t.z, position.y / size.y);
    bool v = AngleChartHit(x, t.x, t.y, size.x);
    bool h = AngleChartHit(y, t.z, t.w, size.y);
    float3 bg = eye == 0 ? float3(.06, .06, .12) : float3(.12, .06, .06);
    if (encode.z != 0) {
        uint n = AngleChartNoise(uint(position.x), uint(position.y), uint(encode.w));
        bg = float3(uint3(n, n >> 8, n >> 16) & 255u) * (0.5 / 255); // under the lines
    }
    float3 c = v || h ? float3(h ? 1 : 0, v ? 1 : 0, h ? 1 : 0) : bg;
    return float4(AngleChartEncode(c.r, encode.x, encode.y), AngleChartEncode(c.g, encode.x, encode.y),
                  AngleChartEncode(c.b, encode.x, encode.y), 1);
}
