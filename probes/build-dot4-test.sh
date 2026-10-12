#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the INT8 dot-product experiment: two copies of the probe app, both in
# PS5 Pro memory mode (attribute3 bit 22), identical except that F carries
# enable-dot4.txt, which turns on the patched driver's accelerated v_dot4.
#   E = control (driver behaves exactly as stock)
#   F = experiment (v_dot4 enabled; the probe checks every answer)
# Needs RADV rebuilt with driver/ps5-dot4-experiment.patch (see README section
# "INT8 dot-product experiment"); with a stock driver F behaves like E.
#
# Usage: PROSPERO_ROOT=/path/to/ProsperoAI bash build-dot4-test.sh
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
: "${PROSPERO_ROOT:?Set PROSPERO_ROOT to your ProsperoAI checkout}"
pro='{"attribute3": 5242880}'

echo "=== PPSA99091: Probe E - dot4 off (control)"
PROBE_TITLE=PPSA99091 PROBE_NAME="Probe E - dot4 off" PARAM_PATCH="$pro" SKIP_MEMORY_TEST=1 \
    bash "$here/build-ps5.sh" | tail -2
echo "=== PPSA99092: Probe F - dot4 ON"
PROBE_TITLE=PPSA99092 PROBE_NAME="Probe F - dot4 ON" PARAM_PATCH="$pro" SKIP_MEMORY_TEST=1 DOT4=1 \
    bash "$here/build-ps5.sh" | tail -2
ls "$here/build/ps5/PPSA99092/enable-dot4.txt" >/dev/null && echo "F has enable-dot4.txt"
