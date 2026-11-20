# ZoneServe: Tail Latency Reduction with Host-guided Data Management

Run the commands below from `nvmevirt-kit/` on the Linux host that will provide the NVMeVirt device. **The runner unloads and reloads the `nvmev` kernel module and replays writes to the selected block device.** Check the device and memory reservation before running it. The commands assume the wrapper's default `/dev/nvme3n1`, reserved memory beginning at `8G`, and isolated CPUs `0,1`; change `DEVICE`, `MEMORY_START`, and `CPU_LIST` in `run_experiment.sh` if this host differs. [NVMeVirt kit README](nvmevirt-kit/README.md) covers kernel setup and build prerequisites.

## Prepare the trace and binaries

The input for this result set is `../traces/nvme_occ90.txt`, generated with occupancy target `0.9`, seed `42`, a 7,800-second horizon, and measurement start at 2,400 seconds. The generated `.txt` and stats JSON are ignored by Git, so regenerate them if absent. Do not substitute `tr_occ90.txt`: it is a different trace.

```bash
# From the repository root
./sim/gen_trace.sh
cd nvmevirt-kit
./build.sh
make replayer
```

`build.sh` produces `nvmev_zns.ko` and `nvmev_conv.ko`. Reserve enough physical memory for the module: the ZNS invocation passes `32769M` to `load.sh` for `size=32GiB`, while conventional mode passes `32G`. The recorded ZNS device reported 512 zones of 64 MiB each. The conventional device reported about 29.9 GiB usable capacity. See each `*_device_info.txt` for the recorded device geometry.

## Run the same exploration

```bash
# From nvmevirt-kit/; use sudo so SUDO_USER is set for output ownership
sudo ./run_exploration.sh ../traces/nvme_occ90.txt
```

`run_exploration.sh` calls `run_experiment.sh` in this order:

```bash
sudo ./run_experiment.sh mode=zns  size=32GiB k=4 trace=../traces/nvme_occ90.txt scale=1
sudo ./run_experiment.sh mode=zns  size=32GiB k=8 trace=../traces/nvme_occ90.txt scale=1
sudo ./run_experiment.sh mode=conv size=32GiB     trace=../traces/nvme_occ90.txt scale=1
```

Those are the equivalent commands for running one configuration at a time. Each call reloads the corresponding NVMeVirt module, waits for the block device, records its geometry and block statistics, then runs `replayer`. The conventional run also samples `/proc/nvmev/conv_ftl_stats` every five seconds.

The exploration wrapper moves the generated `*.csv` and `*.txt` files from the current working directory into `../results/YYYY-MM-DD_nvme_occ90/`, using the run date. 

Run in a working directory free of unrelated CSV and TXT files, because the wrapper moves **all** matching files when it archives the results. You can set `RESULT_DIR` to choose a different parent directory (for example, `sudo env RESULT_DIR=/tmp/nvmevirt-repro ./run_exploration.sh ../traces/nvme_occ90.txt`).

## Check the outputs

The run prefixes are `out_hw_zns_k4`, `out_hw_zns_k8`, and `out_hw_conv`. For each prefix, `*_summary.txt` contains `RUN_STATUS` and the `LAT` latency summaries; `*_log.txt` contains replay progress; `*_lat_*.csv` contains latency samples; `*_block_stat_before.csv` and `*_block_stat_after.csv` capture block counters. The conventional run additionally writes `out_hw_conv_conv_ftl_stats.txt`; the ZNS summaries include host GC and WAF counters.