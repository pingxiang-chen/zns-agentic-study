#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: ./get_tracelab_traces.sh [--force]

Fetch TraceLab's public replay examples into ../traces, remove fetched .git
metadata, and convert multi_session_large.csv to the standard simulator trace
format with tracelab2trace.py.

Environment overrides:
  TRACE_DIR          Output/input trace directory. Default: ../traces
  TRACELAB_REPO      TraceLab git repository URL.
                     Default: https://github.com/uw-syfi/TraceLab.git
  CSV_NAME           Replay CSV under replay/examples. Default: multi_session_large.csv
  RESUME             Resume policy: highhit or lru. Default: highhit
  WORKSTATION_OCC    Occupancy for workstation trace. Default: 0.80
  MOBILE_OCC         Occupancy for mobile trace. Default: 0.80
  HORIZON            Horizon seconds passed to tracelab2trace.py. Default: 7800
  MEAS_START         Measurement-start seconds. Default: 2400
  TOOLS_PER_ROUND    Tool events per round. Default: 2.0
  SEED               Replay seed. Default: 42
  PYTHON             Python executable. Default: python3
  FORCE              Set to 1 to replace fetched tree and outputs.

Outputs:
  ../traces/tracelab/replay/examples/*.csv
  ../traces/tr_tlw_${RESUME}.txt
  ../traces/tr_tlm_${RESUME}.txt
  ../traces/tr_tlw_${RESUME}_stats.json
  ../traces/tr_tlm_${RESUME}_stats.json
EOF
}

force="${FORCE:-0}"
if [[ "${1:-}" == "--force" ]]; then
  force=1
  shift
fi
if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi
if [[ $# -ne 0 ]]; then
  usage >&2
  exit 2
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
TRACE_DIR="${TRACE_DIR:-$REPO_ROOT/traces}"
TRACELAB_REPO="${TRACELAB_REPO:-https://github.com/uw-syfi/TraceLab.git}"
CSV_NAME="${CSV_NAME:-multi_session_large.csv}"
RESUME="${RESUME:-highhit}"
WORKSTATION_OCC="${WORKSTATION_OCC:-0.80}"
MOBILE_OCC="${MOBILE_OCC:-0.80}"
HORIZON="${HORIZON:-7800}"
MEAS_START="${MEAS_START:-2400}"
TOOLS_PER_ROUND="${TOOLS_PER_ROUND:-2.0}"
SEED="${SEED:-42}"
PYTHON="${PYTHON:-python3}"

FETCH_DIR="$TRACE_DIR/tracelab"
RAW_DIR="$FETCH_DIR/replay/examples"
RAW_CSV="$RAW_DIR/$CSV_NAME"

case "$RESUME" in
  highhit|lru) ;;
  *)
    echo "error: RESUME must be highhit or lru, got: $RESUME" >&2
    exit 2
    ;;
esac

mkdir -p "$TRACE_DIR"

outputs=(
  "$TRACE_DIR/tr_tlw_${RESUME}.txt"
  "$TRACE_DIR/tr_tlm_${RESUME}.txt"
  "$TRACE_DIR/tr_tlw_${RESUME}_stats.json"
  "$TRACE_DIR/tr_tlm_${RESUME}_stats.json"
)

if [[ "$force" != "1" ]]; then
  for path in "$FETCH_DIR" "${outputs[@]}"; do
    if [[ -e "$path" ]]; then
      echo "error: $path already exists; rerun with --force to replace it" >&2
      exit 1
    fi
  done
else
  rm -rf "$FETCH_DIR"
  for path in "${outputs[@]}"; do
    rm -f "$path"
  done
fi

tmp_fetch="$TRACE_DIR/.tracelab.tmp.$$"
rm -rf "$tmp_fetch"
trap 'rm -rf "$tmp_fetch"' EXIT

git clone --depth 1 --filter=blob:none --sparse "$TRACELAB_REPO" "$tmp_fetch"
git -C "$tmp_fetch" sparse-checkout set --no-cone \
  README.md \
  LICENSE \
  LICENSE-DATASET.md \
  replay/examples
rm -rf "$tmp_fetch/.git"
mv "$tmp_fetch" "$FETCH_DIR"
trap - EXIT

if [[ ! -r "$RAW_CSV" ]]; then
  echo "error: missing TraceLab replay CSV: $RAW_CSV" >&2
  exit 1
fi

convert_trace() {
  local mode="$1"
  local occ="$2"
  local output="$3"
  local stats="$4"

  "$PYTHON" "$SCRIPT_DIR/tracelab2trace.py" \
    --csv "$RAW_CSV" \
    --mode "$mode" \
    --resume "$RESUME" \
    --occ "$occ" \
    --horizon "$HORIZON" \
    --meas-start "$MEAS_START" \
    --tools-per-round "$TOOLS_PER_ROUND" \
    --seed "$SEED" \
    --out "$output" \
    --stats-out "$stats"
}

convert_trace workstation "$WORKSTATION_OCC" \
  "$TRACE_DIR/tr_tlw_${RESUME}.txt" \
  "$TRACE_DIR/tr_tlw_${RESUME}_stats.json"
convert_trace mobile "$MOBILE_OCC" \
  "$TRACE_DIR/tr_tlm_${RESUME}.txt" \
  "$TRACE_DIR/tr_tlm_${RESUME}_stats.json"

echo "Fetched TraceLab replay examples under: $RAW_DIR"
echo "Converted simulator traces under: $TRACE_DIR"
