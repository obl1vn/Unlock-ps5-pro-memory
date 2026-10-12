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

Happy to open a PR for the param.json and lint change if you want it.
