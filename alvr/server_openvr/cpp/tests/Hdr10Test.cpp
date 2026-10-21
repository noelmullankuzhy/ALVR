#include "../alvr_server/Hdr10.h"
#include <cassert>
#include <iostream>
#include <limits>

int main() {
    using namespace hdr10;
    const auto near = [](double a, double b, double tolerance) {
        assert(std::abs(a - b) <= tolerance);
    };
    near(EncodePq(0), 0, 0);
    near(EncodePq(100), 0.5081, 0.0005);
    near(EncodePq(203), 0.5807, 0.0005);
    near(EncodePq(1000), 0.7518, 0.0005);
    near(EncodePq(10000), 1, 1e-12);
    near(EncodePq(-100), 0, 0);
    near(EncodePq(std::numeric_limits<double>::quiet_NaN()), 0, 0);
    near(EncodePq(20000), 1, 1e-12);
    near(EncodePq(std::numeric_limits<double>::infinity()), 1, 1e-12);
    for (const auto& row : bt709ToBt2020) near(row[0] + row[1] + row[2], 1, 1e-4);
    // Red is not white: independently check the orientation of the matrix.
    near(bt709ToBt2020[0][0], 0.6274, 1e-4);
    near(bt709ToBt2020[1][0], 0.0691, 1e-4);
    near(bt709ToBt2020[2][0], 0.0164, 1e-4);
    near(64 + std::round(EncodePq(203) * 876), 64 + std::round(0.5807 * 876), 1);
    const auto params = MakeShaderParams(true, true, 203);
    near(params.referenceWhiteScale, 203.0 / 10000, 1e-8);
    near(params.yOffset, 64, 0);
    near(params.yScale, 876, 0);
    near(params.chromaScale, 896, 0);
    near(params.pqEnabled, 1, 0);
    near(params.m1, m1, 1e-8);
    near(params.m2, m2, 1e-8);
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            near(params.matrixRows[row][col], bt709ToBt2020[row][col], 1e-7);
    assert(sizeof(params) == 96);
    const auto sdr = MakeShaderParams(false, true, 203);
    near(sdr.yOffset, 0, 0);
    near(sdr.yScale, 1023, 0);
    near(sdr.pqEnabled, 0, 0);
    for (int mask = 0; mask < 16; ++mask) {
        const bool nvenc = mask & 1, hevc = mask & 2, tenBit = mask & 4, software = mask & 8;
        assert(SupportsPq(nvenc, hevc, tenBit, software) == (mask == 7));
    }
    // The probe's scene-linear values above 1 must remain distinct through PQ.
    double previous = 0;
    for (double level : {0.05, 0.25, 0.5, 1.0, 2.0, 4.0, 4.926}) {
        const double code = EncodePq(level * 203);
        assert(code > previous && code < 1);
        previous = code;
    }
    std::cout << "HDR10 reference tests passed\n";
}
