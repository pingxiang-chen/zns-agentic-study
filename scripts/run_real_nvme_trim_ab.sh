#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
KIT_DIR="$REPO_ROOT/nvmevirt-kit"

DEVICE=""
TRACE="$REPO_ROOT/traces/tr_occ90.txt"
RESULTS_DIR="$REPO_ROOT/results/hw"
RUN_SET="both"
SCALE="1"
CONFIRM=""
PRECONDITION="1"
PRECOND_LOOPS="2"
PRECOND_BS="128k"
QDEPTH=""
TASKSET_CPUS=""
REPLAYER="$KIT_DIR/replayer"

usage() {
    cat <<EOF
Usage:
  sudo $0 device=/dev/nvme0n2 confirm=YES_I_UNDERSTAND [options]

Required:
  device=/dev/nvmeXnY             Raw conventional NVMe namespace to overwrite
  confirm=YES_I_UNDERSTAND        Required acknowledgement of destructive writes

Options:
  run=both|trim|notrim            Default: both
  trace=PATH                      Default: traces/tr_occ90.txt
  results_dir=PATH                Default: results/hw
  scale=0.25                      Replayer time scale
  precondition=1|0                Run fio before each replay; default: 1
  precond_loops=2                 fio write loops; default: 2
  precond_bs=128k                 fio write block size; default: 128k
  qd=N                            Optional replayer queue depth
  taskset_cpus=LIST               Optional CPU list for taskset -c
  replayer=PATH                   Default: nvmevirt-kit/replayer

Examples:
  sudo $0 device=/dev/nvme0n2 confirm=YES_I_UNDERSTAND
  sudo $0 device=/dev/nvme0n2 run=notrim confirm=YES_I_UNDERSTAND scale=1
EOF
}

die() {
    echo "Error: $*" >&2
    exit 1
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

resolve_path() {
    local path="$1"
    local label="$2"
    local resolved

    resolved="$(readlink -f "$path" 2>/dev/null)" || die "cannot resolve $label: $path"
    printf '%s\n' "$resolved"
}

to_abs_path() {
    local path="$1"

    if [[ "$path" == /* ]]; then
        printf '%s\n' "$path"
    else
        printf '%s/%s\n' "$PWD" "$path"
    fi
}

join_csv() {
    local IFS=','
    printf '%s\n' "$*"
}

save_block_stats() {
    local prefix="$1"
    local phase="$2"
    local stat_path="/sys/block/${BLOCK_DEVICE}/stat"
    local out="${prefix}_block_stat_${phase}.csv"
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
        echo "Warning: cannot read $stat_path" >&2
        return 0
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
    } > "$out"
}

capture_smart_log() {
    local prefix="$1"
    local phase="$2"
    local out="${prefix}_smart_${phase}.txt"

    if ! nvme smart-log "$DEVICE" > "$out" 2>&1; then
        echo "Warning: nvme smart-log failed for $DEVICE; see $out" >&2
    fi
}

write_device_info() {
    local out="$RESULTS_DIR/part2_device_info.txt"

    {
        echo "Part 2 real conventional NVMe TRIM/no-TRIM run"
        echo "================================================"
        date -Is
        echo
        echo "Host"
        echo "===="
        uname -a
        echo
        echo "Configuration"
        echo "============="
        echo "Device: $DEVICE"
        echo "Trace: $TRACE"
        echo "Results dir: $RESULTS_DIR"
        echo "Run set: $RUN_SET"
        echo "Scale: $SCALE"
        echo "Precondition: $PRECONDITION"
        echo "Precondition loops: $PRECOND_LOOPS"
        echo "Precondition block size: $PRECOND_BS"
        [[ -n "$QDEPTH" ]] && echo "Queue depth: $QDEPTH"
        [[ -n "$TASKSET_CPUS" ]] && echo "taskset CPUs: $TASKSET_CPUS"
        echo
        echo "lsblk"
        echo "====="
        lsblk -o NAME,MODEL,SERIAL,SIZE,TYPE,MOUNTPOINT,ZONED "$DEVICE"
        echo
        echo "nvme id-ctrl"
        echo "============"
        nvme id-ctrl "$DEVICE" 2>&1 || true
    } > "$out"
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

    chown -R "$SUDO_USER:$group" "$RESULTS_DIR"
}

run_precondition() {
    local prefix="$1"
    local out="${prefix}_precond_fio.txt"
    local -a fio_cmd=(
        fio
        --name=precond
        --filename="$DEVICE"
        --rw=write
        --bs="$PRECOND_BS"
        --loops="$PRECOND_LOOPS"
        --direct=1
        --output="$out"
    )

    echo
    echo "Preconditioning $DEVICE before $(basename "$prefix")..."
    printf 'Running:'
    printf ' %q' "${fio_cmd[@]}"
    echo

    "${fio_cmd[@]}"
}

run_replay() {
    local mode="$1"
    local prefix="$RESULTS_DIR/out_real_conv_${mode}"
    local log="${prefix}_replayer_log.txt"
    local status_file="${prefix}_status.txt"
    local -a replay_cmd=(
        "$REPLAYER"
        -t "$TRACE"
        -d "$DEVICE"
        -m conv
        -o "$prefix"
        -s "$SCALE"
    )
    local -a full_cmd=()

    if [[ -n "$QDEPTH" ]]; then
        replay_cmd+=(-q "$QDEPTH")
    fi

    if [[ -n "$TASKSET_CPUS" ]]; then
        full_cmd=(taskset -c "$TASKSET_CPUS" "${replay_cmd[@]}")
    else
        full_cmd=("${replay_cmd[@]}")
    fi

    if [[ "$PRECONDITION" == "1" ]]; then
        run_precondition "$prefix"
    fi

    capture_smart_log "$prefix" "before"
    save_block_stats "$prefix" "before"

    echo
    if [[ "$mode" == "notrim" ]]; then
        echo "Running no-TRIM replay with BLKDISCARD_DISABLE=1..."
    else
        echo "Running TRIM replay..."
    fi
    printf 'Running:'
    [[ "$mode" == "notrim" ]] && printf ' %q' "BLKDISCARD_DISABLE=1"
    printf ' %q' "${full_cmd[@]}"
    echo

    set +e
    if [[ "$mode" == "notrim" ]]; then
        BLKDISCARD_DISABLE=1 "${full_cmd[@]}" > "$log" 2>&1
    else
        "${full_cmd[@]}" > "$log" 2>&1
    fi
    local replay_status=$?
    set -e

    capture_smart_log "$prefix" "after"
    save_block_stats "$prefix" "after"

    {
        echo "mode=$mode"
        echo "status=$replay_status"
        echo "device=$DEVICE"
        echo "trace=$TRACE"
        echo "scale=$SCALE"
        echo "log=$log"
        echo "summary=${prefix}_summary.txt"
    } > "$status_file"

    if (( replay_status == 0 )); then
        echo "Replay completed: ${prefix}_summary.txt"
    else
        echo "Replay failed with status $replay_status; see $log" >&2
    fi

    return "$replay_status"
}

for argument in "$@"; do
    case "$argument" in
        device=*)
            DEVICE="${argument#*=}"
            ;;
        trace=*)
            TRACE="${argument#*=}"
            ;;
        results_dir=*)
            RESULTS_DIR="${argument#*=}"
            ;;
        run=*)
            RUN_SET="${argument#*=}"
            ;;
        scale=*)
            SCALE="${argument#*=}"
            ;;
        confirm=*)
            CONFIRM="${argument#*=}"
            ;;
        precondition=*)
            PRECONDITION="${argument#*=}"
            ;;
        precond_loops=*)
            PRECOND_LOOPS="${argument#*=}"
            ;;
        precond_bs=*)
            PRECOND_BS="${argument#*=}"
            ;;
        qd=*)
            QDEPTH="${argument#*=}"
            ;;
        taskset_cpus=*)
            TASKSET_CPUS="${argument#*=}"
            ;;
        replayer=*)
            REPLAYER="${argument#*=}"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown argument: $argument"
            ;;
    esac
done

if [[ "$(id -u)" -ne 0 ]]; then
    die "this script must be run with sudo or as root"
fi

[[ -n "$DEVICE" ]] || die "device=/dev/nvmeXnY is required"
[[ "$CONFIRM" == "YES_I_UNDERSTAND" ]] || die "pass confirm=YES_I_UNDERSTAND to acknowledge destructive writes"

case "$RUN_SET" in
    both|trim|notrim)
        ;;
    *)
        die "run must be one of: both, trim, notrim"
        ;;
esac

case "$PRECONDITION" in
    0|1)
        ;;
    *)
        die "precondition must be 0 or 1"
        ;;
esac

[[ "$SCALE" =~ ^[0-9]+([.][0-9]+)?$ ]] || die "scale must be numeric"
[[ "$PRECOND_LOOPS" =~ ^[1-9][0-9]*$ ]] || die "precond_loops must be a positive integer"
[[ -z "$QDEPTH" || "$QDEPTH" =~ ^[1-9][0-9]*$ ]] || die "qd must be a positive integer"

if [[ "$PRECONDITION" == "1" ]]; then
    require_cmd fio
fi
require_cmd lsblk
require_cmd nvme
require_cmd readlink
require_cmd findmnt
if [[ -n "$TASKSET_CPUS" ]]; then
    require_cmd taskset
fi

DEVICE="$(resolve_path "$DEVICE" "device")"
TRACE="$(to_abs_path "$TRACE")"
RESULTS_DIR="$(to_abs_path "$RESULTS_DIR")"
REPLAYER="$(resolve_path "$(to_abs_path "$REPLAYER")" "replayer")"

[[ -b "$DEVICE" ]] || die "$DEVICE is not a block device"
[[ -r "$TRACE" ]] || die "trace not readable: $TRACE"
[[ -x "$REPLAYER" ]] || die "replayer not executable: $REPLAYER; build it with: make -C $KIT_DIR replayer"

BLOCK_DEVICE="$(basename "$DEVICE")"
[[ -d "/sys/block/$BLOCK_DEVICE" ]] || die "$DEVICE is not a whole block device; pass the raw namespace, not a partition"

MOUNTPOINTS="$(lsblk -nr -o MOUNTPOINT "$DEVICE" | sed '/^$/d' || true)"
if [[ -n "$MOUNTPOINTS" ]]; then
    die "$DEVICE or one of its children appears mounted: $MOUNTPOINTS"
fi

ROOT_SOURCE="$(findmnt -no SOURCE / 2>/dev/null || true)"
if [[ -n "$ROOT_SOURCE" ]]; then
    ROOT_DEV="$(readlink -f "$ROOT_SOURCE" 2>/dev/null || true)"
    ROOT_PARENT="$(lsblk -no PKNAME "$ROOT_DEV" 2>/dev/null | head -n 1 || true)"

    if [[ "$ROOT_DEV" == "$DEVICE" || ( -n "$ROOT_PARENT" && "/dev/$ROOT_PARENT" == "$DEVICE" ) ]]; then
        die "$DEVICE contains the root filesystem; refusing to run"
    fi
fi

mkdir -p "$RESULTS_DIR"
write_device_info
trap maybe_chown_outputs EXIT

echo "Configuration:"
echo "  Device:        $DEVICE"
echo "  Trace:         $TRACE"
echo "  Results dir:   $RESULTS_DIR"
echo "  Run set:       $RUN_SET"
echo "  Scale:         $SCALE"
echo "  Precondition:  $PRECONDITION"
echo
echo "This will overwrite $DEVICE. Verify the device name before continuing."

case "$RUN_SET" in
    trim)
        run_replay "trim"
        ;;
    notrim)
        run_replay "notrim"
        ;;
    both)
        run_replay "trim"
        run_replay "notrim"
        ;;
esac

python3 diff_before_after_csv.py --include-metadata -o "$RESULTS_DIR/1_block_difference.csv"
python3 diff_before_after_smart_txt.py --include-metadata -o "$RESULTS_DIR/2_smart_difference.csv"
python3 ../nvmevirt-kit/extract_index_cmds.py  ../results/hw -o "$RESULTS_DIR/3_summary.txt"

maybe_chown_outputs