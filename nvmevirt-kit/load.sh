#!/bin/bash
# usage: sudo ./load.sh conv|zns <memmap_start> <memmap_size> <cpus>
# e.g.:  sudo ./load.sh zns 16G 48G "0,1"
set -e
V=${1:?conv|zns}; START=${2:?memmap_start}; SIZE=${3:?memmap_size}; CPUS=${4:?cpus}
rmmod nvmev 2>/dev/null || true
insmod ./nvmev_$V.ko memmap_start=$START memmap_size=$SIZE cpus=$CPUS
sleep 1; lsblk -o NAME,SIZE,ZONED | grep nvme || true
echo "loaded nvmev_$V.ko"
