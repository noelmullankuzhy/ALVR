# NVENC split-frame encoding and the NVENC-only encode timer

## Setting: `video.encoder_config.nvenc.split_encode_mode`

| Variant | Sent to NVENC (`NV_ENC_INITIALIZE_PARAMS::splitEncodeMode`) |
|---|---|
| `TwoIfAvailable` (default) | `NV_ENC_SPLIT_TWO_FORCED_MODE` when `NV_ENC_CAPS_NUM_ENCODER_ENGINES` >= 2 and the codec is HEVC or AV1, else `NV_ENC_SPLIT_DISABLE_MODE` |
| `Auto` | `NV_ENC_SPLIT_AUTO_MODE`: the driver decides |
| `ForceTwo` | `NV_ENC_SPLIT_TWO_FORCED_MODE` |
| `Disabled` | `NV_ENC_SPLIT_DISABLE_MODE` |

`TwoIfAvailable` is stored as 16 in `openvr_config.nvenc_split_encode_mode`; it is not an NVENC value and
`VideoEncoderNVENC::FillEncodeConfig` resolves it before it reaches the 4-bit `splitEncodeMode` field.
Forced two-strip mode also turns off weighted prediction, sub-frame write, video-memory output and HEVC
alpha, which block splitting. Needs NVENC API 12.1 (bundled header; driver 531.61+ on Windows).

Sessions saved before this change keep the variant they had selected (normally `Auto`); select
`TwoIfAvailable` in the dashboard or reset the setting to pick up the new default.

The server log at encoder start:

```
NVENC engines: 2 (NV_ENC_CAPS_NUM_ENCODER_ENGINES)
NVENC API 12.1: split setting=TwoIfAvailable requested=ForceTwo, preset=P1, tuning=..., coded=WxH, codec=1
NVENC split granted=two strips (derived from engines=2; API has no read-back)
```

API 12.1 cannot report the split the driver actually used. "granted" is derived from the engine count
and the rules in `NV_ENC_SPLIT_ENCODE_MODE` (ForceTwo on one engine encodes one strip; H.264 never
splits; Auto is reported as "driver choice (not reported)"). A successful `CreateEncoder` means the
driver accepted the requested mode.

## Statistic: `encode_nvenc_ms`

`encode_latency_ms` (dashboard "Encoder latency") runs from `ReportComposed` to the end of
`send_video_nal`, so it includes queued GPU composition and colour conversion, the texture copy, NVENC
and the network push. `encode_nvenc_ms` is the wall-clock time of `NvEncoder::EncodeFrame` only
(map input, `nvEncEncodePicture`, completion wait, `nvEncLockBitstream` copy-out, unlock), reported
from C++ through `ReportNvencEncoded(targetTimestampNs, durationNs)`.

- Events websocket: `StatisticsSummary.encode_nvenc_ms` (ms) and `GraphStatistics.encoder_nvenc_s` (s).
  Both are `null` with AMF, software or Linux encoders.
- Dashboard: "NVENC encode" row in the statistics overview and "of which NVENC" in the latency graph
  tooltip (not stacked, it is part of Encode).

Caveat: NVENC waits on GPU work still queued on its input texture, so the timer can include the tail
of the composition/conversion passes when they have not finished by the time `EncodeFrame` starts.
