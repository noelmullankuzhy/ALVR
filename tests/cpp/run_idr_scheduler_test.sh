#!/bin/sh
# Builds and runs the IDRScheduler test (no Windows/OpenVR deps).
set -e
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../../alvr/server_openvr/cpp
out=${TMPDIR:-/tmp}/idr_scheduler_test.$$
trap 'rm -f "$out"' EXIT
clang++ -std=c++17 -Wall -Wextra -Werror -DIDR_SCHEDULER_STANDALONE \
    -I"$src" "$here/idr_scheduler_test.cpp" "$src/alvr_server/IDRScheduler.cpp" -o "$out"
"$out"
