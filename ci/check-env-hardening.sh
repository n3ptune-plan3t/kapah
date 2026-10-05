#!/usr/bin/env bash
# Check that every executable entry point forces software rendering.
#
# Rationale (roadmap section 1): a shell that silently falls back to EGL/GLX on
# a machine with no working DRI will show a black bar and burn 100% of one core.
# We therefore require the environment hardening in *every* launcher path, not
# just in documentation.
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
status=0

require_in() {
    local file="$1" needle="$2"
    if ! grep -qF -- "$needle" "$file"; then
        echo "FAIL: $file does not set $needle"
        status=1
    fi
}

# The daemon and the lock binary are the only two processes that create
# surfaces. Both go through env hardening in main() (src/core/env.cpp,
# KAPAH_HARDEN_ENVIRONMENT) which is covered by the unit test below; here we only
# check the files that can bypass it.

for unit in \
    "$ROOT/contrib/systemd/kapah.service.in" \
    "$ROOT/contrib/niri/config.kdl.in" \
    "$ROOT/contrib/xdg-autostart/kapah.desktop.in"
do
    [[ -f "$unit" ]] || continue
    require_in "$unit" 'QT_QPA_PLATFORM=wayland'
    require_in "$unit" 'QT_QUICK_BACKEND=software'
    require_in "$unit" 'LIBGL_ALWAYS_SOFTWARE=1'
done

# main.cpp must call the hardening helper before constructing the application.
for main_src in "$ROOT/src/app/main.cpp" "$ROOT/src/lock/main.cpp" "$ROOT/src/gallery/main.cpp"; do
    [[ -f "$main_src" ]] || continue
    if ! grep -q 'hardenEnvironment' "$main_src"; then
        echo "FAIL: $main_src does not call hardenEnvironment()"
        status=1
    fi
done

if [[ $status -eq 0 ]]; then
    echo "check-env-hardening: OK"
fi
exit $status