#pragma once
#include <cmath>
#include <cstddef>

namespace hdr10 {
// ST 2084 OETF. Keep EncodePqNormalized textually parallel to sdr_p010.hlsl.
constexpr double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
constexpr double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0;
constexpr double c3 = 2392.0 / 4096.0 * 32.0;
constexpr double bt709ToBt2020[3][3] = {
    {0.627403896, 0.329283038, 0.043313066},
    {0.069097289, 0.919540395, 0.011362316},
    {0.016391439, 0.088013308, 0.895595253}
};
inline double EncodePqNormalized(double normalized) {
    normalized = normalized > 0.0 ? (normalized < 1.0 ? normalized : 1.0) : 0.0;
    if (normalized == 0.0) return 0.0;
    double p = std::pow(normalized, m1);
    return std::pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}
inline double EncodePq(double nits) { return EncodePqNormalized(nits / 10000.0); }
inline bool SupportsPq(bool nvenc, bool hevc, bool tenBit, bool software) {
    return nvenc && hevc && tenBit && !software;
}
// Explicit float4 rows match the HLSL cbuffer without matrix packing assumptions.
struct ShaderParams {
    float yOffset, yScale, chromaScale, referenceWhiteScale;
    float m1, m2, c1, c2;
    float c3, pqEnabled, padding0, padding1;
    float matrixRows[3][4];
};
static_assert(sizeof(ShaderParams) == 96 && offsetof(ShaderParams, matrixRows) == 48);
inline ShaderParams MakeShaderParams(bool pq, bool full, float white) {
    full = full && !pq;
    ShaderParams params = {
        full ? 0.0f : 64.0f, full ? 1023.0f : 876.0f, full ? 1023.0f : 896.0f,
        white / 10000.0f, float(m1), float(m2), float(c1), float(c2), float(c3),
        pq ? 1.0f : 0.0f, 0, 0, {}
    };
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            params.matrixRows[row][col] = float(bt709ToBt2020[row][col]);
    return params;
}
} // namespace hdr10
