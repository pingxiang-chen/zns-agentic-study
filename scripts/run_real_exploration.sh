#!/bin/bash

TRACE="../traces/tr_occ90.txt"
SCALE="1"

# Check if effective user ID is 0
if [ "$(id -u)" -ne 0 ]; then
    echo "Error: This script must be run with sudo or as root." >&2
    exit 1
fi

for (( k=1; k<=8; k*=2 ))
do 
    ./run_real_experiment.sh mode=zns device=/dev/nvme0n1 k=$k confirm=YES_I_UNDERSTAND trace=$TRACE scale=$SCALE
done