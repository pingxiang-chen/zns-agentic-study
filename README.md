# Anchor: Session-Lifetime Data Placement for Agentic LLM Inference on Flash (v3)

**Current paper: `paper.pdf`** — 12-page two-column USENIX FAST submission
draft. v3 adds: the named system (Anchor, with Anchor-Z/ZNS and Anchor-F/FDP
backends), positioning against all 102 FAST 2024–2026 papers (gap confirmed:
no paper combines placement interfaces with LLM/agentic workloads; WARP
FAST'26 supplies the alignment condition our workload satisfies by
declaration), a **real end-to-end capture** (llama.cpp serving, per-turn KV
slot save/restore = 12,304 B/token measured, live tool I/O; 72 turns, zero
errors) replayed through the device models (`e2e/`), the Anchor-F inversion
finding (32 handles under 110 sessions: WAF 1.58 > 1.36 baseline), and a
GPU-server harness for vLLM+LMCache / SGLang HiCache (`e2e/gpu-server-kit/`).
Earlier versions preserved: `paper_v2_techreport.pdf`.

---

## Prior-version summary (v2.0)

Research artifact: can ZNS beat a conventional block-interface SSD by ≥10× for
agentic-LLM KV-cache + agent-memory offloading? **Yes, with the claim now
decomposed by robustness (v2.0, revised after an internal FAST/OSDI-calibrated
review — see REVIEW.md):**

- **Model-robust (survives idealized preemptive-GC, session-granular FDP-32,
  and 28% OP):** agent-memory *write* p99 12.8–13.7× (durability barriers vs
  forced GC), WAF 2.2–2.8 → 1.00, endurance 2.1–2.3×, SLO(5 ms) read-miss rate
  46× lower, zero device GC events.
- **Standard non-preemptive model (matches FEMU, NVMeVirt, and real-device
  measurements incl. ZNSwap's 10× on hardware):** agent-memory *read* p99
  14–60×, p99.9 10–15×, mean 8.6–14.8× — seed-stable (34.3–36.8× at occ-0.86)
  and QD-stable (33–129×).
- **Honest limits:** idealized read-suspension closes the read gap for both
  interfaces; FDP with 32 per-session RUHs reaches WAF 1.34 and p99 3.1 ms but
  keeps a 43 ms p99.9; 28% OP ≈ ZNS tails at −23% capacity; resume reads are
  parity; ZNS has a ~93% occupancy ceiling and needs large zones.

## Contents

- `paper.pdf` — **USENIX FAST-format submission draft** (two-column, 7 pp incl.
  refs), with explicit assumptions (§3.1, A1–A5) and three numbered
  contributions:
  **C1** — first quantification of the block-interface tax for agentic LLM
  inference (workload formulation + controlled same-media measurement);
  **C2** — a robustness decomposition separating interface-fundamental costs
  (write tails ~13×, WAF, endurance) from scheduling artifacts (read tails),
  via suspension bracketing + the placement-granularity law (FDP 3/8/32) + OP
  sweep;
  **C3** — zone-per-session: a host-FTL design for agentic state on ZNS
  (measured WAF 1.000, bounded tails, quantified costs) + open artifact with
  NVMeVirt hardware-replay kit.
  (`paper_v2_techreport.pdf` preserves the single-column tech-report v2.0;
  `build_paper_fast.py` / `build_paper.py` regenerate each from results.json.)
- `REVIEW.md` — the internal senior-reviewer critique of v1.0 (W1–W8) and the
  point-by-point revision log that produced v2.0/FAST.
- `sim/` — the simulator and tooling:
  - `femu_agentic_sim.c` — trace-driven discrete-event port of FEMU's bbssd
    FTL (conv), an FDP-like 3-stream variant, and a ConfZNS-style ZNS model.
    `gcc -O2 -o sim femu_agentic_sim.c && ./sim -t trace -d conv|fdp|zns -o out`
  - `gen_trace.py` — deterministic agentic workload generator.
  - `plots.py` — consolidates results, emits figures + `results_table.csv`.
  - `build_paper.py` — builds `paper.pdf` from measured `results.json`
    (numbers in the paper are formatted programmatically from data).
- `figs/` — figures, `results.json`, `results_table.csv`.
- `results/` — per-run summaries (WAF, GC stats, latency percentiles,
  invariant checks) and 1-s timelines for every device × workload.
- `traces/tr_occ90.txt.gz` — the occ-0.86 trace (drives sim AND replayer).
- `nvmevirt-kit/` — hardware validation on your server (alex@192.168.1.83):
  NVMeVirt build/load scripts (conventional + ZNS personalities) and a
  POSIX-AIO trace replayer (O_DIRECT, BLKDISCARD/BLKRESETZONE). See its README.

## Reproduce everything (≈1 minute of compute)

```bash
cd sim
gcc -O2 -o sim femu_agentic_sim.c
B="--ctx0-median-tok 10000 --turns-mean 16 --newtok-mean 400 --arrival-mean 1.5 \
   --horizon 7800 --meas-start 2400 --seed 42"
python3 gen_trace.py $B --occ 1.30 --growth-reserve 0.02 --hard-cap 0.95 \
   --out tr_occ90.txt --stats-out tr_occ90_stats.json
for m in conv fdp zns; do ./sim -t tr_occ90.txt -d $m -o res/occ90_$m; done
python3 plots.py res figs && python3 build_paper.py figs paper.pdf
```

## Provenance

FEMU @ e2d5413 (MoatLab, June 2026) — FTL semantics ported from
`hw/femu/bbssd/ftl.c`; NVMeVirt @ 61c90f7 (snu-csl, May 2026) for the hardware
kit. Full-system FEMU was not run because the analysis sandbox is ARM64
without KVM and stock FEMU's ZNS mode is not timing-comparable to bbssd
(arXiv:2310.19094); the DES port keeps NAND timing identical across devices
by construction. Literature survey and limitations are documented in the paper.
