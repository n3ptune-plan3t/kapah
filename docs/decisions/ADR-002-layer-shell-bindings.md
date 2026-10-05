# ADR-002: Wayland layer-shell bindings

Status: accepted
Date: 2026-10-03
Roadmap: section 3, section 14.2

## Context

Every layer surface in this shell — bar, launcher, notifications, OSD, quick
settings, power menu, wallpaper — needs `zwlr_layer_shell_v1`. Three ways to get
it:

1. **LayerShellQt** (`LayerShellQt::Interface`). Well tested, ~40 files, ships
   its own `zwlr_layer_shell_v1.xml` copy.
2. **In-tree generated bindings**: vendor the protocol XML, run
   `qtwaylandscanner` at build time, write our own `QWaylandWindow` subclass.
3. Depend on a distro-specific `wlr-protocols`/`wayland-protocols` runtime and
   dlopen the protocol from a path at runtime.

## Decision

**Option 2: in-tree generated bindings**, in `src/wl/`.

Rationale, in order of weight:

* **Packaging.** The roadmap explicitly targets Alpine, Void, OpenMandriva and
  Artix-style distros. LayerShellQt is packaged in some of them and missing or
  stale in others, and where it is stale it is usually built against an older
  `zwlr_layer_shell_v1`. A vendored XML plus `qtwaylandscanner` (already
  required by `qt6-wayland-dev`, which every target has) removes a dependency
  from the APKBUILD/xbps/RPM dep list.
* **The API is 12 requests and 2 events.** `get_layer_surface`, `set_size`,
  `set_anchor`, `set_exclusive_zone`, `set_margin`, `set_keyboard_interactivity`,
  `set_layer`, `get_popup`, `ack_configure`, `destroy`, `configure`, `closed`.
  There is nothing for LayerShellQt to abstract that we do not also want to
  control, and several things we *do* want that it does not expose well: the
  distinction between "exclusive-zone 0" and "unset", an explicit output handle
  override, and per-output exclusive zones.
* **It keeps the seam.** Everything lives behind `src/wl/LayerSurface`, which is
  the same seam LayerShellQt would have provided. If it ever becomes untenable we
  swap the implementation, not the call sites.

Consequence: we link `Qt6::WaylandClientPrivate` and use the Qt private client
API (`QtWaylandClient::QWaylandWindow`, `QWaylandShellSurface`, `QWaylandScreen`).
This is the same approach LayerShellQt itself takes, and it is why
`qt6-base-private-dev` is a build dependency.

The private API is a real maintenance cost, so:

* All private-API use is confined to `src/wl/` — four files.
* `src/wl/qtcompat.h` is the single place where a Qt version difference is
  papered over. It is pinned to **Qt 6.5 through 6.8**; see below.

## Version policy

* **Minimum Qt: 6.5.0.** `qt_generate_wayland_protocol_sources` is stable from
  there and `QWaylandWindow::setInterface` has the signature we rely on.
* **Maximum tested Qt: 6.8.** Qt is in LTS for 6.5, so 6.5 is the version most
  likely to be present on the reference machine in three years.
* 6.9+ is untested. `src/wl/qtcompat.h` has `#if` guards for the two changes we
  already know about, but a new Qt will need a real audit, not a guard.
* CI builds against the distro Qt (6.5 on Ubuntu 24.04) and, when a second Qt is
  available, against it too.

## Protocol version

Vendored `wlr-layer-shell-unstable-v1.xml` is version 4 of
`zwlr_layer_shell_v1` (checked 2026-10-03 against wlr-protocols HEAD). We bind
version 4 and niri 25.11+ implements all of it. `set_layer` is `since="2"`, so
binding version 4 is safe.

We use:

* `layer`: `top` for the bar and OSD, `overlay` for the launcher, notifications,
  quick settings and the power menu, `bottom` for nothing, `background` for the
  wallpaper.
* `keyboard_interactivity`: `none` for the bar and the notification stack (they
  must never steal the keyboard), `exclusive` for the launcher, quick settings
  and the power menu, `on-demand` for tooltips (which are not layer surfaces at
  all — they are child `QToolTip`s inside the bar window).
* `exclusive_zone`: `-1` means "ignore other exclusive zones" and is what the bar
  uses when `auto-hide-on-fullscreen` is on. `0` means "float above other
  exclusive zones" and is what the notifications and OSD use. A positive integer
  is the bar's thickness in logical pixels.
* `anchor`: the bar anchors to `top` or `bottom` plus both horizontal edges.
  Popups anchor to the single edge they grow from.

## Rejected

* **LayerShellQt.** Right call for a Plasma component, wrong call for a shell
  that must package on four distros with a 3-dependency list.
* **Runtime dlopen of the XML** (`wl_registry` + `wl_proxy_marshal` by
  hand-written struct layouts). Zero build dependencies, but the marshalling
  code is a memory-corruption waiting list and we would write it by hand.
