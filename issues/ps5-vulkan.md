**Title:** PS5 Pro findings: Pro memory mode via attribute3 bit 22, full 60 CUs in use, v_dot4 returns 0

Hi Mihawk, thanks for the driver. Some PS5 Pro results (firmware 13.60, PS5_Mesa
`0b2d6d1a61d9` as pinned by ProsperoAI), in case they're useful for
HARDWARE_FINDINGS.

**1. Pro memory mode.** With `attribute3` bit 22 (`0x400000`) in param.json, the
title's direct memory pool (`sceKernelGetDirectMemorySize`) is 12.63 GiB instead
of 11.44 GiB. The winsys picks it up as is: Vulkan heaps 12.63 GiB in total, and
12.50 GiB actually allocated and written (11.31 GiB without the bit).

**2. All 60 CUs run compute.** `radv_ps5_fill_info` describes the base PS5 GPU (36
CUs), but a dispatch where each workgroup takes 64 KiB of LDS (one per CU) only
slows down at about 56–60 workgroups. Packed FP16 FMA reaches ~36 TFLOPS, which
matches 60 CUs at ~2.35 GHz. Reporting 60 CUs on a Pro might help apps that size
their dispatches from the device properties.

**3. `v_dot4` on the Pro.** I forced `has_accelerated_dot_product = true` behind
an env var (patch in the repo below) and checked 262,144 `OpUDot`/`OpSDot` 4×8-bit
results against the CPU: on the Pro every instruction result was 0, while the
non-accelerated path was all correct. Keeping it off is right for the Pro too.

Probe source, the driver patch and full results:
https://github.com/obl1vn/Unlock-ps5-pro-memory
