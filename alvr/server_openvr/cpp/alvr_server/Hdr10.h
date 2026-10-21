#pragma once
#include <array>
#include <cmath>
#include <cstddef>

namespace hdr10 {
// ST 2084 OETF. Keep EncodePqNormalized textually parallel to pq_p010.hlsl.
constexpr double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
constexpr double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0;
constexpr double c3 = 2392.0 / 4096.0 * 32.0;
constexpr double bt709ToBt2020[3][3] = {
    {0.627403896, 0.329283038, 0.043313066},
    {0.069097289, 0.919540395, 0.011362316},
    {0.016391439, 0.088013308, 0.895595253}
};
inline double EncodePqNormalized(double normalized, double m1 = hdr10::m1,
    double m2 = hdr10::m2, double c1 = hdr10::c1, double c2 = hdr10::c2, double c3 = hdr10::c3) {
    normalized = normalized > 0.0 ? (normalized < 1.0 ? normalized : 1.0) : 0.0;
    if (normalized == 0.0) return 0.0;
    double p = std::pow(normalized, m1);
    return std::pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}
inline double EncodePq(double nits) { return EncodePqNormalized(nits / 10000.0); }
inline bool SupportsPq(bool nvenc, bool hevc, bool tenBit, bool software) {
    return nvenc && hevc && tenBit && !software;
}
enum class Backend { Nvenc, Amf, Software };
struct InputPlan { bool pq, hdr, p010, floatComposition; };
inline InputPlan MakeInputPlan(Backend backend, bool requestPq, bool hevc, bool tenBit,
    bool hdr, bool highPrecisionSdr) {
    const bool pq = requestPq && SupportsPq(backend == Backend::Nvenc, hevc, tenBit, false);
    // Unsupported PQ falls back to Srgb; all legacy HDR settings stay intact.
    const bool p010 = backend == Backend::Nvenc && tenBit && (!hdr || pq);
    return {pq, hdr, p010, hdr || pq || (p010 && highPrecisionSdr)};
}
// Explicit float4 rows match the HLSL cbuffer without matrix packing assumptions.
struct ShaderParams {
    float yOffset, yScale, chromaScale, referenceWhiteScale;
    float m1, m2, c1, c2;
    float c3, padding0, padding1, padding2;
    float matrixRows[3][4];
    float lumaCoeffs[4];
};
static_assert(sizeof(ShaderParams) == 112 && offsetof(ShaderParams, matrixRows) == 48);
inline ShaderParams MakeShaderParams(float white) {
    ShaderParams params = {
        64.0f, 876.0f, 896.0f,
        white / 10000.0f, float(m1), float(m2), float(c1), float(c2), float(c3),
        0, 0, 0, {}, {0.2627f, 0.6780f, 0.0593f, 0}
    };
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            params.matrixRows[row][col] = float(bt709ToBt2020[row][col]);
    return params;
}
// Scalar oracle using the uploaded floats, including gamut, exposure and YCbCr.
inline std::array<double, 3> EncodeLinearBt709(const ShaderParams& p, std::array<double, 3> rgb) {
    std::array<double, 3> encoded{};
    for (int row = 0; row < 3; ++row) {
        double normalized = 0;
        for (int col = 0; col < 3; ++col) normalized += p.matrixRows[row][col] * rgb[col];
        encoded[row] = EncodePqNormalized(normalized * p.referenceWhiteScale,
            p.m1, p.m2, p.c1, p.c2, p.c3);
    }
    double y = encoded[0] * p.lumaCoeffs[0] + encoded[1] * p.lumaCoeffs[1] + encoded[2] * p.lumaCoeffs[2];
    return {p.yOffset + p.yScale * y,
        512 + p.chromaScale * (encoded[2] - y) / (2 * (1 - p.lumaCoeffs[2])),
        512 + p.chromaScale * (encoded[0] - y) / (2 * (1 - p.lumaCoeffs[0]))};
}
} // namespace hdr10
