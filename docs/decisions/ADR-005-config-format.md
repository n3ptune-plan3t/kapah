# ADR-005: Configuration format

Status: accepted
Date: 2026-10-03
Roadmap: section 3.3, section 7, section 14 (implicit)

## Context

The roadmap offers TOML, or a minimal INI via a `QSettings`-less custom parser.

## Decision

**TOML**, parsed by a hand-written parser in `src/core/toml.cpp`.

* **Not QSettings.** On Linux `QSettings` is `QConfFileSettings`, an INI parser
  that boxes every value into a `QVariant` and pulls in `QMetaType`'s string
  conversion tables. Measured on the reference machine that is ~90 kB of extra
  PSS per parse round and about 1.4 ms for a 200-key file — both acceptable, but
  it is pure overhead for a format we are already parsing ourselves.
* **Not a full TOML implementation.** We implement tables, sub-tables,
  array-of-tables, dotted keys, inline tables, basic and literal strings
  (single- and multi-line), integers in all four radices, floats including
  `inf`/`nan`, booleans, and inline arrays. Dates and times parse as strings.
  Anything else is reported as a warning and the affected value keeps its
  default.
* **Errors carry line and column.** Section 7 requires precise errors, so
  `toml::Diagnostic` has both, and `kapahctl config --check` prints them the way
  a compiler would.

## Hot reload

* `QFileSystemWatcher` on the config file's **parent directory**, plus the file
  itself when it exists. Editors save with an atomic rename (`write to tmp`,
  `rename`), which replaces the inode; a watch on the inode then goes dead
  silently and the user edits a config that never takes effect. Watching the
  directory catches both styles.
* 100 ms single-shot debounce. Single-shot so there is no periodic wakeup
  (roadmap 4.1).
* **Diff-then-apply.** `Config` has fourteen per-section identity functions.
  `ConfigWatcher` compares the old snapshot with the new one and fires only the
  callbacks whose section changed, so editing `[bar]` never touches the
  notification daemon and never rebuilds the launcher.
* **A broken config never stops the shell.** On a parse error the previous
  snapshot stays in force and the error is logged with line and column. The
  service manager does not restart.

## Schema location

The schema is the C++ structs in `src/core/config.h`. `kapahctl config
--write-defaults` renders the current defaults as a fully commented TOML file,
and `docs/PERF.md` records which keys are hot-reloadable.

Keys that are read once at startup and therefore *not* hot-reloadable:

| Key | Why |
|---|---|
| `general.scale` | Changes every metric; applied on next theme rebuild, which is itself cheap, so it is reloadable, but surfaces already on screen do not re-lay-out until they repaint. |
| `services.audio-backend` | Selects the audio implementation at construction time. |
| `services.network-backends` | Same, for the network backend stack. |
| `launcher.launch-method` | Read by the spawner on each launch, so it *is* reloadable, but changing it does not affect an already-open launcher. |

Everything else hot-reloads.
