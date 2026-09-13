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

if [ ! -f "$TRACE" ]; then
    echo "Error: Trace file does not exist: $TRACE" >&2
    exit 1
fi


zns_model="WZS4C8T1TDSP303"
if ! nvme_list="$(nvme list)"; then
    echo "Error: failed to query NVMe namespaces with 'nvme list'." >&2
    exit 1
fi

mapfile -t zns_devices < <(
    awk -v model="$zns_model" \
        '$1 ~ /^\/dev\/nvme[0-9]+n[0-9]+$/ && index($0, model) { print $1 }' \
        <<< "$nvme_list"
)

if [ "${#zns_devices[@]}" -eq 0 ]; then
    echo "Error: no NVMe namespace found for model '$zns_model'." >&2
    exit 1
elif [ "${#zns_devices[@]}" -gt 1 ]; then
    echo "Error: multiple NVMe namespaces found for model '$zns_model': ${zns_devices[*]}" >&2
    exit 1
fi

device="${zns_devices[0]}"

TRACE="$(readlink -f -- "$TRACE")"

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

    out_dir="$RESULT_DIR/hw-zns/$(trace_output_dir "$TRACE")"

    mkdir -p "$out_dir"

    echo "$out_dir"
    shopt -s nullglob
    outputs=( out_real_zns_k*.csv out_real_zns_k*.txt )
    shopt -u nullglob

    if [ "${#outputs[@]}" -gt 0 ]; then
        mv -- "${outputs[@]}" "$out_dir"/
    fi

    if [ -n "${SUDO_UID:-}" ] && [ -n "${SUDO_GID:-}" ]; then
        chown -R "$SUDO_UID:$SUDO_GID" "$out_dir"
    elif [ -n "${SUDO_USER:-}" ]; then
        chown -R "$SUDO_USER:$SUDO_USER" "$out_dir"
    fi

    echo "Experiment outputs: $out_dir"
}

cd "$SCRIPT_DIR"

./run_real_experiment.sh mode=zns device=$device k=4 \
        confirm=YES_I_UNDERSTAND "trace=$TRACE" "scale=$SCALE"

archive_outputs_for_trace