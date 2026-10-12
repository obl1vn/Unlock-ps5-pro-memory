Suggested addition to the PS5 param.json `attribute3` notes:

> **0x400000 (bit 22): PS5 Pro mode.** On a PS5 Pro (tested on FW 13.60), a title
> with this bit gets a larger direct memory pool: 12.63 GiB instead of 11.44 GiB
> (`sceKernelGetDirectMemorySize`). Found in PS5 Pro Enhanced titles together
> with bit 4 (0x10), which only switches the video output mode. Homebrew can use
> `0x500000` (bit 20 + bit 22). Source:
> https://github.com/obl1vn/Unlock-ps5-pro-memory
