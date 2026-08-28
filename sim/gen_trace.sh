#!/bin/bash

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
TRACE_DIR="${TRACE_DIR:-$REPO_ROOT/traces}"

B="--ctx0-median-tok 10000 --turns-mean 16 --newtok-mean 400 --arrival-mean 1.5 \
   --horizon 7800 --meas-start 2400 --seed 42"
python3 gen_trace.py $B --occ 0.9 --growth-reserve 0.02 --hard-cap 0.95 \
   --out $TRACE_DIR/nvme_occ90.txt --stats-out $TRACE_DIR/nvme_occ90_stats.json