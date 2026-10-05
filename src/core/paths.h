// XDG paths, socket location and other well-known files.
//
// Every path is resolved once and cached. Nothing here touches the filesystem
// at idle; `ensureRuntimeDir()` is the only function that creates anything and
// it is called from main() before the event loop starts.

#pragma once

#include <QString>

namespace kapah::paths {

// $XDG_CONFIG_HOME/kapah, else $HOME/.config/kapah
QString configDir();
// $XDG_CONFIG_HOME/kapah/config.toml
QString configFile();
// $XDG_STATE_HOME/kapah, else $HOME/.local/state/kapah
QString stateDir();
// $XDG_CACHE_HOME/kapah, else $HOME/.cache/kapah
QString cacheDir();
// $XDG_RUNTIME_DIR/kapah, else a deterministic fallback under /tmp for the
// rare case of a session with no runtime dir (integration tests, containers).
QString runtimeDir();
// The shelld control socket consumed by kapahctl.
QString controlSocket();

// Best-effort "which user is logged in", used by the lock screen for the
// greeting. Falls back to $USER / $LOGNAME.
QString userName();
QString hostName();

// Session id of the user's systemd login session, e.g. "c2". Used for logind
// D-Bus calls that need an explicit session path. Empty when not under logind.
QString logindSessionId();

// Path to the current desktop's wallpaper hint, per the XDG desktop background
// specification. Empty if unset.
QString desktopBackground();

} // namespace kapah::paths