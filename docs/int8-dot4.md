# INT8 dot product (`v_dot4`) on the PS5 Pro

## Why it matters

Many AI kernels multiply 8-bit integers. AMD's RDNA2 GPUs have instructions
like `v_dot4_u32_u8` that do four 8-bit multiply-adds in one go, twice the
packed FP16 rate. llama.cpp's Vulkan backend uses them, through Vulkan's
integer dot-product feature, to process prompts faster for quantized models.

Mihawk's RADV driver turns these instructions off on the PS5
(`has_accelerated_dot_product = false`), because on the base PS5's GPU they
compute wrong. The driver then runs dot products as ordinary integer math,
which is correct but slower. The PS5 Pro's GPU is a different chip, so we
tested whether the instruction works there.

## Method

- [`probes/driver/ps5-dot4-experiment.patch`](../probes/driver/ps5-dot4-experiment.patch)
  adds one switch to the driver: when the app sets the environment variable
  `RADV_PS5_DOT4=1`, accelerated dot products are turned back on. Without the
  variable the driver behaves exactly like the stock one.
- The probe app sets the variable when an `enable-dot4.txt` file is next to
  `eboot.bin`.
- `dot_check.comp` computes 262,144 dot products of pseudo-random 4×8-bit
  vectors, the first few being edge cases (bytes 0x80, 0x7F and 0xFF), signed
  and unsigned, both with the instruction (`OpUDot` / `OpSDot`) and with plain
  integer math. The app checks every result
  against the CPU.
- `dot_speed.comp` times both versions.
- `probes/build-dot4-test.sh` builds two copies: E (control, no switch) and F
  (switch on). Both use Pro memory mode.

## Result (PS5 Pro, firmware 13.60)

| Build | Instruction results wrong | Manual results wrong |
| --- | --- | --- |
| E (driver's software path) | 0 of 262,144 | 0 |
| F (hardware `v_dot4`) | **262,144 of 262,144: every result is 0** | 0 |

On the Pro, the hardware instruction returns 0 every time. Either the
instruction doesn't exist on this chip under that encoding, or it is disabled.
**The driver's default (off) is correct on the PS5 Pro too.**

## What this does and doesn't tell us

- It does not touch Sony's custom ML instructions for the Pro (the "300 TOPS"
  used by PSSR). Those are separate instructions that public drivers and
  compilers don't know about.
- It does not slow anything down: token generation is limited by memory
  bandwidth, and prompt processing still runs on the FP16 path (~36 TFLOPS).
- If you test other encodings (`v_dot2`, `v_dot8_i32_i4`) or find the right one,
  please open an issue.
