#!/usr/bin/env bash
# Kill leftover FOS QEMU guests and wipe their temp files.
#
# smoke.py / shot.py / qemu_console.Machine often fail to terminate in a
# sandbox, so guests sit around at 128–512 MiB each.
#
#   scripts/qemu_cleanup.sh            # FOS VMs only (default)
#   scripts/qemu_cleanup.sh --dry-run  # print, don't touch
#   scripts/qemu_cleanup.sh --all      # every qemu-system-x86_64
#   make qemu-clean

set -euo pipefail

DRY=0
ALL=0

usage() {
    sed -n '2,12p' "$0" | sed 's/^# \?//'
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run|-n) DRY=1 ;;
        --all|-a)     ALL=1 ;;
        --help|-h)    usage ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

is_fos_qemu() {
    local cmd=$1
    case "$cmd" in
        *boot.img*|*data.img*|*fos-test-*|*fos-qemu-*|*fos-smoke*|*fos-capture-*|*fos-int*|*fosshot*|*fb.img*|*fd.img*|*"$ROOT"*|*/fos/.shot-*|*bochs-display*)
            return 0
            ;;
    esac
    return 1
}

cmd_of() {
    tr '\0' ' ' < "/proc/$1/cmdline" 2>/dev/null || true
}

list_qemu_pids() {
    local pid cmd
    for pid in /proc/[0-9]*; do
        pid=${pid#/proc/}
        [ -r "/proc/$pid/cmdline" ] || continue
        cmd=$(cmd_of "$pid")
        case "$cmd" in
            *qemu-system-x86_64*)
                if [ "$ALL" -eq 1 ] || is_fos_qemu "$cmd"; then
                    printf '%s\n' "$pid"
                fi
                ;;
        esac
    done
}

list_wrapper_pids() {
    local pid cmd
    for pid in /proc/[0-9]*; do
        pid=${pid#/proc/}
        [ -r "/proc/$pid/cmdline" ] || continue
        cmd=$(cmd_of "$pid")
        case "$cmd" in
            *qemu_cleanup.sh*) continue ;;
        esac
        case "$cmd" in
            *scripts/smoke.py*|*scripts/shot.py*|*scripts/audio_check.py*|*qemu_console*)
                printf '%s\n' "$pid"
                ;;
        esac
    done
}

kill_pids() {
    local pids=$1
    local pid
    [ -n "$pids" ] || return 0
    if [ "$DRY" -eq 1 ]; then
        for pid in $pids; do
            echo "would kill $pid  $(cmd_of "$pid" | cut -c1-120)"
        done
        return 0
    fi
    kill -TERM $pids 2>/dev/null || true
    sleep 1
    for pid in $pids; do
        if [ -d "/proc/$pid" ]; then
            kill -KILL "$pid" 2>/dev/null || true
        fi
    done
}

QEMU_PIDS=$(list_qemu_pids | sort -u | tr '\n' ' ')
WRAP_PIDS=$(list_wrapper_pids | sort -u | tr '\n' ' ')

n_qemu=0
n_wrap=0
[ -n "${QEMU_PIDS// }" ] && n_qemu=$(echo "$QEMU_PIDS" | wc -w)
[ -n "${WRAP_PIDS// }" ] && n_wrap=$(echo "$WRAP_PIDS" | wc -w)

echo "FOS QEMU leftovers: $n_qemu guest(s), $n_wrap wrapper(s)"
kill_pids "$QEMU_PIDS"
kill_pids "$WRAP_PIDS"

# Temp junk from qemu_console / smoke / shot / audio_check / ad-hoc runs.
# Catch-all fos-* : ad-hoc -D /tmp/fos-midi-int.log etc. are multi-GB and
# were missed by the old fos-int*.log / fos-smoke-int.log list.
TMP="${TMPDIR:-/tmp}"
JUNK=(
    "$TMP"/fos-*
    "$TMP"/fos_*
    /tmp/fb.img
    /tmp/fd.img
    "$ROOT"/fos-sb16.wav
    "$ROOT"/.shot-*
)

shopt -s nullglob
cleaned=0
for path in "${JUNK[@]}"; do
    # Unquoted so globs in JUNK expand.
    for f in $path; do
        [ -e "$f" ] || continue
        if [ "$DRY" -eq 1 ]; then
            echo "would rm $f"
        else
            rm -rf -- "$f"
        fi
        cleaned=$((cleaned + 1))
    done
done
shopt -u nullglob

echo "temp files: $cleaned"
if [ "$DRY" -eq 0 ]; then
    leftover=$(ps -eo cmd | awk '/[q]emu-system-x86_64/' | wc -l)
    echo "qemu-system-x86_64 still running: $leftover"
    free -h | awk 'NR==2 {print "ram:", $3, "used /", $2, "total,", $7, "available"}'
fi
