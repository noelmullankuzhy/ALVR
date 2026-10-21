"""CPU reference audit of SDR P010 packing/range math; does not execute HLSL."""

import math
import struct
import unittest


def f32(value):
    return struct.unpack("f", struct.pack("f", value))[0]


def pack(code):
    rounded = math.floor(min(1023, max(0, code)) + 0.5)
    # Model the shader's float32 product and R16_UNORM quantization.
    normalized = f32(f32(rounded) * f32(64.0 / 65535.0))
    return math.floor(normalized * 65535.0 + 0.5)


def yuv(rgb, full):
    r, g, b = rgb
    y = 0.2126 * r + 0.7152 * g + 0.0722 * b
    return (
        (0 if full else 64) + (1023 if full else 876) * y,
        512 + (1023 if full else 896) * (b - y) / (2 * (1 - 0.0722)),
        512 + (1023 if full else 896) * (r - y) / (2 * (1 - 0.2126)),
    )


class SdrP010Audit(unittest.TestCase):
    def test_all_1024_codes_and_rounding_boundaries(self):
        for code in range(1024):
            self.assertEqual(pack(code), code << 6)
            self.assertEqual(pack(code) & 63, 0)
            if code < 1023:
                self.assertEqual(pack(code + 0.499), code << 6)
                self.assertEqual(pack(code + 0.5), (code + 1) << 6)
        self.assertEqual(pack(-1), 0)
        self.assertEqual(pack(1024), 65472)

    def test_full_and_limited_gray_ramps(self):
        for full in (True, False):
            previous = -1
            for step in range(4097):
                gray = step / 4096
                y, u, v = map(pack, yuv((gray,) * 3, full))
                self.assertGreaterEqual(y, previous)
                self.assertEqual((u, v), (32768, 32768))
                self.assertEqual(y & 63, 0)
                previous = y
            self.assertEqual(pack(yuv((0, 0, 0), full)[0]), 0 if full else 4096)
            self.assertEqual(pack(yuv((1, 1, 1), full)[0]), 65472 if full else 60160)

    def test_bt709_patches(self):
        # Independent expected rounded limited-range BT.709 10-bit codes.
        for rgb, expected in (
            ((1, 0, 0), (250, 409, 960)),
            ((0, 1, 0), (691, 167, 105)),
            ((0, 0, 1), (127, 960, 471)),
        ):
            self.assertEqual(tuple(pack(c) >> 6 for c in yuv(rgb, False)), expected)


if __name__ == "__main__":
    unittest.main()
