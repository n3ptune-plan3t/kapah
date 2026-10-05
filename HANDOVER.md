# HANDOVER.md — how to continue this project

Read this first, then `niri-qt6-shell-roadmap.md` sections 0-3, then
`docs/decisions/`.

This file is written by an agent that ran out of budget mid-implementation. It
records (a) what is already in the tree and what its contracts are, so you do not
have to re-derive them, (b) the conventions every existing file follows so your
new files match, and (c) the exact remaining work, in dependency order.

Also read `STATUS.md`, which is the per-phase table. This file is the *how*.

---

## 0. Hard rules for any agent picking this up

These are not style preferences; they come from the roadmap's section 0 and are
what the whole design is defending.

1. **No `repaint()`.** Only `update()` and `update(rect)`. `repaint()` is
   synchronous and will show up as a latency spike in `tools/perf/`.
2. **No `QTimer` with a repeating interval.** The only permitted repeating-ish
   construct is `RetryPolicy` in `src/services/service.h`, which is single-shot
   with exponential backoff capped at 5 s and exists solely to recover from a
   daemon restart. The clock is a single-shot re-armed to the next minute
   boundary with `Qt::VeryCoarseTimer`.
3. **No polling, ever.** File descriptors go through `QSocketNotifier`. D-Bus
   calls are `QDBusPendingCallWatcher`. If you need a change notification from
   sysfs, use udev (see the ADR-007 comment at the top of
   `src/services/backlight.cpp` for the measurements behind that decision).
4. **No new dependency without an ADR** in `docs/decisions/`, stating RSS,
   startup cost, what it replaces, and distro availability.
5. **`ci/check-banned-symbols.sh` must stay green.** It rejects `QOpenGL*`,
   `QQuick*`, `QQml*`, `QtWebEngine`, `QtMultimedia`, `QtSvg` and any `.qml` or
   `.ui` file under `src/` or `tests/`. If you feel you need one of those, write
   a measured justification and get owner approval first.
6. **Every service gets `available()`, a `state()` snapshot and a `changed()`
   signal**, and every one of those must survive its daemon disappearing. See
   `src/services/service.h` for the contract and `RetryPolicy` for the recovery
   mechanism.
7. **Everything is lazy-destroyed.** Popups and the launcher destroy their
   surface after a grace period (`Popup::setDestroyGraceMs`,
   `[launcher] destroy-grace`). "Zero surface cost at idle" is an acceptance
   criterion, not a nicety.
8. **Report PSS and wakeups with every change.** `tools/perf/rss.sh` and
   `tools/perf/wakeups.sh`. Budgets are in the roadmap section 0.

---

## 1. Decisions already made (do not re-open without cause)

Owner answered roadmap section 14 on 2026-10-03:

| Question | Answer | Where |
|---|---|---|
| Shell name | `kapah` → `kapahd`, `kapah-lock`, `kapahctl`, `kapah-gallery`, `kapah-settings` | `docs/decisions/ADR-000-shell-name.md` |
| Layer shell | **in-tree generated bindings**, no LayerShellQt | `ADR-002-layer-shell-bindings.md` |
| Audio | **libpulse** (works over pipewire-pulse) | `ADR-003-audio-backend.md` |
| Network | **NetworkManager + netlink fallback** (no iwd/ConnMan at launch) | `ADR-005`, `src/services/network.h` |
| Visual | flat rounded (6-12 px), dark **and** light, Qt ≥ 6.5, niri 25.x, English-only at launch | `ADR-004`, `src/ui/theme.cpp` |

Consequences worth knowing before you write code:

* **Qt is pinned to 6.5-6.8.** `src/wl/layersurface.cpp` uses
  `QtWaylandClient`'s *private* client API (`QWaylandWindow`,
  `QWaylandWindowPlugin`, `QWaylandShellSurface`, `QWaylandScreen`,
  `QWaylandIntegration::Capability::LayerShell`, `QWaylandWindow::registerPlugin`).
  This is exactly what LayerShellQt does; there is no other way to make a
  `QWindow` become a layer surface. `src/wl/qtcompat.h` is the **only** file
  allowed to change when Qt's private headers move. Read its header comment
  before touching it.
* **No `Qt6::Svg`.** Icons resolve through `QIcon::fromTheme` but are rasterised
  once at exactly (size, dpr) and the `QIcon` is dropped immediately — a retained
  `QIcon` keeps every size variant the file provides. See `ADR-004` for the
  fallback-name table, which lives in `src/ui/icons.cpp`.
* **`wmww/LayerShellQt` and `gitlab.freedesktop.org` are not cloneable from this
  environment.** The protocol XMLs were vendored from
  `github.com/swaywm/wlr-protocols` and `gitlab.freedesktop.org/wayland/wayland-protocols`
  (a redirect to gitlab.freedesktop.org works for `wayland/wayland-protocols` but
  *not* for `wlroots/wlr-protocols` — use the swaywm mirror). `tools/update-protocols.sh`
  does not exist yet; write it if you need to re-sync.

---

## 2. Verified external facts (do not re-research, but do re-check if in doubt)

Checked on 2026-10-03 against `github.com/YaLTeR/niri` at commit `ed22699d9`
(version **26.4.0**). The IPC schema in `src/niri/protocol.h` is transcribed from
`niri-ipc/src/lib.rs` at that commit and is authoritative for this tree.

**Request enum** (all unit variants unless noted): `Version`, `Outputs`,
`Workspaces`, `Windows`, `Layers`, `KeyboardLayouts`, `FocusedOutput`,
`FocusedWindow`, `PickWindow`, `PickColor`, `Action(Action)`, `Output{name,
action}`, `EventStream`, `ReturnError`, `OverviewState`, `Casts`.

**Reply** is `Result<Response, String>` → `{"Ok": …}` / `{"Err": "…"}`.
**Response**: `Handled`, `Version(String)`, `Outputs(HashMap)`, `Workspaces([])`,
`Windows([])`, `Layers([])`, `KeyboardLayouts`, `FocusedOutput(Option)`,
`FocusedWindow(Option)`, `PickedWindow`, `PickedColor`, `OutputConfigChanged`,
`OverviewState`, `Casts([])`.

**Events** (externally tagged, one per line after the `EventStream` handshake):
`WorkspacesChanged`, `WorkspaceUrgencyChanged`, `WorkspaceActivated`,
`WorkspaceActiveWindowChanged`, `WindowsChanged`, `WindowOpenedOrChanged`,
`WindowClosed`, `WindowFocusChanged`, `WindowFocusTimestampChanged`,
`WindowUrgencyChanged`, `WindowLayoutsChanged` (payload `changes` is an array of
`[id, layout]` **tuples**, not objects — handled in `Event::fromJson`),
`KeyboardLayoutsChanged`, `KeyboardLayoutSwitched`, `OverviewOpenedOrClosed`,
`ConfigLoaded`, `ScreenshotCaptured`, `CastsChanged`, `CastStartedOrChanged`,
`CastStopped`.

**Critical transport fact** (from `niri-ipc/src/lib.rs` doc comment): after
`Request::EventStream`, niri **stops reading requests on that socket**. Two
sockets are mandatory. `IpcClient` owns both.

**Window struct gotchas** that are easy to get wrong and are handled in
`protocol.cpp`: `title`/`app_id`/`pid`/`workspace_id` are all *optional*;
`workspace_id` is `Option<u64>`; `physical_size` is a `(u32, u32)` tuple;
`layout.tile_size` is `(f64, f64)`; `pos_in_scrolling_layout` is `Option<(usize,
usize)>` with **1-based** indices; `layout.transform` is an externally-tagged
unit enum serialising as the strings `"Normal" | "90" | "180" | "270" |
"Flipped" | "Flipped90" | …`.

**Actions**: 141 variants. `src/niri/protocol.h` has an `ActionId` enum covering
the subset kapah sends; `kActionNames` in `protocol.cpp` maps each to its exact
serde spelling (load-bearing — serde will not deserialise a mismatch). Actions
that take `id: Option<u64>` are listed in `hasOptionalId()`; when there is no
explicit target you must serialise `"id": null` so niri means "the focused one",
not `"id": 0`.

**Wayland protocols niri 26.4 actually implements** (checked in
`niri/src/niri.rs` and `niri/src/protocols/`):

| Protocol | Present | Note |
|---|---|---|
| `wlr-layer-shell-unstable-v1` | yes | via smithay; we bind v4 |
| `ext-session-lock-v1` | yes | `SmithaySessionLock` |
| `ext-idle-notify-v1` | yes | `IdleNotifierState` |
| `zwlr_gamma_control_manager_v1` | yes | `src/protocols/gamma_control.rs` — **so the Phase 9.2 night light IS feasible**; it is in niri's own protocols dir |
| `ext-data-control-v1` **and** `wlr-data-control-v1` | both | `niri.rs` holds both states |
| `ext-workspace-v1` | yes | unused so far |
| `ext-foreign-toplevel-list` | yes | fallback only; IPC is primary |
| `ext-image-copy-capture`, `ext-output-management`, `zwlr-virtual-pointer-v1` | yes | unused |

XMLs vendored in `src/wl/protocols/`: layer-shell, session-lock, idle-notify,
data-control, gamma-control, output-power-management.

**Qt symbols relied upon** — listed at the top of `src/wl/layersurface.h`. If the
first build fails there, that list is your debugging checklist.

---

## 3. Conventions every existing file follows

Match these; a reviewer will check.

### File headers

Every `.h` starts with a comment block saying *why* the file exists and which
roadmap section/phase it serves. Several include the measurements or the rejected
alternatives. Keep that density — it is the main defence against the next agent
"simplifying" a decision that was expensive.

### Logging

```cpp
LOG_INFO(Core, "text");                    // literal, no format
LOG_WARNF(Network, "no backend: %s", qPrintable(why));
LOG_CAT(Wayland, "only in debug builds");
```

`KAPAH_DEBUG_BUILD` is 0 in Release, so `LOG_CAT` and `LOG_INFOF` compile to
nothing there. `LOG_WARN*` is always compiled in — it is the only thing that
survives a stripped build. The macros are defined at the bottom of
`src/core/logging.h`; use them, never `qDebug()`/`qWarning()`.

Categories are a bitmask in `log::Category`. Add a new one there rather than
using `Core` for everything.

### Ownership and construction

Widgets and services take `Theme*` and/or `IconCache*` as constructor
arguments. There are no singletons in the UI layer (the *process-level* state
that genuinely is global — the Qt app, the niri client, the config watcher — is
owned by `src/app/daemon.{h,cpp}`, which does not exist yet).

### Painting

Every pixel goes through a helper in `src/ui/paint.h`. Never call
`QPainter::setRenderHint(QPainter::Antialiasing, true)` ad hoc; the helpers do
it and restore it, because the policy differs per primitive (rounded corners: AA;
1 px separators and flat fills: no AA — see the header comment).

`Theme::px(int)` scales and rounds a logical metric. `Theme::font(role)` with
roles 0=small 1=regular 2=medium 3=bold 4=mono.

### Configuration

Never read `Config` fields ad hoc in a component. Register a callback:

```cpp
m_configWatcher->onBarChanged([this] { rebuildFromConfig(); });
```

`ConfigWatcher` diffs per section and fires only what changed, which is the
mechanism behind "never rebuild everything". New sections need a new
`on*Changed` in both `config.h` and `config.cpp`.

### Wayland surfaces

Never touch `QWaylandWindow` outside `src/wl/`. Components use `LayerShellWindow`
(for bar/overlay windows) or `Popup` (for popups) and never think about anchors,
exclusive zones or namespaces. `Popup::makeConfig()` gives you a correct overlay
surface in one line.

### D-Bus

No `QDBusReply` blocking calls except the two documented exceptions
(`NetworkService::wifiDevicePath`, reached only from an open panel). Everything
else is `QDBusConnection::asyncCall` + `QDBusPendingCallWatcher`. Wrap every
call in a lambda that `deleteLater()`s the watcher.

### Tests

`tests/unit/` uses QtTest, one executable per subject, no GUI needed
(`QTEST_GUILESS_MAIN`). `tests/headless/` sets `QT_QPA_PLATFORM=offscreen` and
renders to a `QImage` for golden comparison. `tests/integration/` drives a real
nested niri. `tests/fuzz/` has libFuzzer harnesses, one per parser
(`fuzz_toml`, `fuzz_niri_events`, `fuzz_desktop_entry`, `fuzz_notification_markup`,
`fuzz_dbusmenu_layout`).

---

## 4. What exists, with its contract

### `src/core/` — no Qt GUI, no external libs

| File | Contract |
|---|---|
| `logging.{h,cpp}` | `kapah::log` |
| `toml.{h,cpp}` | `kapah::toml::parse(QString) -> Document{root, diagnostics}`; `Diagnostic` has line **and** column; accessors are total (wrong type → the fallback you passed) |
| `config.{h,cpp}` | `kapah::Config` (14 sections), `ConfigLoadResult`, `ConfigWatcher`. `Config::defaults()` must produce a fully working shell with no file |
| `paths.{h,cpp}` | `paths::configFile()`, `controlSocket()`, `stateDir()`, `cacheDir()`, `userName()`, `logindSessionId()`, `desktopBackground()`. All cached in function-local statics |
| `env.{h,cpp}` | `kapah::hardenEnvironment()` — **call first in every `main.cpp`**, before the app object. `tuneAllocator()`, `releaseFreedMemory()` |
| `util.{h,cpp}` | `fold`, `isSubsequence`, `elide`, `readSysfs`, `writeSysfs`, `logindSessionPath` |
| `version.h.in` | configured into `${CMAKE_BINARY_DIR}/generated/kapah/version.h`; **included as `"kapah/version.h"`** |

### `src/wl/` — Qt Gui only

| File | Contract |
|---|---|
| `qtcompat.h` | includes every Qt private wayland header + `wlcompat::layerShellAvailable()`, `wlcompat::outputForScreen(QScreen*)` |
| `globals.{h,cpp}` | `Globals::start()` installs the `wl_registry` listener; `globalVersions()` / `globalId(name)` / `hasGlobal(name, min)` / `protocolRequirements()` / `outputNames()`. **Every optional feature must ask this first** |
| `layersurface.{h,cpp}` | `Layer`, `KeyboardInteractivity`, `AnchorFlag`, `LayerConfig`, `LayerSurface`, `LayerShellWindow`, `installLayerShellPlugin()`, `layerShellAvailable()`, `queryCapabilities()` |
| `idlenotify.{h,cpp}` | `IdleNotifier` — `start()`, `available()`, `idled()`/`resumed()`. Uses v2 `get_input_idle_notification` when advertised |
| `sessionlock.{h,cpp}` | `SessionLock` + `LockSurface`. **`unlockAndDestroy()` is private and has exactly one legal call site** (see the file header; docs/SECURITY.md item S2 does not exist yet — write it) |

The `LayerShellWindow` → `LayerConfig` mechanism has two paths (QWindow-pointer
map, FIFO fallback) because `wlr-layer-shell` forbids changing
anchor/layer/exclusive-zone after the first configure, so the config must be
reachable from `WaylandWindow::initWindow()`. Read the `LayerShellRegistry`
comment before changing anything here.

### `src/niri/` — Qt Core + JSON only, headless-testable

| File | Contract |
|---|---|
| `protocol.{h,cpp}` | `RequestKind`, `Reply`, `Output`, `WindowInfo`, `WorkspaceInfo`, `KeyboardLayouts`, `LayerSurfaceInfo`, `CastInfo`, `Event`, `ActionId`, `buildAction()`, `VersionSupport` |
| `ipcclient.{h,cpp}` | `Connection` (one socket), `RetryingConnection` (backoff ≤ 5 s), `IpcClient` (owns both sockets, `eventReceived(Event)`, `send`/`sendWithReply`/`action`/`actionWithReply`, `versionReceived(QString)`) |
| `state.{h,cpp}` | `NiriState` — attach to an `IpcClient`, then read. Fine-grained signals; `apply(const Event&)` is public so fixtures can drive it |
| `actionsender.{h,cpp}` | typed `focusWorkspace/closeWindow/focusMonitor/...` + `withReply()` for the few places where the result changes the UI |

### `src/ui/` — Qt Widgets

| File | Contract |
|---|---|
| `theme.{h,cpp}` | `Theme` singleton-ish, `apply(const Config&)`, `themeChanged()`. `palette::` names are the only colours a widget should name |
| `paint.{h,cpp}` | `fillRounded`, `strokeRounded`, `fillRoundedPartial`, `separatorLine`, `drawLevelBar`, `drawBattery`, `drawSignalBars`, `drawChevron`, `paintFlatShadow`, `elideText`, `lighten`/`darken`/`withAlpha`/`blend`, `roundedPath` (both overloads) |
| `icons.{h,cpp}` | `IconCache` (LRU 64), `IconKey`, and the semantic helpers `iconNameForVolume/Network/Battery/Bluetooth/...` |
| `widgets.{h,cpp}` | `Label`, `IconButton`, `Pill`, `Slider`, `Toggle`, `Separator`, `ScrollFrame` |
| `popup.{h,cpp}` | `Popup` base: `popup(screen)`, `dismiss()`, `setAnchorWidget()`, `setPlacement()`, `setDestroyGraceMs()`, `resizeToContent()`, `dismissRequested()` |
| `calendar.{h,cpp}` | month grid, no `QCalendarWidget` |

### `src/services/` — Qt DBus + libudev + libpulse

| File | Contract |
|---|---|
| `service.{h,cpp}` | `RetryPolicy`, `ServiceNameWatcher` |
| `logind.{h,cpp}` | brightness write, `Inhibitor` (RAII fd), suspend/hibernate/poweroff/reboot, `canHibernate`, `lockSession`, `PrepareForSleep` |
| `power.{h,cpp}` | `BatteryInfo`, `takeLowNotification()`, `takeCriticalNotification()` (once-per-discharge latches) |
| `network.{h,cpp}` | `NetworkService`; `requestScan()`/`activate()` only while a panel is open; `activate()` on an unknown SSID returns the ADR-008 message |
| `audio.{h,cpp}` | `AudioService`; `nudgeVolume()`, `setVolumePercent()`, `toggleMute()`, `sinks()`, `setDefaultSink()`, `volumeChangedByUser` (fires the OSD once per gesture) |
| `backlight.{h,cpp}` | `BacklightService`; writes via logind, falls back to sysfs, reports `writeMethod()` |
| `tray.{h,cpp}` | `TrayService` — watcher **and** host, `contextMenuRequested()` signal, `fetchMenu()` is lazy. **partial**: see known issues |
| `mpris`, `bluetooth`, `powerprofiles` | **do not exist yet** but are listed in `src/services/CMakeLists.txt` — create them or the build breaks |

---

## 5. Known issues in what is already written

Fix these before building on top. They are not hypothetical; the previous agent
ran out of time mid-review.

1. **`src/services/CMakeLists.txt` references `mpris.cpp`, `bluetooth.cpp`,
   `powerprofiles.cpp`** which do not exist. Either write them or guard the
   `target_sources` calls. This will fail at configure time today.
2. **`src/services/tray.cpp` `TrayService::activate`** — the
   `DoubleClick` branch currently builds a `createMethodCall` with the *same*
   method name in both arms. The SNI spec has only `Activate(x, y)`, so drop the
   double-click special case and map it to `Activate(4, 0,0)` or reject it.
3. **`TrayService::activate`** does a **blocking** `QDBus::Block` call on the
   click path, and another for `Activate`. Move both to
   `QDBusPendingCallWatcher`. (`showContextMenu` also blocks twice; those are
   tolerable because the user has just clicked, but the `activate` ones are on a
   wheel-and-click path and should be async.)
4. **`src/ui/popup.cpp` `computePlacementRect`** — the `BelowAnchor` and
   `AboveAnchor` cases contain leftover `Q_UNUSED(g)` / dead `QPoint c`
   assignments and the anchor math is wrong. Rewrite them properly:
   `anchorBottom = anchor->mapToGlobal(QPoint(0, anchor->height())).y()`,
   `anchorLeft = anchor->mapToGlobal(QPoint(anchor->width()/2, 0)).x() - content.width()/2`.
5. **`src/services/network.cpp` `refreshActiveConnection`** reads `ip4` and
   `devicePath` and then does nothing useful with them; the SSID is actually
   recovered from the device list. Simplify it, and set `m_activeApPath` from the
   wifi device's `ActiveAccessPoint` so `AccessPoint::active` ever becomes true.
6. **`src/services/audio.cpp` `onSinkInfo`** computes `isDefault` as
   `(info->index == PA_INVALID_INDEX ? false : true) && m_defaultSink == info->index`,
   which is a convoluted way of writing `m_defaultSink == info->index` and is
   wrong before `onServerInfo` has ever run. Also `setOptimisticVolume` is called
   from inside a libpulse callback while the mainloop lock is held — check for
   re-entrancy (it only touches Qt fields, so it is safe, but say so in a comment).
7. **`src/services/audio.cpp` `AudioService::start`** spins up to 200 times with
   a 5 ms sleep. That is a bounded wait, not polling, but it can cost up to 1 s of
   startup latency on a machine with a slow pipewire-pulse. Consider moving the
   context creation into a single-shot timer so the bar paints first.
8. **`src/services/logind.cpp` `onPropertiesChanged`** has a dead
   `static bool signalsConnected` guard and a `QObject *m = m_watcher;
   Q_UNUSED(m);`. Connect the logind signals **once, in `start()`**, not lazily
   from a property callback.
9. **`src/services/power.cpp`** declares `m_upowerWatcher`, `m_propWatcher` and
   `m_upowerSeen` that are never used. Delete them.
10. **`src/services/service.cpp` `ServiceNameWatcher`** has a stray
    `connect(&m_retry, &QTimer::timeout, parent, [] {})`. Delete it. Also the
    `isServiceRegistered` call in `update()` is a **blocking** `QDBusConnection`
    property access on every retry tick — replace with `QDBusServiceWatcher`'s
    own cached view (`QDBusConnection::sessionBus().interface()->isServiceRegistered`
    is in-process and does not block, but confirm that in your Qt version and
    document it).
11. **`src/core/config.cpp`** — `QDir` is included but unused; `parseStringMap`'s
    `where` parameter is only used on one branch. Cosmetic, but do not leave
    unused includes in a `-Werror` CI.
12. **Nothing in the tree has been compiled.** There is no compiler on the
    machine that produced it. Expect the first `cmake --build` to surface
    signature mismatches against Qt 6.5's `qwaylandwindow_p.h`; work from the
    symbol list in `src/wl/layersurface.h`.

---

## 6. Remaining work, in dependency order

Each step lists the files to create and what "done" means. Do them in order —
later steps assume earlier ones.

### Step 1 — `src/wl/sessionlock` consumers + `src/lock/` (Phase 8, part 1)

Do this before the lock service is reachable from the daemon.

* `src/lock/main.cpp` — `hardenEnvironment()`, `tuneAllocator()`, `QApplication`,
  parse `--password-timeout-ms`, `SessionLock::begin()`, then `exec()`.
  Never connect this binary's lifetime to `kapahd`'s.
* `src/lock/pam.{h,cpp}` — `PamAuthenticator` on **one dedicated `QThread`**,
  `pam_start("kapah", user, &conv, &handle)`, `pam_authenticate()`,
  `pam_acct_mgmt()`, `pam_end()` in the destructor. `explicit_bzero` the password
  buffer immediately after the call, and again in the destructor. Signals:
  `result(bool ok, int pamError)`.
* `src/lock/passwordfield.{h,cpp}` — custom-painted, `Qt::NoEcho`-equivalent
  behaviour implemented by hand (do not use `QLineEdit` — it caches the string in
  an undo stack you cannot clear). Bullet characters are fine, they are not a
  secret.
* `src/lock/lockview.{h,cpp}` — **the security-critical file.** Paints a fully
  opaque frame to an `ARGB8888` shm buffer, then
  `LockSurface::attachOpaqueBufferAndAcknowledge(...)`. Only
  `attachOpaqueBufferAndAcknowledge` on the *first* configure; afterwards use
  `updateOpaqueBuffer` (no re-ack). Show the clock with the same minute-aligned
  single-shot timer discipline as the bar. Show keyboard layout and caps-lock by
  **asking niri over IPC** (`KeyboardLayoutsChanged`) — do not read evdev and do
  not poll.
  **The unlock path must be exactly:** `pam.result(true) → SessionLock::unlockAndDestroy() → exit`.
  No timeout, no "close window", no error branch that unlocks.
* Wire `prepareForSleep` / lid-close in `kapahd` to spawn `kapah-lock` before
  suspending, and take a `logind` delay inhibitor while it comes up (roadmap
  Phase 8.4).
* Write `docs/SECURITY.md` with the checklist the roadmap asks for and have a
  second agent review `src/lock/` and `src/wl/sessionlock.cpp` against it.

### Step 2 — `src/components/bar/` (Phase 3)

* `bar.{h,cpp}` — `BarItem` per output plus a `Bar` that owns them.
  `QGuiApplication::screenAdded/screenRemoved` (and `Globals::outputsChanged`)
  create/destroy exactly one `LayerShellWindow` per screen. Per-output config via
  `BarConfig::forOutput(name)`. Visibility modes: `Always`, `AutoHide` (hide when
  a window is fullscreen on that output — niri does not expose fullscreen state
  directly, so derive it from `WindowInfo::layout.tile_pos_in_workspace_view`
  being absent/at 0,0 with tile size == output size, and document the heuristic),
  `Hidden`.
* `modules.{h,cpp}` — `BarModule : QWidget` interface with
  `configure(const ModuleConfig&)`, `preferredWidth()`, `invalidate()`.
  Factory function mapping `BarModuleId` → constructor.
* Each module in its own file: `workspaceitem`, `windowtitle`, `clock`,
  `batteryitem`, `networkitem`, `audioitem`, `backlightitem`, `trayitem`,
  `mprisitem`, `layoutitem`, `sessionitem`.
  * **Clock**: minute-aligned single-shot `QTimer` with
    `Qt::VeryCoarseTimer`, re-armed to the next minute boundary
    (`msToNextMinute()`), plus a `Calendar` popup. Hover tooltip created on
    hover and destroyed on hide (roadmap 3, cross-cutting).
  * **Battery**: threshold crossings come from
    `PowerService::takeLowNotification()` / `takeCriticalNotification()` and are
    forwarded to the notification daemon exactly once.
  * **Audio**: wheel adjusts via `nudgeVolume`, click toggles mute, right-click
    opens the sink selector popup. Never enumerate sinks outside the popup.
  * **Backlight**: wheel adjusts via `setPercent`.
  * **Workspaces**: `Pill` in indicator-only mode, click to focus,
    scroll to switch with a ≤100 ms debounce single-shot.
  * **Window title**: `Label` from `NiriState::focusedWindow()`.
* Bar acceptance to verify: < 40 MB PSS, 0 wakeups/s idle except the minute tick,
  changes reflect within 200 ms, killing NM/PipeWire/UPower leaves the bar alive.

### Step 3 — `src/app/` (the missing composition root)

Nothing works until this exists.

* `daemon.{h,cpp}` — owns everything: `QApplication`, `ConfigWatcher`, `Globals`,
  `installLayerShellPlugin()`, `IpcClient`, `NiriState`, `ActionSender`, all the
  services, `Bar`, the popups, `IdleService`, `IpcServer`, and the lock spawner.
  Construction order matters: config → globals → plugin → niri → services → UI.
  Destruction order is the reverse.
* `main.cpp` — `hardenEnvironment()`, `tuneAllocator()`, `raiseSchedulingPriority()`,
  parse args (`--config`, `--log`, `--perf-logging`, `--check`, `--version`),
  construct `Daemon`, `app.exec()`. Emit the startup markers
  `kapah: startup: env_ms=…`, `config_ms=…`, `niri_ms=…`, `bar_ms=…`,
  `kapah: startup: ready` — `tools/perf/startup.sh` greps for the last one.
* `ipcserver.{h,cpp}` — roadmap section 8. Unix socket at
  `paths::controlSocket()`, newline-delimited JSON, `QLocalServer`. Commands
  exactly as listed in roadmap section 8; the version guard uses
  `KAPAH_IPC_VERSION_MAJOR`.
* `idleservice.{h,cpp}` — `IdleNotifier` → one single-shot per step
  (dim, lock, DPMS off via `ActionSender::powerOffMonitors()`, suspend via
  `LogindService::suspend()`). Re-arm all on `resumed()`. Inhibitors from
  `[idle]` config, `LogindService::takeInhibitor` held as `LogindService::Inhibitor`
  members so their lifetime is the scope.
* `src/ctl/` — `kapahctl`. Qt Core + socket only, **no Widgets**. Must start in
  < 5 ms: parse argv by hand, do not construct a `QGuiApplication`, and prefer
  a raw `socket()` + `connect()` over `QLocalSocket` so no event loop is needed.

### Step 4 — Phase 4, notifications

* `src/services/notifydaemon.{h,cpp}` — own `org.freedesktop.Notifications` via
  a `QDBusAbstractAdaptor` with `Q_CLASSINFO("D-Bus Interface", …)`.
  `Notify` **must** return the id synchronously (it is `uint32`, not async), so
  parse the notification synchronously and only do the expensive work (image
  decode) lazily. Support: `replaces_id`, `expire_timeout`, urgency hints,
  actions, body markup subset (`<b> <i> <u> <a href>`), `image-data`,
  `image-path`, `app_icon`, `category`, `desktop-entry`, `transient`, `resident`,
  `x-canonical-private-synchronous`, `synchronous`.
  On `name-has-no-owner`, log clearly and exit with a distinct code — do **not**
  steal the name.
* `markup.{h,cpp}` — sanitiser. Whitelist approach: parse a tiny subset and
  re-emit escaped text. Never pass through unknown tags. Strip `<script>`,
  `<style>`, event handlers, `javascript:` and `data:` URLs. Cap entity
  expansion to prevent billion-laughs. **This file gets a second-agent review
  against `docs/SECURITY.md`.**
* `src/components/notifications/` — `stack.{h,cpp}` (ONE overlay surface sized
  to the stack, anchored per config, `keyboard_interactivity = none`, resized as
  items change, **destroyed when nothing is visible**), `popup.{h,cpp}`,
  `history.{h,cpp}` (ring buffer, cap 50, bodies truncated, images ≤ 64 px,
  optional lazy persist), `panel.{h,cpp}` (virtualised centre list).
* Rate limiting: per-app-per-minute counter (cap 20), burst collapsing by
  app+summary within `[notifications] burst-window`.

### Step 5 — Phase 5, launcher

* `desktopentry.{h,cpp}` — custom minimal parser. **Not `QSettings`.** Handle
  `Name`/`GenericName`/`Keywords`/`Comment` with locale suffixes, `Exec` with
  `%f %u %U %F %i %c %k %v %m`, `TryExec`, `Terminal`, `DBusActivatable`,
  `NoDisplay`, `Hidden`, `OnlyShowIn`, `NotShowIn`, `Categories`, `Actions`.
  Tolerate missing/unknown keys (roadmap 4.4).
* `appindex.{h,cpp}` — scan `$XDG_DATA_DIRS/applications`, intern strings,
  on-disk cache keyed by directory mtimes, **inotify** on each directory for
  incremental refresh. Target < 3 MB resident for 500 apps.
* `fuzzy.{h,cpp}` — subsequence + prefix + word-boundary scoring over
  pre-lowercased strings. **< 5 ms per keystroke for 1000 apps**; benchmark it in
  a unit test with `QElapsedTimer` and assert the budget.
* `frecency.{h,cpp}` — tiny file, cap 256.
* `launcher.{h,cpp}` — one overlay surface, `keyboard_interactivity = exclusive`,
  `ScrollFrame` + hand-painted rows (paint only visible rows), lazy icons,
  full keyboard nav (up/down/enter/escape/tab, ctrl+n/p), destroyed after
  `[launcher] destroy-grace` with the index kept.
* `spawner.{h,cpp}` — `posix_spawn` (or `systemd-run --user --scope` per
  `[launcher] launch-method`). Honour `Terminal=true`.
* `modes/` — calculator (recursive-descent parser over a real grammar, **no JS**),
  window switcher (from `NiriState::windowsByRecency()`), command runner (`>`
  prefix, parse argv correctly — no shell), clipboard and emoji behind flags.

### Step 6 — Phases 6-9

* `src/components/osd/` — ONE overlay surface created on demand, destroyed 1.5 s
  after the last event. Suppress for `suppress-after-startup` ms. Only fire on
  `*ChangedByUser` signals, never on server round trips. No animation.
* `src/components/quicksettings/` — Popup subclass anchored under the bar;
  volume + sink list, mic mute, brightness, wifi toggle + AP list, bluetooth
  toggle + device list, DND, power profile, night-light toggle, battery details,
  session buttons. **All enumeration bound to the open state** — start it in
  `onAboutToShow()`, stop it in `onHidden()`. Network secret agent deferred;
  write ADR-008 saying so before shipping.
* `src/components/powermenu/` — lock, logout (`ActionSender::quit(true)`),
  suspend, hibernate (only when `LogindService::canHibernate()`), reboot,
  shutdown. Confirmations for the destructive ones, keyboard reachable.
* `src/components/wallpaper/` — background-layer surface per output, decode once
  to exact output resolution, store as shm, free the source. Enforce the
  `max-rss-kb-per-output` budget from config and fall back to solid colour.
* Night light: `zwlr_gamma_control_v1` (verified present in niri 26.4 —
  `niri/src/protocols/gamma_control.rs`). Write ADR-009. Do **not** guess the
  request/response shapes; read the vendored
  `src/wl/protocols/wlr-gamma-control-unstable-v1.xml` first. It is
  `get_gamma_control`, `set_gamma(r, g, b size, then r[], g[], b[])` with a
  `gamma_size` event carrying max values and a `failed` event.
* Clipboard: `ext-data-control-v1`, text only by default.
* `src/gallery/` — `kapah-gallery`, every primitive with labels. `QT_QPA_PLATFORM=offscreen`
  in CI so it can be smoke-tested without a compositor.
* `src/settings/` — only if you want it; the roadmap allows skipping it in favour
  of the TOML file.

### Step 7 — Phase 10

* `tests/unit/` — one QtTest binary per parser: `tst_toml`, `tst_niri_events`,
  `tst_desktopentry`, `tst_fuzzy`, `tst_config`, `tst_markup`, `tst_exec_expand`,
  `tst_niri_reducer`.
* `tests/fixtures/niri/*.jsonl` — record real event streams from niri 25.11 and
  26.4 (just run niri headless with `NIRI_SOCKET` set and cat the socket), and
  include a synthetic stream containing an **unknown** event variant to prove the
  version-tolerance path.
* `tests/integration/` — `dbus-run-session`, a nested niri, `notify-send` and
  fake D-Bus services via python-dbusmock. `run-all.sh` is referenced by CI.
* `tests/fuzz/` — five libFuzzer harnesses + seed corpora.
* `contrib/` — `kapah.service`, `kapah.desktop`, `niri/config.kdl.in` with the
  `shellctl` keybinds, sample `config.toml`, man pages, and APKBUILD/xbps/RPM
  templates. **Every one of these must set
  `QT_QPA_PLATFORM=wayland`, `QT_QUICK_BACKEND=software`,
  `LIBGL_ALWAYS_SOFTWARE=1`** — `ci/check-env-hardening.sh` checks it.
* `docs/` — `ARCHITECTURE.md`, `PROTOCOLS.md` (niri version support matrix, the
  Wayland protocols and their fallbacks, the Qt private symbols relied on),
  `PERF.md` (budgets vs measured, with the environment recorded per entry),
  `SECURITY.md`, `CONTRIBUTING.md` (section 4 of the roadmap as a checklist).
* Measure and record the Phase 0 baseline PSS of an empty `QApplication` before
  anything else — it is the floor every other number is measured against, and it
  does not exist yet.

---

## 7. First commands to run

```sh
# 1. the gates, which need no compiler
ci/check-banned-symbols.sh
ci/check-env-hardening.sh

# 2. configure (will fail today: services/CMakeLists.txt names three files that
#    do not exist yet -- see section 5, item 1)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKAPAH_WERROR=ON
cmake --build build -j"$(nproc)"

# 3. once it builds
ctest --test-dir build --output-on-failure -LE integration
tools/perf/rss.sh --all-kapah
tools/perf/wakeups.sh --process kapahd --seconds 60
```

`cmake -S . -B build -DKAPAH_WERROR=ON` from the start. The warning set is
deliberately aggressive (`-Wconversion -Wsign-conversion -Wshadow
-Wold-style-cast -Wnon-virtual-dtor`) and it will surface real bugs, but only if
you keep it on. Developing without it and enabling it in CI wastes a cycle.

---

## 8. If you disagree with something here

The ADRs record *why*. If you think one is wrong, write a new ADR that supersedes
it, keep the old one, and say what measurement changed your mind. Do not silently
edit an ADR — the reasoning is the deliverable, not the conclusion.