#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build four copies of the probe app that differ only in param.json, to find
# which setting makes a PS5 Pro give an app Pro-mode resources. Each copy has
# its own title ID and name, so all four sit on the home screen side by side.
# The memory-filling test is skipped: the kernel's "direct memory pool" line at
# the top of each report is the number we compare (11.44 GiB in the baseline).
#
# The settings come from comparing a PS5 Pro Enhanced game (EA SPORTS FC 27)
# with a game that is not (Ghost of Tsushima) and with our own param.json:
#   attribute3: FC 27 0x00400010 (bits 4, 22), Ghost 0x00000004, ours 0x00100000
#   FC 27 only: "psml": {"mfsrVersion": "09.40"} (PlayStation ML upscaling)
#   FC 27: sdkVersion 0x12000000..., requiredSystemSoftwareVersion 0x13600000...
#
# Usage: PROSPERO_ROOT=/path/to/ProsperoAI bash build-variants.sh
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
: "${PROSPERO_ROOT:?Set PROSPERO_ROOT to your ProsperoAI checkout}"

build() {
    local title=$1 name=$2 patch=$3
    echo
    echo "=== $title: $name"
    PROBE_TITLE=$title PROBE_NAME="$name" PARAM_PATCH="$patch" SKIP_MEMORY_TEST=1 \
        bash "$here/build-ps5.sh" | tail -3
}

build PPSA99081 "Probe A - attr3 bit 22" '{"attribute3": 5242880}'
build PPSA99082 "Probe B - FC27 attr3 + PSML" '{"attribute3": 4194320, "psml": {"mfsrVersion": "09.40"}}'
build PPSA99083 "Probe C - SDK 12.00" '{"sdkVersion": "0x1200000000000000", "requiredSystemSoftwareVersion": "0x1360000000000000"}'
build PPSA99084 "Probe D - B and C together" '{"attribute3": 4194320, "psml": {"mfsrVersion": "09.40"}, "sdkVersion": "0x1200000000000000", "requiredSystemSoftwareVersion": "0x1360000000000000"}'

echo
echo "Built: $(ls -d "$here"/build/ps5/PPSA9908? | xargs -n1 basename | tr '\n' ' ')"
echo "Upload each folder to /data/homebrew/, refresh ShadowMount, run each, and send the"
echo "first ~6 lines of every report (the 'direct memory pool' line matters most)."
