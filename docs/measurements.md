# PS5 Pro GPU measurements from homebrew

Measured with the probes in [`probes/`](../probes/) on a PS5 Pro (firmware
13.60), through Mihawk's RADV Vulkan driver at PS5_Mesa `0b2d6d1a61d9` (the
version ProsperoAI pins). The numbers are rounded and come from several runs.

## Compute and bandwidth

| What | Measured | Probe |
| --- | --- | --- |
| Workgroups running at once (≈ compute units in use) | ~56–60 | `staircase.comp` |
| FP16 packed FMA (`v_pk_fma_f16`) | ~36 TFLOPS | `alu16.comp` |
| FP32 FMA | ~13.7 TFLOPS | `alu.comp` |
| Memory read bandwidth | ~550 GB/s | `bandwidth.comp` |

**Compute units.** The driver's PS5 GPU model is fixed to the base PS5 (36
compute units). The staircase probe gives each workgroup 64 KiB of shared memory,
so only one fits per compute unit, then raises the workgroup count until the
time per dispatch jumps. The jump comes at about 56–60, not 36. So the Pro's
full GPU runs homebrew compute even though the driver reports a base PS5 GPU.

**FP16.** 36 TFLOPS matches 60 compute units × 64 lanes × 2 (packed halves) × 2
(multiply + add) × ~2.35 GHz. This is the rate llama.cpp's FP16 paths can reach
when they are not limited by memory.

**FP32.** Our FP32 shader reaches less than half the FP16 rate, so it falls
short of the theoretical peak. FP16 is the number that matters for AI work.

**Bandwidth.** ~550 GB/s of the Pro's 576 GB/s GDDR6 peak (about 95%). For
generating LLM tokens this is the limit that matters: each token reads every
weight once.

## Memory

| Mode | Direct memory pool | Vulkan heaps | Allocation ceiling |
| --- | --- | --- | --- |
| Base PS5 mode (`attribute3` 0x100000) | 11.44 GiB | 11.44 GiB | 11.31 GiB |
| Pro mode (`attribute3` 0x500000) | 12.63 GiB | 12.63 GiB | 12.50 GiB |

See [pro-memory-mode.md](pro-memory-mode.md).

## Features

| Feature | Result |
| --- | --- |
| `shaderFloat16` | supported |
| `shaderIntegerDotProduct` | supported, but not accelerated: the driver runs it as ordinary integer math (see [int8-dot4.md](int8-dot4.md)) |
| `VK_KHR_cooperative_matrix` | not exposed (RDNA2-class GPUs have no matrix cores for it) |

## What it means for local LLMs (estimates)

These are estimates, not measurements. ProsperoAI's published base PS5 figure
for Mistral 7B Q4_0 is 69.7 tokens/s.

- **Token generation** is limited by bandwidth: about 550 / 448 ≈ 1.25× the base
  PS5. Expect roughly 85–90 tokens/s for Mistral 7B Q4_0 on the Pro.
- **Prompt processing** is limited by compute: 60 vs 36 compute units at higher
  clocks. Expect roughly 1.6–1.7× the base PS5.
- **Model size**: with Pro memory mode, models and KV cache up to ~11–12 GiB
  together.
