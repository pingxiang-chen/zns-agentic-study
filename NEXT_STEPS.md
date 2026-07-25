# Next steps to finish the paper

Plan of record for taking "Anchor" (currently v3.4, 13 pp, `paper.pdf`) from
its present state to submission. Two roles: **Helper** (runs the real-device
experiments on the server; everything you need is in `REPRODUCE_HW.md`) and
**Author** (integrates results, finishes the manuscript). Milestones are
ordered by dependency; M1+M2 are the only blocking items.

## Where the paper stands

All simulation, public-trace, harness, and crash-test results are done and
audited. A simulated FAST-calibrated review panel (see `REVIEWS_FAST26_v2.md`)
scored it 4/3/3 — accept-with-shepherd — with one substantive condition left:
**at least one device-level run**. Every other shepherding item has been
executed. The paper builds reproducibly from measured data
(`sim/build_paper_fast.py`), so integrating new results is mechanical.

---

## M1 — NVMeVirt conv vs ZNS replay (Helper, ~3 h, BLOCKING)

Follow `REPRODUCE_HW.md` Parts 0-1 exactly. RAM-backed, no risk to any disk.

**Deliverables** (drop in `results/hw/`, naming as shown):
- `nvmevirt_conv_summary.txt`, `nvmevirt_conv_lat_*.csv`
- `nvmevirt_zns_summary.txt`, `nvmevirt_zns_lat_*.csv`
- `manifest_m1.json`: `{host, kernel: "uname -r", cpu, ram_gb, nvmevirt_commit,
  memmap, trace: "tr_occ90", tscale, date}`
- `dmesg | grep -i nvmev` tail, and `nvme smart-log` output for both devices.

**Definition of done:** both replays complete; the two summary files parse
(they use the same `LAT <class> ...` format as `results/*_summary.txt`).

**Expected result:** zns `index_read_cmd`/`index_write_cmd` p99.9 clearly
below conv; conv shows a GC-correlated tail mode. Absolute numbers WILL
differ from the simulator — that is fine and expected.

**If it diverges** (e.g., conv ≈ zns everywhere): do not tune toward the
paper. Record it, and also capture NVMeVirt's FTL parameters
(`dmesg | grep -i nvmev`, and `ssd_config.h` from the build) — its GC
thresholds decide which column of the paper's Table 6 it should be compared
against. Send everything; interpretation is the Author's job.

## M2 — TRIM on/off on a real NVMe (Helper, ~half day, BLOCKING)

`REPRODUCE_HW.md` Part 2. **Destructive to the target namespace — spare
device only.** The `BLKDISCARD_DISABLE=1` toggle is already built into the
replayer.

**Deliverables:** `real_trim_summary.txt`, `real_notrim_summary.txt`, their
CSVs, smart-log before/after each run (the `data_units_written` deltas are
the WAF proxy), `manifest_m2.json` with the drive model/firmware/namespace
size and the preconditioning command used.

**Definition of done:** both runs on the same namespace with identical
preconditioning; smart-log deltas recorded.

**Expected:** no-TRIM shows a higher smart-log write delta and fatter write
tails. If the drive hides it (huge OP, idle GC), that is a result — note the
drive class.

## M3 — Real ZNS drive + Anchor-on-zones (Helper, optional, ~1 day)

Only if a ZNS SSD is available. Parts 3-4 of `REPRODUCE_HW.md`. The
`-DANCHOR_ZBD` path in `anchor-proto/` is expected to need ~1 h of porting
(seams are marked in the code). Deliverables mirror M1/M2 plus
`crash_results_hw.json`.

## M4 — GPU serving stack (Helper, optional stretch, ~1 day)

Part 5: vLLM+LMCache (or SGLang HiCache) with the KV directory on each
NVMeVirt personality, driven by the agent harness. This retires the last
reviewer caveat (synthetic-weight capture). Deliverables: the harness oplog
plus `iostat -x 1` logs for both mounts.

---

## M5 — Integrate hardware results (Author, ~half day after M1-M2 arrive)

Mechanical steps, in order:
1. Place files under `results/hw/`; run `cd sim && python3 plots.py
   ../results/hw figs_hw` for the side-by-side parse.
2. Add §6.6 "Hardware validation" (~0.4 pp): one table
   (sim / NVMeVirt-conv / NVMeVirt-zns / real-NVMe TRIM / real-NVMe no-TRIM
   across WAF-proxy, rd p99/p99.9, wr p99/p99.9) + one paragraph comparing
   direction and magnitude against Table 6's columns. Numbers formatted
   programmatically like every other table (extend `build_paper_fast.py`).
3. Update the abstract's final sentence from "kits ... accompany the
   artifact" to state what was confirmed on hardware.
4. Update §5.7 (hardware path) from future to past tense; move the weaker of
   Figures 3/8 to Appendix A if the page budget needs the room.
5. Re-run the claims audit; extend it with 3 checks over the hw files.
6. Update `REVIEW.md` changelog (shepherding condition 1: done).

## M6 — Camera-quality pass (Author, ~half day)

- Cross-reference sweep: every "Table N"/"Figure N"/"Section N" against the
  rendered PDF (the table renumbering in v3.4 makes this mandatory).
- Hand-verify the remaining survey-sourced references not yet individually
  checked: [20] Strata/OSDI '26, [32] Samsung white paper, [35] Continuum,
  [40] Kioxia, [46] AITurbo, and the FAST '26 entries ([41], [42], [43],
  [44], [45], [48], [51]). Three flagged ones ([30], [31], [33]) are already
  verified; drop or replace anything unverifiable.
- Decide Reviewer A's suggestion: promote the conditions matrix (Table 6) to
  a first-page figure.
- One full read-aloud pass for revision-churn seams.

## M7 — Anonymize, package, submit (Author + Helper, ~half day)

- Double-blind: replace the author block with "Paper #", delete the
  AI-assistance note (disclose per venue policy instead), scrub
  `alex@192.168.1.83` and any personal paths from all shipped files
  (`grep -ri` the tree), make the artifact link anonymous (anonymous.4open
  or a Zenodo deposit).
- Artifact package for AE badges: the automated claims audit and 3 s-per-run
  determinism are the Functional/Reproduced story; write the 1-page AE
  appendix (inventory, hardware/software requirements, 15-min "kick the
  tires": build sim, run occ90 conv+zns, run the audit).
- Check the actual FAST '27 CFP dates on usenix.org when they post
  (historically: abstract registration late September, full papers ~1 week
  later); build in a one-week buffer for M5/M6 after the last hardware data
  arrives.

---

## Working agreement

- After **M1**, send the two summary files before starting M2 — five
  minutes of checking then can save a day of rework.
- Never edit result files by hand; if a run is bad, rerun it and say why.
- Every run gets a manifest JSON; unmanifested numbers cannot go in the
  paper.
- Divergence from expectations is data. The paper's whole argument is a
  conditions matrix; a hardware point that lands in a different column is
  integrated, not massaged.

## Risk notes

- NVMeVirt vs newest kernels: if the module does not build on the server's
  kernel, check the repo's issue tracker for the kernel-version patch or use
  an LTS kernel (5.15/6.1) in a VM with PCI passthrough as fallback.
- memmap reservation silently wrong → no device: values in grub and
  `load.sh` must match exactly.
- Time-boxing: M1+M2 fit in one working day; do not let M3/M4 delay
  submission — they upgrade the paper but M1+M2 satisfy the shepherd.
