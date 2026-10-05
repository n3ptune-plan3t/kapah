# Roadmap: A Low-Resource Qt6 Wayland Shell for niri

Audience: AI agents (and humans) collaborating on this project.
Owner: Plawan. Read sections 0-3 fully before touching code. Every phase has acceptance criteria; do not start phase N+1 until phase N's criteria are met and measured.

---

## 0. Mission and non-negotiable constraints

Build a complete desktop shell (bar, launcher, notifications, OSD, lock screen, quick settings, wallpaper, session/power menu) for the **niri** compositor using **Qt 6**, from scratch, for **low-end laptops**.

Hard constraints (these override every convenience):

1. **No GPU dependency.** Everything must render correctly and fast on CPU only (llvmpipe/no DRI). Never require OpenGL/Vulkan/EGL.
2. **Minimal CPU.** Idle shell = ~0.0% CPU. No timers that tick when nothing changes. No animations running at idle. No polling.
3. **Minimal RAM.** Target budgets (measured as PSS, see section 11):
   - Idle, bar only: **< 40 MB PSS**
   - Idle, everything loaded (bar + notification daemon + launcher resident): **< 70 MB PSS**
   - Launcher open with 500 apps: **< +15 MB**
4. **Event-driven only.** All state comes from D-Bus signals, netlink, inotify, udev, PipeWire/PulseAudio events, and niri IPC event stream. Polling is a bug unless explicitly justified in writing in `docs/decisions/`.
5. **Coherent and complete.** One consistent visual language, one config format, one IPC story. Features must be finished (error states, empty states, reconnect handling), not demos.
6. **Battery is a first-class metric.** Every feature PR reports wakeups/sec at idle.

Non-goals: GPU effects, blur, shadows beyond cheap static ones, QML-heavy animated UIs, a plugin system in v1, X11 support, supporting compositors other than niri (keep a clean seam, but do not implement).

---

## 1. Key architecture decision: Qt Widgets, not QtQuick

Default decision: **Qt Widgets with the `raster` backend (QPainter on CPU), C++20, no QML in the hot path.**

Rationale:
- QtQuick/QML pulls in the scene graph, JS engine (V4), and typically an RHI backend. Even with the software backend, baseline RSS is substantially higher and idle behavior is harder to guarantee.
- Widgets + QPainter render only on invalidation (`update()`), which fits "zero idle CPU".
- Binary and dependency footprint is smaller: link only `Qt6::Core`, `Qt6::Gui`, `Qt6::Widgets`, `Qt6::DBus`, `Qt6::WaylandClient` (+ `Qt6::Network` only if proven necessary).

Agents must **not** introduce QtQuick, QML, QtWebEngine, QtMultimedia, QtSvg-heavy pipelines, or Qt Charts. If an agent believes QML is needed for a specific component, it must write a measured justification (RSS, wakeups, startup time) in `docs/decisions/` and get owner approval.

Force software rendering everywhere as a safety net:
- `QT_QPA_PLATFORM=wayland`
- `QT_QUICK_BACKEND=software` (in case any Quick item sneaks in)
- `QT_WAYLAND_DISABLE_WINDOWDECORATION=1`
- Never create `QOpenGLWidget` / `QQuickWidget`. Add a CI grep that fails on these symbols.

---

## 2. Process model

Choose **one** of these and record it as ADR-001. Recommended: **Model B**.

- **Model A: single process, many layer surfaces.** Lowest total RAM (one Qt runtime, shared fonts/caches). Risk: one crash kills everything; lock screen must be isolated for security reasons anyway.
- **Model B (recommended): small number of processes.**
  - `shelld` — main process: bar, notifications daemon, OSD, launcher (resident but hidden), quick settings, wallpaper owner, IPC server. Single Qt runtime.
  - `shell-lock` — separate process for the session lock (ext-session-lock-v1), so a crash in shelld can never unlock the session and so lock can be tiny.
  - `shellctl` — tiny CLI client (no Qt GUI; Qt Core + DBus or plain sockets) for keybinds: `shellctl launcher toggle`, `shellctl volume +5`, `shellctl notify dismiss-all`.
- **Model C: one process per component.** Rejected: duplicates the Qt runtime (~15-25 MB each).

Supervision: run under `systemd --user` or niri's `spawn-at-startup`; `shelld` must tolerate being restarted at any time and rebuild all state from the system.

---

## 3. Repository layout and build

```
/                      CMakeLists.txt (top-level, C++20, Qt6 only)
/src/core/             logging, config, event-source abstractions, utilities
/src/niri/             niri IPC client (socket protocol, JSON event stream, models)
/src/wl/               wayland-specific glue (layer-shell, session-lock, foreign-toplevel wrappers)
/src/services/         system services: power, network, audio, backlight, tray, mpris, notifications, bluetooth, idle
/src/ui/               shared widgets, theme, icon cache, text layout cache
/src/components/       bar/, launcher/, notifications/, osd/, quicksettings/, powermenu/, wallpaper/
/src/lock/             shell-lock binary
/src/ctl/              shellctl binary
/tests/                unit + integration + headless tests
/docs/                 ARCHITECTURE.md, decisions/ (ADRs), PROTOCOLS.md, PERF.md
/contrib/              niri config snippet, systemd units, desktop files
```

Build rules:
- CMake >= 3.25, `CMAKE_CXX_STANDARD 20`, `-O2` default, `-Os` option for release-small, LTO on, `-fno-exceptions` is **not** required (Qt), but avoid exceptions in hot paths.
- Compile flags: `-Wall -Wextra -Wpedantic -Werror` in CI.
- Link with `-Wl,--gc-sections -ffunction-sections -fdata-sections`; strip release binaries.
- Use `qt_standard_project_setup()`; no Qt Designer `.ui` files (they bloat and hide logic); hand-written widgets.
- Wayland layer-shell glue: use **LayerShellQt** (`LayerShellQt::Interface`) if acceptable as a dependency; otherwise implement `zwlr_layer_shell_v1` via `qtwaylandscanner` + Qt private client APIs. Decide in ADR-002 (see Phase 1). Prefer LayerShellQt for speed of delivery; keep the wrapper in `src/wl/` so it is swappable.
- Dependencies allowed: Qt6 (Core, Gui, Widgets, DBus, WaylandClient), LayerShellQt, libudev (battery/backlight hotplug), libpipewire or libpulse (pick one, ADR-003; recommend PipeWire via `pipewire` C API with a `QSocketNotifier` bridge, or PulseAudio compat via libpulse which works with PipeWire-pulse and is simpler), libxkbcommon only if lock screen needs it, PAM for lock auth.
- Distro packaging considerations: produce one install target with desktop files; keep the dependency list short so it is easy to package for Alpine/Void/OpenMandriva/Artix-style distros.

---

## 4. Core engineering rules (apply to every phase)

### 4.1 Event loop discipline
- Single `QCoreApplication` event loop on the main thread. **No worker threads** unless a blocking operation cannot be made async (e.g., PAM auth in lock, initial desktop-file scan). Use `QtConcurrent`-free design: one short-lived `std::thread` or `QThread` with a clear owner for those cases.
- File descriptors (netlink, udev monitor, niri socket, pipewire) are attached via `QSocketNotifier`. Disable the notifier when not needed.
- No `QTimer` with a periodic interval except: (a) clock, aligned to the minute boundary using a single-shot timer re-armed to the next minute; (b) transient UI timeouts (notification expiry) as single-shots; (c) debounce timers (single-shot, <=100 ms). Any other periodic timer needs an ADR.
- Use `QTimer::setTimerType(Qt::VeryCoarseTimer)` for the clock and other non-critical timers to allow wakeup coalescing.
- Pause clock/time-based UI when the bar surface is not visible or session is locked/idle where meaningful.

### 4.2 Rendering discipline
- Custom-paint widgets with `QPainter`; avoid stylesheet (QSS) parsing at runtime in hot UI — QSS is slow and memory-hungry. Use a small `Theme` struct with colors/metrics and `QPalette`, painting by hand.
- Cache pre-rendered `QPixmap`s for icons and static text where cheap; invalidate on theme/DPI change.
- Call `update(rect)` with the smallest dirty rect. Never call `repaint()`.
- Use `Qt::WA_OpaquePaintEvent` / `Qt::WA_NoSystemBackground` for opaque widgets; avoid per-pixel alpha surfaces unless needed (bar can be opaque; notifications may use a flat translucent background without blur).
- Prefer shared memory buffers (the default for the wayland raster backend). Avoid large surfaces: the bar is a thin strip; the launcher is an overlay sized to content; never allocate full-screen transparent layer surfaces except when strictly necessary (e.g., click-outside-to-dismiss — prefer keyboard-interactivity grab and `exclusive` layer instead).
- Animations: off by default. If enabled, limit to 2-3 simple property animations at ≤30 fps, auto-stop at the end, and gate behind config `animations = false` default on battery.
- Fonts: one family, at most 2 weights. Avoid emoji/color fonts fallback loading unless a notification contains them. Cache `QFontMetrics`/layouts.

### 4.3 Memory discipline
- Lazy construction: windows and widgets for launcher, quick settings, power menu are created on first show and **destroyed after a grace period** (e.g., 30 s hidden) unless config says "resident". Measure which is better per component; default launcher = destroy UI, keep the app index only.
- App index stored compactly (see Phase 5): interned strings, no duplicated icons, icon loaded on demand and LRU-capped (e.g., 64 icons).
- Notification history capped (default 50) and bodies truncated for storage; images downscaled to ≤64 px for history.
- Periodically (on idle after launcher close) call `malloc_trim(0)` to return memory to the OS.
- Set `MALLOC_ARENA_MAX=1` (or `mallopt`) at startup — single-threaded app, one arena saves RSS.
- Do not load `Qt6::Svg` unless icon themes require it; prefer PNG icon themes, or render symbolic SVG once with a tiny custom path if needed (decide in ADR).
- Avoid `QImageReader` plugins you don't use; set `QT_PLUGIN_PATH` hygiene so only needed imageformat plugins load. Link statically where distro policy allows to cut plugin loads (optional, measured).

### 4.4 Robustness
- Every service (`NetworkService`, `PowerService`, ...) exposes: `state()` snapshot, `changed()` signal, `available()` boolean. UI must render a sane "unavailable" state, never crash if a daemon is missing (no NetworkManager, no PipeWire, no bluez, no upower).
- Reconnect logic: D-Bus name owner watching via `QDBusServiceWatcher`; niri socket reconnect with backoff (exponential, capped 5 s, no busy loops).
- All parsers (niri JSON, desktop files, MPRIS metadata) must tolerate unknown/missing fields.
- No blocking D-Bus calls on the main thread. Use `QDBusPendingCallWatcher`.

---

## 5. Roadmap phases

Each phase lists: **Goal**, **Tasks**, **Deliverables**, **Acceptance criteria**. Agents must update `docs/PERF.md` with measurements at the end of every phase.

### Phase 0 — Foundations and decisions (no UI)
**Goal:** Pin down decisions and build the measurement harness first.

Tasks:
1. Create repo skeleton (section 3), CMake, CI (build + clang-tidy + a grep-gate for banned symbols: `QOpenGL`, `QQuick`, `QML`, `QtWebEngine`, `QtMultimedia`).
2. Write ADRs: ADR-001 process model, ADR-002 layer-shell approach, ADR-003 audio backend, ADR-004 icon strategy (PNG vs SVG), ADR-005 config format.
3. Config format: **TOML** (or a minimal INI via `QSettings`-less custom parser). Hot-reload via `QFileSystemWatcher`/inotify on the directory (watch the parent dir to survive atomic replace). Schema documented, unknown keys warn but do not fail.
4. Logging: tiny category-based logger, compile-time disable of debug in release.
5. Build the **perf harness**: scripts in `/tools/perf/`:
   - `rss.sh` — reads `/proc/<pid>/smaps_rollup` for PSS/RSS.
   - `wakeups.sh` — counts wakeups using `perf stat -e sched:sched_wakeup` or `powertop --csv`/`strace -c -f -e trace=epoll_wait,ppoll,futex` fallback; at minimum read `/proc/<pid>/status` voluntary context switches over 60 s.
   - `startup.sh` — time to first frame using `WAYLAND_DEBUG` timestamps or app-side logging.
   - A "reference low-end machine profile" document: e.g., dual-core x86-64 @ ~1.5 GHz, 4 GB RAM, Intel iGPU disabled or llvmpipe forced (`LIBGL_ALWAYS_SOFTWARE=1`). Run niri itself with software rendering as the baseline.

Acceptance: CI green on an empty shell binary that starts a QApplication on wayland and quits; baseline PSS of an empty Qt Widgets app recorded in PERF.md (this is the floor, expect ~20-30 MB).

### Phase 1 — Wayland integration spike and niri IPC client
**Goal:** Prove a layer-shell surface renders on niri with raster backend, and talk to niri.

Tasks:
1. Create a `LayerSurface` wrapper in `src/wl/`: anchor, margins, exclusive zone, layer (top/overlay/bottom/background), keyboard interactivity (none/on-demand/exclusive), namespace. Backed by LayerShellQt (ADR-002).
2. Implement a minimal bar-shaped window (a colored strip) anchored top, exclusive zone = height, per output. Handle output add/remove (`QGuiApplication::screenAdded/Removed`) creating/destroying one bar per screen.
3. **niri IPC client** (`src/niri/`):
   - Connect to `$NIRI_SOCKET` (Unix stream socket) with `QLocalSocket` or raw fd + `QSocketNotifier`.
   - Implement the request/reply protocol (newline-delimited JSON). Send `"EventStream"` request and parse the continuous event stream: workspaces changed/activated, windows opened/closed/changed/focus changed, keyboard layout changes, overview state, etc. **Agents must read the current niri IPC documentation and the `niri-ipc` crate for the exact schema and version in use; do not assume event names from memory.** Pin the supported niri version in docs and add a version-tolerance policy.
   - Maintain an in-memory model: outputs, workspaces (per output, with active/focused/urgent), windows (id, title, app_id, workspace, focused), focused window, keyboard layout.
   - Expose it as a `NiriState` QObject with fine-grained signals (`workspacesChanged`, `focusedWindowChanged`, ...) so widgets repaint only what changed.
   - Provide an action sender for niri actions (focus workspace, close window, etc.) via the IPC `Action` request.
4. Unit-test the parser with recorded event-stream fixtures (`tests/fixtures/niri/*.jsonl`).

Acceptance: A strip on each output; a debug label shows focused window title and active workspace updating live; killing/restarting niri socket does not crash and reconnects; idle wakeups ≈ 0 when nothing changes.

### Phase 2 — Theme system and shared UI toolkit
**Goal:** One consistent, cheap visual language.

Tasks:
1. `Theme`: palette (bg, fg, accent, urgent, muted), radii, spacing scale, font sizes, DPI/scale-aware metrics. Load from config; support light/dark; hot-reload re-polishes all components via a single `themeChanged` signal.
2. Primitive widgets (hand-painted, no QSS): `Label` (elided, cached layout), `IconButton`, `Pill`, `Slider` (volume/brightness), `Toggle`, `ListView`-like virtualized list (see launcher), `Popup` base (layer-shell popup with outside-click handling).
3. Icon strategy (ADR-004): implement an `IconCache` — resolve via freedesktop icon theme spec (`QIcon::fromTheme` is acceptable but measure), render at the exact needed pixel size & DPR, LRU-capped, no multi-resolution `QIcon` retention.
4. Text rendering: ensure `QFont::setHintingPreference`/`setStyleStrategy(PreferDefault | NoSubpixelAntialias)` chosen deliberately for crispness vs cost; document the choice.

Acceptance: A gallery debug binary (`shell-gallery`) shows all primitives; PSS delta over Phase 0 baseline < 8 MB; no timers running at idle.

### Phase 3 — The bar (v1)
**Goal:** A complete, useful bar.

Modules (each a self-contained widget class + service, individually enable/disable/order-able in config; left/center/right sections):
1. **Workspaces** (from `NiriState`): per-output workspace indicators, active/urgent states, click to focus, scroll to switch (debounced).
2. **Window title / focused app** (elided, from `NiriState`).
3. **Clock**: minute-aligned single-shot timer, `Qt::VeryCoarseTimer`, locale-aware format from config; tooltip/popup with a lightweight calendar (custom painted month grid, no `QCalendarWidget`).
4. **Battery** (`PowerService`): prefer UPower over D-Bus signals (`org.freedesktop.UPower` `PropertiesChanged`); fallback to udev `power_supply` uevents via `libudev` monitor (event-driven) and `/sys/class/power_supply` read on event only. Show percent, charging state, time remaining if provided. Low/critical thresholds trigger a notification once.
5. **Network** (`NetworkService`): backend options — NetworkManager D-Bus (signals: `StateChanged`, `PropertiesChanged`, active connection / access point strength) with graceful absence; additionally support a netlink (`NETLINK_ROUTE`) fallback for link up/down + IP presence when no NM (ADR for iwd/ConnMan support later). Show wifi strength, ethernet, VPN, disconnected.
6. **Audio** (`AudioService`): default sink volume/mute, default source mute; subscribe to server events (PipeWire registry/metadata or pulse subscribe). Scroll on the widget adjusts volume, click mutes, right-click opens sink selector popup.
7. **Backlight** (`BacklightService`): read via `/sys/class/backlight/*/brightness` + inotify/udev change events (sysfs `brightness` does not always generate inotify events; use udev `backlight` subsystem monitor and/or logind `SetBrightness` D-Bus method for writes — ADR needed). Write via `org.freedesktop.login1.Session.SetBrightness` (no root needed).
8. **System tray** (StatusNotifierItem): implement `org.kde.StatusNotifierWatcher` + host (or use a minimal in-house implementation) and render icons with `dbusmenu` support for context menus (implement a minimal `com.canonical.dbusmenu` client; lazy-fetch menu layout only when the menu is opened). Cache icons; handle `IconPixmap` data and themed names; cap tray items.
9. **MPRIS** mini-widget (optional module, lazy): current track, play/pause; only subscribed when the module is enabled.
10. **Layout/keyboard layout indicator** from niri state.
11. **Session button** → power menu (Phase 8).

Cross-cutting:
- Bar visibility modes: always, auto-hide (via niri overview/fullscreen awareness; hide on fullscreen windows if desired), per-output config.
- Hover tooltips are created lazily and destroyed on hide.
- Every module defines "unavailable" and "loading" visuals.

Acceptance: Bar < 40 MB PSS; 0 wakeups/s idle except once per minute (clock); battery/network/audio changes reflect within 200 ms; no module blocks the UI thread; killing NetworkManager/PipeWire/UPower leaves the bar alive with those modules in an unavailable state and they recover when daemons restart.

### Phase 4 — Notification daemon
**Goal:** Fully spec-compliant `org.freedesktop.Notifications` server.

Tasks:
1. Own bus name `org.freedesktop.Notifications` (handle name-already-owned by another daemon with a clear log and optional replace flag).
2. Implement: `Notify`, `CloseNotification`, `GetCapabilities`, `GetServerInformation`, signals `NotificationClosed` and `ActionInvoked`. Support: replaces_id, expire_timeout, urgency hint, actions, body markup subset (`<b> <i> <u> <a href>`; sanitize everything else), image-data/image-path/app_icon, category/desktop-entry hints, `x-canonical-private-synchronous`/`synchronous` for volume-OSD-like replacement, `transient`, `resident`.
3. Popup stack as **one** layer-shell overlay surface sized to the stack (resized as items change), anchored top-right (config), non-focusable (`keyboardInteractivity = none`), click to invoke default action, close button, optional swipe/click-dismiss.
4. Expiry using single-shot timers per visible notification only; pause timers on hover.
5. Do-not-disturb mode (config + quick toggle), per-app rules (mute, force-persist), rate limiting / grouping for spam (collapse identical bursts).
6. History store: capped ring buffer in memory; optional persistence to a small file written lazily on idle/exit (not on every notification).
7. Notification center panel (list from history) reachable from the bar or quick settings, virtualized rendering.
8. Surface destroyed when no notifications are visible (so there is zero surface cost at idle).

Acceptance: Passes a conformance script (`notify-send` variants, `dbus-send` edge cases, urgency/expiry/replace/close reasons) in `tests/integration/`; idle RSS delta < 5 MB; popup surface absent when idle.

### Phase 5 — Launcher
**Goal:** Fast, tiny application launcher.

Tasks:
1. App index: scan `XDG_DATA_DIRS`/applications `.desktop` files; honor `NoDisplay`, `Hidden`, `OnlyShowIn`/`NotShowIn`, `TryExec`, localized `Name`/`GenericName`/`Keywords`; parse with a **custom minimal parser** (not `QSettings`) for speed and memory. Cache the index on disk (binary or compact JSON) keyed by directory mtimes; watch directories via inotify to refresh incrementally and lazily.
2. Search: simple, fast fuzzy match (subsequence + prefix + word-boundary scoring) over precomputed lowercase strings; frecency ranking stored in a tiny file; target < 5 ms per keystroke for 1000 apps on the reference machine.
3. UI: single overlay layer surface with `keyboardInteractivity = exclusive`, text input (custom-painted line edit or minimal `QLineEdit` with tuned styling — measure), virtualized result list (paint only visible rows, e.g., 8-10), icons lazily loaded via `IconCache`, keyboard navigation (up/down/enter/escape/tab, ctrl+n/p), mouse support.
4. Launching: use `QProcess::startDetached` replaced by a double-fork/`posix_spawn` helper, or `systemd-run --user --scope` as option; honor `Exec` field codes (`%f %u %U %F %i %c %k`), `Path`, `Terminal=true` (configurable terminal), `DBusActivatable` (call `org.freedesktop.Application.Activate/Open`), desktop `Actions` surfaced as secondary entries.
5. Extra modes (all lazy, each a separate small provider interface): calculator (tiny expression parser, no JS), window switcher (from `NiriState`: jump to window), command runner (`>` prefix), clipboard history (optional, see Phase 9), emoji (optional, off by default due to font/memory cost).
6. Toggle via `shellctl launcher toggle` (bind in niri config). UI destroyed after grace period; index retained in compact form.

Acceptance: Open-to-first-frame < 80 ms on the reference machine; 500-app index < 3 MB resident; closing and waiting 30 s reduces PSS back near baseline; zero wakeups while closed.

### Phase 6 — OSD (volume/brightness/caps-lock etc.)
**Goal:** Tiny transient feedback surface.

Tasks:
1. Single overlay layer surface created on demand, destroyed ~1.5 s after the last update (single-shot timer reset on each event).
2. Triggered by service change signals and optionally by `shellctl osd ...`; avoid triggering on startup or on external initial-state reads (suppress first N ms after daemon start).
3. Cheap rendering: flat rounded rect + icon + bar, no animation by default.
4. Lock-key state: via niri/keyboard indicators if available through niri IPC or evdev-free approach; otherwise skip (do not read evdev with polling). Document limitations.

Acceptance: 0 resident cost when idle; no flicker when holding a volume key; appears correctly on the focused output.

### Phase 7 — Quick settings panel
**Goal:** One popup for common toggles.

Tasks:
1. Layer-shell popup anchored under the bar, created lazily, destroyed on close after grace period.
2. Controls: volume slider + sink selector, microphone mute, brightness slider, wifi toggle + network list (NetworkManager `AccessPoint` list fetched only while open; request scan only on user action), Bluetooth toggle + device list (BlueZ via D-Bus `ObjectManager`, only active when the panel/module is enabled), DND toggle, power profile (`net.hadess.PowerProfiles` / `power-profiles-daemon` D-Bus), night light (see Phase 9) toggle, battery details, session buttons.
3. Network password prompt: delegate to NetworkManager secret agent implemented minimally (only when the user connects to a secured new network) — gate behind its own ADR since it is a sizeable feature; initially support only known/saved networks and open networks.
4. Scanning, discovery, and polling of devices strictly bound to the panel's open state.

Acceptance: panel open < 100 ms; all background discovery stopped after close; panel feature set degrades gracefully when daemons are absent.

### Phase 8 — Lock screen, idle, session/power menu
**Goal:** Security-correct session handling.

Tasks:
1. `shell-lock` binary using **ext-session-lock-v1** (`ext_session_lock_manager_v1`) — one lock surface per output, opaque, solid color/pre-blurred-at-build-time wallpaper (never real-time blur). If the client dies while locked, niri must keep the session locked (this is protocol behavior); never design a path that unlocks on crash.
2. Authentication via **PAM** in a worker thread (single dedicated thread; PAM conversation blocking), password entry custom-painted widget, no echo, clear buffer after use (`explicit_bzero`), handle failure/lockout delays, show keyboard layout and caps-lock state.
3. Idle management: implement `ext-idle-notify-v1` client in `shelld` (or reuse niri's/swayidle-compatible tooling, but own implementation is preferred for fewer processes): configurable timeouts — dim, lock, DPMS off (via niri action `power-off-monitors`), suspend (logind `Suspend`). Inhibit support: `org.freedesktop.ScreenSaver`/`org.freedesktop.login1` inhibitors, Wayland `idle-inhibit` (compositor side), MPRIS-playing or fullscreen heuristics optional.
4. Lock on suspend/lid-close: subscribe to logind `PrepareForSleep` and take a delay inhibitor lock, lock the session, then release the inhibitor.
5. Power/session menu popup: lock, logout (niri `Quit` action), suspend, hibernate (if available per logind `CanHibernate`), reboot, shutdown; confirmation for destructive actions; keyboard accessible.

Acceptance: Lock cannot be bypassed by killing `shelld`; lock surfaces appear on hotplugged outputs; password handling reviewed by a second agent against a security checklist in `docs/SECURITY.md`; idle path causes no wakeups beyond the compositor's idle notifications.

### Phase 9 — Extras (each optional, each behind a build flag and a config switch)
1. **Wallpaper**: background-layer surface per output, decode once to the exact output resolution, store as a shared-memory buffer, then free the decoded source; support solid color default (zero cost). Optionally defer to an external tool (e.g., `swaybg`) — prefer built-in only if RSS cost < 4 MB per output.
2. **Night light / color temperature**: only if niri supports a gamma-control protocol in the target version; otherwise omit and document.
3. **Clipboard history**: `wlr-data-control`/`ext-data-control` listener, text-only by default, capped, optional encryption-free in-memory ring; images off by default.
4. **Screenshot UI** hooks: delegate to niri's built-in screenshot actions; shell only offers launch entries.
5. **Overview/dock-like module**: only if it can stay event-driven and cheap; low priority.
6. **Settings UI** (standalone, short-lived process, never resident): edit config with validation; or skip and rely on the TOML file + hot reload.

### Phase 10 — Hardening, packaging, documentation, release
Tasks:
1. Soak tests: 24 h idle with synthetic event generators (fake notifications every 10 s, fake volume changes) — verify no RSS growth (leak check with `valgrind --tool=massif` / `heaptrack` on a dev machine; ASAN/UBSAN CI job).
2. Fuzz parsers (niri JSON, desktop entries, notification markup, dbusmenu layouts) with libFuzzer or AFL++.
3. Accessibility basics: sensible keyboard navigation everywhere, high-contrast theme, scalable fonts, no color-only state.
4. i18n: use `QTranslator` with compiled `.qm` only for languages enabled; keep strings in one place.
5. Packaging: install layout, `.desktop`, systemd user units (`shelld.service` with `Restart=on-failure`), niri config snippet (keybinds calling `shellctl`), sample config, man pages. Provide recipes for target distros (APKBUILD, xbps template, RPM spec) in `/contrib/packaging/` with minimal dependency lists.
6. Documentation: ARCHITECTURE.md (component diagram, data flow), PROTOCOLS.md (niri IPC version support, Wayland protocols used and fallbacks), PERF.md (budgets vs measured), CONTRIBUTING.md (rules from section 4 as a checklist).

Acceptance: All budgets in section 0 met on the reference machine and recorded in PERF.md; zero known crashers; feature list complete per this roadmap or explicitly deferred in an ADR.

---

## 6. Wayland protocols to use (verify availability on target niri)

| Purpose | Protocol | Notes |
|---|---|---|
| Bar, popups, OSD, notifications, wallpaper | `wlr-layer-shell-unstable-v1` | via LayerShellQt or generated bindings |
| Lock | `ext-session-lock-v1` | separate `shell-lock` process |
| Idle | `ext-idle-notify-v1` | plus compositor `idle-inhibit` |
| Clipboard history | `ext-data-control-v1` / `wlr-data-control` | optional module |
| Foreign toplevels (fallback only) | `wlr-foreign-toplevel-management` / `ext-foreign-toplevel-list` | niri IPC is the primary source; use protocol only if IPC lacks data |
| Output info | `xdg-output` (via Qt) + niri IPC outputs | |

Agents must check the current niri release notes for which protocols are implemented; never assume support. Add runtime capability detection and degrade features cleanly.

---

## 7. Configuration design

- Location: `$XDG_CONFIG_HOME/<shellname>/config.toml` (shell name TBD by owner; leave as a CMake variable `SHELL_NAME`).
- Sections: `[general]` (scale, font, theme, animations), `[bar]` (position, height, modules per section, per-output overrides), `[launcher]`, `[notifications]`, `[osd]`, `[lock]`, `[idle]`, `[quicksettings]`, `[wallpaper]`, `[services]`.
- Hot reload: debounced (100 ms), diff old vs new, update only affected components, never rebuild everything.
- Defaults must produce a fully usable shell with **no config file**.
- Validate and log precise errors (line/column); a bad config must never prevent startup — fall back to last good in-memory config.

---

## 8. IPC for keybinds and scripting

- `shelld` exposes a Unix socket at `$XDG_RUNTIME_DIR/<shellname>.sock` (newline-delimited JSON or a tiny text protocol) and optionally a D-Bus interface `org.<shellname>.Shell1`. Pick **one primary** (recommend the Unix socket for lowest overhead; D-Bus mirror optional).
- `shellctl` commands (minimum): `launcher toggle|show|hide`, `osd show <kind>`, `volume up|down|mute [step]`, `brightness up|down [step]`, `notify dnd on|off|toggle`, `notify clear`, `notify history`, `powermenu toggle`, `quicksettings toggle`, `reload`, `quit`, `status` (JSON dump of service states for debugging), `perf` (prints PSS/wakeups of itself).
- Volume/brightness commands should be implemented **inside the shell** (so OSD logic and clamping live in one place), not by shelling out to other tools.
- `shellctl` must start in < 5 ms and avoid linking Qt GUI.

---

## 9. Testing strategy

- **Unit tests** (QtTest): parsers, fuzzy matcher, config diffing, notification markup sanitizer, desktop-entry Exec expansion, niri model reducer.
- **Service tests**: run against fake D-Bus services using a private `dbus-daemon` per test (`dbus-run-session`), and python-dbusmock where convenient.
- **Headless UI tests**: `QT_QPA_PLATFORM=offscreen` for painting/layout assertions (render to `QImage`, golden-image diff with tolerance).
- **Integration tests**: nested niri instance (`niri` can run as a nested window under another compositor, or use a headless CI config) driving `shellctl`, `notify-send`, and fake services; assert surfaces via `WAYLAND_DEBUG` or a tiny protocol-inspection client.
- **Perf regression gate**: CI job runs the perf harness against a scripted scenario and fails if PSS or wakeups exceed budgets by >10%.

---

## 10. Agent working rules

1. **Read before writing.** Read the relevant ADRs, this roadmap's phase, and existing code in the target directory first.
2. **One phase, one branch.** Small PRs (< 600 changed lines) each with: description, acceptance criteria checked, PSS/wakeup numbers before/after.
3. **No new dependency without an ADR.** State the RSS/startup cost, what it replaces, and the distro availability.
4. **No polling, no hidden timers, no threads** unless the exception process in 4.1 is followed.
5. **Verify protocol and API details against current documentation** (niri IPC docs, Qt 6 docs, freedesktop specs, LayerShellQt headers) — do not rely on memory for names, signatures, or event schemas. When uncertain, write a tiny probe program and record its output in `docs/PROTOCOLS.md`.
6. **Never guess about performance.** Measure with the harness; include numbers.
7. **Fail soft.** Missing daemon/protocol/hardware → degraded but working shell.
8. **Security-sensitive code (lock, PAM, secrets, notification markup) requires a second-agent review** using `docs/SECURITY.md`.
9. **Keep scope honest.** If a feature threatens the budgets, propose a lighter design or defer it in an ADR rather than silently exceeding limits.
10. **Leave the repo runnable** at the end of every task: builds, tests pass, `shelld` starts under niri.

---

## 11. Measurement protocol (how to prove the budgets)

- **Memory:** use **PSS** from `/proc/<pid>/smaps_rollup` (`Pss:` line), not RSS, because shared Qt libraries inflate RSS. Also record `Private_Dirty` and `Anonymous`. Take readings 30 s after startup settles and after exercising each component (open/close launcher 10 times, send 100 notifications).
- **CPU/wakeups:** 60 s idle sample: user+sys jiffies delta (target ~0), voluntary context switches/sec (target < 0.1/s excluding the minute tick), `perf stat -e task-clock,context-switches -p <pid>`, and `powertop` wakeups attribution on a real battery device.
- **Startup:** time from spawn to bar first frame; target < 300 ms on reference machine.
- **Frame cost:** log paint time per component in a debug build; no single paint > 4 ms on the reference machine for bar/OSD, < 8 ms for launcher list.
- Record the environment (CPU model, RAM, Qt version, niri version, kernel) in every PERF.md entry.

---

## 12. Risk register and fallbacks

| Risk | Mitigation |
|---|---|
| Qt Widgets baseline RSS higher than budget | Static link + plugin trimming; `MALLOC_ARENA_MAX=1`; destroy-on-hide windows; if still over, renegotiate budget in ADR before adding features |
| LayerShellQt dependency unavailable/old on target distros | Keep `src/wl/` wrapper; fall back to own `zwlr_layer_shell_v1` binding via `qtwaylandscanner` |
| niri IPC schema changes between versions | Version-tolerant parser, fixtures per version, pinned min/max supported version, CI job against latest niri |
| System tray complexity (SNI + dbusmenu) | Ship minimal SNI first, add dbusmenu in a follow-up; lazy menu fetching |
| Network secret agent scope creep | Defer to post-v1; support saved/open networks first |
| Software rendering too slow at HiDPI | Cap surface sizes, cache pixmaps, avoid large translucent surfaces, make animations optional/off |
| Notification flood | Rate limiting and grouping, hard cap on visible items and history |
| Lock screen bugs | Keep it separate, tiny, reviewed; rely on compositor's lock-on-crash semantics |

---

## 13. Suggested milestone order (calendar-agnostic)

1. **M0** Phase 0 + 1: skeleton, perf harness, layer-shell strip, niri IPC model.
2. **M1** Phase 2 + 3 (core modules: workspaces, title, clock, battery, network, audio): usable bar.
3. **M2** Phase 4 + 6: notifications + OSD, plus `shellctl` and the IPC socket (Section 8).
4. **M3** Phase 5: launcher.
5. **M4** Phase 3 remainder (tray, MPRIS, backlight polish) + Phase 7 quick settings.
6. **M5** Phase 8: lock, idle, power menu.
7. **M6** Phase 9 extras as desired, then Phase 10 hardening and packaging.

Definition of "v1 done": daily-drivable on the reference low-end laptop for a week with no restarts, all budgets met, all non-extra phases complete.

---

## 14. Open questions for the owner (agents: ask before assuming)

1. Final project/shell name (affects socket paths, bus names, config dir).
2. Is a LayerShellQt dependency acceptable, or must everything be generated in-tree?
3. PulseAudio-compat (libpulse) vs native PipeWire for audio.
4. Which network stack must be supported at launch: NetworkManager only, or also iwd/ConnMan?
5. Visual direction (flat minimal, rounded, specific color scheme, matching an existing theme project).
6. Minimum supported niri and Qt versions.
7. Languages/locales required at launch.
