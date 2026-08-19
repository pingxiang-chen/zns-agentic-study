#!/bin/bash

TRACE="../traces/tr_occ90.txt"
SCALE="0.25"

# Check if effective user ID is 0
if [ "$(id -u)" -ne 0 ]; then
    echo "Error: This script must be run with sudo or as root." >&2
    exit 1
fi

for (( k=1; k<=16; k*=2 ))
do 
   ./run_experiment.sh mode=zns size=40GiB k=$k trace=$TRACE scale=$SCALE
done

./run_experiment.sh mode=conv size=40GiB trace=$TRACE scale=$SCALE