#!/bin/bash

if [ "$#" -ne 1 ] || [ -z "$1" ]; then
    echo "Usage: sudo $0 <trace_file>" >&2
    exit 1
fi

TRACE="$1"
SCALE="1"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
RESULT_DIR="${RESULT_DIR:-$REPO_ROOT/results}"

# Check if effective user ID is 0
if [ "$(id -u)" -ne 0 ]; then
    echo "Error: This script must be run with sudo or as root." >&2
    exit 1
fi

trace_output_dir() {
    local trace_file
    local trace_prefix

    trace_file="$(basename -- "$1")"
    trace_prefix="${trace_file%.*}"

    if [ -z "$trace_prefix" ] || [ "$trace_prefix" = "$trace_file" ]; then
        trace_prefix="$trace_file"
    fi

    printf "%s_%s\n" "$(date +%Y-%m-%d)" "$trace_prefix"
}

archive_outputs_for_trace() {
    local out_dir
    local -a outputs

    out_dir="$(trace_output_dir "$TRACE")"

    out_dir="$RESULT_DIR/$out_dir"

    mkdir -p "$out_dir"

    shopt -s nullglob
    outputs=( *.csv *.txt )
    shopt -u nullglob

    if [ "${#outputs[@]}" -gt 0 ]; then
        mv -- "${outputs[@]}" "$out_dir"/
    fi

    if [ -n "${SUDO_UID:-}" ] && [ -n "${SUDO_GID:-}" ]; then
        chown -R "$SUDO_UID:$SUDO_GID" "$out_dir"
    elif [ -n "${SUDO_USER:-}" ]; then
        chown -R "$SUDO_USER:$SUDO_USER" "$out_dir"
    fi
}

./run_experiment.sh mode=zns size=40GiB k=8 "trace=$TRACE" scale=$SCALE

./run_experiment.sh mode=conv size=40GiB "trace=$TRACE" scale=$SCALE

archive_outputs_for_trace