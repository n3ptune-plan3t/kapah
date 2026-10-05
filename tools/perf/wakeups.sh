#!/usr/bin/env bash
# wakeups.sh -- how often does the shell make the CPU do work while nothing is
# happening?
#
# Roadmap section 11. Three tiers, most-accurate first:
#
#   1. perf stat -e task-clock,context-switches,cpu-migrations,page-faults -p <pid>
#      Over a window. task-clock delta is CPU jiffies consumed, context-switches
#      is voluntary+involuntary, page-faults shows churn.
#
#   2. /proc/<pid>/status voluntary_ctxt_switches delta. Always available, needs
#      no privileges, and is the number the CI gate uses. It is the *ceiling*:
#      an epoll_wait that returns EAGAIN immediately shows up here but costs no
#      actual wakeup.
#
#   3. powertop --csv, on a laptop, attributing wakeups to the process. Slow
#      (needs a measurement window) and not available in CI; included for the
#      manual PERF.md runs.
#
# Usage:
#   wakeups.sh <pid> [--seconds 60] [--json]
#   wakeups.sh --process kapahd --seconds 60
#   wakeups.sh --pid-from-file /run/kapahd.pid --seconds 60
#   wakeups.sh --powertop --pid 1234
set -euo pipefail

SECONDS_WINDOW=60
PID=""
JSON=0
USE_PERF=auto
USE_POWERTOP=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --seconds)   SECONDS_WINDOW="$2"; shift 2 ;;
        --pid)       PID="$2"; shift 2 ;;
        --process)   PID=$(pgrep -x -- "${2:-kapahd}" | head -1); shift 2 ;;
        --pid-from-file) PID=$(cat "$2"); shift 2 ;;
        --powertop)  USE_POWERTOP=1; shift ;;
        --no-perf)   USE_PERF=0; shift ;;
        --json)      JSON=1; shift ;;
        -h|--help)   sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "wakeups.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

if [[ -z "$PID" ]]; then
    echo "wakeups.sh: no pid given" >&2
    exit 2
fi
if [[ ! -d "/proc/$PID" ]]; then
    echo "wakeups.sh: no such process: $PID" >&2
    exit 1
fi

if [[ "$USE_PERF" == "auto" ]]; then
    if command -v perf >/dev/null 2>&1 && perf stat -e task-clock -p "$$" 2>&1 | grep -q 'not supported\|No permission\|access'; then
        USE_PERF=0
    elif ! command -v perf >/dev/null 2>&1; then
        USE_PERF=0
    fi
fi

# ---------------------------------------------------------------------------
# powertop tier
# ---------------------------------------------------------------------------
if [[ "$USE_POWERTOP" == "1" ]]; then
    if ! command -v powertop >/dev/null 2>&1; then
        echo "wakeups.sh: powertop not installed" >&2
        exit 1
    fi
    echo "# powertop needs to own the machine for the whole window; run it as root." >&2
    exec powertop --csv --time "$((SECONDS_WINDOW / 2))" >/dev/null
fi

# ---------------------------------------------------------------------------
# /proc-based sampling
# ---------------------------------------------------------------------------
read_cs() {
    awk -v k="$1" '
        $1 == k { print $2; found = 1; exit }
        END { if (!found) print 0 }
    ' "/proc/$PID/status"
}

snap() {
    local vol nonvol utime stime minflt majflt threads
    vol=$(read_cs voluntary_ctxt_switches)
    nonvol=$(read_cs nonvoluntary_ctxt_switches)
    threads=$(awk '/^Threads:/ {print $2}' "/proc/$PID/status")
    minflt=$(awk '/^minflt:/ {print $2}' "/proc/$PID/status")
    majflt=$(awk '/^majflt:/ {print $2}' "/proc/$PID/status")
    # utime/stime are in clock ticks (usually 100 Hz).
    read -r utime stime < <(awk '/^utime:/ {u=$2} /^stime:/ {s=$2} END {print u+0, s+0}' "/proc/$PID/status")
    echo "$vol $nonvol $utime $stime $minflt $majflt $threads"
}

v0=$(read_cs voluntary_ctxt_switches)
n0=$(read_cs nonvoluntary_ctxt_switches)
read -r _ _ u0 s0 pf0 pf1 _ <<< "$(snap)"
t_start=$(date +%s.%N)

echo "# sampling pid=$PID for ${SECONDS_WINDOW}s" >&2
sleep "$SECONDS_WINDOW"

v1=$(read_cs voluntary_ctxt_switches)
n1=$(read_cs nonvoluntary_ctxt_switches)
read -r _ _ u1 s1 qf0 qf1 threads <<< "$(snap)"
t_end=$(date +%s.%N)

elapsed=$(echo "$t_end - $t_start" | bc -l)

vol_delta=$((v1 - v0))
nonvol_delta=$((n1 - n0))
jiffies=$(( (u1 - u0) + (s1 - s0) ))
# CLK_TCK is almost always 100 but read it rather than assume.
clk_tck=$(getconf CLK_TCK 2>/dev/null || echo 100)
cpu_ms=$(echo "scale=3; $jiffies * 1000 / $clk_tck" | bc -l)
cpu_pct=$(echo "scale=4; $jiffies * 100 / ($clk_tck * $elapsed)" | bc -l)
vol_rate=$(echo "scale=4; $vol_delta / $elapsed" | bc -l)
minflt_delta=$((qf0 - pf0))

# ---------------------------------------------------------------------------
# perf tier (extra detail when available)
# ---------------------------------------------------------------------------
perf_out=""
if [[ "$USE_PERF" == "1" ]]; then
    perf_out=$(perf stat -e task-clock,context-switches,cpu-migrations,page-faults \
        -p "$PID" -- sleep "$SECONDS_WINDOW" 2>&1 | sed 's/^/    /' || true)
    # The /proc numbers above were sampled over the same window; perf ran after
    # it. We report perf separately rather than mixing the windows.
fi

if [[ "$JSON" == "1" ]]; then
    printf '{"pid":%s,"seconds":%s,"cpu_ms":%s,"cpu_percent":%s,"voluntary_ctxt_switches":%s,"nonvoluntary_ctxt_switches":%s,"voluntary_per_sec":%s,"minor_faults":%s,"threads":%s}\n' \
        "$PID" "$elapsed" "$cpu_ms" "$cpu_pct" "$vol_delta" "$nonvol_delta" "$vol_rate" \
        "$minflt_delta" "$threads"
else
    echo "process              $(awk '/^Name:/ {print $2}' "/proc/$PID/status") ($PID)"
    echo "window               ${elapsed}s"
    printf 'cpu                  %s ms  (%s%% of one core)\n' "$cpu_ms" "$cpu_pct"
    printf 'voluntary ctxsw      %s  (%s /s)\n' "$vol_delta" "$vol_rate"
    printf 'involuntary ctxsw    %s\n' "$nonvol_delta"
    printf 'minor faults         %s\n' "$minflt_delta"
    printf 'threads              %s\n' "$threads"
    if [[ -n "$perf_out" ]]; then
        echo "perf:"
        echo "$perf_out"
    fi
    echo
    echo "# Budget (roadmap section 0/11):"
    echo "#   cpu            ~0 ms per minute"
    echo "#   voluntary/s    < 0.1 excluding the once-a-minute clock tick"
fi