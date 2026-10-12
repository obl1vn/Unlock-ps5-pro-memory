#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Compile the compute shaders to SPIR-V, validate them, and embed them in
# <out>/shaders.h. Usage: compile-shaders.sh <out-dir>
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out=${1:?usage: compile-shaders.sh <out-dir>}
mkdir -p "$out/spv"
spvs=()
for src in "$here"/shaders/*.comp; do
    name=$(basename "$src" .comp)
    glslc --target-env=vulkan1.3 -O "$src" -o "$out/spv/$name.spv"
    if command -v spirv-val >/dev/null; then
        spirv-val --target-env vulkan1.3 "$out/spv/$name.spv"
    fi
    spvs+=("$out/spv/$name.spv")
done
python3 "$here/tools/embed_spirv.py" "$out/shaders.h" "${spvs[@]}"
echo "Embedded ${#spvs[@]} shaders in $out/shaders.h"
