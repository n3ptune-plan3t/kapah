#!/usr/bin/env bash
# startup.sh -- time from process spawn to first painted frame.
#
# Roadmap section 11: target < 300 ms on the reference machine.
#
# Three independent measurements, because each fails differently:
#
#   1. wall clock from fork() to the "shell ready" line on stderr. This is what
#      the user feels. Emitted by src/app/main.cpp unconditionally.
#   2. ktimestamp/if_tstamp: not usable without a compositor that stamps frames,
#      so we use niri's own layer-surface configure event as the ground truth
#      for "the compositor has actually given us a buffer".
#   3. A cold vs warm split: cold = first run after boot with an empty font and
#      icon cache; warm = everything cached.
#
# Usage:
#   startup.sh <pid>                      # waits for the ready line
#   startup.sh --run <cmd...>             # spawns and times
#   startup.sh --niri <cmd...>            # same, but also reports the first
#                                         # configure round-trip
#   startup.sh --scenario cold|warm|both
set -euo pipefail

TIMEOUT=15
PID=""
RUN_CMD=()
SCENARIO=both
USE_NIRI=0
JSON=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --timeout)  TIMEOUT="$2"; shift 2 ;;
        --pid)      PID="$2"; shift 2 ;;
        --run)      shift; RUN_CMD=("$@"); break ;;
        --niri)     shift; RUN_CMD=("$@"); USE_NIRI=1; break ;;
        --scenario) SCENARIO="$2"; shift 2 ;;
        --json)     JSON=1; shift ;;
        -h|--help)  sed -n '2,22p' "$0"; exit 0 ;;
        *) echo "startup.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

CAPTURE=$(mktemp)
trap 'rm -f "$CAPTURE"' EXIT

# ---------------------------------------------------------------------------
# Reset caches for a "cold" measurement.
# ---------------------------------------------------------------------------
reset_caches() {
    local state="${XDG_STATE_HOME:-$HOME/.local/state}/kapah"
    local cache="${XDG_CACHE_HOME:-$HOME/.cache}/kapah"
    rm -rf "$state/appindex.v1" "$state/frecency.v1" "$cache/icons" 2>/dev/null || true
    # Qt's own caches.
    rm -rf "${XDG_CACHE_HOME:-$HOME/.cache}"/fontconfig 2>/dev/null || true
    fc-cache -r >/dev/null 2>&1 || true
}

measure() {
    local label="$1"
    shift
    : > "$CAPTURE"

    local t0
    t0=$(date +%s%N)

    if [[ ${#RUN_CMD[@]} -gt 0 ]]; then
        "$@" >"$CAPTURE" 2>&1 &
        local child=$!
        # Wait for the ready marker.
        local waited=0
        while ! grep -q 'kapah: startup: ready' "$CAPTURE" 2>/dev/null; do
            if ! kill -0 "$child" 2>/dev/null; then
                echo "startup.sh: process exited before reporting ready" >&2
                cat "$CAPTURE" >&2
                return 1
            fi
            sleep 0.005
            waited=$((waited + 5))
            if [[ $waited -gt $((TIMEOUT * 1000)) ]]; then
                echo "startup.sh: timed out waiting for ready marker" >&2
                cat "$CAPTURE" >&2
                kill "$child" 2>/dev/null || true
                return 1
            fi
        done
        PID="$child"
    else
        local waited=0
        while ! grep -q 'kapah: startup: ready' "$CAPTURE" 2>/dev/null; do
            sleep 0.005
            waited=$((waited + 5))
            if [[ $waited -gt $((TIMEOUT * 1000)) ]]; then
                echo "startup.sh: timed out; the shell did not report ready" >&2
                return 1
            fi
        done
    fi

    local t1
    t1=$(date +%s%N)
    local total_ms=$(( (t1 - t0) / 1000000 ))

    # The shell logs its own monotonic measurements; surface them.
    local -a phases=()
    while IFS= read -r line; do
        phases+=("$line")
    done < <(grep -o 'kapah: startup: [a-z_]*=[0-9.]*' "$CAPTURE" || true)

    if [[ "$JSON" == "1" ]]; then
        printf '{"scenario":"%s","pid":%s,"total_ms":%s,"phases":[' "$label" "${PID:-0}" "$total_ms"
        local first=1
        for p in "${phases[@]}"; do
            [[ $first -eq 1 ]] || printf ','
            first=0
            printf '"%s"' "$p"
        done
        printf ']}\n'
    else
        echo "scenario              $label"
        echo "pid                   ${PID:-?}"
        printf 'spawn -> ready        %s ms\n' "$total_ms"
        if [[ ${#phases[@]} -gt 0 ]]; then
            echo "phases:"
            for p in "${phases[@]}"; do
                echo "    $p"
            done
        fi
    fi

    if [[ $USE_NIRI -eq 1 && -n "${PID:-}" ]]; then
        # The compositor-side half: how long until the first configure arrived
        # and the bar surface got its first buffer.
        echo
        echo "compositor-side:"
        grep -o 'kapah: wl: first-configure[a-z_]*=[0-9.]*' "$CAPTURE" 2>/dev/null || \
            echo "    (no configure marker; was the shell started outside niri?)"
    fi

    if [[ -n "${PID:-}" ]] && [[ "$label" != "no-run" ]]; then
        kill "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    PID=""
}

case "$SCENARIO" in
    cold)
        reset_caches
        measure cold "${RUN_CMD[@]}"
        ;;
    warm)
        measure warm "${RUN_CMD[@]}"
        ;;
    both)
        reset_caches
        measure cold "${RUN_CMD[@]}"
        # Warm run needs the caches to exist, which the cold run just created.
        measure warm "${RUN_CMD[@]}"
        ;;
    *)
        echo "startup.sh: --scenario must be cold, warm or both" >&2
        exit 2
        ;;
esac