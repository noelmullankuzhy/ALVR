#include "../alvr_server/shader/AngleChartAxis.h"
#include "../alvr_server/shader/AngleChartColor.h"
#include <cassert>
#include <initializer_list>
// Client SDR decode (alvr-visionos VideoColour.h): sRGB EOTF, then the encoding gamma.
static float ClientLinear(float code, float gamma) {
    const float l = code < 0.04045f ? code / 12.92f : std::pow((code + 0.055f) / 1.055f, 2.4f);
    return std::pow(l, gamma);
}
static bool Near(float a, float b, float tolerance = 1e-4f) { return std::abs(a - b) <= tolerance; }
static void TestColour() {
    for (float mode : {ANGLE_CHART_SDR, ANGLE_CHART_HDR, ANGLE_CHART_PQ})
        for (float invGamma : {1.f, 1.f / 1.5f}) {
            assert(AngleChartEncode(0, mode, invGamma) == 0);
            assert(Near(AngleChartEncode(1, mode, invGamma), 1)); // line colours are unchanged
        }
    assert(Near(AngleChartEncode(0.06f, ANGLE_CHART_SDR, 1), 0.06f));
    assert(Near(AngleChartEncode(0.06f, ANGLE_CHART_SDR, 1.f / 1.5f), 0.153262f));
    assert(Near(AngleChartEncode(0.06f, ANGLE_CHART_PQ, 1.f / 1.5f), 0.06f));
    // m6 (enable_hdr, PQ off, gamma 1.5): the code that the client decodes back to 0.06 / 0.12.
    assert(Near(AngleChartEncode(0.06f, ANGLE_CHART_HDR, 1.f / 1.5f), 0.427886f));
    assert(Near(AngleChartEncode(0.12f, ANGLE_CHART_HDR, 1.f / 1.5f), 0.530364f));
    for (float value : {0.0005f, 0.06f, 0.12f, 0.5f})
        for (float gamma : {1.f, 1.5f, 2.2f})
            assert(Near(ClientLinear(AngleChartEncode(value, ANGLE_CHART_HDR, 1 / gamma), gamma), value, value * 1e-4f));
    // The old pass wrote 0.12 unencoded: the client shows ~77x darker (capture sRGB ~5-6 counts).
    assert(Near(ClientLinear(0.12f, 1.5f), 0.001553f, 1e-6f));
}
static void TestNoise() {
    assert(AngleChartNoise(10, 20, 1) == AngleChartNoise(10, 20, 1));
    int changed = 0, bits = 0;
    for (unsigned x = 0; x < 256; ++x) {
        const unsigned a = AngleChartNoise(x, 7, 41), b = AngleChartNoise(x, 7, 42);
        changed += (a & 0xffffff) != (b & 0xffffff);
        for (unsigned n = a & 0xffffff; n; n &= n - 1) ++bits;
    }
    assert(changed == 256);                     // every pixel changes every frame
    assert(bits > 256 * 24 * 45 / 100 && bits < 256 * 24 * 55 / 100); // ~half the RGB bits set
    assert(AngleChartNoise(1, 0, 0) != AngleChartNoise(0, 1, 0));
}
int main() {
    TestColour();
    TestNoise();
    assert(AngleChartHit(0, -1, 1, 1000));
    assert(AngleChartHit(0.007f, -1, 1, 1000)); // inclusive 7 px axis edge
    assert(!AngleChartHit(0.0071f, -1, 1, 1000));
    const float t15 = std::tan(15.f * 0.017453292519943295f);
    assert(AngleChartHit(t15 + 0.0049f, -1, 1, 1000));
    assert(!AngleChartHit(t15 + 0.0051f, -1, 1, 1000));
    const float t5 = std::tan(5.f * 0.017453292519943295f);
    assert(AngleChartHit(t5 + 0.0029f, -1, 1, 1000));
    assert(!AngleChartHit(t5 + 0.0031f, -1, 1, 1000));
    assert(AngleChartHit(-t15, -0.8f, 1.2f, 1000));
    assert(!AngleChartHit(0.001f, 0.0005f, 1, 100)); // line outside FoV
    assert(AngleChartHit(t5 + 0.0059f, -2, 2, 1000)); // scaled FoV
    assert(!AngleChartHit(t5 + 0.0061f, -2, 2, 1000));
}
