# Head-locked angle chart (Windows)

Create `angle_chart.enable` beside the running streamer's `session.json` to
replace both composed eyes. Remove it to restore normal frames. The renderer
checks file existence at most once per second; no restart/rebuild is needed.
The path is derived from `g_sessionPath`, not the working directory. For a build
package it is `<package>\angle_chart.enable` (beside `<package>\session.json`).

The pass writes before colour correction, FFR and YUV conversion. Its colours are
display-referred linear SDR, encoded so the client's fixed decode for the active
mode (SDR, legacy HDR, or PQ) returns them; see
[the colour path](../../docs/lumen/angle-chart-color.md).
It uses the same locked view-projection tangents as that frame's composition:
client view config → Hmd::SetViewsConfig → CEncoder → FrameRender. No pose is used.
On enable or changed tangents the session log emits one line per eye:
`angle_chart eye=L|R w= h= tanL= tanR= tanD= tanU= foveation= centerSize=x,y centerShift=x,y edgeRatio=x,y coded=WxH`.
Enable/disable also emits `angle_chart enabled=0|1 stress=0|1`, and each buffer
rebuild emits `angle_chart encode mode=0|1|2 invGamma= stress=` (0 SDR, 1 legacy HDR, 2 PQ).
Foveation fields are the configured values; FFR's existing alignment/packing maths runs unchanged.

The flag content is comma-separated words, read when the flag appears:
`dump`, `stress`, or `stress,dump`. `stress` replaces the flat background with
per-pixel noise that changes every frame (hashed from x, y and a frame counter,
0..0.5 linear per channel), with the chart lines drawn on top, so the encoder runs
at its bitrate cap in the simulator. Remove and recreate the flag to change mode.

Put `dump` in the flag **before enabling** for one dump per enable. Remove then
recreate the flag (allowing a poll between) to dump again. Output is
`C:\Users\noelm\claude-tmp\angle-chart\<timestamp>\`: `chart-L.png`, `chart-R.png`,
`encoder-input.png` (packed stereo RGB immediately before YUV), and `meta.json`.
Tangents are ordered L/R eyes, each `[tanL,tanR,tanD,tanU]`; sizes are pixels.
PNGs are 8-bit RGBA: sRGB bytes for sRGB targets, clamped float values as bytes for
float targets (in legacy HDR these are the encoded codes, e.g. background 109/135). They are diagnostics, not HDR screenshots. Staging copies are
one-shot; GPU readback polls without waiting, and PNG/disk work runs on a worker.

Run `c++ -std=c++17 alvr/server_openvr/cpp/tests/AngleChartTest.cpp -o /tmp/angle-chart-test && /tmp/angle-chart-test`
for the shared CPU/HLSL line-hit, colour-encode and noise tests. Live capture proof is performed by the GM.
