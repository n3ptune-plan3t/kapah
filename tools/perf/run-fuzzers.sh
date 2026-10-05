#!/usr/bin/env bash
# run-fuzzers.sh -- build-and-run smoke for the libFuzzer targets.
#
# In CI this does not attempt to find crashes (that takes hours and belongs on
# a dedicated fuzzing machine, see docs/PROTOCOLS.md). It only proves the targets
# link, that the corpus seeds are accepted, and that a sanitized build survives
# a few thousand executions.
#
# Locally, pass --long to actually fuzz:
#   tools/perf/run-fuzzers.sh --long 600
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
RUNS=2000
JOBS="$(nproc 2>/dev/null || echo 2)"

if [[ "${1:-}" == "--long" ]]; then
    RUNS="${2:-600}"
fi

mapfile -t targets < <(find "$BUILD" -maxdepth 3 -type f \( -name 'fuzz_*' -o -name '*_fuzz' \) 2>/dev/null | sort)

if [[ ${#targets[@]} -eq 0 ]]; then
    echo "run-fuzzers: no fuzz targets found; configure with -DKAPAH_ENABLE_FUZZERS=ON" >&2
    exit 2
fi

status=0

for t in "${targets[@]}"; do
    name="$(basename "$t")"
    # Corpus directory is derived from the target name: fuzz_toml -> fuzz/toml
    corpus="$ROOT/tests/fuzz/corpus/$name"
    mkdir -p "$corpus"

    echo "== $name (seed corpus + $RUNS execs) =="
    if [[ ! -d "$corpus" ]] || [[ -z "$(ls -A "$corpus" 2>/dev/null)" ]]; then
        # No seeds yet: run once on empty input to prove the harness works.
        if ! "$t" -runs=1 -max_len=1 </dev/null >/dev/null 2>"$BUILD/$name.seed.log"; then
            echo "FAIL: $name failed on trivial input" >&2
            sed 's/^/    /' "$BUILD/$name.seed.log" >&2
            status=1
            continue
        fi
    fi

    if ! "$t" -runs="$RUNS" -max_len=65536 -timeout=5 -rss_limit_mb=2048 \
            -jobs="$JOBS" -workers="$JOBS" "$corpus" \
            >"$BUILD/$name.log" 2>&1; then
        echo "FAIL: $name crashed" >&2
        tail -60 "$BUILD/$name.log" | sed 's/^/    /' >&2
        status=1
        continue
    fi

    # A crash artifact is written next to the corpus as crash-<hash>.
    if compgen -G "$corpus/crash-*" >/dev/null; then
        echo "FAIL: $name left crash artifacts in $corpus" >&2
        ls -la "$corpus"/crash-* | sed 's/^/    /' >&2
        status=1
        continue
    fi

    echo "   ok"
done

exit $status