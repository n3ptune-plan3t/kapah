# ADR-004: Icon strategy (SVG vs PNG)

Status: accepted
Date: 2026-10-03
Roadmap: section 3, section 4.3, Phase 2.3

## Context

Every icon in the shell — bar modules, launcher rows, notification app icons,
tray items — has to resolve from the freedesktop icon theme. The choice is
whether to link `Qt6::Svg`.

## Decision

**No `Qt6::Svg`.** Resolve with `QIcon::fromTheme`, but only through our own
cache, and only rasterised PNG/PNG-compressed assets are supported. SVG assets in
the icon theme are handled by Qt's built-in *SVG icon theme* path only if it is
already loaded by another process; otherwise we render a missing-icon glyph.

Concretely:

* `IconCache` (in `src/ui/icons.cpp`) resolves a themed name **and** an absolute
  file path.
* Resolution order:
  1. Exact file path, if the caller passed one (notification `image-path`, tray
     `IconPixmap` is different, see below).
  2. `QIcon::fromTheme(name)` — we accept that this dlopens
     `libqsvgicon` if the theme contains SVGs, because on a machine that has such
     a theme, the alternative is a wrong picture. On a machine that does not,
     nothing extra loads.
  3. `QIcon::fromTheme(freedesktop fallback name)` per the lookup table below.
  4. A painted glyph placeholder (never an empty box).
* The result is rasterised **once** at exactly the pixel size × device pixel
  ratio that will be painted, and stored as a `QPixmap`. The `QIcon` itself is
  dropped immediately. This is the important half of the decision: a retained
  `QIcon` holds a `QIconLoaderEngine` that keeps every size variant the file
  provides, which for a 512×512 symbolic pack is a lot of resident memory for
  something we draw at 16 px.
* The cache is an LRU of 64 entries (roadmap 4.3). Traying the launcher evicts
  in bulk.

## Fallback names

When a themed name is not found, we try the freedesktop "conventional" names, so
that a shell with an Adwaita-only system still shows a speaker:

| Purpose | Names tried, in order |
|---|---|
| volume | `audio-volume-high`, `audio-volume-medium`, `audio-volume-low`, `audio-volume-muted`, `preferences-system-sound` |
| microphone | `microphone`, `audio-input-microphone`, `audio-muted` |
| network up | `network-wireless`, `network-wired`, `network-mobile-broadband`, `network-offline` |
| wifi strength | `network-wireless-signal-excellent` … `-good` … `-ok` … `-weak` … `-low`, `network-wireless` |
| battery | `battery-full`, `battery-good`, `battery-medium`, `battery-low`, `battery-caution`, `battery-empty`, `battery-missing` |
| charging | `battery-full-charging`, … |
| bluetooth | `bluetooth`, `bluetooth-disabled` |
| brightness | `video-display`, `display-brightness-symbolic` |
| power | `system-shutdown`, `system-lock-screen`, `system-switch-user` |
| session | `preferences-system`, `system-run` |
| search | `edit-find`, `system-search` |
| terminal | `utilities-terminal`, `terminal` |
| calculator | `accessories-calculator`, `calculator` |

## Consequences

* `Qt6::Svg` is **not** in the link list. `ci/check-banned-symbols.sh` fails the
  build if `QtSvg` appears in a `target_link_libraries` line.
* A user whose only icon theme is SVG gets a bar full of placeholders. This is
  accepted because: the target machines are 4 GB laptops where the biggest
  available win is 1.5 MB of resident library, and because Adwaita, Papirus,
  Breeze and Tango all ship raster assets. It is documented in
  `docs/PROTOCOLS.md` and in the install README.
* StatusNotifierItem icons are handled differently: `IconPixmap` is raw ARGB32
  data over D-Bus and never goes through the theme at all. Themed names on an
  SNI *do* go through `IconCache`. See `src/services/tray.cpp`.
* Emoji in notification bodies are a special case: they need a colour font, and
  Qt will pull one in on demand. We accept that cost only when a notification
  actually contains a character outside Latin-1, and we log it (see
  `docs/PERF.md`, "emoji font load").
