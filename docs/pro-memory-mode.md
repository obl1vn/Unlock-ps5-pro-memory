# PS5 Pro memory mode for homebrew: `attribute3` bit 22

## Summary

On a PS5 Pro (firmware 13.60), a homebrew app whose `sce_sys/param.json` has
bit 22 (`0x400000`) set in `attribute3` gets a larger direct-memory pool from the
kernel: **12.63 GiB instead of 11.44 GiB**. With Mihawk's RADV driver, the
Vulkan heaps grow by the same amount, and an app can allocate and use
**12.50 GiB** of GPU memory instead of 11.31 GiB.

Recommended value: `"attribute3": 5242880` (`0x500000` = bit 20 + bit 22).

## Background

- A PS5 game gets a fixed share of the console's 16 GB GDDR6. Sony quotes about
  12.5 GB for base PS5 games and about 13.7 GB for PS5 Pro games. The Pro frees
  some GDDR6 by moving system software into its extra 2 GB of DDR5.
- The kernel gives each app a "direct memory pool". Mihawk's RADV driver
  (`src/amd/vulkan/winsys/ps5/` in PS5_Mesa) reads it with
  `sceKernelGetDirectMemorySize()` and splits it into the Vulkan memory heaps.
  So the pool size is the most memory a Vulkan homebrew app can use.
- ProsperoAI's Vulkan log on a base PS5 shows 11712 MiB (11.44 GiB) of heap.
  Our probes showed exactly the same 11.44 GiB on a PS5 Pro. Without a flag, a
  homebrew app on a Pro gets base PS5 memory.
- Homebrew param.json files (ProsperoAI's included) use
  `"attribute3": 1048576` (`0x100000`, bit 20). ProsperoAI's docs describe bit
  20 as "VRR off".

## Method

The probe app (`probes/` in this repo) reports, in this order:

1. The kernel's numbers: `sceKernelGetDirectMemorySize()` (the pool) and
   `sceKernelAvailableDirectMemorySize()` (the largest free block).
2. The `attribute3`, `sdkVersion` and `mfsrVersion` values it was built with,
   read back from `/app0/sce_sys/param.json`, so every report shows which
   variant produced it.
3. The Vulkan heaps and the memory budget.
4. An allocation ceiling test: it allocates device memory in 256 MiB chunks
   (then 64 MiB) from every heap until the driver refuses, writes to it, prints
   the total, and frees everything. It can be turned off with an empty
   `skip-memory-test.txt` next to `eboot.bin`.

To find which setting matters, we compared the param.json of a PS5 Pro Enhanced
game with that of a game without Pro enhancements, and with our own. Only
`attribute3` values and version fields were compared. No game files are
included here. The Pro Enhanced game had:

- `attribute3` = `0x400010` (bits 4 and 22). The non-Pro game had neither bit.
- a `psml` block with `mfsrVersion` (PlayStation Spectral Super Resolution).
- a newer `sdkVersion` and `requiredSystemSoftwareVersion`.

`probes/build-variants.sh` builds four copies of the probe app that differ only
in those settings, each with its own title ID so they can be installed side by
side.

## Results (PS5 Pro, firmware 13.60)

| Build | Title | `attribute3` | Other changes | Pool | Ceiling | Side effects |
| --- | --- | --- | --- | --- | --- | --- |
| baseline | PPSA99077 | `0x100000` | none | 11.44 GiB | 11.31 GiB | none |
| A | PPSA99081 | `0x500000` | none | **12.63 GiB** | **12.50 GiB** | none |
| B | PPSA99082 | `0x400010` | `psml.mfsrVersion` | **12.63 GiB** | not run | TV re-syncs at launch |
| C | PPSA99083 | `0x100000` | newer SDK versions | 11.44 GiB | not run | none |
| D | PPSA99084 | `0x400010` | B + C | **12.63 GiB** | not run | TV re-syncs at launch |

Conclusions:

- **Bit 22 alone** gives the larger pool (A). The SDK version fields do nothing
  for memory (C). `psml` is not needed (A has none).
- **Bit 4** makes the console switch the TV's video mode when the app starts (a
  short black screen, as with some games' 120 Hz / VRR modes). It is not needed
  for memory.
- The allocation ceiling is 0.13 GiB below the pool in both modes. That gap is
  the driver's own bookkeeping and alignment.

### GiB vs GB

Sony's "13.7 GB" is in decimal gigabytes. 12.63 GiB × 1.0737 = **13.56 GB**,
and the ceiling 12.50 GiB = **13.42 GB**. The base pool 11.44 GiB = 12.28 GB,
which matches Sony's "12.5 GB" for base games in the same way. The remaining
~0.1–0.15 GB are probably the app's own code, stacks and system allocations
that don't come from the GPU pool.

## Is this new?

As far as we could find, nobody had published that this bit changes the memory a
homebrew app gets. psdevwiki's param.json notes list `4194368` (`0x400040`,
which includes bit 22) with the comment "HFR + PS5 Pro ?". So the bit was
already linked to the PS5 Pro, but its memory effect was not documented, and
homebrew param.json files don't set it. If you know of an earlier report,
please open an issue and we'll credit it.

## Open questions

- **Other firmware versions.** Only 13.60 is tested.
- **Base PS5.** Untested. It probably changes nothing (there is no extra memory
  to give), but it could also be refused at launch.
- **What else bit 22 changes.** It may switch on other Pro-mode settings
  besides memory. Our compute probes show all 60 compute units running homebrew
  work without the bit, so it isn't needed for the GPU. We have not compared
  clock speeds between the two modes.
- **The other DDR5 memory.** The Pro's extra 2 GB of DDR5 is used by the system.
  We found no way for an app to allocate it directly.

## How to reproduce

See [building.md](building.md). In short:

```bash
PROSPERO_ROOT=~/ProsperoAI bash probes/build-variants.sh
```

Upload the four folders to `/data/homebrew/`, register them, launch each, and
open `http://<PS5-IP>:9050/` in a browser. The first lines of each report show
the pool and the `attribute3` value.
