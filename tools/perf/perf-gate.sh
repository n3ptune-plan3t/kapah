#!/usr/bin/env bash
# perf-gate.sh -- CI gate. Fails when PSS or wakeups exceed the roadmap budgets
# by more than 10%.
#
# Run under a real (software-rendered) niri, or under the Xvfb + niri nested
# setup that tests/integration/run-all.sh prepares.
#
# Usage:
#   tools/perf/perf-gate.sh [--binary build/bin/kapahd] [--tolerance 10]
#   tools/perf/perf-gate.sh --offline    # compare against docs/perf-baseline.json
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BINARY="$ROOT/build/bin/kapahd"
TOLERANCE=10
OFFLINE=0
SETTLE=30
IDLE_WINDOW=60
NIRI_BIN="${NIRI_BIN:-niri}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --binary)    BINARY="$2"; shift 2 ;;
        --tolerance) TOLERANCE="$2"; shift 2 ;;
        --offline)   OFFLINE=1; shift ;;
        --settle)    SETTLE="$2"; shift 2 ;;
        --idle)      IDLE_WINDOW="$2"; shift 2 ;;
        -h|--help)   sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "perf-gate.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

if [[ "$OFFLINE" == "1" ]]; then
    BASELINE="$ROOT/docs/perf-baseline.json"
    if [[ ! -f "$BASELINE" ]]; then
        echo "perf-gate: no baseline at $BASELINE; nothing to compare" >&2
        exit 0
    fi
    python3 "$ROOT/tools/perf/check_budgets.py" \
        --baseline "$BASELINE" \
        --actual "$ROOT/docs/perf-current.json" \
        --tolerance "$TOLERANCE"
    exit $?
fi

if [[ ! -x "$BINARY" ]]; then
    echo "perf-gate: $BINARY is not executable; build first" >&2
    exit 2
fi

WORKDIR=$(mktemp -d)
NIRI_LOG="$WORKDIR/niri.log"
SHELL_LOG="$WORKDIR/kapahd.log"
NIRI_SOCK="$WORKDIR/niri.sock"
export XDG_RUNTIME_DIR="$WORKDIR/run"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

cleanup() {
    [[ -n "${SHELL_PID:-}" ]] && kill "$SHELL_PID" 2>/dev/null || true
    [[ -n "${NIRI_PID:-}" ]] && kill "$NIRI_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

echo "== starting niri (software rendering) =="
LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe WLR_BACKENDS=headless \
    "$NIRI_BIN" --session --debug-config=off >"$NIRI_LOG" 2>&1 &
NIRI_PID=$!

for _ in $(seq 1 100); do
    if [[ -S "$NIRI_SOCK" ]]; then break; fi
    sleep 0.1
done
if [[ ! -S "$NIRI_SOCK" ]]; then
    echo "perf-gate: niri did not create its socket" >&2
    tail -40 "$NIRI_LOG" >&2
    exit 2
fi

echo "== starting kapahd =="
export NIRI_SOCKET="$NIRI_SOCK"
export KAPAH_LOG=all
"$BINARY" --perf-logging >"$SHELL_LOG" 2>&1 &
SHELL_PID=$!

echo "== settling ${SETTLE}s =="
sleep "$SETTLE"

echo "== scenario 1/4: idle, bar only =="
"$ROOT/tools/perf/rss.sh" "$SHELL_PID" --json > "$WORKDIR/m_bar.json"

echo "== scenario 2/4: idle wakeups over ${IDLE_WINDOW}s =="
"$ROOT/tools/perf/wakeups.sh" "$SHELL_PID" --seconds "$IDLE_WINDOW" --json \
    > "$WORKDIR/w_idle.json"

echo "== scenario 3/4: launcher churn (10 open/close cycles) =="
CTL="$ROOT/build/bin/kapahctl"
if [[ -x "$CTL" ]]; then
    for _ in $(seq 1 10); do
        "$CTL" launcher show >/dev/null 2>&1 || true
        sleep 0.4
        "$CTL" launcher hide >/dev/null 2>&1 || true
        sleep 0.4
    done
else
    echo "   (kapahctl not built; skipping launcher churn)"
fi
sleep 32  # launcher destroy-grace + trim
"$ROOT/tools/perf/rss.sh" "$SHELL_PID" --json > "$WORKDIR/m_after_launcher.json"

echo "== scenario 4/4: 100 notifications =="
if command -v notify-send >/dev/null 2>&1; then
    for i in $(seq 1 100); do
        notify-send -a perf-gate "Message $i" "Body line for message $i" >/dev/null 2>&1 || true
        sleep 0.05
    done
else
    echo "   (notify-send not installed; skipping)"
fi
sleep 3
"$ROOT/tools/perf/rss.sh" "$SHELL_PID" --json > "$WORKDIR/m_after_notify.json"

# Assemble an "actual" document in the same shape as the baseline.
python3 - "$WORKDIR" > "$WORKDIR/actual.json" <<'PY'
import json, sys, os
work = sys.argv[1]

def load(name):
    path = os.path.join(work, name)
    if not os.path.exists(path):
        return None
    with open(path) as fh:
        return json.load(fh)

m_bar = load("m_bar.json")
w_idle = load("w_idle.json")
m_after = load("m_after_launcher.json")
m_notif = load("m_after_notify.json")

doc = {
    "scenarios": {
        "idle-bar": {"pss_kb": (m_bar or {}).get("pss_kb")},
        "idle-wakeups": {
            "voluntary_per_sec": (w_idle or {}).get("voluntary_per_sec"),
            "cpu_ms": (w_idle or {}).get("cpu_ms"),
        },
        "after-launcher": {"pss_kb": (m_after or {}).get("pss_kb")},
        "after-notifications": {"pss_kb": (m_notif or {}).get("pss_kb")},
    },
    "env": {},
}
if m_bar:
    doc["env"] = {k: m_bar.get(k) for k in ("name", "threads", "fds")}
print(json.dumps(doc))
PY

echo "== comparing against docs/perf-baseline.json =="
BASELINE="$ROOT/docs/perf-baseline.json"
if [[ ! -f "$BASELINE" ]]; then
    cp "$WORKDIR/actual.json" "$BASELINE"
    echo "no baseline existed; wrote $BASELINE with this run's numbers."
    echo "Review them, commit them, and re-run to get a real gate."
    exit 0
fi

python3 "$ROOT/tools/perf/check_budgets.py" \
    --baseline "$BASELINE" \
    --actual "$WORKDIR/actual.json" \
    --tolerance "$TOLERANCE"