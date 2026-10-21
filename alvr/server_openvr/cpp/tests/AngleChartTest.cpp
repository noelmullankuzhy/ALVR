#include "../alvr_server/shader/AngleChartAxis.h"
#include <cassert>
int main() {
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
