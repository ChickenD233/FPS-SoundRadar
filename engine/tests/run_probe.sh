#!/bin/sh
# Host-side probe for the pure detection path (macOS/Linux, no audio devices).
# Usage: sh engine/tests/run_probe.sh [--dump]
set -e
dir=$(dirname "$0")
out="${TMPDIR:-/tmp}/sr_probe"
c++ -std=c++17 -O2 -o "$out" "$dir/sr_probe.cpp" "$dir/../analysis.cpp" "$dir/../classify.cpp" "$dir/../downmix.cpp"
exec "$out" "$@"
