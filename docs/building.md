# Building and running the probes

The probes are a small Vulkan app. On the PS5 they link with Mihawk's RADV
driver using ProsperoAI's toolchain. On a PC they run on any Vulkan driver,
which is useful for checking changes before you go to the console.

You need a jailbroken PS5 that can run homebrew, a way to upload folders to it
(for example ps5upload), and a launcher that registers homebrew titles
(for example ShadowMount).

## 1. Tools (Ubuntu 24.04, also works in WSL2 on Windows)

```bash
sudo apt-get update
sudo apt-get install -y git curl unzip rsync python3-venv python3-pip \
    clang-18 lld-18 clang-19 lld-19 llvm-19-dev libclang-19-dev libclang-cpp19-dev \
    libclc-19-dev libllvmspirvlib-19-dev cmake ninja-build ccache pkg-config \
    glslc glslang-tools spirv-headers spirv-tools libvulkan-dev mesa-vulkan-drivers \
    libzstd-dev zlib1g-dev libedit-dev libffi-dev libxml2-dev libgl-dev bison flex
```

## 2. ProsperoAI toolchain and the RADV driver (one time)

```bash
cd ~
git clone https://github.com/blackbearreloaded/ProsperoAI.git
cd ProsperoAI
bash tools/setup-native-dependencies.sh   # PS5 payload SDK and zlib
bash tools/setup-vulkan-ci.sh             # fetches pinned sources, builds RADV for the PS5
bash tools/build-host-tools.sh            # ps5-native-tool (links and signs eboot.bin)
ls runtime/libc.prx || bash tools/rebuild-libc.sh
```

Building RADV takes 30–60 minutes the first time.

## 3. Optional: run the probes on your PC

```bash
cd Unlock-ps5-pro-memory/probes
bash build-host.sh
```

## 4. Build for the PS5

```bash
cd Unlock-ps5-pro-memory/probes
PROSPERO_ROOT=~/ProsperoAI PROBE_TITLE=PPSA99077 bash build-ps5.sh
```

Options (environment variables):

| Variable | Effect |
| --- | --- |
| `PROBE_TITLE` | Title ID, `PPSA` + 5 digits. Pick one that isn't used on your console. |
| `PROBE_NAME` | Name on the home screen |
| `PARAM_PATCH` | JSON merged into param.json, e.g. `'{"attribute3": 5242880}'` |
| `SKIP_MEMORY_TEST=1` | Skips the allocation ceiling test |
| `DOT4=1` | Turns on the INT8 dot-product experiment (needs the patched driver, see [int8-dot4.md](int8-dot4.md)) |

Scripted experiments:

- `bash build-variants.sh` builds the four param.json variants A–D
  ([pro-memory-mode.md](pro-memory-mode.md)).
- `bash build-dot4-test.sh` builds the INT8 control and test apps E and F.

The output is a folder in `build/ps5/<TITLE>/`.

## 5. Run on the console

1. Upload the whole folder to `/data/homebrew/`. `eboot.bin` and
   `sce_module/libc.prx` must stay executable (permissions 755).
2. Register it with your launcher (ShadowMount: refresh) and start it from the
   home screen.
3. The screen stays black. That's expected: the app has no display.
4. On a phone or PC on the same network, open `http://<PS5-IP>:9050/`. The page
   says `STILL RUNNING` until the probes finish (about a minute), then ends with
   `PROBES COMPLETE`.
5. Close the app with the PS button.

The report is also printed to klog (lines start with `[ps5-probes]`) and saved to
`/data/ps5-probes-report.txt`.

If you rebuild with the same title ID, delete the old folder and re-register,
or the home screen may keep launching the old copy. The first line of the report
shows the probe version, so you can check which build ran.

## Sharing your results

Please open an issue with your console model, firmware version, and the report.
Remove anything personal first (the report itself contains no IP address or
serial number).
