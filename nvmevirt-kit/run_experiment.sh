#!/usr/bin/env bash

set -euo pipefail

# Check if effective user ID is 0
if [ "$(id -u)" -ne 0 ]; then
    echo "Error: This script must be run with sudo or as root." >&2
    exit 1
fi

# -----------------------------
# Default configuration
# -----------------------------
MODE=""
SIZE=""
K=""

MEMORY_START="8G"
CPU_LIST="0,1"
DEVICE="/dev/nvme3n1"
TRACE="../traces/tr_occ90_10.txt"
SCALE="0.25"
OUTPUT=""
LOAD_SIZE=""
STAT_FILE=""

usage() {
    cat <<EOF
Usage:
  sudo $0 mode=zns size=<XGiB> k=<number>
  sudo $0 mode=conv size=<XGiB>

Optional arguments:
  device=/dev/nvme3n1
  trace=../traces/tr_occ90.txt
  output=output_directory
  memory=8GiB
  cpus=0,1
  scale=0.25

Examples:
    sudo $0 mode=zns size=32GiB k=8
    sudo $0 mode=conv size=32GiB
    sudo $0 mode=zns size=32GiB k=16 output=out_zns_k16
EOF
}

join_csv() {
    local IFS=','
    printf '%s\n' "$*"
}

save_block_stats() {
    local stat_path="/sys/block/${BLOCK_DEVICE}/stat"

    local -a field_names=(
        reads_completed
        reads_merged
        sectors_read
        ms_reading
        writes_completed
        writes_merged
        sectors_written
        ms_writing
        ios_in_progress
        ms_doing_io
        weighted_ms_doing_io
        discards_completed
        discards_merged
        sectors_discarded
        ms_discarding
        flushes_completed
        ms_flushing
    )

    local -a field_values=()

    if [[ ! -r "$stat_path" ]]; then
        echo "Error: cannot read $stat_path" >&2
        return 1
    fi

    read -r -a field_values < "$stat_path"

    local value_count="${#field_values[@]}"
    local name_count="${#field_names[@]}"

    # Handle possible future kernel fields not listed above.
    if (( value_count > name_count )); then
        local i

        for ((i = name_count; i < value_count; i++)); do
            field_names+=("unknown_field_$((i + 1))")
        done
    fi

    {
        join_csv "${field_names[@]:0:value_count}"
        join_csv "${field_values[@]}"
    } > "$STAT_FILE"
}

# -----------------------------
# Parse key=value arguments
# -----------------------------
for argument in "$@"; do
    case "$argument" in
        mode=*)
            MODE="${argument#*=}"
            ;;
        size=*)
            SIZE="${argument#*=}"
            ;;
        k=*)
            K="${argument#*=}"
            ;;
        device=*)
            DEVICE="${argument#*=}"
            ;;
        trace=*)
            TRACE="${argument#*=}"
            ;;
        output=*)
            OUTPUT="${argument#*=}"
            ;;
        memory=*)
            LOAD_SIZE="${argument#*=}"
            ;;
        cpus=*)
            CPU_LIST="${argument#*=}"
            ;;
        scale=*)
            SCALE="${argument#*=}"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Error: unknown argument: $argument" >&2
            usage
            exit 1
            ;;
    esac
done

# -----------------------------
# Validate arguments
# -----------------------------
if [[ -z "$MODE" || -z "$SIZE" ]]; then
    echo "Error: mode and size are required." >&2
    usage
    exit 1
fi

if [[ "$MODE" != "zns" && "$MODE" != "conv" ]]; then
    echo "Error: mode must be either zns or conv." >&2
    exit 1
fi

# Accept values such as 38GiB, 38G, or 38.5GiB.
if [[ ! "$SIZE" =~ ^([0-9]+([.][0-9]+)?)(GiB|G)$ ]]; then
    echo "Error: size must use GiB or G, such as size=38GiB." >&2
    exit 1
fi

SIZE_VALUE="${BASH_REMATCH[1]}"

if [[ "$MODE" == "zns" ]]; then
    if [[ -z "$K" ]]; then
        echo "Error: k is required when mode=zns." >&2
        exit 1
    fi

    if [[ ! "$K" =~ ^[1-9][0-9]*$ ]]; then
        echo "Error: k must be a positive integer." >&2
        exit 1
    fi

    # Convert GiB to MiB:
    # 1 GiB = 1024 MiB
    SIZE_MIB="$(
        awk -v size="$SIZE_VALUE" \
            'BEGIN { printf "%.0f", size * 1024 + 1 }'
    )"

    LOAD_SIZE="${SIZE_MIB}M"
else
    # Keep the non-ZNS size in GiB/G form.
    LOAD_SIZE="${SIZE_VALUE}G"

    if [[ -n "$K" ]]; then
        echo "Warning: k is ignored when mode=conv." >&2
    fi
fi

if [[ -z "$OUTPUT" ]]; then
    if [[ "$MODE" == "zns" ]]; then
        OUTPUT="out_hw_zns_k${K}"
    else
        OUTPUT="out_hw_conv"
    fi
fi

BLOCK_DEVICE="$(basename "$DEVICE")"
INFO_FILE="${OUTPUT}_device_info.txt"

# -----------------------------
# Display configuration
# -----------------------------
echo "Configuration:"
echo "  Mode:          $MODE"
echo "  Input size:    $SIZE"
echo "  Memory start:  $MEMORY_START"
echo "  Memory size:   $LOAD_SIZE"
echo "  CPU list:      $CPU_LIST"
echo "  Device:        $DEVICE"
echo "  Trace:         $TRACE"
echo "  Output:        $OUTPUT"
echo "  Scale:         $SCALE"

if [[ "$MODE" == "zns" ]]; then
    echo "  k:             $K"
fi

# -----------------------------
# Reload NVMeVirt
# -----------------------------
echo
echo "Removing existing nvmev module..."

rmmod nvmev 2>/dev/null || true

echo "Loading NVMeVirt..."

./load.sh \
    "$MODE" \
    "$MEMORY_START" \
    "$LOAD_SIZE" \
    "$CPU_LIST"

# -----------------------------
# Wait for block device
# -----------------------------
echo "Waiting for $DEVICE..."

for _ in {1..10}; do
    if [[ -b "$DEVICE" ]]; then
        break
    fi

    sleep 1
done

if [[ ! -b "$DEVICE" ]]; then
    echo "Error: block device $DEVICE was not created." >&2
    exit 1
fi

# -----------------------------
# Collect device information
# -----------------------------
{
    echo "NVMeVirt configuration"
    echo "======================="
    echo "Mode: $MODE"
    echo "Requested size: $SIZE"
    echo "load.sh size: $LOAD_SIZE"
    echo "Device: $DEVICE"
    echo

    echo "lsblk information"
    echo "================="
    lsblk -o NAME,SIZE,ZONED "$DEVICE"
    echo

    STAT_FILE="${OUTPUT}_block_stat_before.csv"
    save_block_stats

    if [[ "$MODE" == "zns" ]]; then
        echo "Zone chunk size in 512-byte sectors"
        echo "==================================="
        cat "/sys/block/$BLOCK_DEVICE/queue/chunk_sectors"
        echo

        echo "Zone count"
        echo "=========="
        blkzone report "$DEVICE" | wc -l
        echo
    fi
} | tee "$INFO_FILE"

# -----------------------------
# Run trace replayer
# -----------------------------
REPLAYER_COMMAND=(
    ./replayer
    -t "$TRACE"
    -d "$DEVICE"
    -m "$MODE"
)

if [[ "$MODE" == "zns" ]]; then
    REPLAYER_COMMAND+=(
        -k "$K"
    )
fi

REPLAYER_COMMAND+=(
    -o "$OUTPUT"
    -s "$SCALE"
)

echo
echo "Running:"
printf ' %q' "${REPLAYER_COMMAND[@]}"
echo

"${REPLAYER_COMMAND[@]}" > "${OUTPUT}_log.txt" 2>&1

echo

STAT_FILE="${OUTPUT}_block_stat_after.csv"
save_block_stats

echo "Replay completed."
echo "Device information: $INFO_FILE"
echo "Replay output:       $OUTPUT"

chown $SUDO_USER:$SUDO_USER *$OUTPUT*.csv
chown $SUDO_USER:$SUDO_USER *$OUTPUT*.txt