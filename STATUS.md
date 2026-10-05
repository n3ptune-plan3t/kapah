# Implementation status

This file maps the roadmap (`niri-qt6-shell-roadmap.md`) to what actually exists
in the tree, and is honest about what does not. Update it at the end of every
phase, per roadmap section 10 rule 10 ("Leave the repo runnable").

Legend: **done** = implemented and reviewable · **partial** = interfaces and the
load-bearing logic exist, some leaf behaviour is missing · **todo** = not started.

## Decisions recorded (Phase 0.2)

| ADR | File | State |
|---|---|---|
| ADR-000 shell name (`kapah`) | `docs/decisions/ADR-000-shell-name.md` | done |
| ADR-001 process model (Model B) | `docs/decisions/ADR-001-process-model.md` | done |
| ADR-002 in-tree layer-shell bindings | `docs/decisions/ADR-002-layer-shell-bindings.md` | done |
| ADR-003 libpulse audio | `docs/decisions/ADR-003-audio-backend.md` | done |
| ADR-004 icons, no Qt6::Svg | `docs/decisions/ADR-004-icon-strategy.md` | done |
| ADR-005 TOML config | `docs/decisions/ADR-005-config-format.md` | done |
| ADR-006 icon cache shape (folded into ADR-004) | — | merged |
| ADR-007 backlight change detection | `src/services/backlight.cpp` header comment | done |
| ADR-008 NM secret agent deferred | see Phase 7.3 below | todo |
| ADR-009 night light via zwlr_gamma_control_v1 | see Phase 9.2 below | todo |
| ADR-010 Bluetooth stays a D-Bus client | see ADR-001 consequences | done |
| ADR-011 animation policy | see Phase 11 risk register below | todo |

## Phase 0 — foundations

| Item | Where | State |
|---|---|---|
| Repo skeleton, CMake (C++20, Qt6 only, LTO, gc-sections) | `CMakeLists.txt`, `cmake/` | done |
| CI banned-symbol gate | `ci/banned-symbols.txt`, `ci/check-banned-symbols.sh` | done |
| CI env-hardening gate | `ci/check-env-hardening.sh` | done |
| CI workflow (build / tidy / asan / fuzz / integration / perf) | `.github/workflows/ci.yml` | done |
| Logging (category, compile-time mask) | `src/core/logging.{h,cpp}` | done |
| TOML parser with line/column diagnostics | `src/core/toml.{h,cpp}` | done |
| Config schema, defaults, validation, hot reload, per-section diff | `src/core/config.{h,cpp}` | done |
| XDG paths, control socket path | `src/core/paths.{h,cpp}` | done |
| Env hardening, allocator tuning, `malloc_trim` | `src/core/env.{h,cpp}` | done |
| Perf harness: `rss.sh`, `wakeups.sh`, `startup.sh`, `perf-gate.sh`, `check_budgets.py` | `tools/perf/` | done |
| Reference machine profile | `tools/perf/reference-machine.md` | done |
| Empty-shell PSS baseline measured | `docs/PERF.md` | **todo — needs a run** |

## Phase 1 — Wayland spike + niri IPC

| Item | Where | State |
|---|---|---|
| Vendored protocol XML + `qt_generate_wayland_protocol_sources` | `src/wl/protocols/`, `src/wl/CMakeLists.txt` | done |
| Qt-version compat shim | `src/wl/qtcompat.h` | done |
| Capability detection (wl_registry listener, global→version map) | `src/wl/globals.{h,cpp}` | done |
| `zwlr_layer_shell_v1`: `LayerSurface`, `LayerShellWindow`, config registry, `QWaylandWindow` plugin | `src/wl/layersurface.{h,cpp}` | done |
| niri IPC wire types, transcribed from niri-ipc 26.4.0 | `src/niri/protocol.{h,cpp}` | done |
| Non-blocking socket, `QSocketNotifier`, two sockets (events + requests), reconnect backoff | `src/niri/ipcclient.{h,cpp}` | done |
| `NiriState` reducer with fine-grained signals | `src/niri/state.{h,cpp}` | done |
| Action sender (exact serde variant spellings) | `src/niri/actionsender.{h,cpp}` | done |
| Version-tolerance policy | `src/niri/protocol.cpp` (`VersionSupport`), `docs/PROTOCOLS.md` | done |
| Per-output bar widget | `src/components/bar/` | **todo** |
| Event-stream fixtures + parser unit tests | `tests/fixtures/niri/`, `tests/unit/` | **todo** |

## Phase 2 — theme + UI toolkit

| Item | Where | State |
|---|---|---|
| `Theme` (dark/light, scale, fonts, metrics, one `themeChanged`) | `src/ui/theme.{h,cpp}` | done |
| Painting helpers + documented anti-aliasing policy | `src/ui/paint.{h,cpp}` | done |
| `IconCache` (render-once, LRU 64, fallback table) | `src/ui/icons.{h,cpp}` | done |
| `Label`, `IconButton`, `Pill`, `Slider`, `Toggle`, `Separator`, `ScrollFrame` | `src/ui/widgets.{h,cpp}` | done |
| `Popup` (overlay + exclusive keyboard, no full-screen catcher, grace-period destroy) | `src/ui/popup.{h,cpp}` | done |
| Hand-painted month-grid `Calendar` | `src/ui/calendar.{h,cpp}` | done |
| `shell-gallery` binary | `src/gallery/` | **todo** |
| Headless golden-image tests | `tests/headless/` | **todo** |

## Phase 3 — the bar

| Item | Where | State |
|---|---|---|
| `PowerService` (UPower + udev/sysfs) | `src/services/power.{h,cpp}` | done |
| `NetworkService` (NM + netlink + sysfs merge) | `src/services/network.{h,cpp}` | done |
| `AudioService` (libpulse, fd-integrated mainloop) | `src/services/audio.{h,cpp}` | done |
| `BacklightService` (logind write, udev+inotify read) | `src/services/backlight.{h,cpp}` | done |
| `LogindService` (brightness, inhibitors, power, lid) | `src/services/logind.{h,cpp}` | done |
| `TrayService` (SNI watcher+host, dbusmenu lazily fetched) | `src/services/tray.{h,cpp}` | partial |
| `MprisService` | `src/services/mpris.{h,cpp}` | **todo** |
| `BluetoothService` | `src/services/bluetooth.{h,cpp}` | **todo** |
| `PowerProfilesService` | `src/services/powerprofiles.{h,cpp}` | **todo** |
| `Bar` + per-output instances | `src/components/bar/` | **todo** |
| Module widgets (workspaces, title, clock, battery, network, audio, backlight, tray, mpris, layout, session) | `src/components/bar/` | **todo** |

## Phase 4 — notification daemon

| Item | Where | State |
|---|---|---|
| `org.freedesktop.Notifications` server, markup sanitiser, popup stack, history, centre panel | `src/components/notifications/`, `src/services/notifydaemon.*` | **todo** |

## Phase 5 — launcher

`src/components/launcher/` — **todo** (desktop-entry parser, app index + disk cache
+ inotify, fuzzy matcher, frecency, virtualised UI, `posix_spawn` launcher,
calculator / window switcher / command runner modes).

## Phase 6 — OSD

`src/components/osd/` — **todo**.

## Phase 7 — quick settings

`src/components/quicksettings/` — **todo**. Network secret agent deferred
(ADR-008 still to be written).

## Phase 8 — lock, idle, power menu

| Item | Where | State |
|---|---|---|
| `ext-session-lock-v1` client, with the "never unlock without auth" invariant documented at the call site | `src/wl/sessionlock.{h,cpp}` | done |
| `ext-idle-notify-v1` client | `src/wl/idlenotify.{h,cpp}` | done |
| PAM worker, `LockView`, `kapah-lock` main | `src/lock/` | **todo** |
| `IdleService` (dim/lock/DPMS/suspend + inhibitors) | `src/app/idleservice.{h,cpp}` | **todo** |
| Power/session menu | `src/components/powermenu/` | **todo** |
| `docs/SECURITY.md` | `docs/` | **todo** |

## Section 8 — IPC and `kapahctl`

`src/app/ipcserver.{h,cpp}`, `src/ctl/` — **todo**. The protocol is already pinned
in `src/core/version.h` (`KAPAH_IPC_VERSION_MAJOR`).

## Phase 9 — extras

`src/components/wallpaper/` (background layer surface), night light
(`zwlr_gamma_control_v1`, verified present in niri 26.4), clipboard history
(`ext-data-control-v1`, also present), settings binary — all **todo**.

## Phase 10 — hardening, packaging, docs

| Item | Where | State |
|---|---|---|
| Soak / leak tests, fuzz harnesses | `tests/fuzz/`, `tests/integration/` | **todo** |
| i18n plumbing | — | **todo** (owner chose English-only at launch) |
| Install rules, desktop files, systemd units, niri config snippet, packaging recipes | `contrib/` | **todo** |
| `docs/ARCHITECTURE.md`, `PROTOCOLS.md`, `PERF.md`, `SECURITY.md`, `CONTRIBUTING.md` | `docs/` | **todo** |
| Man pages | `contrib/man/` | **todo** |

## Known risks carried forward

1. **Qt private API.** `src/wl/layersurface.cpp` uses `QtWaylandClient`'s private
   `QWaylandWindow` / `QWaylandWindowPlugin`, exactly as LayerShellQt does. This is
   the biggest maintenance risk in the tree and is why ADR-002 pins Qt 6.5-6.8.
   `src/wl/qtcompat.h` is the only file allowed to change when Qt moves.
2. **Unverified build.** Nothing in this tree has been compiled yet. Treat the
   first `cmake --build` as a review pass, not a formality; expect to fix
   signature mismatches against Qt 6.5's `qwaylandwindow_p.h`.
3. **Audio latency path.** `AudioService` optimistically applies volume and
   reconciles from the server callback. The OSD is only correct if the
   reconciliation does not double-fire; that is the Phase 6 acceptance test.
4. **Network scan complexity.** `NetworkService::requestScan` walks NM's managed
   objects. That is acceptable *because* it only runs while the wifi selector is
   open, but it is the most complex code in `src/services/`.
