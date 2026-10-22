// Shared with the CPU test. Chart colours are display-referred linear SDR (1 = SDR white);
// encode them so the client's fixed decode for the active mode returns them.
#ifdef __cplusplus
#pragma once
#include <cmath>
using std::abs;
using std::pow;
#define ANGLE_CHART_INLINE inline
#else
#define ANGLE_CHART_INLINE
#endif
#define ANGLE_CHART_SDR 0.0 // linear^(1/gamma); the sRGB target or sdr_p010 applies the sRGB transfer
#define ANGLE_CHART_HDR 1.0 // legacy enable_hdr: client decodes sRGB EOTF then ^gamma, so write sRGB(linear^(1/gamma))
#define ANGLE_CHART_PQ 2.0  // scene-linear; pq_p010 applies reference white and ST 2084
ANGLE_CHART_INLINE float AngleChartEncode(float value, float mode, float invGamma) {
    if (mode == ANGLE_CHART_PQ) return value;
    float v = pow(abs(value), invGamma); // abs is an fxc pow-base guard; chart colours are >= 0
    if (mode == ANGLE_CHART_HDR)
        v = v <= 0.0031308f ? 12.92f * v : 1.055f * pow(abs(v), 1.f / 2.4f) - 0.055f;
    return v;
}
ANGLE_CHART_INLINE unsigned int AngleChartHash(unsigned int x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
// Stress background: independent per pixel and per frame, one byte per channel.
ANGLE_CHART_INLINE unsigned int AngleChartNoise(unsigned int x, unsigned int y, unsigned int frame) {
    return AngleChartHash(x + AngleChartHash(y + AngleChartHash(frame)));
}
