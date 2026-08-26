#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
KIT_DIR="$REPO_ROOT/nvmevirt-kit"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# -----------------------------
# Default configuration
# -----------------------------
MODE=""
SIZE=""
K=""

DEVICE=""
TRACE="../traces/tr_occ90.txt"
SCALE="0.25"
OUTPUT=""
STAT_FILE=""
CONFIRM=""
QDEPTH=""
MAX_ACTIVE_ZONES=""
TASKSET_CPUS=""
REPLAYER="$KIT_DIR/replayer"

usage() {
    cat <<EOF
Usage:
  sudo $0 mode=conv device=/dev/nvmeXnY confirm=YES_I_UNDERSTAND
  sudo $0 mode=zns device=/dev/nvmeXnY k=<number> confirm=YES_I_UNDERSTAND

Required arguments:
  mode=conv|zns
  device=/dev/nvmeXnY
  confirm=YES_I_UNDERSTAND
  k=<number>                  Required when mode=zns

Optional arguments:
  size=<XGiB>                 Minimum-capacity check only; does not provision the device
  trace=../traces/tr_occ90.txt
  output=output_prefix
  scale=0.25
  qd=32
  max_active_zones=<number>   Passed to replayer -A in zns mode
  taskset_cpus=0,1
  replayer=./replayer

Examples:
    sudo $0 mode=conv device=/dev/nvme0n1 confirm=YES_I_UNDERSTAND
    sudo $0 mode=zns device=/dev/nvme1n1 k=8 confirm=YES_I_UNDERSTAND
EOF
}

die() {
    echo "Error: $*" >&2
    exit 1
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

resolve_existing_path() {
    local path="$1"
    local label="$2"
    local resolved

    resolved="$(readlink -f "$path" 2>/dev/null)" || die "cannot resolve $label: $path"
    printf '%s\n' "$resolved"
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

maybe_chown_outputs() {
    if [[ -z "${SUDO_USER:-}" || "${SUDO_USER:-}" == "root" ]]; then
        return 0
    fi

    local group
    group="$(id -gn "$SUDO_USER" 2>/dev/null || true)"

    if [[ -z "$group" ]]; then
        return 0
    fi

    shopt -s nullglob
    local -a outputs=( "${OUTPUT}"*.csv "${OUTPUT}"*.txt )
    if (( ${#outputs[@]} > 0 )); then
        chown "$SUDO_USER:$group" "${outputs[@]}" || true
    fi
    shopt -u nullglob
}

format_bytes_gib() {
    awk -v bytes="$1" 'BEGIN { printf "%.2f GiB", bytes / 1073741824 }'
}

size_to_bytes() {
    local size="$1"
    local value="$2"

    awk -v size="$value" 'BEGIN { printf "%.0f", size * 1024 * 1024 * 1024 }'
}

check_unmounted_whole_device() {
    BLOCK_DEVICE="$(basename "$DEVICE")"

    if [[ ! -d "/sys/block/$BLOCK_DEVICE" ]]; then
        die "$DEVICE is not a whole block device; pass the raw namespace, not a partition"
    fi

    local mountpoints
    mountpoints="$(lsblk -nr -o MOUNTPOINT "$DEVICE" | sed '/^$/d' || true)"
    if [[ -n "$mountpoints" ]]; then
        die "$DEVICE or one of its children appears mounted: $mountpoints"
    fi

    local root_source root_dev root_parent
    root_source="$(findmnt -no SOURCE / 2>/dev/null || true)"
    if [[ -z "$root_source" ]]; then
        return 0
    fi

    root_dev="$(readlink -f "$root_source" 2>/dev/null || true)"
    root_parent="$(lsblk -no PKNAME "$root_dev" 2>/dev/null | head -n 1 || true)"

    if [[ "$root_dev" == "$DEVICE" || ( -n "$root_parent" && "/dev/$root_parent" == "$DEVICE" ) ]]; then
        die "$DEVICE contains the root filesystem; refusing to run"
    fi
}

write_device_info() {
    {
        echo "Real block-device experiment"
        echo "============================"
        date -Is
        echo

        echo "Configuration"
        echo "============="
        echo "Mode: $MODE"
        [[ -n "$SIZE" ]] && echo "Minimum requested size: $SIZE"
        echo "Device: $DEVICE"
        echo "Device size: $(format_bytes_gib "$DEVICE_BYTES") ($DEVICE_BYTES bytes)"
        echo "Trace: $TRACE"
        echo "Replayer: $REPLAYER"
        echo "Output: $OUTPUT"
        echo "Scale: $SCALE"
        [[ -n "$QDEPTH" ]] && echo "Queue depth: $QDEPTH"
        [[ -n "$TASKSET_CPUS" ]] && echo "taskset CPUs: $TASKSET_CPUS"
        if [[ "$MODE" == "zns" ]]; then
            echo "k: $K"
            [[ -n "$MAX_ACTIVE_ZONES" ]] && echo "Max active/open zones override: $MAX_ACTIVE_ZONES"
        fi
        echo

        echo "lsblk information"
        echo "================="
        lsblk -o NAME,MODEL,SERIAL,SIZE,TYPE,MOUNTPOINT,ZONED "$DEVICE" 2>/dev/null \
            || lsblk -o NAME,SIZE,TYPE,MOUNTPOINT "$DEVICE"
        echo

        echo "Queue zoned mode"
        echo "================"
        cat "/sys/block/$BLOCK_DEVICE/queue/zoned"
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
            if command -v blkzone >/dev/null 2>&1; then
                blkzone report "$DEVICE" | wc -l
            else
                echo "blkzone not installed"
            fi
            echo
        fi
    } | tee "$INFO_FILE"
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
        scale=*)
            SCALE="${argument#*=}"
            ;;
        qd=*)
            QDEPTH="${argument#*=}"
            ;;
        max_active_zones=*|max_open_zones=*)
            MAX_ACTIVE_ZONES="${argument#*=}"
            ;;
        taskset_cpus=*)
            TASKSET_CPUS="${argument#*=}"
            ;;
        replayer=*)
            REPLAYER="${argument#*=}"
            ;;
        confirm=*)
            CONFIRM="${argument#*=}"
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
if [[ "$(id -u)" -ne 0 ]]; then
    die "this script must be run with sudo or as root"
fi

if [[ -z "$MODE" || -z "$DEVICE" ]]; then
    echo "Error: mode and device are required." >&2
    usage
    exit 1
fi

if [[ "$CONFIRM" != "YES_I_UNDERSTAND" ]]; then
    die "pass confirm=YES_I_UNDERSTAND to acknowledge destructive writes to $DEVICE"
fi

if [[ "$MODE" != "zns" && "$MODE" != "conv" ]]; then
    die "mode must be either zns or conv"
fi

if [[ "$MODE" == "zns" ]]; then
    if [[ -z "$K" ]]; then
        die "k is required when mode=zns"
    fi

    if [[ ! "$K" =~ ^[1-9][0-9]*$ ]]; then
        die "k must be a positive integer"
    fi
else
    if [[ -n "$K" ]]; then
        echo "Warning: k is ignored when mode=conv." >&2
    fi

    if [[ -n "$MAX_ACTIVE_ZONES" ]]; then
        echo "Warning: max_active_zones is ignored when mode=conv." >&2
        MAX_ACTIVE_ZONES=""
    fi
fi

SIZE_BYTES=""
if [[ -n "$SIZE" ]]; then
    if [[ ! "$SIZE" =~ ^([0-9]+([.][0-9]+)?)(GiB|G)$ ]]; then
        die "size must use GiB or G, such as size=38GiB"
    fi

    SIZE_BYTES="$(size_to_bytes "$SIZE" "${BASH_REMATCH[1]}")"
fi

if [[ ! "$SCALE" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    die "scale must be numeric"
fi

if [[ -n "$QDEPTH" && ! "$QDEPTH" =~ ^[1-9][0-9]*$ ]]; then
    die "qd must be a positive integer"
fi

if [[ -n "$MAX_ACTIVE_ZONES" && ! "$MAX_ACTIVE_ZONES" =~ ^[1-9][0-9]*$ ]]; then
    die "max_active_zones must be a positive integer"
fi

require_cmd awk
require_cmd blockdev
require_cmd findmnt
require_cmd lsblk
require_cmd readlink
require_cmd sed
if [[ -n "$TASKSET_CPUS" ]]; then
    require_cmd taskset
fi

DEVICE="$(resolve_existing_path "$DEVICE" "device")"
TRACE="$(resolve_existing_path "$TRACE" "trace")"
REPLAYER="$(resolve_existing_path "$REPLAYER" "replayer")"

[[ -b "$DEVICE" ]] || die "$DEVICE is not a block device"
[[ -r "$TRACE" ]] || die "trace not readable: $TRACE"
[[ -x "$REPLAYER" ]] || die "replayer not executable: $REPLAYER; build it with: make replayer"

check_unmounted_whole_device

DEVICE_BYTES="$(blockdev --getsize64 "$DEVICE")"
if [[ -n "$SIZE_BYTES" && "$DEVICE_BYTES" -lt "$SIZE_BYTES" ]]; then
    die "$DEVICE is smaller than size=$SIZE ($(format_bytes_gib "$DEVICE_BYTES") < $(format_bytes_gib "$SIZE_BYTES"))"
fi

ZONED_MODE="$(cat "/sys/block/$BLOCK_DEVICE/queue/zoned" 2>/dev/null || true)"
if [[ "$MODE" == "zns" && "$ZONED_MODE" == "none" ]]; then
    die "$DEVICE reports zoned=none; use mode=conv or pass a zoned namespace"
fi

if [[ "$MODE" == "conv" && "$ZONED_MODE" != "none" ]]; then
    echo "Warning: $DEVICE reports zoned=$ZONED_MODE while mode=conv." >&2
fi

if [[ -z "$OUTPUT" ]]; then
    if [[ "$MODE" == "zns" ]]; then
        OUTPUT="out_real_zns_k${K}"
    else
        OUTPUT="out_real_conv"
    fi
fi

OUTPUT_DIR="$(dirname "$OUTPUT")"
if [[ "$OUTPUT_DIR" != "." ]]; then
    mkdir -p "$OUTPUT_DIR"
fi

INFO_FILE="${OUTPUT}_device_info.txt"
trap maybe_chown_outputs EXIT

# -----------------------------
# Display configuration
# -----------------------------
echo "Configuration:"
echo "  Mode:          $MODE"
[[ -n "$SIZE" ]] && echo "  Min size:      $SIZE"
echo "  Device:        $DEVICE"
echo "  Trace:         $TRACE"
echo "  Replayer:      $REPLAYER"
echo "  Output:        $OUTPUT"
echo "  Scale:         $SCALE"
[[ -n "$QDEPTH" ]] && echo "  Queue depth:   $QDEPTH"
[[ -n "$TASKSET_CPUS" ]] && echo "  taskset CPUs:  $TASKSET_CPUS"

if [[ "$MODE" == "zns" ]]; then
    echo "  k:             $K"
    [[ -n "$MAX_ACTIVE_ZONES" ]] && echo "  max zones:     $MAX_ACTIVE_ZONES"
fi

echo
echo "Using existing block device $DEVICE; skipping NVMeVirt module unload/load."
echo "This will overwrite data on $DEVICE."

# -----------------------------
# Collect device information
# -----------------------------
write_device_info

# -----------------------------
# Run trace replayer
# -----------------------------
REPLAYER_COMMAND=(
    "$REPLAYER"
    -t "$TRACE"
    -d "$DEVICE"
    -m "$MODE"
)

if [[ "$MODE" == "zns" ]]; then
    REPLAYER_COMMAND+=(
        -k "$K"
    )

    if [[ -n "$MAX_ACTIVE_ZONES" ]]; then
        REPLAYER_COMMAND+=(
            -A "$MAX_ACTIVE_ZONES"
        )
    fi
fi

if [[ -n "$QDEPTH" ]]; then
    REPLAYER_COMMAND+=(
        -q "$QDEPTH"
    )
fi

REPLAYER_COMMAND+=(
    -o "$OUTPUT"
    -s "$SCALE"
)

if [[ -n "$TASKSET_CPUS" ]]; then
    FULL_COMMAND=(taskset -c "$TASKSET_CPUS" "${REPLAYER_COMMAND[@]}")
else
    FULL_COMMAND=("${REPLAYER_COMMAND[@]}")
fi

echo
echo "Running:"
printf ' %q' "${FULL_COMMAND[@]}"
echo

set +e
"${FULL_COMMAND[@]}" > "${OUTPUT}_log.txt" 2>&1
REPLAYER_STATUS=$?
set -e

echo

STAT_FILE="${OUTPUT}_block_stat_after.csv"
save_block_stats

if (( REPLAYER_STATUS == 0 )); then
    echo "Replay completed."
else
    echo "Replay stopped early with status $REPLAYER_STATUS."
    echo "See ${OUTPUT}_log.txt and ${OUTPUT}_summary.txt."
fi
echo "Device information: $INFO_FILE"
echo "Replay output:       $OUTPUT"

maybe_chown_outputs

exit "$REPLAYER_STATUS"
