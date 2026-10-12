#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build ProsperoAI for the PS5 Pro:
#   - llama.cpp's Vulkan backend with our PS5 tuning switches
#   - vulkan_env.txt support and a quieter interface during generation
#   - Pro memory mode (attribute3 = 5242880)
#   - tuned defaults: vulkan_env.txt + batch/ubatch 512
#
# It patches your ProsperoAI checkout in place (git diff shows every change;
# `git checkout src vulkan` undoes the app part).
#
# Usage: PROSPERO_ROOT=~/ProsperoAI bash build-prosperoai-pro.sh
# Output: <ProsperoAI>/dist/PPSA99004/  -> upload over /data/homebrew/PPSA99004
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "${PROSPERO_ROOT:-$HOME/ProsperoAI}" && pwd)
# ProsperoAI's font step needs a plain `clang++`; Ubuntu may only have clang++-18.
if [[ -z ${HOST_CXX:-} ]] && ! command -v clang++ >/dev/null; then export HOST_CXX=clang++-18; fi
command -v llvm-objcopy-18 >/dev/null || { echo "Missing llvm-objcopy-18: sudo apt-get install -y llvm-18" >&2; exit 2; }

echo "==> fetching anything ProsperoAI still needs (first time only)"
(cd "$root" && make deps)
python3 "$root/tools/deps.py" fetch stable-diffusion-vulkan llama-prism

echo "==> patching"
python3 "$root/tools/prepare-llama-ps5-io.py" --source "$root/build/prism-llama-source"
python3 "$here/patches/ggml-vulkan-ps5-knobs.py" "$root/build/prism-llama-source"
python3 "$here/patches/prosperoai-ps5pro.py" "$root"

echo "==> building ProsperoAI (llama.cpp rebuilds only what changed)"
(cd "$root" && make app)

app="$root/dist/PPSA99004"
[[ -f $app/eboot.bin ]] || { echo "build finished but $app/eboot.bin is missing" >&2; exit 2; }
python3 - "$app/sce_sys/param.json" <<'PY'
import json, sys
p = json.load(open(sys.argv[1]))
p['attribute3'] = 5242880
for v in p['localizedParameters'].values():
    if isinstance(v, dict):
        v['titleName'] = 'ProsperoAI Pro'
json.dump(p, open(sys.argv[1], 'w'), indent=2)
PY
cp "$here/prosperoai/vulkan_env.txt" "$app/vulkan_env.txt"
printf '512\n' > "$app/batch_size.txt"
printf '512\n' > "$app/ubatch_size.txt"
chmod 755 "$app/eboot.bin" "$app/sce_module/"*.prx 2>/dev/null || true
echo
echo "ProsperoAI Pro folder: $app"
echo "Upload it over /data/homebrew/PPSA99004 (same title: your models and chats stay)."
