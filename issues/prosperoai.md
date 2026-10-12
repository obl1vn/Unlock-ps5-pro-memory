**Title:** PS5 Pro: attribute3 bit 22 gives the app 12.63 GiB instead of 11.44 GiB

Hi, and thanks for ProsperoAI. Its toolchain and docs made this possible.

On a PS5 Pro (firmware 13.60), setting bit 22 (`0x400000`) in `attribute3` puts a
homebrew title in Pro memory mode:

| `attribute3` | Direct memory pool | Largest Vulkan allocation that succeeds |
| --- | --- | --- |
| `1048576` (`0x100000`, current) | 11.44 GiB | 11.31 GiB |
| `5242880` (`0x500000`, bit 20 + bit 22) | 12.63 GiB | 12.50 GiB |

RADV sizes its heaps from `sceKernelGetDirectMemorySize()`, so ProsperoAI would
get the extra ~1.19 GiB with no code change. That's more room for model weights
and KV cache. (On a Pro with today's `0x100000`, the heap is the same 11712 MiB
that `VK_LOG.md` shows for the base PS5.)

How I tested: a probe app built with ProsperoAI's RADV recipe. I built four
copies differing only in param.json. Only the ones with bit 22 got the larger
pool. Bit 4 (seen in Pro Enhanced games) only switches the TV's video mode, and
newer `sdkVersion` fields changed nothing. Full write-up, probe source and
results: https://github.com/obl1vn/Unlock-ps5-pro-memory

Things to note:

- `tools/lint.sh` currently requires `attribute3 == 0x100000`, so it would need
  to accept `0x500000`.
- Not tested on a base PS5. It probably changes nothing there, but it should be
  checked before making it the default. A separate Pro build, or a note in
  `BUILDING.md`, may be the safer first step.
- Same probes on the Pro: about 56–60 compute units take work, ~36 TFLOPS packed
  FP16, ~550 GB/s read bandwidth. INT8 `v_dot4` returns 0 for every input on the
  Pro, so the driver is right to keep it off.

Measured in ProsperoAI 01.005.000 itself (only param.json changed): your log shows
`Vulkan memory: reported ... total=13555990528 bytes` (12.63 GiB), speed unchanged at
86.9 tok/s for Mistral 7B Q4_0. Details: docs/prosperoai-results.md in the repo.
Also, `/api/tags` returned a garbled model name on my console (the chat page works).

**Update: +11% generation speed.** With your release source, two small changes took
Mistral 7B Q4_0 on the Pro from 86.9 to **96.6 tok/s** (prompt reading 692 -> 740):

1. ggml-vulkan mat-vec: four waves per workgroup and wave32 on this GPU (+4.5%,
   together with batch/ubatch 512). Added as opt-in environment switches.
2. While a generation runs, the interface sleeps 50 ms after each frame (+6.4%).
   Without it the full-HD redraw competes with inference for the GPU (bare llama.cpp
   was ~8% faster than the app).

Patches, measurements and a build script: docs/llm-tuning.md and llm-bench/ in the
repo. I'd be glad to turn them into PRs (the interface change could also become a
setting).

Happy to open a PR for the param.json and lint change if you want it.
