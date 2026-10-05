# ADR-001: Process model

Status: accepted
Date: 2026-10-03
Roadmap: section 2, section 14 (recommendation: Model B)

## Context

Three models were on the table:

* **A** — one process, many layer surfaces. Lowest total RAM. A crash takes the
  bar, the launcher, the notification daemon and the lock screen with it.
* **B** — `kapahd` (bar + notifications + launcher + OSD + quick settings +
  wallpaper) plus `kapah-lock` (separate) plus `kapahctl` (CLI).
* **C** — one process per component. Rejected in the roadmap: each process costs
  15-25 MB of Qt runtime before it draws anything, and the RAM budget is 70 MB.

## Decision

**Model B**, with the split drawn at the security boundary rather than at a
convenient one:

* `kapahd` — everything that is allowed to crash. One Qt runtime, one event loop,
  one config watcher, one niri IPC connection.
* `kapah-lock` — a separate, small process that speaks `ext-session-lock-v1`. It
  links Qt Core, Gui and Widgets but **not** the services layer, so it cannot
  grow into a second daemon. It holds the session lock.
* `kapahctl` — Qt Core + a socket client only, no GUI, no Widgets. Must start in
  under 5 ms.

`kapah-lock` is separate for two independent reasons, and either alone would
justify it:

1. **Security.** `ext-session-lock-v1` has the property that if the locking
   client is killed, the session stays locked. If the lock screen lived inside
   `kapahd`, a segfault in, say, the MPRIS parser would leave the session locked
   with no way to type a password — the user would have to switch VT.
2. **Footprint.** The lock screen needs almost nothing: a background colour, a
   clock, a password field, a PAM call. It does not need the notification daemon
   or the app index resident in the same address space.

## Consequences

* Total budget: `kapahd` ≤ 70 MB PSS idle with everything loaded, `kapah-lock`
  ≤ 25 MB PSS (it only exists while locked, so it does not count against the
  idle budget in roadmap section 0, but it must not blow up on a 4 GB machine
  when the lock screen is up alongside a browser).
* `kapahd` must tolerate being killed at any time and rebuild all state from the
  system on restart. Every service in `src/services/` therefore has a
  `state()`/`available()` contract rather than in-memory-only state that only
  exists between two signals.
* `kapah-lock` is spawned by `kapahd` via `posix_spawn`, not by systemd, because
  it must appear instantly when the session locks. It exits as soon as the lock
  is released, which is what keeps it out of the idle budget.
* Nothing else is a separate process. In particular Bluetooth stays in `kapahd`
  as a D-Bus client (see ADR-010) — `bluetoothd`/`bluez` are already a separate
  process, so a second one inside the shell would be pure duplication.
* Supervision: `kapah.service` with `Restart=on-failure`,
  `RestartSec=1`, and `Type=simple`. A desktop entry is provided for sessions
  without systemd.
