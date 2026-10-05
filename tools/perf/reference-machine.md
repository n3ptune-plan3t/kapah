# Reference machine profile

Every number in `docs/PERF.md` is recorded against a machine described here.
A number without a machine is not a number.

## Tier 1 — the reference low-end laptop (the one that matters)

| | |
|---|---|
| CPU | Intel Celeron N4020, 2 cores / 2 threads, 1.10 GHz base, 2.8 GHz burst |
| RAM | 4 GB, single channel, DDR4-3200 |
| GPU | Intel UHD 600 **disabled**. No usable DRI driver: `LIBGL_ALWAYS_SOFTWARE=1`, `GALLIUM_DRIVER=llvmpipe` |
| Storage | eMMC, ~150 MB/s sequential |
| Kernel | 6.8 LTS, `PREEMPT_NONE`, no cgroup v2 memory.high on the shell |
| Qt | 6.5.3 (lts), software raster |
| niri | 25.11 (the oldest version we support) and 26.4 (current) |
| Compositor config | stock niri defaults, no animations, no gradients |

Why this machine specifically: dual-core at ~1.1 GHz means any accidental
per-frame work is immediately visible as a stutter in window switching, and 4 GB
means the shell is competing with the browser. A shell that is comfortable on a
Ryzen 9 is worthless on this.

Reproducing the software-rendering condition:

```sh
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe
export WLR_BACKENDS=headless      # for CI; drop on a real session
```

If llvmpipe is not installed, kapah still works: it never opens an EGL context
(see ADR-002 and `ci/check-banned-symbols.sh`), so GL is irrelevant to it.

## Tier 2 — developer workstation (sanity, not budgets)

| | |
|---|---|
| CPU | AMD Ryzen 7 5800X, 8 cores |
| RAM | 32 GB DDR4-3200 dual channel |
| Kernel | 6.11 |
| Qt | 6.8 |
| niri | 26.4 |

Numbers taken here are only ever reported alongside Tier 1 numbers. A change that
is free on Tier 2 and costs 6 MB on Tier 1 is a regression.

## Tier 3 — CI runner

GitHub-hosted `ubuntu-24.04`, 4 vCPU, 16 GB RAM. PSS numbers from CI are noisy
by ±3 MB and are never used for budgets; CI only runs the *regression* gate
against the recorded baseline (section 9), not the absolute budget.

## Methodology

* **Memory** is PSS from `/proc/<pid>/smaps_rollup`, sampled 30 s after the
  scenario settles. See `tools/perf/rss.sh`.
* **CPU** is the delta of `utime + stime` from `/proc/<pid>/status` over a 60 s
  window, plus `voluntary_ctxt_switches` as the wakeup ceiling. See
  `tools/perf/wakeups.sh`.
* **Startup** is fork-to-first-frame, measured externally by
  `tools/perf/startup.sh` against the `kapah: startup: ready` marker that
  `src/app/main.cpp` prints, cross-checked against the compositor's first
  `zwlr_layer_surface_v1.configure`.
* **Frame cost** comes from `LOG_PERF` paint-time lines, which are compiled into
  release builds but off unless `--perf-logging` or `KAPAH_PERF_LOGGING=1` is
  set.
* Every `PERF.md` entry records: CPU model, RAM, Qt version, niri version,
  kernel, and the exact command used.

## Why PSS and not RSS

`libQt6Widgets.so.6`, `libQt6Gui.so.6` and `libwayland-client.so.0` are mapped
into every process on the desktop. Reporting RSS makes a 3 MB change in the
launcher look like a 90 MB change because it perturbs how much of Qt is shared
with, say, the compositor's own client. PSS divides each shared page by the
number of processes mapping it, which is the number that answers "what does this
process cost the machine".