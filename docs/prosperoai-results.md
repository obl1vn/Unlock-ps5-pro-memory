# Results in a real AI app: ProsperoAI on a PS5 Pro

Measured with [ProsperoAI](https://github.com/blackbearreloaded/ProsperoAI)
01.005.000 (official release, unmodified except for `param.json` and the tuning
files named below), on a PS5 Pro, firmware 13.60.

- Model: Mistral 7B Instruct v0.3 Q4_0, the same file as ProsperoAI's own
  benchmark (`Mistral-7B-Instruct-v0.3.Q4_0.gguf`, SHA-256 `19c3cf87…7c8b7f`).
- Tool: [`tools/prosperoai-bench.ps1`](../tools/prosperoai-bench.ps1). It calls
  ProsperoAI's `/api/generate` endpoint and uses the timings the console itself
  reports, so network delay is not included.
- Context 4096, three runs per setting, median shown.
- **Generation**: 256 tokens. Rate = (generated tokens − 1) / generation time,
  the same formula ProsperoAI's benchmark uses.
- **Prompt reading**: a 1,211-token prompt, each run starting with a different
  word so the prompt cache can't reuse the previous run.

## Pro memory mode inside ProsperoAI

ProsperoAI's own debug log, with `attribute3 = 5242880`:

```
Vulkan memory: reported free=13230669824 total=13555990528 bytes
```

13,555,990,528 bytes = **12.63 GiB**, against 11.44 GiB (11,712 MiB) in
ProsperoAI's published base-PS5 log. With Mistral 7B loaded and context on Auto,
ProsperoAI chose a 32,768-token context (8-bit KV cache, flash attention).

## Speed

| Setup | Generation (tok/s) | Prompt reading (tok/s) |
| --- | ---: | ---: |
| PS5 Pro, normal param.json | 86.9 | 692 |
| PS5 Pro, Pro memory mode | 86.9 | 695 |
| Pro mode + `batch_size.txt` / `ubatch_size.txt` = 512 | 86.9 | ~725 |
| Pro mode + `flash_attention.txt` = 2 (force on) | 86.8 | 692 |
| Pro mode + `flash_attention.txt` = 1 (force off) | 83.2 | 661 |

For comparison, ProsperoAI's published base PS5 result for the same model is
**69.7 tok/s** (firmware 12.70, 128 tokens, short prompt).

## What this shows

1. **The Pro is 1.25× faster than a base PS5 at generating text** (86.9 vs
   69.7 tok/s). That matches the memory bandwidth ratio we measured with the
   probes (~550 vs ~448 GB/s): generating text is limited by how fast the
   weights can be read.
2. **The memory flag costs nothing.** It adds 1.19 GiB and leaves speed
   unchanged, because the Pro's full GPU and bandwidth are already used without
   it.
3. **Best settings:** Pro memory mode, batch/ubatch 512 (+~5% prompt reading),
   flash attention left on auto (forcing it off costs ~4%).

## Headroom

Each generated token reads the ~4.1 GB of weights once: 86.9 tok/s × 4.1 GB ≈
**360 GB/s**, about **65%** of the ~550 GB/s the GPU can read (the base PS5 is
at the same ~64%). llama.cpp's Vulkan backend reaches 80–85% on well-tuned
desktop GPUs, so better-tuned kernels for this GPU could plausibly reach
~105–115 tok/s. That is a goal for further work, not a measurement.

## Side findings

- ProsperoAI's `/api/tags` returned a garbled model name on this console, so
  the benchmark takes the model name as a parameter (`-Model`).
- The PS5 on-screen keyboard doesn't open in ProsperoAI here (the log shows
  `keyboard_handles=0` and `load_prx failed due to 0x00000063`). The browser
  interface at `http://<PS5-IP>:11434/` works instead.
