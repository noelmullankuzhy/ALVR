# Head-locked angle chart (Windows)

Create `angle_chart.enable` beside the running streamer's `session.json` to
replace both composed eyes. Remove it to restore normal frames. The renderer
checks file existence at most once per second; no restart/rebuild is needed.
The path is derived from `g_sessionPath`, not the working directory. For a build
package it is `<package>\angle_chart.enable` (beside `<package>\session.json`).

The pass writes linear RGB before colour correction, FFR and YUV conversion.
It uses the same locked view-projection tangents as that frame's composition:
client view config → Hmd::SetViewsConfig → CEncoder → FrameRender. No pose is used.
On enable or changed tangents the session log emits one line per eye:
`angle_chart eye=L|R w= h= tanL= tanR= tanD= tanU= foveation= centerSize=x,y centerShift=x,y edgeRatio=x,y coded=WxH`.
Enable/disable also emits `angle_chart enabled=0|1`. Foveation fields are the
configured values; FFR's existing alignment/packing maths runs unchanged.

Put `dump` in the flag **before enabling** for one dump per enable. Remove then
recreate the flag (allowing a poll between) to dump again. Output is
`C:\Users\noelm\claude-tmp\angle-chart\<timestamp>\`: `chart-L.png`, `chart-R.png`,
`encoder-input.png` (packed stereo RGB immediately before YUV), and `meta.json`.
Tangents are ordered L/R eyes, each `[tanL,tanR,tanD,tanU]`; sizes are pixels.
PNGs are 8-bit RGBA: sRGB bytes for sRGB targets, clamped linear bytes for float
HDR/SDR targets. They are diagnostics, not HDR screenshots. Staging copies are
one-shot; GPU readback polls without waiting, and PNG/disk work runs on a worker.

Run `c++ -std=c++17 alvr/server_openvr/cpp/tests/AngleChartTest.cpp -o /tmp/angle-chart-test && /tmp/angle-chart-test`
for the shared CPU/HLSL line-hit tests. Live capture proof is performed by the GM.
