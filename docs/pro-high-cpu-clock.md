# High CPU clock in Pro memory mode: `attribute3` bit 27

## Summary

Bit 22 gives a PS5 Pro homebrew app the larger memory pool, but it also changes
the CPU/GPU power class the kernel puts the app in: from a high-CPU-frequency
class to "Default". **Bit 27 (`0x8000000`) asks for the high CPU frequency class
as a Pro app**, so with bits 22 and 27 together an app gets both the larger pool
and the high CPU clock.

| `attribute3` | Memory pool | Power class in the kernel log | `proc gen` |
| --- | --- | --- | --- |
| `0x40040` (bit 22 off) | base-PS5 size | Gen2 UB High CPU Frequency | 2 |
| `0x440040` (bit 22 on) | Pro size (+1.27 GB) | Default | 3 |
| **`0x8440040`** (bits 22 and 27) | **Pro size** | **High CPU Frequency** | 3 |

(`0x40040` is the high-frame-rate output declaration. It is in every row and did
not affect the class.)

For an app that uses the README's `5242880` (`0x500000`), adding bit 27 gives
**`139460608` (`0x8500000`)**.

## What it changes

Without bit 22 the console treats a homebrew app on a Pro as a base-PS5 game
and runs it in the "Ultra Boost" profile with the high CPU clock, but with the
smaller pool. With bit 22 alone the app gets the larger pool and loses that
clock. That is a real trade-off for CPU-bound apps: our app (an emulator) ran
about 8% faster with bit 22 off, at the cost of more texture evictions.

With bit 27 added the trade-off goes away:

| Our app, PS5 Pro, same build | Frames a second (mean) | Medians of three runs |
| --- | --- | --- |
| Bits 22 + 27 (High CPU Frequency) | 41.3 | 43.0, 42.0, 41.9 |
| Bit 22 only (Default) | 38.2 | 38.4, 38.8, 40.2 |

The memory pool stayed at the Pro size, texture evictions stayed about the same
(about 11.5k against 10.6k a run), and there were no crashes or GPU faults.
Four more runs of the packaged build gave 41.4, 41.9, 41.5 and 40.2 fps.

## Method

Each `attribute3` bit was set in turn on top of `0x440040` (31 variants), and
each `attribute2` bit on its own (32 variants). For each variant the app's
`sce_sys/param.json` was replaced over FTP, the app was launched and quit
through its own launcher (all 63 exited cleanly), and the power class was read
from the kernel log line `[BAPM] CPU/GPU Frequency`. Only bit 27 changed the
class.

## Limits

- **The kernel log names a power class, not a clock.** The frame-rate gain is
  the evidence that the clock is higher; we did not read the clock itself.
- **One console**, a PS5 Pro. Other firmware versions are not checked.
- **Base PS5: untested.** Retail Pro-enhanced games ship one package for both
  consoles, so the bit should be harmless there, but that is not verified.
- Not checked: power draw, fan noise or thermals in the high class.
