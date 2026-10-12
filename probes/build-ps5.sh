#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the probes as a PS5 app folder, reusing ProsperoAI's proven RADV
# toolchain (same compiler wrapper, RADV archive, link recipe and signer as
# its `make vulkan-smoke` target).
#
# One-time setup inside a ProsperoAI checkout (see its docs/BUILDING.md):
#   make deps                          # SDK, Vulkan headers, Mihawk's RADV sources
#   make radv                          # RADV for the PS5 (heavy: needs LLVM 19 etc.)
#   bash tools/build-host-tools.sh     # ps5-native-tool (links and signs eboot.bin)
#
# Then:
#   PROSPERO_ROOT=/path/to/ProsperoAI PROBE_TITLE=PPSA99077 bash build-ps5.sh
# Optional: DOT4=1 (adds enable-dot4.txt; needs the patched driver), PROBE_NAME="Shown name", PARAM_PATCH='{"attribute3":5242880}' (merged into
# param.json), SKIP_MEMORY_TEST=1 (adds skip-memory-test.txt to the folder).
# Output: build/ps5/<PROBE_TITLE>/  -> upload that folder to /data/homebrew/
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "${PROSPERO_ROOT:?Set PROSPERO_ROOT to a ProsperoAI checkout that has run make deps and make radv}" && pwd)
title=${PROBE_TITLE:-PPSA99077}
[[ $title =~ ^PPSA[0-9]{5}$ ]] || { echo "PROBE_TITLE must look like PPSA12345" >&2; exit 2; }

ref="$root/.deps/mihawk-vulkan-review"
sdk="$ref/.deps/native/ps5-payload-sdk"
archive="$ref/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
tool="$root/build/host/ps5-native-tool"
headers="$root/.deps/Vulkan-Headers/include"
for need in "$sdk/bin/prospero-lld" "$archive" "$tool" "$headers/vulkan/vulkan.h" "$ref/tools/radv-link.sh"; do
    [[ -e $need ]] || { echo "Missing $need; finish the one-time setup above first." >&2; exit 2; }
done

work="$here/build/ps5"
app="$work/$title"
rm -rf "$app"
mkdir -p "$work/obj" "$work/stubs" "$app/sce_sys" "$app/sce_module"
cc() { PS5_PAYLOAD_SDK="$sdk" USE_CCACHE=0 sh "$root/tooling/prospero-clang18" "$@"; }

# Shaders -> SPIR-V embedded in a header (host glslc).
bash "$here/tools/compile-shaders.sh" "$work"

for name in probes platform_ps5; do
    cc -std=gnu11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
        -I "$headers" -I "$work" -I "$here/src" -c "$here/src/$name.c" -o "$work/obj/$name.o"
done
for source in app_crt app_cpp_runtime; do
    cc -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
        -c "$ref/tooling/native/$source.cpp" -o "$work/obj/$source.o"
done
for lib in Agc AgcDriver; do
    source=agc_canary_link_stub.c
    [[ $lib != AgcDriver ]] || source=agc_driver_canary_link_stub.c
    cc -std=c11 -O2 -fPIC -c "$ref/vendor/ps5/sdk/stubs/$source" -o "$work/obj/$lib.o"
    "$sdk/bin/prospero-lld" --shared -soname "libSce$lib.prx" -o "$work/stubs/libSce$lib.so" "$work/obj/$lib.o"
done

# RADV link recipe from Mihawk's PS5_Vulkan (sets radv_linker_script, radv_link_flags, radv_link_inputs).
source "$ref/tools/radv-link.sh"
export PS5_CLANG=${PS5_CLANG:-clang-18}
radv_link_recipe "$ref" "$sdk" "$archive"
"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
    --version-script "$ref/tooling/native/app-symbols.map" --exclude-libs=ALL -e _start \
    -o "$work/llvm-pie.elf" "$work/obj/app_crt.o" "$work/obj/app_cpp_runtime.o" \
    "$work/obj/probes.o" "$work/obj/platform_ps5.o" \
    "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" "${radv_link_inputs[@]}" \
    --as-needed "$sdk"/target/lib/*.so
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" --stub-dir "$sdk/target/lib" \
    --stub "$work/stubs/libSceAgc.so" --stub "$work/stubs/libSceAgcDriver.so" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
"$tool" self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F

cp "$root/sce_sys/"{icon0.png,pic0.dds,pic1.dds,snd0.at9} "$app/sce_sys/"
python3 - "$root/sce_sys/param.json" "$app/sce_sys/param.json" "$title" "${PROBE_NAME:-PS5 GPU Probes}" \
    "${PARAM_PATCH:-{\}}" <<'PY'
import json, sys
p = json.load(open(sys.argv[1]))
t, name, patch = sys.argv[3], sys.argv[4], json.loads(sys.argv[5])
p.update(titleId=t, conceptId=t[4:], contentId=f'UP9000-{t}_00-PS5PROBES0000001')
for v in p['localizedParameters'].values():
    if isinstance(v, dict):
        v['titleName'] = name

def merge(dst, src):
    for k, v in src.items():
        if isinstance(v, dict) and isinstance(dst.get(k), dict):
            merge(dst[k], v)
        else:
            dst[k] = v

merge(p, patch)
with open(sys.argv[2], 'w') as f:
    json.dump(p, f, indent=2)
if patch:
    print('param.json patch applied:', json.dumps(patch))
PY
cp "$root/runtime/libc.prx" "$app/sce_module/libc.prx"
chmod 755 "$app/eboot.bin" "$app/sce_module/libc.prx"
[[ ${SKIP_MEMORY_TEST:-0} == 0 ]] || : > "$app/skip-memory-test.txt"
[[ ${DOT4:-0} == 0 ]] || : > "$app/enable-dot4.txt"
echo "PS5 probe app folder: $app"
echo "Upload it to /data/homebrew/$title (keep eboot.bin and libc.prx executable, chmod 755)."
