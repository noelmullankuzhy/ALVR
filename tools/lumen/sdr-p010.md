# SDR P010 precision audit

`video.encoder_config.high_precision_sdr` defaults to false and requires a driver
restart. The setting is copied into `openvr_config.high_precision_sdr`, then read
by C++ Settings. It takes effect on Windows NVIDIA hardware with 10-bit encoding
enabled and HDR disabled (and software encoding not selected in GPL builds).
It does not change the client protocol or choose a codec. Use HEVC for Main10.

| Stage | Setting false | Setting true |
| --- | --- | --- |
| Composition, optional color correction, optional FFR | RGBA8 sRGB targets | RGBA16 float targets |
| Values sampled by correction/FFR | Linear RGB via sRGB SRVs | Linear RGB directly |
| SDR conversion | sRGB OETF, BT.709 YCbCr, P010 | Same |
| NVENC registration | YUV420_10BIT / DXGI P010 | Same |
| HEVC configuration | Explicit Main10, 10-bit 4:2:0 | Same |
| HEVC VUI | BT.709 primaries/matrix, IEC61966-2-1 transfer, configured range | Same |

The existing composition shader's encoding gamma and SDR input classification
are retained. In particular, float SDR does **not** take the HDR shader correction
branch: there is no extra sRGB OETF in composition. Composition, blending, color
correction and FFR remain linear, with sRGB transfer applied once at the final
conversion. No PQ or BT.2020 is introduced. No new original game precision can be
recovered from an 8-bit source; float16 preserves fractional processing results.

Both SDR 10-bit modes now use P010. This removes the old ABGR10 registration of an
RGBA8 allocation without relying on SDK support for converting 8-bit encoder input
to 10-bit output. Evidence: `VideoEncoderNVENC::Initialize` selects YUV420_10BIT;
`NvEncoderD3D11::GetD3D11Format` maps it to P010. The unused ABGR10 mapping is also
corrected to R10G10B10A2, matching the bundled NVENC header's packed RGB definition.
The false setting retains 8-bit intermediate precision, but now uses the explicit
SDR matrix, range and chroma sampling instead of NVENC's RGB conversion.

The existing HDR shader, coefficient tables, transfer handling and VUI selection
are unchanged. AMD/software paths retain their existing texture formats. FFR
inherits its input format, which gives the same formats as before outside float SDR.

## Conversion and packing

The shader uses BT.709 Kr=0.2126, Kg=0.7152, Kb=0.0722. Full-range Y is
`1023*Y`; limited Y is `64+876*Y`. Chroma is centered on 512, with span 1023
for full range or 896 for limited range; `Cb=(B-Y)/(2*(1-Kb))` and
`Cr=(R-Y)/(2*(1-Kr))`. Full-range extremes are saturated to 0..1023. UV is
the average of the four nonlinear RGB samples in each 2x2 block, equivalent to
averaging YCbCr chroma. Separate Y and UV draws use full and half-sized viewports;
the old HDR rendering workaround remains confined to its existing branch.

[Microsoft's P010 definition](https://learn.microsoft.com/en-us/windows/win32/medfound/10-bit-and-16-bit-yuv-video-formats)
requires samples in bits 15:6, with the low six bits zero. The shader rounds a
code `c` to `q=floor(clamp(c,0,1023)+0.5)` (ties upward), then outputs
`q*64/65535` to R16_UNORM / R16G16_UNORM plane views. UNORM quantization yields
the word `q<<6`, rather than relying on truncation of an arbitrary 16-bit result.

The audited ramp is all 1024 integer codes and both sides of every half-code
boundary, modeled with float32 shader multiplication and rounded R16_UNORM
storage. Every integer produces exactly `q<<6`; the low six bits are always zero.
Examples: 0 -> 0x0000, 1 -> 0x0040, 64 -> 0x1000, 512 -> 0x8000,
940 -> 0xEB00, 1023 -> 0xFFC0. A 4097-step gray ramp is monotonic in both ranges,
with neutral U/V=512; limited black/white are 64/940, and limited chroma endpoints
are 64/960. The CPU audit also checks red/green/blue BT.709 code patches.

## Validation and remaining measurements

Run `python3 tools/lumen/test-sdr-p010.py` for the packing/range reference audit
and `cargo test -p alvr_session --locked` for defaults, migration and serialized
driver-setting coverage. The Windows build compiles `sdr_p010.hlsl` using the
existing Windows SDK's fxc (ps_5_0, warnings as errors), embeds the generated
header, and leaves all existing CSOs unchanged.

UNVERIFIED until the GM runs streaming: actual GPU plane readback/packing,
decoded float/P010 ramps, client ALVRFORMAT HEVC Main10 + BT.709 + 10-bit decoder
pixel format, picture color, banding reduction and ALVR compositor/encoder timing.
Compare both setting values with identical full/limited range, gamma, FFR, bitrate
and scene; also retain an HDR regression capture. Target measurement is reduced
added gradient quantization without a color shift, with the extra GPU cost measured.
