#!/bin/bash

if [ "$#" -ne 1 ] || [ -z "$1" ]; then
    echo "Usage: sudo $0 <trace_file>" >&2
    exit 1
fi

TRACE="$1"
SCALE="1"

# Check if effective user ID is 0
if [ "$(id -u)" -ne 0 ]; then
    echo "Error: This script must be run with sudo or as root." >&2
    exit 1
fi

if [ ! -f "$TRACE" ]; then
    echo "Error: Trace file does not exist: $TRACE" >&2
    exit 1
fi

TRACE="$(readlink -f -- "$TRACE")"

get_nvme_namespace() {
    local model="$1"

    nvme list | awk -v model="$model" '
        index($0, model) {
            print $1
            exit
        }
    '
}

device=$(get_nvme_namespace "WD Blue SN570 500GB")

./run_real_nvme_trim_ab.sh device="$device" confirm=YES_I_UNDERSTAND trace="$TRACE"