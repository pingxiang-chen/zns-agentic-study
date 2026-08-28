#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: ./get_mooncake_traces.sh [--force]

Fetch Mooncake FAST'25 traces into ../traces, remove the fetched .git
metadata, and convert the JSONL traces to the standard simulator trace format.

Environment overrides:
  TRACE_DIR      Output/input trace directory. Default: ../traces
  MOONCAKE_REPO  Mooncake git repository URL.
                 Default: https://github.com/kvcache-ai/Mooncake.git
  OCC            Occupancy target passed to mooncake2trace.py. Default: 0.90
  SHARD          Consistent-hash shard count. Default: 1
  PYTHON         Python executable. Default: python3
  FORCE          Set to 1 to replace an existing fetched tree and outputs.

Outputs:
  ../traces/mooncake-fast25/FAST25-release/traces/*.jsonl
  ../traces/tr_mooncake_conversation.txt
  ../traces/tr_mooncake_toolagent.txt
  ../traces/tr_mooncake_synthetic.txt
  ../traces/tr_mooncake_*_stats.json
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
MOONCAKE_REPO="${MOONCAKE_REPO:-https://github.com/kvcache-ai/Mooncake.git}"
OCC="${OCC:-0.90}"
SHARD="${SHARD:-1}"
PYTHON="${PYTHON:-python3}"

FETCH_DIR="$TRACE_DIR/mooncake-fast25"
RAW_DIR="$FETCH_DIR/FAST25-release/traces"

mkdir -p "$TRACE_DIR"

if [[ -e "$FETCH_DIR" ]]; then
  if [[ "$force" != "1" ]]; then
    echo "error: $FETCH_DIR already exists; rerun with --force to replace it" >&2
    exit 1
  fi
  rm -rf "$FETCH_DIR"
fi

tmp_fetch="$TRACE_DIR/.mooncake-fast25.tmp.$$"
rm -rf "$tmp_fetch"
trap 'rm -rf "$tmp_fetch"' EXIT

git clone --depth 1 --filter=blob:none --sparse "$MOONCAKE_REPO" "$tmp_fetch"
git -C "$tmp_fetch" sparse-checkout set FAST25-release/traces
rm -rf "$tmp_fetch/.git"
mv "$tmp_fetch" "$FETCH_DIR"
trap - EXIT

convert_trace() {
  local stem="$1"
  local input="$RAW_DIR/${stem}_trace.jsonl"
  local output="$TRACE_DIR/tr_mooncake_${stem}.txt"
  local stats="$TRACE_DIR/tr_mooncake_${stem}_stats.json"

  if [[ ! -r "$input" ]]; then
    echo "error: missing Mooncake trace: $input" >&2
    exit 1
  fi
  if [[ "$force" != "1" && ( -e "$output" || -e "$stats" ) ]]; then
    echo "error: output exists for $stem; rerun with --force to replace it" >&2
    exit 1
  fi

  "$PYTHON" "$SCRIPT_DIR/mooncake2trace.py" \
    --trace "$input" \
    --occ "$OCC" \
    --shard "$SHARD" \
    --out "$output" \
    --stats-out "$stats"
}

convert_trace conversation
convert_trace toolagent
convert_trace synthetic

echo "Fetched Mooncake traces under: $RAW_DIR"
echo "Converted simulator traces under: $TRACE_DIR"
