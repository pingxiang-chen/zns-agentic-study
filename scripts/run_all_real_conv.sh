#!/bin/bash

traces=("../traces/nvme_occ90.txt" "../traces/tr_tlw_lru.txt" ""../traces/tr_azcode.txt"" "../traces/tr_azconv.txt" )
SCALE="1"

# Check if effective user ID is 0
if [ "$(id -u)" -ne 0 ]; then
    echo "Error: This script must be run with sudo or as root." >&2
    exit 1
fi

# Loop through each traces
for trace in "${traces[@]}"; do
    ./run_real_exploration_conv.sh $trace
done