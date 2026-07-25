# Hardware reproduction guide

This document is for the person running the real-device experiments for the
Anchor paper (target machine: `alex@192.168.1.83`, x86_64 Linux, GPU,
spare NVMe capacity). You do not need to have followed the project; this file
is self-contained. Every simulated result in the paper already exists; your
job is to produce the hardware counterparts that the (simulated) FAST
shepherding requires. Questions the experiments answer, in order of value:

| # | Experiment | Time | Hardware needed | Validates (paper) |
|---|-----------|------|-----------------|-------------------|
| 1 | NVMeVirt conv vs ZNS replay | ~3 h | none (RAM-backed) | Table 6 columns, Figs 2-6 direction |
| 2 | TRIM on/off on a real NVMe | ~half day | any spare NVMe namespace | the "no-TRIM deployed" column |
| 3 | Real ZNS drive replay | ~half day | a ZNS SSD (e.g. WD ZN540) | Anchor-Z rows end to end |
| 4 | Anchor durability on real zones | ~half day | same ZNS SSD | Section 4.5 on hardware |
| 5 | GPU stack (vLLM+LMCache / SGLang) | ~1 day | the GPU | Section 5.7 end-to-end path |

Do them in order; Experiment 1 alone satisfies the minimum shepherding
condition. Everything below assumes this artifact directory is copied to the
machine (e.g. `scp -r zns-agentic-study alex@192.168.1.83:~/`).

---

## Part 0: One-time machine preparation

Packages:
```bash
sudo apt install build-essential git make gcc python3 python3-pip \
                 nvme-cli fio linux-headers-$(uname -r) libzbd-dev
pip3 install numpy matplotlib
```

Reserve contiguous physical memory for NVMeVirt (pick sizes that fit your
RAM; 48 GiB reserved starting at 16 GiB shown):
```bash
sudo sed -i 's/^GRUB_CMDLINE_LINUX="/&memmap=48G\\\\\\$16G /' /etc/default/grub
sudo update-grub && sudo reboot
cat /proc/cmdline          # must show memmap=48G$16G
```
If `insmod` later panics in `__pci_enable_msix()`, add `intremap=off` to the
same line and reboot (NVMeVirt/IOMMU incompatibility).

Decompress the paper trace and build the tools:
```bash
cd ~/zns-agentic-study
gunzip -k traces/tr_occ90.txt.gz
cd nvmevirt-kit && make replayer && ./build.sh      # builds nvmev_conv.ko + nvmev_zns.ko
cd ../sim && gcc -O2 -o sim femu_agentic_sim.c -lm  # simulator, for side-by-side comparison
```
Other traces can be regenerated on the machine with the converters in
`sim/` (`gen_trace.py`, `mooncake2trace.py`, `tracelab2trace.py`,
`reqpool2trace.py`); their exact command lines are in `README.md` and in the
tops of the scripts.

---

## Part 1: NVMeVirt, conventional vs ZNS (the minimum required run)

NVMeVirt (FAST '23) emulates NVMe devices inside the kernel with published
deviation <=11.8% (conventional, Samsung-970Pro profile with a full FTL and
7% OP — the same OP the paper uses) and <=3.3% (ZNS, ZN540 profile).

**Device sizing is not free to choose (first-run lesson).** The trace embeds
the paper's occupancy design: its dataset peaks at ~14.1 GiB, sized for a
**16 GiB** device (0.86 mean / 0.91 peak occupancy). The conventional
namespace must be ~16 GiB — on a 32 GiB namespace the FTL runs at ~44%
fill, garbage collection is nearly free, and conventional will look
excellent; that is the paper's low-occupancy regime, not the M1 point. (If
you already made such a run, keep it — label it `*_lowocc_*`; it is a valid
extra datapoint.)

**Conventional personality (~16 GiB namespace):**
```bash
cd ~/zns-agentic-study/nvmevirt-kit
sudo ./load.sh conv 16G 16G "0,1"
lsblk -b -o NAME,SIZE,ZONED       # size should be ~16-17e9 bytes
sudo ./replayer -t ../traces/tr_occ90.txt -d /dev/nvme1n1 -m conv -o out_hw_conv -s 0.25
```
If the namespace comes up materially smaller than 15.5 GiB (NVMeVirt keeps
some reservation for metadata), bump memmap_size until `lsblk -b` shows at
least 16 GiB; the replayer will fail with "dev full" if the namespace is
too small for the dataset.
`-s 0.25` compresses think-time gaps 4x (finishes in ~35 min); use `-s 1`
for a full-fidelity 2 h 10 m run if time allows. The replayer prints
progress and writes `out_hw_conv_summary.txt` plus per-class latency CSVs in
the same format as the simulator.

**ZNS personality** (unload first, then load the zoned module; ~18 GiB
namespace at 64 MiB zones, or ~22 GiB if you built 128 MiB zones):
```bash
sudo rmmod nvmev
sudo ./load.sh zns 16G 18G "0,1"
lsblk -o NAME,SIZE,ZONED          # ZONED column must say host-managed
cat /sys/block/nvmeXnY/queue/chunk_sectors   # zone size in 512B sectors
sudo blkzone report /dev/nvmeXnY | wc -l     # zone count
sudo ./replayer -t ../traces/tr_occ90.txt -d /dev/nvmeXnY -m zns -k 8 -o out_hw_zns -s 0.25
```
The `-k 8` flag (new; `git pull` / re-copy the kit if your replayer predates
it) stripes each KV blob across 8 parallel zone chains, each written
strictly sequentially. Without it, zone write-pointer ordering forces QD1
writes per blob, and ZNS write latency is unfairly serialized relative to
both the conventional device (QD32 writes) and the paper's Anchor design
(full-stripe zones); with `-k 1` you can reproduce that handicap
deliberately as a sensitivity point. Run conv and zns with the same `-s`.

**Zone sizing (important).** The replayer adapts to whatever zone geometry
the device reports, but the trace needs roughly 250 zones' worth of
concurrent state at the paper's 64 MiB zone size (the simulation used
256 x 64 MiB zones, 254 usable, ending at 203 in use / 51 free at
occ-0.86). `build.sh` therefore patches NVMeVirt's WD_ZN540 profile from
its shipped `ZONE_SIZE GB(2)` (only ~24 zones on 48 GiB — zone-per-session
would exhaust them) down to `MB(64)`, giving 768 zones on a 48 GiB
namespace. Sanity-check after loading: `chunk_sectors` should print
`131072` (64 MiB) and the zone count should be in the hundreds. If you
rebuilt NVMeVirt yourself without the kit's patch, either re-run
`./build.sh` or expect "ZNS out of zones"-style failures. Also record
`sudo nvme zns id-ns /dev/nvmeXnY -H | grep -i -A1 'active\|open'` in the
manifest: if the module enforces a small max-active/open-zones limit,
report it — the replayer holds ~35 zones open concurrently.

**What success looks like.** Compare `out_hw_conv_summary.txt` against
`out_hw_zns_summary.txt` and against the paper's Table 6 (tuned column for
whatever GC behavior NVMeVirt's FTL exhibits; its conv FTL is
non-preemptive, so expect it between the paper's tuned and as-shipped
columns). The claims being validated are directional, per the paper's
scaling argument (Section 7): zns p99.9 for `index_read_cmd` /
`index_write_cmd` markedly below conv; conv shows GC-correlated tail modes;
medians comparable. Absolute values will differ from the simulator (real
timing profile, kernel I/O path) — that is expected and fine.

**Also capture** (attach to the results):
```bash
dmesg | grep -i nvmev | tail -50       # module config echo
cat /sys/block/nvmeXnY/stat            # BEFORE and AFTER each run
```
Note: `nvme smart-log` reporting zeros on NVMeVirt is **normal** — the
module stubs the SMART log page and does not account data units. Use the
kernel's block-layer counters instead: field 7 of
`/sys/block/<dev>/stat` is sectors written (field 3 is sectors read);
before/after deltas give the host-write volume. Device-internal write
amplification is not observable on NVMeVirt from the host at all — for M1
the evidence is the latency distributions, which is what the paper's
hardware section uses.

---

## Part 2: TRIM on/off on a real conventional NVMe (the deployed-reality column)

This is the cheapest high-value hardware experiment: it reproduces the
paper's "tuned, no TRIM" column on physical flash. **It is destructive to
the target namespace — use a spare namespace or a drive you can wipe.**

The replayer issues `BLKDISCARD` on every session delete in conv mode. Run
the same trace twice on the raw device:

```bash
# Run A: deletes reach the device (immediate discard)
sudo ./replayer -t ../traces/tr_occ90.txt -d /dev/nvme0n2 -m conv -o out_real_trim -s 0.25

# Run B: deletes withheld (edit toggle: run with discard disabled)
sudo BLKDISCARD_DISABLE=1 ./replayer -t ../traces/tr_occ90.txt -d /dev/nvme0n2 -m conv -o out_real_notrim -s 0.25
```
(If your copy of `replayer.c` predates the `BLKDISCARD_DISABLE` env check,
add `if (getenv("BLKDISCARD_DISABLE")) errno = EOPNOTSUPP; else` before the
`ioctl(fd, BLKDISCARD, ...)` call and rebuild — one line.)

Precondition the drive identically before each run (fills the FTL to steady
state; ~10 min per pass on a small namespace):
```bash
sudo fio --name=precond --filename=/dev/nvme0n2 --rw=write --bs=128k --loops=2 --direct=1
```
Capture `nvme smart-log` before/after each run: the delta of
`data_units_written` versus the replayer's logged user bytes is the
device-level WAF proxy. Expected result: Run B shows higher WAF proxy and a
fatter `index_read/index_write` tail than Run A, in the direction of the
paper's Table 6 no-TRIM column. On a drive with generous OP or idle-time GC
the gap may be smaller — report whatever you measure.

---

## Part 3: Real ZNS drive (if available)

Kernel must have `CONFIG_BLK_DEV_ZONED` (any distro kernel >= 5.10). Check
the drive:
```bash
sudo nvme zns id-ns /dev/nvme2n1 -H | head -20
sudo blkzone report /dev/nvme2n1 | head -5
cat /sys/block/nvme2n1/queue/chunk_sectors
```
Then simply:
```bash
sudo ./replayer -t ../traces/tr_occ90.txt -d /dev/nvme2n1 -m zns -o out_realzns -s 0.25
```
The replayer reads the zone size from sysfs, places KV blobs
zone-per-session, resets with `BLKRESETZONE`, and enforces QD1 intra-zone
writes. Note the paper's zone-width result: if the drive's zones stripe a
subset of dies, expect the resume/write-bandwidth penalty of Figure 10 —
that is a result, not a failure. Compare against Part 2's Run A on the same
box.

---

## Part 4: Anchor durability prototype on real zones

`anchor-proto/anchor_meta.c` runs its crash campaign against a backing file
by default; the `-DANCHOR_ZBD` path targets libzbd but is **untested** — the
honest expectation is an hour of porting (zone size discovery from
`zbd_get_info`, replacing the file `pwrite` with zone-append or WP-tracked
`pwrite`, and using the real Zone Report in recovery instead of the tag
scan; the tag-scan code marks exactly the seams). Once it compiles:
```bash
cd ~/zns-agentic-study/anchor-proto
gcc -O2 -DANCHOR_ZBD -o anchor_meta anchor_meta.c -lzbd
python3 crash_test.py ./anchor_meta /dev/nvme2n1 600     # ~10 min of kill -9 trials
```
Target: reproduce "zero committed-data losses" on hardware; recovery time
will drop versus the file-backed 63 ms because the Zone Report replaces the
scans. File-backed results (175 trials, 0 losses) are in
`anchor-proto/crash_results.json` for comparison.

---

## Part 5: GPU serving stack (optional, strongest possible upgrade)

Follow `e2e/gpu-server-kit/README.md`: serve a real model with vLLM +
LMCache's local-disk backend (or SGLang HiCache file tier), mount the KV
directory on the NVMeVirt conventional namespace, run the agent harness,
then repeat with the KV directory on the zoned setup. This closes the last
reviewer caveat (synthetic-weight capture) with a real model end to end.

---

## Handing results back

Send back, per experiment: the `out_*_summary.txt` files, the
`out_*_lat_*.csv` files, the smart-log before/after snippets, `dmesg`
excerpts, and one line about the hardware (drive model, namespace size,
kernel). Drop them in `results/hw/`. The analysis in `sim/plots.py` parses
the same summary format, so a side-by-side "simulated vs hardware" figure is
one command:
```bash
cd sim && python3 plots.py ../results/hw figs_hw
```

## Troubleshooting

- `insmod` panic in `__pci_enable_msix` -> add `intremap=off` to grub.
- No device appears after `load.sh` -> check `dmesg | grep nvmev`; memmap
  values must exactly match the grub reservation.
- Replayer "cannot read zone size" -> the namespace is not zoned; check
  `lsblk -o NAME,ZONED`.
- `BLKRESETZONE` EPERM -> run as root on the whole device node, not a
  partition.
- Replayer slower than `-s` implies -> the device is saturated; that is
  data, not an error (note it).

## Safety

Parts 2-4 write to and discard/reset raw block devices. Triple-check device
names with `lsblk` before every command. Never point the replayer at the
system disk. The NVMeVirt namespaces (Part 1) are RAM-backed and safe.
