# Simulated FAST '26 PC reviews — "Anchor: Session-Lifetime Data Placement for Agentic LLM Inference on Flash" (v3.1)

Calibration: FAST '26 accepted 44 of 253 (~17%). Scale: 5=strong accept,
4=accept, 3=weak accept, 2=weak reject, 1=reject. Expertise: X=expert,
Y=knowledgeable, Z=some familiarity.

---

## Review A — flash management / data placement expert (WARP/DOGI-adjacent)

**Overall merit: 3 (weak accept). Expertise: X.**

**Summary.** The paper measures what the block interface costs agentic LLM
serving (per-session KV blobs, episodic memory logs, vector indexes) on a
discrete-event port of FEMU's FTL, validated against hardware-measured WAF at
matched over-provisioning. The central result is a conditions matrix: the tax
ranges from ~zero (tuned firmware + immediate whole-object TRIM + cohort-sized
objects) through 5-8x tail inflation (no TRIM, the deployed default) to 14-21x
(as-shipped research-platform firmware), while a session-aligned placement
layer (Anchor, on ZNS or FDP) is invariant to all conditions at WAF 1.000.
Secondary contributions: a two-sided FDP granularity requirement with an
adaptive handle allocator, and a cohort-packing rule derived from replaying
Mooncake's production tool-agent trace.

**Strengths.**
+ First paper to put an LLM/agentic workload under a placement-directed
  device. The gap is real; I checked my own shelf of 2024-26 placement papers
  and none uses this workload class.
+ The declared-lifetime observation is the right framing. WARP told us
  alignment is the necessary condition; DOGI pays an ML model to predict
  invalidation times. This workload hands the oracle over for free. That is a
  genuinely useful thing for this community to have in writing, with numbers.
+ The validation section is more honest than most simulation papers: the
  authors discovered their platform's shipped thresholds are 1.9x off
  hardware-measured WAF, fixed the configuration, and *re-based all their
  headline numbers on the less favorable (to them) tuned baseline*. The
  Mooncake favorable-side result (zero tax when all conditions hold)
  convinces me the models are not rigged. Few papers do this.
+ The two-sided granularity result (32 static handles under 110 sessions are
  *worse* than no hints; adaptive allocation recovers it) is a finding I will
  cite regardless of this paper's fate. It matches, and sharpens, what we saw
  in our own RUH-interference experiments.
+ Deterministic artifact with an automated claims audit. Rare and welcome.

**Weaknesses.**
- No result in this paper was produced by a physical flash device. The WAF
  validation compares against *another paper's* published measurements at one
  workload point. WARP validated against three real FDP SSDs; DOGI ran a real
  ZN540 for end-to-end. For a paper whose claims live entirely inside FTL
  behavior, one cheap ZNS drive or even the authors' own NVMeVirt kit
  (included but never executed!) would have moved me a full point.
- Anchor is a policy, not yet a system. There is no persistence/recovery story
  for the slot maps and zone metadata, no concurrency design, no filesystem
  integration (ZenFS is cited but not touched). Section 4 reads as a
  simulator configuration described in systems language.
- The 16 GiB scaled device makes collection *durations* non-portable, and the
  device-wide superblock assumption maximizes blast radius. The scaling
  discussion (7) is reasonable but deserves a sensitivity run with per-die
  collection domains.

**Questions for rebuttal.**
1. Can you produce any single measurement from NVMeVirt or a real ZNS device
   before camera-ready, even for one configuration pair?
2. At tuned+TRIM, the residual p99.9 gap (2.8-3.5x) is attributed to the
   memory-stream superblocks. Can you show a breakdown (per-stream victim
   composition) proving it?
3. How does Anchor's slot/zone metadata survive a crash?

---

## Review B — ML-serving systems (Mooncake/IMPRESS-adjacent)

**Overall merit: 2 (weak reject). Expertise: Y (storage), X (LLM serving).**

**Summary.** [as above]

**Strengths.**
+ The paper asks a question our community has been sloppy about. We built
  Mooncake-class tiers, published TTFT wins, and never once reported device
  WAF or what our churn does to the SSD's collector. The CHEOPS'25 citation
  is apt: everyone gestures at ZNS/FDP; nobody measured. C1 is a real
  contribution even where the measured tax is small.
+ Using the released Mooncake tool-agent trace with its hash-chain prefix
  structure is the right call, and cohort packing is a practical rule I could
  apply to our block store tomorrow.
+ Writing is disciplined; the conditions matrix (Table 5) is the clearest
  artifact of its kind I have seen in a submission this cycle.

**Weaknesses.**
- The evidence has an uncomfortable shape: the one workload with real
  production provenance (Mooncake) shows **zero tax**, and the tax-bearing
  workload is the authors' synthetic agent model. The paper's honest framing
  (immutable cohort-sized whole-death objects satisfy all conditions) is
  correct, but it means the headline rests on the *synthetic* assumption that
  agent-memory streams (4 KiB index updates, log appends) share media with KV
  at high occupancy. No production trace in the paper demonstrates that
  mixture. TraceLab's released tool-call records could have grounded it.
- The end-to-end capture is weaker than presented: synthetic model weights, a
  scripted tool policy, 12 sessions amplified 400x. I accept the KV geometry
  and file-I/O mechanics are real, but calling it a "live agent stack" in the
  abstract oversells it. On GPUs with vLLM+LMCache (config shipped, never
  run), the picture could differ: chunked prefill, layerwise transfers, and
  LMCache's own aggregation change the write shape.
- The deployment-relevant comparison is thinner than it looks. The no-TRIM
  column is real, but the fix is `-o discard` or an unlink hook — a one-line
  change any serious operator makes before adopting a new storage interface.
  What survives that fix is 2.8-3.5x at p99.9 plus endurance ~1.1x (tuned):
  meaningful, not transformative. The strongest surviving argument is
  invariance/predictability, and the paper should lead with it rather than
  with the as-shipped 21x.
- GPU cache hit rates of 4-5% (cyclic LRU) drive the resume-heavy load. With
  production-grade prefix caching (TraceLab: 96%), the SSD sees far fewer
  full-blob reads and relatively more small memory I/O — probably *helping*
  the paper's tail story, but unmeasured.

**Questions for rebuttal.**
1. Justify the synthetic memory-stream intensities against any measurable
   agent framework (Mem0/MemGPT database write rates, DiskANN insert fanout
   on a real index).
2. Run one configuration with a high prefix-hit GPU cache. Does the tail gap
   grow or shrink?
3. If discard-on-unlink is enabled (the one-line fix), what is the remaining
   quantitative case for ZNS adoption, including endurance at tuned baseline?

---

## Review C — senior PC member, methodology focus

**Overall merit: 2 (weak reject). Expertise: Y.**

**Summary.** [as above]

**Strengths.**
+ Exceptional methodological transparency: the paper documents that its own
  platform's defaults were wrong, re-bases on the corrected configuration,
  reports the less favorable numbers, and ships an automated audit. If every
  submission did this, my job would be easier.
+ The conditions framing is the intellectually correct result, and braver
  than the 36x headline this paper visibly once had (the artifact's revision
  log shows the evolution — commendable).
+ Related work is comprehensive and correctly positioned against FAST 24-26.

**Weaknesses.**
- Simulation-only, self-built simulator, for claims about proprietary device
  behavior. The single-point validation (one workload, one OP level, against
  literature values) supports the WAF *scale*, not tail behavior — and tails
  are the paper's currency. Latency validation against any real device is
  absent. SepBIT passed as simulation-only because it proposed an algorithm
  evaluated on WAF over public traces; this paper's tail-latency claims are a
  higher evidentiary bar.
- The paper is three papers compressed: a characterization (C1), two design
  rules (C2), and a system sketch (C3). Each is ~60% complete. I would
  rather see the characterization alone, done to death — including hardware —
  or the system built for real.
- Reference hygiene: several 2026 arXiv identifiers ([30], [31], [33], [34])
  could not be resolved at review time; two venue attributions (Strata/OSDI
  '26 [20], Continuum/ICLR '26 [35]) need verification. Possibly fine, but
  the burden is on the authors.
- Minor: hit-turn TTFT (n=176) should be an appendix note, not a result;
  Figure 5's GC strip needs the tuned baseline, not just the load; the
  anonymization pass has not been done (author name, AI-assistance note,
  artifact paths).

**Questions for rebuttal.**
1. Which claims in Table 5 would you stake on hardware reproducing within
   2x, and why?
2. Why should tail conclusions transfer given no latency-level validation?

---

## Meta-review (PC discussion summary)

Scores: 3 / 2 / 2. **Outcome in this form: reject, with unusually
encouraging language.** All three reviewers want this paper to exist; none is
comfortable accepting device-internal tail claims produced solely by the
authors' simulator, and B considers the workload's tax-bearing component
insufficiently grounded in production evidence.

The discussion converged on what would flip the room:

**P0 (flips B and C): any real-device or kernel-level result.**
Run the shipped NVMeVirt kit (conventional vs ZNS personality) on one
machine; better, one commodity ZNS drive with the replayer. Even a partial
reproduction of the conditions matrix (TRIM on/off on a real conventional
NVMe is a one-day experiment producing the no-TRIM column on hardware)
converts the paper's category from "simulator study" to "validated study."

**P1 (flips B): ground the memory streams and prefix sharing.**
Replay TraceLab tool-call records (public, CC-BY) for the memory-stream
intensities; add one high-prefix-hit configuration. If the deep-tail gap
survives both, the synthetic-workload objection dies.

**P2 (strengthens A): make Anchor real enough to crash.**
A minimal libzbd/ZenFS-backed prototype of the KV path with metadata
persistence — or an LMCache storage-connector implementation — plus a
recovery paragraph. Alternatively, retitle the contribution as a
characterization + design rules and drop the system claim: A and C both said
they would take the characterization alone if the hardware existed.

**Also required:** citation verification pass; anonymization; per-stream
victim-composition breakdown (A.2); lead the abstract with invariance rather
than the as-shipped multiplier.

The panel noted the fit is right (two AI-storage sessions this year), the
writing passes, and the artifact is ahead of community norms. This is a
resubmission the PC expects to accept once the evidence catches up with the
framing — FAST '27 with P0+P1, or HotStorage '26 for the characterization as
is.
