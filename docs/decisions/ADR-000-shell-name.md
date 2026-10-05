# ADR-000: Shell name

Status: accepted
Date: 2026-10-03
Roadmap: section 14.1

## Context

The roadmap left the shell name as a CMake variable (`SHELL_NAME`) because it
determines every user-visible identifier. Getting it wrong after the fact means
renaming a config directory, a socket, two D-Bus names and three binaries.

## Decision

The shell is named **kapah**.

Derived identifiers, all from `SHELL_NAME` in the top-level `CMakeLists.txt`:

| Thing | Value |
|---|---|
| Daemon binary | `kapahd` |
| Lock binary | `kapah-lock` |
| CLI binary | `kapahctl` |
| Gallery binary | `kapah-gallery` |
| Settings binary | `kapah-settings` |
| Control socket | `$XDG_RUNTIME_DIR/kapah.sock` |
| Config file | `$XDG_CONFIG_HOME/kapah/config.toml` |
| State dir | `$XDG_STATE_HOME/kapah/` |
| Cache dir | `$XDG_CACHE_HOME/kapah/` |
| Notification bus name | `org.freedesktop.Notifications` (freedesktop-owned, not ours) |
| Shell bus name (optional mirror) | `org.kapah.Shell1` |
| Systemd unit | `kapah.service` |

## Consequences

* `SHELL_NAME` stays a cache variable so a packager can rename the project
  wholesale, but the values above are what gets documented, shipped and tested.
* The D-Bus name `org.kapah.Shell1` is a *mirror* only. The primary IPC is the
  Unix socket, per roadmap section 8.
* Renaming later is a mechanical change but a user-visible one: existing config
  files, frecency files and app-index caches would need migration. Treat a rename
  as a breaking release.
