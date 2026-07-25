# Simulated FAST '26 PC reviews, round 2 — "Anchor" (v3.2, 13 pp)

Same three reviewers as round 1 (REVIEWS_FAST26.md), re-reviewing after the
revision. Scale: 5=strong accept, 4=accept, 3=weak accept, 2=weak reject.

---

## Review A — flash management / data placement expert

**Overall merit: 4 (accept). Expertise: X. (Round 1: 3.)**

**What changed my score.** My three rebuttal questions were answered with
experiments, not prose. (1) The victim-composition instrumentation now
*measures* the tail attribution I asked for: 76% not-yet-dead KV / 22%
re-copies / 2% memory-stream pages at occ-0.86, inverting to 100%
memory-stream in the TraceLab replay — the mechanism claims are no longer
asserted. (2) The durability layer exists and was attacked: journaled
metadata with epoch/CRC records, alternating checkpoint superblocks,
generation-checked write-pointer reconciliation, and a 175-trial SIGKILL
campaign with zero committed-data losses and 63 ms median recovery. This is
the ZenFS-style recovery story I wanted, implemented rather than sketched.
(3) The simulator-validation section, which I found honest in round 1, has
been folded into the baseline properly.

**Remaining weaknesses.**
- Still no physical flash device. I am now willing to accept on the strength
  of the validated-simulation + crash-tested-prototype package — the
  SepBIT/MiDAS precedent applies — but I want the shepherd to require the
  NVMeVirt run the authors keep shipping and not executing, and the
  camera-ready to say plainly that all latency results are emulated.
- The crash campaign is file-backed; the libzbd path is compiled but
  untested. Acceptable for a prototype claim, must stay clearly labeled.
- The conditions matrix now carries the paper. I would promote it to
  Figure 1.

**Question:** the adaptive handle allocator's spill handle re-creates a
mixed-lifetime stream under overload; did you measure the spill fraction at
which its benefit disappears?

---

## Review B — ML-serving systems

**Overall merit: 3 (weak accept). Expertise: Y/X. (Round 1: 2.)**

**What changed my score.** My core objection — the tax-bearing workload was
the authors' own synthetic model — is substantially addressed. The workload
now has provenance tiers: TraceLab's released real-agent sessions drive the
session shapes and, critically, the tool-call rate (2.0/round, measured);
Mooncake's production tool-agent trace covers the block-pool pattern; and
the memory-augmented profile is now explicitly a *different agent class*
(DiskANN/Mem0-style) with an intensity sweep connecting the two. I
particularly credit the paper for reporting the uncomfortable result
plainly: at TraceLab's measured tool rate, with retention-managed pools,
the block interface at the tuned baseline is fine (WAF 1.001, tails at
parity). The paper's claim has become "the tax is the price of specific,
named conditions," and every condition now has a measured row. The mobile
configuration (3 pages/token, ZUFS-scale zones) answers my deployment-
breadth question.

**Remaining weaknesses.**
- The intensity sweep shows WAF is set by occupancy and churn rather than
  memory-op count, and the memory streams set only *where the tail lands*.
  That is a subtler claim than the abstract still implies; one more sentence
  of scoping there.
- The end-to-end capture remains synthetic-weight with a scripted policy;
  the vLLM+LMCache/SGLang kit remains unexecuted. With the TraceLab tier now
  present this is no longer decisive, but the "live agent stack" phrasing
  should be softened.
- The high-prefix-hit question is answered structurally (TraceLab retention
  pool has essentially no resume traffic) rather than as the ablation I
  asked for; acceptable, worth one sentence connecting the dots.

---

## Review C — senior PC member, methodology

**Overall merit: 3 (weak accept). Expertise: Y. (Round 1: 2.)**

**What changed my score.** The two demands I care most about were met. The
system is now falsifiable: a real metadata implementation with a crash
campaign, an ordering discipline, and a recovery procedure whose one
residual flag (un-acknowledged in-flight delete) is correctly identified as
ambiguity rather than loss — that is precisely the right analysis. And the
evidence chain is now self-checking end to end: hardware-anchored WAF
validation, victim-composition measurement, favorable-side null result on
the production trace, automated claims audit. Few submissions in my stack
this year audit themselves this way.

**Remaining weaknesses, in order.**
- Tail-latency claims remain emulation-only; my round-1 position stands,
  softened from "reject-able" to "shepherd-able" by the validation package.
- Scope breadth is now the paper's biggest self-inflicted risk: four
  workload tiers, three conventional realities, two Anchor backends, a
  durability prototype, and a mobile point in 13 pages. It holds together
  because of the conditions frame, but §6 needs a roadmap paragraph and the
  weakest material (hit-turn TTFT; the eager-reset sensitivity) should move
  to an appendix or out.
- Citation verification remains outstanding for several 2026 arXiv entries
  ([30], [31], [33]); this is now my only hard camera-ready condition.
- Presentation nits from the revision churn: confirm figure cross-references
  after the renumbering; Table 4's eleven rows need a visual grouping rule
  between the conv, FDP, and Anchor families.

---

## Meta-review (round 2)

Scores: 4 / 3 / 3 (was 3 / 2 / 2). **Outcome: accept-with-shepherd at a
typical FAST PC; at worst, top of the discuss pile.** The round-1 blockers
were answered with executed work: real-agent-trace grounding (TraceLab,
Mooncake), measured victim attribution, an adaptive allocator that converts
a negative into a design, and a crash-tested durability layer. What remains
is one class of evidence (a physical or kernel-level device run) and
hygiene (citations, scope pruning, cross-reference check).

Shepherding conditions the PC would set:
1. Execute the NVMeVirt kit (or any real ZNS/conventional pair) for at
   least the occ-0.86 conv/Anchor-Z comparison; report alongside emulation.
2. Verify or remove the unresolvable 2026 arXiv citations.
3. Soften "live agent stack"; scope the abstract's tail-attribution
   sentence; move two weakest sensitivities to an appendix.
4. State emulation scope in the abstract's final sentence.

Panel note: the revision trajectory (documented in the artifact's review
log) did something rare — the headline number went *down* with each
methodological improvement while the paper got stronger. That is what made
reviewer C move.
