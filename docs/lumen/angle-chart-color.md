# Angle chart colour path (why the client background was ~6 sRGB counts)

Evidence: `Management/goals/rdr2-vr/evidence/chart-matrix-20261022/m6/<point>/` (server dump and client
capture, all 10 points) and `proof-v2/SUMMARY.md`. Server develop `b1a60783`, client develop `ece16e3`.

## Configuration in m6 (from the logs)

- Server: `enable_hdr: true`, `hdr_transfer` not PQ (client logs `hdr=off`), `encoding_gamma: 1.5`, HEVC 10-bit, foveated encoding on.
- Client: `ALVRCOLOUR primaries=bt2020 transfer=sRGB matrix=bt2020 range=full bits=10 out=srgb path=explicit hdr=off`.
  Drawable `rgba16Float` (linear), capture written with `"encoding":"srgb"`.

## Chain

| Stage | Format / transfer | What the normal (game) path puts there |
|---|---|---|
| Composition texture (chart pass target) | `R16G16B16A16_FLOAT` (`m_enableHdr`) | `FrameRender.fx`: `linear^(1/encoding_gamma)`, then the sRGB OETF only for sRGB-format game textures or with `force_hdr_srgb_correction` (`inputColorAdjust=1`); other float sources get the gamma only |
| Colour correction, FFR | same float format | unchanged codes (clamp 0..1) |
| `rgbtoyuv420.hlsl` → P010 | BT.2020 matrix, full range, 10-bit; **no transfer applied** | codes → Y'CbCr |
| Client decode (`VideoColour.h` SDR branch) | `yuvToRGB` → sRGB EOTF → `^encoding_gamma` | back to linear |
| Client output | BT.2020 → BT.709 matrix into the linear `rgba16Float` drawable | |
| Capture PNG | sRGB-encoded drawable | |

Whatever the game source, the client decode is fixed: every value in that texture is treated as an
**encoded code** (sRGB EOTF, then `^encoding_gamma`). The chart wrote its spec colours (linear 0.06/0.12)
straight in, so they were decoded as codes: `client linear = sRGB_EOTF(0.12)^1.5 = 0.00155` instead of 0.12.
The chart encode therefore targets the client's decode, not the game texture's format.

Per server mode (one fixed linear value is wrong in two of three):

| Mode | Downstream of the chart pass, up to the client's linear output | Chart must write |
|---|---|---|
| SDR (`R8G8B8A8_UNORM_SRGB`, or float + `sdr_p010`) | the sRGB view or `sdr_p010` applies the sRGB OETF once; client sRGB EOTF, `^γ` | `linear^(1/γ)` |
| Legacy HDR (`enable_hdr`, PQ off), **m6** | no transfer on the server; client sRGB EOTF, `^γ` | `sRGB_OETF(linear^(1/γ))` |
| HDR10 PQ (`hdr_transfer` PQ) | `pq_p010` applies reference white and ST 2084; client PQ EOTF, `/white` | `linear` (1.0 = reference white) |

## Predicted vs measured (left eye, background (0.06, 0.06, 0.12))

| | Client linear (BT.2020) | Capture sRGB counts (after BT.2020→709) |
|---|---|---|
| Before fix, predicted | (0.000339, 0.000339, 0.001561) | (0.8, 1.1, 5.6) → **(1, 1, 6)** |
| Before fix, measured | – | **(1, 1, 6)** modal in all 10 m6 captures (f100: 25k of 26k samples; (1,1,5) the rest) |
| After fix, predicted | (0.060, 0.060, 0.120) | (67, 69, 100); without the gamut matrix (69, 69, 97) |

Right eye (not captured; one simulator slice): before (7.8, 0.6, 1.0) → (8, 1, 1), after (111, 65, 69).
Predicted values include 10-bit full-range code quantisation (flat field, so 4:2:0 chroma does not matter) and use
the sRGB OETF/EOTF, γ = 1.5 and the BT.2020→709 matrix built from the client's chromaticities. The background was 77x (blue) and 177x (red/green) darker in linear, not the ~30x first estimated.
Lines are 0 or 1, which every encode maps to 0 or 1, so they stayed correct; that is why only the background was wrong.

The server dump agreed with the spec because it writes the float texture as clamped bytes (0.12 → 31), with
no transfer applied. It also reads R/B swapped (m6 `chart-L.png` background is (31, 15, 15) though the left eye is blue
on the client and in the shader): the WIC PNG encoder appears to substitute BGRA for the requested RGBA. The dump
now swaps when WIC reports BGRA (untested on Windows).

## Fix

`AngleChartColor.h` (shared by `angle_chart.hlsl` and the CPU test) treats chart colours as display-referred linear SDR
(1 = SDR white) and applies the transfer that matches the target, selected by `FrameRender` from `m_hdrPq`,
`m_enableHdr` and `1/m_encodingGamma` (logged once as `angle_chart encode mode= invGamma= stress=`). With the m6
settings the background is written as codes (0.428, 0.428, 0.530) and the client decodes it to (0.06, 0.06, 0.12),
about 69/97 sRGB counts against the 0 surround, well above the overlay tool's 8-count separation. No client change is needed.

Note: the server's legacy HDR path labels BT.709 game content as BT.2020 without converting it; the chart follows the
game content, so the client's BT.2020→709 matrix shifts it slightly ((67, 69, 100) instead of (69, 69, 97)). This is an
upstream ALVR behaviour and is not changed here.
