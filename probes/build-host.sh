#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build and run the probes on a desktop Linux machine to check they work.
# Needs: glslc, python3, a C compiler, Vulkan headers/loader (libvulkan-dev)
# and any Vulkan driver (mesa-vulkan-drivers gives the lavapipe CPU driver).
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
out="$here/build/host"
mkdir -p "$out/spv"
bash "$here/tools/compile-shaders.sh" "$out"
cc -std=gnu11 -O2 -Wall -Wextra -Werror -I "$out" -I "$here/src" \
    "$here/src/probes.c" "$here/src/platform_host.c" -lvulkan -o "$out/ps5-probes"
echo "Built $out/ps5-probes"
if [[ ${RUN:-1} == 1 ]]; then
    "$out/ps5-probes"
fi
