#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build ps5-llm-bench: a no-UI llama.cpp test app for the PS5, using the exact
# llama.cpp ("Prism") source that ProsperoAI ships, plus our PS5 tuning switches.
#
# Needs a ProsperoAI checkout where these already ran:
#   bash tools/setup-native-dependencies.sh
#   bash tools/setup-vulkan-ci.sh          (RADV for the PS5)
#   bash tools/build-host-tools.sh         (ps5-native-tool)
#   bash tools/build-vulkan-runtime.sh ps5 (fetches and prepares the Prism source)
#
# Usage: PROSPERO_ROOT=~/ProsperoAI bash build.sh
# Output: build/ps5/PPSA99095/  -> upload to /data/homebrew/PPSA99095
# Options: TITLE=PPSA99095  JOBS=<n>  CLEAN_LLAMA=1 (rebuild llama.cpp from scratch)
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "${PROSPERO_ROOT:-$HOME/ProsperoAI}" && pwd)
title=${TITLE:-PPSA99095}
jobs=${JOBS:-$(nproc)}
[[ $title =~ ^PPSA[0-9]{5}$ ]] || { echo "TITLE must look like PPSA12345" >&2; exit 2; }

ref="$root/.deps/mihawk-vulkan-review"
sdk="$ref/.deps/native/ps5-payload-sdk"
radv="$ref/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
tool="$root/build/host/ps5-native-tool"
prism="$root/build/prism-llama-source"
for need in "$sdk/bin/prospero-lld" "$radv" "$tool" "$prism/ggml/src/ggml-vulkan/ggml-vulkan.cpp" \
            "$root/tools/ps5-llama-toolchain.cmake" "$root/runtime/libc.prx" "$ref/tools/radv-link.sh"; do
    [[ -e $need ]] || { echo "Missing $need - finish the ProsperoAI setup steps listed at the top of this script." >&2; exit 2; }
done

work="$here/build"
src="$work/llama-src"
lib="$work/llama-ps5"
app="$work/ps5/$title"
mkdir -p "$work/obj" "$work/stubs"

# 1. Our own copy of ProsperoAI's llama.cpp source, with the PS5 tuning switches.
if [[ ${CLEAN_LLAMA:-0} == 1 ]]; then rm -rf "$src" "$lib"; fi
if [[ ! -d $src ]]; then
    echo "==> copying ProsperoAI's llama.cpp source"
    cp -a "$prism" "$src"
fi
python3 "$root/tools/prepare-llama-ps5-io.py" --source "$src"
python3 "$here/patches/ggml-vulkan-ps5-knobs.py" "$src"

# 2. Build llama.cpp for the PS5 (first time ~20-40 min, later only what changed).
mkdir -p "$root/build/llama-vulkan/include-shim/spirv"
ln -sfn /usr/include/spirv/unified1 "$root/build/llama-vulkan/include-shim/spirv/unified1"
echo "==> building llama.cpp + ggml-vulkan for the PS5 ($jobs jobs)"
cmake -S "$src" -B "$lib" -G Ninja -U CMAKE_CXX_FLAGS \
    -DCMAKE_TOOLCHAIN_FILE="$root/tools/ps5-llama-toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DGGML_VULKAN=ON \
    -DGGML_AVX512=OFF -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON \
    -DVulkan_INCLUDE_DIR="$root/.deps/Vulkan-Headers/include" -DVulkan_LIBRARY="$radv" \
    -DSPIRV-Headers_DIR=/usr/share/cmake/SPIRV-Headers \
    -DLLAMA_BUILD_COMMON=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_EXAMPLES=OFF \
    -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_APP=OFF -DLLAMA_CURL=OFF -DLLAMA_OPENSSL=OFF >/dev/null
cmake --build "$lib" --target llama ggml-vulkan --parallel "$jobs"

# 3. The app.
echo "==> compiling and linking the app"
cc() { PS5_PAYLOAD_SDK="$sdk" USE_CCACHE=0 sh "$root/tooling/prospero-clang18" "$@"; }
cc -std=c++17 -O2 -Wall -Wextra -fexceptions -fcxx-exceptions -ffunction-sections -fdata-sections \
    -I "$src/include" -I "$src/ggml/include" -c "$here/src/bench.cpp" -o "$work/obj/bench.o"
cc -std=c++17 -O2 -Wall -Wextra -ffunction-sections -fdata-sections \
    -c "$here/src/ps5_compat.cpp" -o "$work/obj/ps5_compat.o"
cc -std=c11 -O2 -c "$root/tooling/native/llama_ps5_compat.c" -o "$work/obj/llama_compat.o"
for source in app_crt app_cpp_runtime; do
    cc -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
        -c "$ref/tooling/native/$source.cpp" -o "$work/obj/$source.o"
done
for name in Agc AgcDriver; do
    stub=agc_canary_link_stub.c
    [[ $name != AgcDriver ]] || stub=agc_driver_canary_link_stub.c
    cc -std=c11 -O2 -fPIC -c "$ref/vendor/ps5/sdk/stubs/$stub" -o "$work/obj/$name.o"
    "$sdk/bin/prospero-lld" --shared -soname "libSce$name.prx" -o "$work/stubs/libSce$name.so" "$work/obj/$name.o"
done

source "$ref/tools/radv-link.sh"
export PS5_CLANG=${PS5_CLANG:-clang-18}
radv_link_recipe "$ref" "$sdk" "$radv"
for symbol in pthread_once strtof strtod fseek ftell strcasestr; do
    radv_link_flags+=("--defsym=$symbol=prospero_$symbol")
done
cat > "$work/compat.map" <<'MAP'
{ local: pthread_once; strtof; strtod; fseek; ftell; strcasestr; };
MAP
"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
    --version-script "$work/compat.map" --version-script "$ref/tooling/native/app-symbols.map" \
    --exclude-libs=ALL -e _start -o "$work/llvm-pie.elf" \
    "$work/obj/app_crt.o" "$work/obj/app_cpp_runtime.o" "$work/obj/bench.o" "$work/obj/ps5_compat.o" \
    "$work/obj/llama_compat.o" "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" \
    --start-group "$lib/src/libllama.a" "$lib/ggml/src/libggml.a" "$lib/ggml/src/libggml-base.a" \
    "$lib/ggml/src/libggml-cpu.a" "$lib/ggml/src/ggml-vulkan/libggml-vulkan.a" --end-group \
    --defsym=vkGetInstanceProcAddr=radv_GetInstanceProcAddr --defsym=vkGetDeviceProcAddr=vk_common_GetDeviceProcAddr \
    --defsym=vkCmdCopyBuffer=vk_common_CmdCopyBuffer \
    --defsym=vkGetPhysicalDeviceFeatures2=vk_common_GetPhysicalDeviceFeatures2 \
    --defsym=execlp=prospero_execlp --defsym=posix_madvise=prospero_posix_madvise \
    "${radv_link_inputs[@]}" --as-needed "$sdk"/target/lib/*.so
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" --stub-dir "$sdk/target/lib" \
    --stub "$work/stubs/libSceAgc.so" --stub "$work/stubs/libSceAgcDriver.so" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf

# 4. The app folder, in PS5 Pro memory mode (attribute3 bit 22).
rm -rf "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
"$tool" self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
cp "$root/sce_sys/"{icon0.png,pic0.dds,pic1.dds,snd0.at9} "$app/sce_sys/"
python3 - "$root/sce_sys/param.json" "$app/sce_sys/param.json" "$title" <<'PY'
import json, sys
p = json.load(open(sys.argv[1]))
t = sys.argv[3]
p.update(titleId=t, conceptId=t[4:], contentId=f'UP9000-{t}_00-PS5LLMBENCH00001', attribute3=5242880)
for v in p['localizedParameters'].values():
    if isinstance(v, dict):
        v['titleName'] = 'LLM Bench'
json.dump(p, open(sys.argv[2], 'w'), indent=2)
PY
cp "$root/runtime/libc.prx" "$app/sce_module/libc.prx"
cp "$here/env.txt" "$app/env.txt"
chmod 755 "$app/eboot.bin" "$app/sce_module/libc.prx"
echo
echo "LLM Bench app folder: $app"
echo "Upload it to /data/homebrew/$title (eboot.bin and sce_module/libc.prx must stay 755)."
