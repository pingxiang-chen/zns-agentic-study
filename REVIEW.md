# Reviews and revision log

## v3.4: five public real-world datasets

Added request-level KV-pool replays of three more public production traces
alongside the existing Mooncake tool-agent and TraceLab tiers:
- Mooncake conversation trace (FAST'25 release): cohort-packed, parity.
- Azure LLM inference traces 2023, conversation (19,366 req) and code
  (8,814 req) services: real production arrivals/sizes at 175-248 MB/s
  churn. Finding: whole-blob request pools are the block interface's best
  case (WAF 1.000, TRIM-insensitive), and naive zone-per-request zoned
  placement loses 3.4x write-stripe bandwidth.
- BurstGPT (KDD'25; densest 2 h window, 48,107 req): tiny-request mix
  makes zone-per-request infeasible outright (zone exhaustion).
Together these sharpen the cohort rule into a symmetric design law:
lifetime-aligned placement needs the right unit, and the unit is the
cohort, not the request. New Table 2 (workload provenance) and Table 9
(public-replay results); tables renumbered; refs [54] (Azure), [55]
(BurstGPT) added. Two engine fixes from these replays (in-flight writes to
capacity-evicted objects are now dropped on both interfaces, matching
real-system abort semantics) verified determinism-preserving on all prior
results.



## v3.3 shepherding revision (responding to REVIEWS_FAST26_v2.md)

- **Citations (C's hard condition):** all three flagged 2026 arXiv entries
  verified as real via web lookup — DualPath (arXiv 2602.21548, DeepSeek/
  Tsinghua/PKU), DUAL-BLADE (arXiv 2604.26557, Sogang/FSU), Agent Memory
  Below the Prompt (arXiv 2603.04428). Kept; TraceLab verified by direct
  repository clone.
- **Abstract:** final sentence now states emulation scope explicitly;
  "live agent stack" softened to "harness capture ... scripted agent
  policy" (B); tail-attribution scoping clause added (B).
- **Appendix A added:** hit-turn TTFT (small-n) and eager-reset sensitivity
  moved out of the main evaluation (C).
- **A's spill question answered with data:** RUHPOOL instrumentation added;
  measured spill fraction 7% at 31 sessions, 69% at 110 sessions with WAF
  1.011 — graceful degradation, no cliff (new sentence in §6.3).
- **Table 4:** family grouping rules added (C); Table 5 referenced from the
  introduction (A).
- **Consistency:** crash-campaign count corrected to the shipped clean
  campaign (175 trials); section numbering artifacts (5.1b/5.2b) removed;
  recovery median corrected to 63 ms.
- **Outstanding (requires hardware):** shepherding condition 1 — execute
  the NVMeVirt kit or a real ZNS/conventional pair for at least the
  occ-0.86 comparison.


## v3.1 revision (acceptance-routine pass)

Following the FAST acceptance patterns of SepBIT (public production traces),
MiDAS (analytic/hardware cross-check), and WARP/DOGI (validated emulation +
small prototype), v3.1 made five changes:

1. **Simulator validation (MiDAS-style).** Uniform-random-overwrite WAF
   cross-checked against hardware measurements at matched OP: FEMU's shipped
   thresholds land 1.9x off (7.7 vs 3.9-4.1 at 14.7% OP) because the
   background collector over-cleans at high fill; a tuned configuration
   (6% free target, dedicated GC write pointer, both standard in production
   firmware) lands within 16% (4.75) with a monotone OP sweep
   (11.6/4.75/2.08). All conventional/FDP results now use the validated
   configuration; as-shipped is reported as its own condition.
2. **Conditional reframing (the honest consequence of #1).** Under the tuned
   baseline with immediate TRIM, conventional WAF drops to 1.12 and the tax
   concentrates in the deep tail (3.5x/2.8x rd/wr p99.9). Two further
   measured conditions restore it: no-TRIM deployed semantics (7.6x/5.4x,
   WAF 1.47) and as-shipped firmware (21x/14x at p99, WAF 2.49). Table 5 is
   now a conditions matrix; Anchor's column is invariant.
3. **Public production trace (SepBIT-style).** Mooncake's FAST'25 tool-agent
   trace (23,608 requests, prefix-hash chains) replayed as a sharded KV block
   pool. Yields the cohort-packing design rule (4x median restore parallelism)
   and a favorable-side validation: a workload satisfying all conditions shows
   zero tax, demonstrating the models do not penalize the block interface as
   such.
4. **Anchor-F adaptive handle allocator.** Turns the 110-sessions/32-handles
   inversion (WAF 1.58 > 1.36) into a design: exclusive handles for live
   sessions with spill; WAF 1.011 (real trace) / 1.004 (synthetic),
   approaching Anchor-Z's 1.000.
5. **Figures reduced ~9% further and captions tightened; related work extended
   with SepBIT/MiDAS/DOGI positioning and industry FDP deployments.**

Remaining before submission: real-hardware runs (NVMeVirt kit and/or a ZNS
drive on the GPU server), double-blind anonymization, artifact packaging for
badges.

---

# Review of v1.0 (senior PC member, FAST/OSDI calibration) + revision log

## Paper summary

The paper argues that agentic-LLM KV-cache and agent-memory offloading, today built
exclusively on block-interface SSDs, pays a large hidden garbage-collection tax, and
quantifies — via a FEMU-lineage discrete-event FTL model with identical NAND
geometry/timing across devices — that ZNS with zone-per-session placement eliminates
device GC (WAF 2.2–2.8 → 1.00) and improves latency-critical agent-memory read tails
by 10–60× at p99/p99.9, at 0.83–0.86 steady-state occupancy under multi-agent churn.
Negative results (resume-bandwidth parity, a weak FDP baseline, ~93% occupancy
ceiling) are reported.

## Strengths

S1. Timely and genuinely open intersection: the CHEOPS'25 study explicitly calls for
this; no prior ZNS/FDP × LLM-serving system or study exists as of mid-2026.
S2. Controlled methodology: identical media, identical timing, byte-identical traces;
only the interface/placement varies. Counter-level verification (WAF identity,
erase/program balance, percentile recomputation) is unusually thorough.
S3. Honest negative results and a hardware-validation kit (NVMeVirt) are exactly the
right artifacts.
S4. The workload model is grounded in current agent-trace literature (TraceLab,
DualPath, CachedAttention) rather than fio presets.

## Weaknesses (must-fix before acceptance)

W1 — **Baseline fairness / GC pessimism (the central threat).** The conventional
baseline inherits FEMU's non-preemptive, device-wide-superblock GC: a single GC
event blocks all 64 LUNs for up to ~57 ms and reads cannot preempt. Modern
enterprise firmware implements program/erase suspension and per-die GC domains;
without a sensitivity analysis the 35–60× p99 headline is open to the charge that it
measures a modeling artifact, not the interface. Required: (a) an idealized
preemptive-GC variant (bounded suspension) as an upper bound on what firmware
scheduling could recover, applied symmetrically to ZNS; (b) explicit triangulation
against real-device measurements (ZNSwap's 10× p99 on real hardware; the ICPP'24
8× p99.9 inflation from 5 MB/s of background writes); (c) a claims-robustness
decomposition separating model-dependent findings (read tails under non-preemptive
GC) from interface-fundamental ones (write-path blocking, WAF, endurance).

W2 — **The FDP baseline is a strawman.** Three class-level streams cannot separate
session lifetimes; real FDP exposes up to ~128 reclaim-unit handles. A fair
comparison must include session-granular FDP (e.g., 8 and 32 RUHs with sessions
hashed to handles). If per-session RUHs close most of the WAF gap, the paper's
contribution shifts from "ZNS vs. block" to "placement granularity is the axis" —
which is a better paper; say so.

W3 — **Write-path latency is collected but never reported.** Agent-memory writes
(episodic-log appends, index updates) are durability-relevant and on the turn's
critical path if the agent must persist memory before acting. Forced GC blocks
writes regardless of read suspension, so write tails are likely the most
model-robust result — the paper currently hides its strongest evidence.

W4 — **Over-provisioning rebuttal missing.** The obvious operator response is "buy
more OP." Show conv at 7/14/28% OP on a fixed dataset: does OP fix WAF, tails,
both, or neither?

W5 — **Statistical rigor.** Single seed, single run per cell. Percentile tails from
one trace need at least a seed-replication check; QD sensitivity should be shown
not asserted.

W6 — **TTFT framing is coarse.** turn_storage_stall mixes pre-decode (TTFT-relevant)
and post-decode I/O. Decompose; additionally split by GPU-cache hit vs. miss turns —
on hit turns the storage stall should be ~0, and GC destroying hit-turn latency is
precisely the agentic-serving story.

W7 — **Overclaiming in title/abstract.** "Can beat the block interface by an order
of magnitude" is a model-scoped result presented as unconditional. Retitle and
rescope. Also report an SLO-style metric (fraction of memory ops exceeding a
deadline), which practitioners will trust more than p99.9 ratios.

W8 — **Threats/limitations too thin.** Missing: SLC/pSLC cache dynamics of consumer
drives (which produce additional GC-like cliffs favoring the paper's thesis, but
must be stated), wear leveling, read disturb, mapping-table cache misses, filesystem
overheads, ZNS wear spread under FIFO reuse, and the per-page conv allocator's
fragmentation being latency-neutral in-model.

Minor: Fig. 2 label collisions at log-scale; Table 2 workload-label rows wrap;
missing wear statistics; state closed-loop throughput parity explicitly.

## Revision log (v1.0 → v2.0) — what was changed

- W1: Added `-S` bounded-suspension model (100 µs cap, 50 µs resume overhead,
  applied to BOTH conv and ZNS; documented as optimistic-for-conv upper bound);
  added real-device triangulation and a claims-robustness matrix (Table 5);
  headline read-tail claims rescoped to the non-preemptive (consumer-class) model
  with the suspension bound reported alongside.
- W2: Added FDP with 8 and 32 per-session RUHs; new granularity-axis results
  (Fig. 7) and rewritten F5.
- W3: Agent-memory write tails now first-class (new Fig. 6, finding F2); verified
  ≥10× across ALL device variants including suspension and 28% OP.
- W4: New OP sweep at fixed dataset (7/14/28%), finding F8.
- W5: Seed replication (3 seeds) and QD sweep (8/32/64) reported in F9;
  determinism of the engine re-verified after code changes.
- W6: New per-turn pre-decode stall metric split by GPU hit/miss turns (F7).
- W7: Retitled; abstract rewritten with scoped claims; added SLO-miss-rate metric
  (>5 ms deadline) throughout.
- W8: Threats-to-validity section rewritten and expanded; wear-spread statistics
  added to engine output and reported.
- Minor: Table 2 label rows spanned; figure annotations decluttered; throughput
  parity stated with numbers.
