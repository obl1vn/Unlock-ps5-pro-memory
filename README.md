# Unlock PS5 Pro memory for homebrew

**One param.json flag gives a PS5 Pro homebrew app about 1.2 GiB more GPU memory.**

PS5 Pro games get more memory than base PS5 games (Sony says about 13.7 GB
instead of 12.5 GB). Homebrew apps don't get that extra memory automatically.
They run in base PS5 memory mode even on a Pro. Setting **bit 22 of
`attribute3`** in the app's `sce_sys/param.json` switches the app into Pro
memory mode:

| `attribute3` | Direct memory pool | Largest allocation that succeeds |
| --- | --- | --- |
| `1048576` (`0x100000`), the usual homebrew value | 11.44 GiB | 11.31 GiB |
| **`5242880` (`0x500000`), the same plus bit 22** | **12.63 GiB** | **12.50 GiB** |

That is **+1.19 GiB** for the app. 12.63 GiB is 13.56 GB in Sony's decimal
units, which is close to the ~13.7 GB Sony quotes for Pro games.

For a local LLM this is the difference between, say, an 8B model at Q8 with a
short context and the same model with a long context, or room for a
bigger model at Q4. Any Vulkan homebrew built on Mihawk's RADV driver picks
it up with no code change. The driver sizes its memory heaps from the
kernel's pool, so the extra memory just appears.

Tested on: **PS5 Pro, firmware 13.60**, with homebrew built on
[ProsperoAI](https://github.com/blackbearreloaded/ProsperoAI)'s toolchain and
[Mihawk's RADV driver](https://github.com/mihawk-99/PS5_Mesa). One console so
far. Reports from other firmware versions are welcome (open an issue).

## How to use it

In your app's `sce_sys/param.json`, change

```json
"attribute3": 1048576,
```

to

```json
"attribute3": 5242880,
```

Then re-register the app (ShadowMount or your launcher) so the system reads the
new param.json. That's all.

- `5242880` = bit 20 (`0x100000`, the value homebrew already uses; ProsperoAI
  documents it as "VRR off") **+ bit 22** (`0x400000`, the Pro memory mode).
- **Don't** copy bit 4 (`16`) from a Pro Enhanced game's param.json. It
  switches the TV's video mode (the screen goes black for a moment while the TV
  re-syncs) and has nothing to do with memory.
- **On a base PS5** this is untested. A base PS5 has no extra memory to give,
  so expect no gain. Test carefully if you try it.

## How we found it

We wrote a small probe app (in [`probes/`](probes/)) that runs on the console,
asks the kernel and the Vulkan driver how much memory the app has, then
allocates GPU memory until the driver refuses. It reports through a web page on
port 9050. We then built four copies that differ only in param.json, using
settings that differ between a PS5 Pro Enhanced game and our app:

| Probe | param.json change | Pool |
| --- | --- | --- |
| baseline | `attribute3` 0x100000 | 11.44 GiB |
| A | `attribute3` 0x500000 (bit 22 added) | **12.63 GiB** |
| B | `attribute3` 0x400010 (bits 4 + 22) and a `psml` block | **12.63 GiB**, plus a TV mode switch |
| C | newer `sdkVersion` / `requiredSystemSoftwareVersion` only | 11.44 GiB (no change) |
| D | B and C together | **12.63 GiB**, plus a TV mode switch |

Only the probes with bit 22 gained memory. Probe A then ran the full
allocation test: 12.50 GiB allocated and used, against 11.31 GiB before.

The full write-up is in [docs/pro-memory-mode.md](docs/pro-memory-mode.md).

## Other results from the same probes

Full numbers are in [docs/measurements.md](docs/measurements.md).

- **The whole Pro GPU does run homebrew compute.** Mihawk's driver describes
  the GPU as a base PS5 GPU (36 compute units), but about 56–60 workgroups run
  at once, and packed FP16 reaches **~36 TFLOPS**. That matches all 60 of the
  Pro's compute units. Read bandwidth is **~550 GB/s**.
- **INT8 dot product (`v_dot4`) does not work on the Pro either.** The driver
  turns it off because it computes wrong on the base PS5. We patched the driver
  to turn it back on for a test app: on the Pro every one of 262,144 test
  results came back 0. The driver is right to keep it off. Details in
  [docs/int8-dot4.md](docs/int8-dot4.md).

## What's in this repo

| Path | What it is |
| --- | --- |
| [`docs/pro-memory-mode.md`](docs/pro-memory-mode.md) | The memory finding: method, evidence, open questions |
| [`docs/measurements.md`](docs/measurements.md) | GPU compute, bandwidth and memory numbers on a PS5 Pro |
| [`docs/int8-dot4.md`](docs/int8-dot4.md) | The INT8 dot-product experiment |
| [`docs/building.md`](docs/building.md) | How to build and run the probes yourself |
| [`probes/`](probes/) | Probe app source: C, GLSL compute shaders, build scripts, driver patch |
| [`issues/`](issues/) | The reports sent to ProsperoAI, Mihawk's PS5_Vulkan and psdevwiki |

This repo contains only our own code and measurements. It contains no Sony
code, no SDK files, no firmware, no game files and no binaries. To build the
probes you need ProsperoAI's public toolchain (see
[docs/building.md](docs/building.md)).

## Credits

- [ProsperoAI](https://github.com/blackbearreloaded/ProsperoAI) and
  [ps5-gpu-research](https://github.com/blackbearreloaded/ps5-gpu-research) by
  blackbearreloaded: the toolchain, the build recipe and the documentation this
  work stands on.
- Mihawk's [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan),
  [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) and
  [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK): the RADV Vulkan
  driver for the PS5 that the probes run on.
- [psdevwiki](https://www.psdevwiki.com/ps5/) for its param.json notes.

Research and testing by **obl1vn**.

## License

GPL-3.0-or-later, the same as ProsperoAI and PS5_Vulkan, which the probes
link with. See [LICENSE](LICENSE).
