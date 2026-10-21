// Shared with the CPU test: render-pixel widths, no antialiasing.
#ifdef __cplusplus
#pragma once
#include <cmath>
using std::abs;
using std::tan;
#endif
bool AngleChartHit(float coordinate, float lo, float hi, float pixels) {
    for (int k = -17; k <= 17; ++k) {
        float t = tan(float(k) * 5.f * 0.017453292519943295f);
        float width = k == 0 ? 7.f : (((unsigned int)(k + 18)) % 3 == 0 ? 5.f : 3.f);
        if (t >= lo && t <= hi && abs(coordinate - t) <= width * 0.5f * (hi - lo) / pixels)
            return true;
    }
    return false;
}
