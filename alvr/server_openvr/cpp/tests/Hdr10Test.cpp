#include "../alvr_server/Hdr10.h"
#include <array>
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
    const auto params = MakeShaderParams(203);
    near(params.referenceWhiteScale, 203.0 / 10000, 1e-8);
    near(params.yOffset, 64, 0);
    near(params.yScale, 876, 0);
    near(params.chromaScale, 896, 0);
    near(params.m1, 2610.0 / 16384, 1e-8);
    near(params.m2, 2523.0 / 32, 1e-8);
    near(params.c1, 3424.0 / 4096, 1e-8);
    near(params.c2, 2413.0 / 128, 1e-8);
    near(params.c3, 2392.0 / 128, 1e-8);
    near(params.lumaCoeffs[0], 0.2627, 1e-7);
    near(params.lumaCoeffs[1], 0.6780, 1e-7);
    near(params.lumaCoeffs[2], 0.0593, 1e-7);
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            near(params.matrixRows[row][col], bt709ToBt2020[row][col], 1e-7);
    assert(sizeof(params) == 112);
    near(MakeShaderParams(80).referenceWhiteScale, 0.008, 1e-8);
    near(MakeShaderParams(400).referenceWhiteScale, 0.04, 1e-8);
    // Independent BT.2020 -> BT.709 matrix supplies scene-linear inputs to the
    // same constants that are uploaded. Test absolute 1000-nit RGB and white.
    const double to709[3][3] = {
        {1.660491002, -0.587641139, -0.072849863},
        {-0.124550475, 1.132899897, -0.008349423},
        {-0.018150763, -0.100578898, 1.118729661}
    };
    const double encoded[4][3] = {
        {0.751827096, 0, 0}, {0, 0.751827096, 0},
        {0, 0, 0.751827096}, {0.751827096, 0.751827096, 0.751827096}
    };
    for (int color = 0; color < 4; ++color) {
        std::array<double, 3> input{};
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                input[row] += to709[row][col] * (color == 3 || color == col ? 1000.0 / 203 : 0);
        const auto codes = EncodeLinearBt709(params, input);
        const double y = 0.2627 * encoded[color][0] + 0.6780 * encoded[color][1]
            + 0.0593 * encoded[color][2];
        near(codes[0], 64 + 876 * y, 0.02);
        near(codes[1], 512 + 896 * (encoded[color][2] - y) / 1.8814, 0.02);
        near(codes[2], 512 + 896 * (encoded[color][0] - y) / 1.4746, 0.02);
    }
    for (int mask = 0; mask < 16; ++mask) {
        const bool nvenc = mask & 1, hevc = mask & 2, tenBit = mask & 4, software = mask & 8;
        assert(SupportsPq(nvenc, hevc, tenBit, software) == (mask == 7));
    }
    for (auto backend : {Backend::Nvenc, Backend::Amf, Backend::Software}) {
        for (int mask = 0; mask < 32; ++mask) {
            const bool request = mask & 1, hevc = mask & 2, tenBit = mask & 4;
            const bool hdr = mask & 8, precision = mask & 16;
            const auto plan = MakeInputPlan(backend, request, hevc, tenBit, hdr, precision);
            assert(plan.pq == (backend == Backend::Nvenc && request && hevc && tenBit));
            assert(plan.hdr == (hdr && backend != Backend::Software));
            assert(plan.p010 == (backend == Backend::Nvenc && tenBit && (!hdr || plan.pq)));
            assert(plan.floatComposition == (plan.hdr || plan.pq || (plan.p010 && precision)));
            if (backend == Backend::Software) assert(!plan.p010 && !plan.hdr && !plan.pq);
        }
    }
    // Regression: an NVENC failure on an NVIDIA adapter must select RGBA for SW.
    const auto nv = MakeInputPlan(Backend::Nvenc, true, true, true, false, true);
    const auto sw = MakeInputPlan(Backend::Software, true, true, true, false, true);
    assert(nv.pq && nv.p010 && nv.floatComposition);
    assert(!sw.pq && !sw.p010 && !sw.floatComposition);
    // The probe's scene-linear values above 1 must remain distinct through PQ.
    double previous = 0;
    for (double level : {0.05, 0.25, 0.5, 1.0, 2.0, 4.0, 4.926}) {
        const double code = EncodePq(level * 203);
        assert(code > previous && code < 1);
        previous = code;
    }
    std::cout << "HDR10 reference tests passed\n";
}
