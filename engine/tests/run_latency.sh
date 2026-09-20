#!/bin/sh
# Host-side latency/regression probe (macOS/Linux, no audio devices).
# It drives the real Analyzer and the real ArrowTracker with 10 ms blocks and
# prints the timings the field report was about: onset delay, direction-change
# delay, the visible tail after the sound stops, and arrows during ambience.
#
# Thresholds that must hold:
#   * ambience alone draws no arrow,
#   * the first arrow appears within 20 ms of the sound,
#   * the arrow follows a source that moves to the next wave channel within
#     100 ms,
#   * the scene is empty within 250 ms of the sound stopping.
#
# Usage: sh engine/tests/run_latency.sh
set -e
dir=$(dirname "$0")
out="${TMPDIR:-/tmp}/sr_latency"
c++ -std=c++17 -O2 -Wall -o "$out" "$dir/sr_latency.cpp" "$dir/../analysis.cpp" \
    "$dir/../classify.cpp" "$dir/../downmix.cpp"
exec "$out" "$@"
