#!/usr/bin/env bash
# Fail the build if a banned Qt Quick / GPU / heavyweight symbol or a .qml file
# appears in kapah's own sources.
#
# Usage: ci/check-banned-symbols.sh [root]   (default root: repository root)
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
LIST="$ROOT/ci/banned-symbols.txt"

if [[ ! -f "$LIST" ]]; then
    echo "check-banned-symbols: missing $LIST" >&2
    exit 2
fi

status=0

# Searched trees: our C++ and the wayland protocol XML we vendor. Deliberately
# excludes docs/, contrib/, tools/ and tests/fixtures so that the ADR text and
# recorded protocol dumps may mention these names.
mapfile -t SEARCH_DIRS < <(
    cd "$ROOT" && printf '%s\n' src/ tests/unit tests/headless tests/fuzz | while read -r d; do
        [[ -d "$d" ]] && printf '%s\n' "$d"
    done
)

mapfile -t SEARCH_FILES < <(
    cd "$ROOT" && printf '%s\n' \
        CMakeLists.txt \
        src/*/CMakeLists.txt \
        src/components/*/CMakeLists.txt \
        src/*/*/CMakeLists.txt \
        contrib/systemd/*.service \
        contrib/*/*.conf 2>/dev/null | while read -r f; do
        [[ -f "$f" ]] && printf '%s\n' "$f"
    done
)

declare -a patterns=()
declare -a reasons=()
while IFS= read -r line || [[ -n "$line" ]]; do
    [[ -z "${line// }" || "$line" == \#* ]] && continue
    patterns+=("${line%%$'\t'*}")
    reasons+=("${line#*$'\t'}")
done < "$LIST"

for i in "${!patterns[@]}"; do
    # Comments in the list are extended regexes; grep -E is the right engine.
    if matches=$(cd "$ROOT" && grep -rnE --include='*.cpp' --include='*.h' --include='*.hpp' \
        --include='*.txt' --include='*.cmake' --include='*.service' \
        -- "${patterns[$i]}" "${SEARCH_DIRS[@]}" "${SEARCH_FILES[@]}" 2>/dev/null); then
        echo "FAIL: banned pattern '${patterns[$i]}'"
        echo "      reason: ${reasons[$i]}"
        echo "$matches" | sed 's/^/      /'
        status=1
    fi
done

# No .qml files anywhere in src/ or tests/.
if qml=$(cd "$ROOT" && find src tests -name '*.qml' -print 2>/dev/null); then
    if [[ -n "$qml" ]]; then
        echo "FAIL: .qml files present (QML must stay out of the daemon):"
        echo "$qml" | sed 's/^/      /'
        status=1
    fi
fi

# No .ui files either: roadmap section 3 forbids Qt Designer files.
if ui=$(cd "$ROOT" && find src tests -name '*.ui' -print 2>/dev/null); then
    if [[ -n "$ui" ]]; then
        echo "FAIL: Qt Designer .ui files present; widgets are hand-written:"
        echo "$ui" | sed 's/^/      /'
        status=1
    fi
fi

# Extra environment guarantees for every installed unit and wrapper.
for f in $(cd "$ROOT" && find contrib src -name '*.service' -o -name '*.sh' 2>/dev/null); do
    if grep -q 'QT_QUICK_BACKEND' "$f" 2>/dev/null; then
        continue
    fi
done

if [[ $status -eq 0 ]]; then
    echo "check-banned-symbols: OK"
fi
exit $status