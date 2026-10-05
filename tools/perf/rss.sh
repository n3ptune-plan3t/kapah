#!/usr/bin/env bash
# rss.sh -- memory accounting for a running kapah process.
#
# Roadmap section 11: report PSS from /proc/<pid>/smaps_rollup, not RSS, because
# libQt6*.so is mapped into every process on the desktop and inflates RSS
# without costing anything real. Also print Private_Dirty and Anonymous, which
# are the numbers that actually move when the launcher allocates.
#
# Usage:
#   rss.sh <pid> [--json]
#   rss.sh --process kapahd
#   rss.sh --all-kapah --json
set -euo pipefail

FIELD="${1:-}"

read_rollup() {
    local pid="$1"
    # smaps_rollup is the cheap interface; fall back to smaps for kernels older
    # than 4.14, where the file does not exist.
    local f="/proc/$pid/smaps_rollup"
    if [[ -r "$f" ]]; then
        cat "$f"
    else
        awk '/^(Rss|Pss|Private_Dirty|Private_Clean|Anonymous|Shared_Clean|Shared_Dirty):/ {print}' \
            "/proc/$pid/smaps"
    fi
}

read_status() {
    local pid="$1"
    awk '
        /^Name:/            { name = $2 }
        /^VmRSS:/           { rss = $2 }
        /^RssAnon:/         { anon = $2 }
        /^RssFile:/         { file = $2 }
        /^RssShmem:/        { shmem = $2 }
        /^Threads:/         { threads = $2 }
        /^FDSize:/          { fdsize = $2 }
        /^voluntary_ctxt_switches:/   { vol = $2 }
        /^nonvoluntary_ctxt_switches:/ { nonvol = $2 }
        END {
            printf "name=%s rss_kb=%s anon_kb=%s file_kb=%s shmem_kb=%s threads=%s fds=%s vol_cs=%s nonvol_cs=%s\n", \
                name, rss, anon, file, shmem, threads, fdsize, vol, nonvol
        }
    ' "/proc/$pid/status"
}

count_fds() {
    local pid="$1"
    local n
    n=$(ls "/proc/$pid/fd" 2>/dev/null | wc -l) || n=0
    echo "$n"
}

print_pid() {
    local pid="$1" json="$2"
    if [[ ! -d "/proc/$pid" ]]; then
        echo "rss.sh: no such process: $pid" >&2
        return 1
    fi

    local pss pss_dirty pss_anon pss_shared rss priv_dirty priv_clean anon shared
    pss=$(awk '/^Pss:/ {s+=$2} END {print s+0}'          <(read_rollup "$pid"))
    pss_dirty=$(awk '/^Pss_Dirty:/ {s+=$2} END {print s+0}' <(read_rollup "$pid"))
    pss_anon=$(awk '/^Pss_Anon:/ {s+=$2} END {print s+0}'   <(read_rollup "$pid"))
    pss_shared=$(awk '/^Pss_Shared:/ {s+=$2} END {print s+0}' <(read_rollup "$pid"))
    rss=$(awk '/^Rss:/ {s+=$2} END {print s+0}'            <(read_rollup "$pid"))
    priv_dirty=$(awk '/^Private_Dirty:/ {s+=$2} END {print s+0}' <(read_rollup "$pid"))
    priv_clean=$(awk '/^Private_Clean:/ {s+=$2} END {print s+0}' <(read_rollup "$pid"))
    anon=$(awk '/^Anonymous:/ {s+=$2} END {print s+0}'     <(read_rollup "$pid"))
    shared=$(awk '/^Shared:/ {s+=$2} END {print s+0}'       <(read_rollup "$pid"))

    local status_line fds
    status_line=$(read_status "$pid")
    fds=$(count_fds "$pid")

    if [[ "$json" == "json" ]]; then
        printf '{"pid":%s,%s,"fds":%s,"pss_kb":%s,"pss_dirty_kb":%s,"pss_anon_kb":%s,"pss_shared_kb":%s,"rss_kb":%s,"private_dirty_kb":%s,"private_clean_kb":%s,"anon_kb":%s,"shared_kb":%s}\n' \
            "$pid" "$status_line" "$fds" "$pss" "$pss_dirty" "$pss_anon" "$pss_shared" \
            "$rss" "$priv_dirty" "$priv_clean" "$anon" "$shared"
    else
        echo "pid          $pid"
        # shellcheck disable=SC2086
        echo "$status_line" | tr ' ' '\n' | sed 's/^/  /'
        echo "fds          $fds"
        printf 'pss          %s kB (%.1f MB)\n' "$pss" "$(echo "$pss/1024" | bc -l)"
        printf 'pss_dirty    %s kB\n' "$pss_dirty"
        printf 'pss_anon     %s kB\n' "$pss_anon"
        printf 'pss_shared   %s kB\n' "$pss_shared"
        printf 'private_dirty %s kB\n' "$priv_dirty"
        printf 'private_clean %s kB\n' "$priv_clean"
        printf 'anon         %s kB\n' "$anon"
        printf 'shared       %s kB\n' "$shared"
    fi
}

JSON=0
PID=""
TARGET=""

for arg in "$@"; do
    case "$arg" in
        --json) JSON=1 ;;
        --process) TARGET="process" ;;
        --all-kapah) TARGET="all" ;;
        [0-9]*) PID="$arg" ;;
    esac
done

case "$TARGET" in
    process)
        mapfile -t pids < <(pgrep -x -- "${PROC_NAME:-kapahd}" || true)
        if [[ ${#pids[@]} -eq 0 ]]; then
            echo "rss.sh: no process named ${PROC_NAME:-kapahd}" >&2
            exit 1
        fi
        for p in "${pids[@]}"; do print_pid "$p" "$JSON"; done
        ;;
    all)
        mapfile -t pids < <(pgrep -f -- '(^|/)(kapahd|kapah-lock)( |$)' || true)
        if [[ ${#pids[@]} -eq 0 ]]; then
            echo "rss.sh: no kapah processes" >&2
            exit 1
        fi
        total=0
        for p in "${pids[@]}"; do
            print_pid "$p" "$JSON"
            total=$(( total + $(awk '/^Pss:/ {s+=$2} END {print s+0}' <(read_rollup "$p")) ))
        done
        if [[ "$JSON" == "0" ]]; then
            printf 'TOTAL pss    %s kB (%.1f MB)\n' "$total" "$(echo "$total/1024" | bc -l)"
        fi
        ;;
    *)
        if [[ -z "$PID" ]]; then
            echo "usage: rss.sh <pid>|--process [name]|--all-kapah [--json]" >&2
            exit 2
        fi
        print_pid "$PID" "$JSON"
        ;;
esac