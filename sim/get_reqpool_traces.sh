#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: ./get_reqpool_traces.sh [--force]

Fetch the public request-pool traces used by reqpool2trace.py into ../traces,
remove fetched .git metadata, and convert the CSV traces to the standard
simulator trace format.

Environment overrides:
  TRACE_DIR          Output/input trace directory. Default: ../traces
  AZURE_REPO         Azure public dataset git URL.
                     Default: https://github.com/Azure/AzurePublicDataset.git
  BURSTGPT_REPO      BurstGPT git URL.
                     Default: https://github.com/HPMLL/BurstGPT.git
  WINDOW_S           Replay window passed to reqpool2trace.py. Default: 7200
  SKIP_S             Initial seconds skipped in each trace. Default: 0
  MAX_CTX_TOK        Max context+generated tokens per request. Default: 32000
  AZURE_CONV_OCC     Occupancy for Azure conversation trace. Default: 0.85
  AZURE_CODE_OCC     Occupancy for Azure code trace. Default: 0.75
  BURSTGPT_OCC       Occupancy for BurstGPT trace. Default: 0.60
  PYTHON             Python executable. Default: python3
  FORCE              Set to 1 to replace fetched trees and outputs.

Outputs:
  ../traces/azure-llm-inference-2023/data/AzureLLMInferenceTrace_*.csv
  ../traces/burstgpt/data/BurstGPT_1.csv
  ../traces/tr_azconv.txt
  ../traces/tr_azcode.txt
  ../traces/tr_bgpt.txt
  ../traces/tr_azconv_stats.json
  ../traces/tr_azcode_stats.json
  ../traces/tr_bgpt_stats.json
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
AZURE_REPO="${AZURE_REPO:-https://github.com/Azure/AzurePublicDataset.git}"
BURSTGPT_REPO="${BURSTGPT_REPO:-https://github.com/HPMLL/BurstGPT.git}"
WINDOW_S="${WINDOW_S:-7200}"
SKIP_S="${SKIP_S:-0}"
MAX_CTX_TOK="${MAX_CTX_TOK:-32000}"
AZURE_CONV_OCC="${AZURE_CONV_OCC:-0.85}"
AZURE_CODE_OCC="${AZURE_CODE_OCC:-0.75}"
BURSTGPT_OCC="${BURSTGPT_OCC:-0.60}"
PYTHON="${PYTHON:-python3}"

AZURE_DIR="$TRACE_DIR/azure-llm-inference-2023"
BURSTGPT_DIR="$TRACE_DIR/burstgpt"

mkdir -p "$TRACE_DIR"

outputs=(
  "$TRACE_DIR/tr_azconv.txt"
  "$TRACE_DIR/tr_azcode.txt"
  "$TRACE_DIR/tr_bgpt.txt"
  "$TRACE_DIR/tr_azconv_stats.json"
  "$TRACE_DIR/tr_azcode_stats.json"
  "$TRACE_DIR/tr_bgpt_stats.json"
)

if [[ "$force" != "1" ]]; then
  for path in "$AZURE_DIR" "$BURSTGPT_DIR" "${outputs[@]}"; do
    if [[ -e "$path" ]]; then
      echo "error: $path already exists; rerun with --force to replace it" >&2
      exit 1
    fi
  done
else
  rm -rf "$AZURE_DIR" "$BURSTGPT_DIR"
  for path in "${outputs[@]}"; do
    rm -f "$path"
  done
fi

tmp_paths=()
cleanup() {
  for path in "${tmp_paths[@]}"; do
    rm -rf "$path"
  done
}
trap cleanup EXIT

sparse_clone() {
  local repo="$1"
  local dest="$2"
  shift 2

  local tmp="${dest}.tmp.$$"
  tmp_paths+=("$tmp")
  rm -rf "$tmp"

  git clone --depth 1 --filter=blob:none --sparse "$repo" "$tmp"
  git -C "$tmp" sparse-checkout set --no-cone "$@"
  rm -rf "$tmp/.git"
  mv "$tmp" "$dest"
}

sparse_clone "$AZURE_REPO" "$AZURE_DIR" \
  AzureLLMInferenceDataset2023.md \
  LICENSE \
  data/AzureLLMInferenceTrace_code.csv \
  data/AzureLLMInferenceTrace_conv.csv

sparse_clone "$BURSTGPT_REPO" "$BURSTGPT_DIR" \
  README.md \
  LICENSE \
  CITATION.cff \
  data/BurstGPT_1.csv

convert_trace() {
  local name="$1"
  local format="$2"
  local occ="$3"
  local input="$4"
  local output="$TRACE_DIR/${name}.txt"
  local stats="$TRACE_DIR/${name}_stats.json"

  if [[ ! -r "$input" ]]; then
    echo "error: missing input trace: $input" >&2
    exit 1
  fi

  "$PYTHON" "$SCRIPT_DIR/reqpool2trace.py" \
    --csv "$input" \
    --format "$format" \
    --occ "$occ" \
    --window-s "$WINDOW_S" \
    --skip-s "$SKIP_S" \
    --max-ctx-tok "$MAX_CTX_TOK" \
    --out "$output" \
    --stats-out "$stats"
}

convert_trace tr_azconv azure "$AZURE_CONV_OCC" \
  "$AZURE_DIR/data/AzureLLMInferenceTrace_conv.csv"
convert_trace tr_azcode azure "$AZURE_CODE_OCC" \
  "$AZURE_DIR/data/AzureLLMInferenceTrace_code.csv"
convert_trace tr_bgpt burstgpt "$BURSTGPT_OCC" \
  "$BURSTGPT_DIR/data/BurstGPT_1.csv"

echo "Fetched Azure traces under: $AZURE_DIR/data"
echo "Fetched BurstGPT trace under: $BURSTGPT_DIR/data"
echo "Converted simulator traces under: $TRACE_DIR"
