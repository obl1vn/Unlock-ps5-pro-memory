# Faster text generation on the PS5 Pro: tuning llama.cpp's Vulkan backend

All numbers: PS5 Pro, firmware 13.60, Pro memory mode, Mistral 7B Instruct v0.3
Q4_0 (4.13 GB of weights), 4096-token context, three runs, median.

## Result

| Setup | Generation | Prompt reading |
| --- | ---: | ---: |
| Base PS5, stock ProsperoAI (published) | 69.7 tok/s | – |
| PS5 Pro, stock ProsperoAI 01.005.000 | 86.9 tok/s | 692 tok/s |
| PS5 Pro, ProsperoAI + tuning settings + batch 512 | 90.8 tok/s | 724 tok/s |
| **PS5 Pro, ProsperoAI Pro** (tuning + quieter interface) | **96.6 tok/s** | **740 tok/s** |
| PS5 Pro, bare llama.cpp, default settings | 94.0 tok/s | 718 tok/s |
| PS5 Pro, bare llama.cpp, tuning settings | 98.9 tok/s | 718 tok/s |

**ProsperoAI Pro is 11% faster than stock ProsperoAI on the same console, and
1.39× a base PS5.** Answers are unchanged: the settings only change how the
same arithmetic is split across the GPU.

## How we got there

### 1. A test app without an interface

[`llm-bench/`](../llm-bench/) is a small PS5 app: llama.cpp (the exact
version ProsperoAI ships) with every layer on the GPU, no interface, report on
`http://<PS5-IP>:9050/`. Settings come from an `env.txt` next to `eboot.bin`,
so each experiment is "edit one file, relaunch", with no rebuild.

The bare app generated 94.0 tok/s, ProsperoAI 86.9: **the interface itself cost
about 8%.** ProsperoAI redraws its full-HD interface every frame, and that
competes with the model for the GPU.

### 2. Where one token's time goes

llama.cpp's own per-operation GPU timer (`GGML_VK_PERF_LOGGER=1`) showed one
token as about 11 ms of GPU work:

| GPU work per token | Time | Weight read speed |
| --- | ---: | ---: |
| FFN gate + up, 14336×4096 (64 jobs) | 4.3 ms | ~495 GB/s |
| FFN down, 4096×14336 (32 jobs) | 2.5 ms | ~426 GB/s |
| Attention Q and O, 4096×4096 (64 jobs) | 1.6 ms | ~370–400 GB/s |
| Attention K and V, 1024×4096 (64 jobs) | 0.6 ms | ~245 GB/s |
| Output layer, Q6_K (1 job) | 0.2 ms | ~500 GB/s |
| ~230 small jobs (RMS norm, RoPE, attention, copies) | ~1.5–1.8 ms | – |

The large matrices already read at ~90% of the ~550 GB/s our probes measured.
The smaller ones did not fill the GPU, and the many small jobs cost mostly
start-up time (a trivial ADD takes ~5.5 µs).

### 3. Switches for llama.cpp's mat-vec shaders

[`llm-bench/patches/ggml-vulkan-ps5-knobs.py`](../llm-bench/patches/ggml-vulkan-ps5-knobs.py)
adds environment switches to `ggml-vulkan.cpp`. With none set, nothing changes.
llama.cpp detects this GPU as RDNA2 and, for AMD, always uses one 64-thread
wave per workgroup for mat-vec.

| Test | Setting | tok/s |
| --- | --- | ---: |
| baseline | – | 94.0 |
| A | `GGML_VK_PS5_DMMV_WG=1` (4 waves per workgroup) | 97.7 |
| B | `GGML_VK_PS5_RM_STDQ=2` (2× rows per workgroup) | 96.9 |
| C | `GGML_VK_PS5_DMMV_SUBGROUP=32` (wave32) | 95.4 |
| D | `RADV_PERFTEST=cswave32` | 94.0 |
| E | fewer, larger GPU submissions | 91.6 |
| F | A + B | 96.6 |
| **G** | **A + C** | **98.9** |
| H | A + B + C | 96.4 |
| I | A + 4× rows | 89.3 |
| J | smaller GPU submissions | 93.5 |

**Best: bigger workgroups + wave32 (G).** With G the FFN-down jobs went from
~426 to ~491 GB/s and Q/O from ~370 to ~420 GB/s. All mat-vec work together now
takes ~8.7 ms per token, against ~8.3 ms at a realistic ~500 GB/s. Adding more
rows per workgroup on top (F, H, I) leaves too few workgroups for the small K/V
matrices. Bigger submissions (E) hurt, because llama.cpp's default overlaps
CPU preparation with GPU work.

### 4. A quieter interface during generation

[`llm-bench/patches/prosperoai-ps5pro.py`](../llm-bench/patches/prosperoai-ps5pro.py)
makes ProsperoAI read `vulkan_env.txt` at start-up and, while a generation is
running, sleep `PROSPERO_UI_SLEEP_MS` after each frame (50 ms ≈ 15 fps instead
of 60). That alone took ProsperoAI from 90.8 to 96.6 tok/s.

## Get it

- Ready-made build: [Releases of obl1vn/ProsperoAI](https://github.com/obl1vn/ProsperoAI/releases)
- Source: [`ps5pro` branch](https://github.com/obl1vn/ProsperoAI/tree/ps5pro)
- Proposed upstream: [ProsperoAI #28](https://github.com/blackbearreloaded/ProsperoAI/pull/28)
  and [#29](https://github.com/blackbearreloaded/ProsperoAI/pull/29)

## Build it

```bash
PROSPERO_ROOT=~/ProsperoAI bash llm-bench/build-prosperoai-pro.sh
```

It patches a ProsperoAI checkout, builds the release with `make app`, and
prepares `dist/PPSA99004` with Pro memory mode, `vulkan_env.txt` and
batch/ubatch 512. Needs `llvm-18` (`llvm-objcopy-18`) on the build machine.

## Not tested yet

- Other models and quantisations (Q4_K_M, Q8_0 and K-quants use different
  shader code). The settings apply to all of them, but their best values may
  differ.
- The base PS5.

## What is left

About 1.5 ms per token is spent in ~230 small GPU jobs. Merging them
(K+V into one mat-vec, RMS norm and RoPE into neighbouring jobs) could bring
bare llama.cpp to roughly 105 tok/s. That needs changes to llama.cpp's Vulkan
graph code, not settings.
