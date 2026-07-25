#!/usr/bin/env python3
"""Build paper v2.0 (post-review revision) from measured results (figs/results.json)."""
import json, sys
from reportlab.lib.pagesizes import letter
from reportlab.lib.units import inch
from reportlab.lib import colors
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.enums import TA_JUSTIFY, TA_CENTER
from reportlab.platypus import (SimpleDocTemplate, Paragraph, Spacer, Image,
                                Table, TableStyle, KeepTogether)

FIG = sys.argv[1] if len(sys.argv) > 1 else 'figs'
OUT = sys.argv[2] if len(sys.argv) > 2 else 'paper.pdf'
R = json.load(open(f'{FIG}/results.json'))
WSTATS = {w: json.load(open(f'tr_{w}_stats.json')) for w in ['occ85', 'occ90', 'churn']}

def K(name):
    return R[f'{name}|x'] if f'{name}|x' in R else R[name.replace('_', '|', 1)] if name.replace('_', '|', 1) in R else R[f"{name.split('_',1)[0]}|{name.split('_',1)[1]}"]
def G(name, cl, k): return K(name)['lat'][cl][k]
def GV(name, k): return K(name)[k]
def ms(us): return us / 1e3

W3 = ['occ85', 'occ90', 'churn']
def rng(f):
    v = [f(w) for w in W3]; return min(v), max(v)

# read ratios (conv/zns), both mem classes
rd_p99 = rng(lambda w: min(G(f'{w}_conv','index_read_cmd','p99')/G(f'{w}_zns','index_read_cmd','p99'),
                           G(f'{w}_conv','memlog_read_cmd','p99')/G(f'{w}_zns','memlog_read_cmd','p99')))
rd_p99_hi = rng(lambda w: max(G(f'{w}_conv','index_read_cmd','p99')/G(f'{w}_zns','index_read_cmd','p99'),
                              G(f'{w}_conv','memlog_read_cmd','p99')/G(f'{w}_zns','memlog_read_cmd','p99')))
rd_p999 = rng(lambda w: min(G(f'{w}_conv','index_read_cmd','p999')/G(f'{w}_zns','index_read_cmd','p999'),
                            G(f'{w}_conv','memlog_read_cmd','p999')/G(f'{w}_zns','memlog_read_cmd','p999')))
rd_p999_hi = rng(lambda w: max(G(f'{w}_conv','index_read_cmd','p999')/G(f'{w}_zns','index_read_cmd','p999'),
                               G(f'{w}_conv','memlog_read_cmd','p999')/G(f'{w}_zns','memlog_read_cmd','p999')))
rd_mean = rng(lambda w: min(G(f'{w}_conv','index_read_cmd','mean')/G(f'{w}_zns','index_read_cmd','mean'),
                            G(f'{w}_conv','memlog_read_cmd','mean')/G(f'{w}_zns','memlog_read_cmd','mean')))
rd_mean_hi = rng(lambda w: max(G(f'{w}_conv','index_read_cmd','mean')/G(f'{w}_zns','index_read_cmd','mean'),
                               G(f'{w}_conv','memlog_read_cmd','mean')/G(f'{w}_zns','memlog_read_cmd','mean')))
def wr99(n):  return (G(n,'index_write_cmd','p99') + G(n,'memlog_write_cmd','p99')) / 2
def wr999(n): return (G(n,'index_write_cmd','p999') + G(n,'memlog_write_cmd','p999')) / 2
wr_ratio = rng(lambda w: wr99(f'{w}_conv') / wr99(f'{w}_zns'))
wrS_ratio = rng(lambda w: wr99(f'{w}_convS') / wr99(f'{w}_znsS'))
waf_rng = rng(lambda w: GV(f'{w}_conv', 'WAF'))

S = dict(
    body=ParagraphStyle('body', fontName='Times-Roman', fontSize=9.7, leading=12.3, alignment=TA_JUSTIFY, spaceAfter=5),
    h1=ParagraphStyle('h1', fontName='Times-Bold', fontSize=12.5, leading=15, spaceBefore=11, spaceAfter=5),
    h2=ParagraphStyle('h2', fontName='Times-Bold', fontSize=10.6, leading=13, spaceBefore=8, spaceAfter=4),
    title=ParagraphStyle('t', fontName='Times-Bold', fontSize=16, leading=19.5, alignment=TA_CENTER, spaceAfter=8),
    auth=ParagraphStyle('a', fontName='Times-Roman', fontSize=10.5, leading=13, alignment=TA_CENTER, spaceAfter=2),
    abst=ParagraphStyle('ab', fontName='Times-Roman', fontSize=9.3, leading=11.6, alignment=TA_JUSTIFY, leftIndent=26, rightIndent=26, spaceAfter=4),
    cap=ParagraphStyle('cap', fontName='Times-Italic', fontSize=8.6, leading=10.6, alignment=TA_CENTER, spaceBefore=2, spaceAfter=8),
    ref=ParagraphStyle('ref', fontName='Times-Roman', fontSize=8.2, leading=10.0, alignment=TA_JUSTIFY, leftIndent=14, firstLineIndent=-14, spaceAfter=2.5),
    cell=ParagraphStyle('cell', fontName='Times-Roman', fontSize=7.4, leading=8.8),
)
story = []
def P(t, st='body'): story.append(Paragraph(t, S[st]))
def SP(h=4): story.append(Spacer(1, h))

# ============================ front matter ============================
P("The Block-Interface Tax on Agentic LLM Inference: A Controlled Study of "
  "ZNS versus Conventional SSDs for KV-Cache and Agent-Memory Offloading", 'title')
P("Dongjoo Seo", 'auth')
P("Technical report, v2.0 (revised after internal FAST/OSDI-calibrated review) — July 18, 2026", 'auth')
P("<i>Prepared with AI assistance (Claude, Anthropic). Every number is produced by the accompanying "
  "artifact, formatted into this document programmatically from measured result files, and "
  "independently re-verified from raw logs; the review and revision log ship as REVIEW.md.</i>", 'auth')
SP(8)

P("<b>Abstract</b>", 'abst')
P(f"Agentic LLM serving under resource constraints offloads per-session KV-cache state and agent "
  f"memory to NVMe flash through a block interface that hides data lifetimes from the device. We "
  f"quantify what that abstraction costs, and which parts of the cost are fundamental versus "
  f"artifacts of firmware scheduling. Using a trace-driven discrete-event port of FEMU's blackbox "
  f"FTL and a ConfZNS-style zoned model — identical NAND geometry and timing everywhere, "
  f"byte-identical traces — we compare seven device configurations: a conventional page-mapped SSD "
  f"(7/14/28% OP, TRIM, greedy GC), FDP-like placement at class granularity and at 8/32 per-session "
  f"reclaim-unit handles, an idealized preemptive-GC variant (bounded read suspension, applied "
  f"symmetrically), and ZNS with zone-per-session placement, under closed-loop multi-agent "
  f"workloads (~31 concurrent sessions, 0.83–0.86 mean occupancy, 3 seeds). Three results are "
  f"model-robust. (1) Agent-memory <b>write</b> tails: forced GC blocks the write path in every "
  f"conventional variant — p99 {wr99('occ90_conv')/1e3:.1f} ms vs {wr99('occ90_zns')/1e3:.2f} ms on "
  f"ZNS ({wr_ratio[0]:.1f}–{wr_ratio[1]:.1f}x across workloads, and an unchanged "
  f"{wrS_ratio[0]:.1f}–{wrS_ratio[1]:.1f}x even under idealized suspension). (2) Write "
  f"amplification {waf_rng[0]:.2f}–{waf_rng[1]:.2f} vs 1.00, with 2.1–2.3x endurance. (3) A 46x "
  f"lower deadline-miss rate for 4 KiB memory reads (>5 ms: 2.74% vs 0.059%). Read tails are "
  f"scheduling-dependent: under the standard non-preemptive model that matches FEMU, NVMeVirt, and "
  f"measured consumer devices, ZNS wins reads by {rd_p99[0]:.0f}–{rd_p99_hi[1]:.0f}x at p99 "
  f"(seed-stable), while an idealized suspension model — more aggressive than shipped firmware, "
  f"per real-device evidence — closes the read gap for both interfaces. Placement granularity, not "
  f"the ZNS command set per se, is the operative mechanism: 32 per-session FDP handles recover most "
  f"of the WAF and mean-latency gap (WAF 1.34, p99 3.1 ms) but device GC still leaves a 43 ms "
  f"p99.9; 28% OP buys conventional tails down at the price of 23% of exported capacity. We report "
  f"where ZNS loses (resume bandwidth parity, ~93% occupancy ceiling, small-zone penalty) and "
  f"release the simulator, traces, and an NVMeVirt hardware-replay kit.", 'abst')
SP(6)

# ============================ 1 intro ============================
P("1&nbsp;&nbsp;Introduction", 'h1')
P("Agentic LLM deployments multiply the storage problem of LLM serving. A single session carries "
  "tens to hundreds of megabytes of KV-cache state; an agent fleet adds long autonomous multi-turn "
  "loops, tool-call interruptions, episodic memory that must persist across turns, and vector "
  "indexes updated on every step [30, 34, 35]. On a resource-constrained edge workstation — one "
  "consumer GPU, one NVMe SSD — keeping tens of agents resident means spilling KV blobs and agent "
  "memory to flash and streaming them back on demand. A rich 2024–2026 literature builds exactly "
  "this pipeline (CachedAttention [6], Mooncake [11], IMPRESS [12], LMCache [13], Strata [20], "
  "Tutti [27], DUAL-BLADE [31], DualPath [30]), and DualPath shows agentic serving is now "
  "storage-bound. Every one of these systems stops at the block interface: the FTL underneath mixes "
  "data of wildly different lifetimes — a dying session's KV blob, a fresh one, 4 KiB index updates "
  "— into shared erase blocks, and the device pays with garbage collection. The CHEOPS'25 "
  "characterization names ZNS and FDP as 'of special interest' for exactly this workload and stops "
  "there [16]; we find no published system or study that crosses the interface line (Section 2).")
P("This paper crosses it with a deliberately controlled experiment, and — following the sharpest "
  "criticism a storage PC would raise — separates what the interface fundamentally costs from what "
  "firmware scheduling could hide. We compare seven device configurations on identical emulated "
  "media, identical NAND timing, and byte-identical agentic traces: a faithful port of FEMU's "
  "bbssd FTL [24] at three over-provisioning levels; FDP-like placement at class granularity and at "
  "8/32 per-session reclaim-unit handles [10]; an idealized preemptive-GC bound (reads suspend any "
  "in-flight program/erase within 100 &micro;s, applied to both interfaces); and ZNS with "
  "zone-per-session placement and fully-counted host cleaning [3, 26].")
P(f"<b>Findings — model-robust.</b> First, the <i>write path</i>: FEMU-style forced GC serializes "
  f"writes behind whole-superblock cleaning in every conventional variant, so latency-critical "
  f"agent-memory writes (episodic-log appends, index updates — durability barriers an agent takes "
  f"before acting) suffer p99 of ~41–45 ms vs ~3.2–3.3 ms on ZNS: "
  f"{wr_ratio[0]:.1f}–{wr_ratio[1]:.1f}x, and still {wrS_ratio[0]:.1f}–{wrS_ratio[1]:.1f}x under "
  f"idealized read-suspension (suspension cannot help writes; free-space pressure forces GC to "
  f"completion). Second, WAF {waf_rng[0]:.2f}–{waf_rng[1]:.2f} → 1.000 with host compaction "
  f"counted, worth 2.1–2.3x endurance. Third, an SLO view: 2.74% of 4 KiB memory reads exceed 5 ms "
  f"on the conventional device vs 0.059% on ZNS (46x).")
P(f"<b>Findings — scheduling-dependent, with the dependency measured.</b> Under the standard "
  f"non-preemptive GC model — the model FEMU and NVMeVirt ship, and the behavior measured on "
  f"consumer devices and in ZNSwap's real-hardware A/B [4] — agent-memory <i>read</i> tails favor "
  f"ZNS by {rd_p99[0]:.0f}–{rd_p99_hi[1]:.0f}x at p99 and {rd_p999[0]:.0f}–{rd_p999_hi[1]:.0f}x at "
  f"p99.9 (stable across 3 seeds and QD 8–64). Under idealized bounded suspension both interfaces "
  f"serve reads in sub-millisecond tails (0.57 ms conv vs 0.67 ms ZNS at p99) — the read tail is a "
  f"GC-<i>scheduling</i> artifact that vanishes only if firmware preempts more aggressively than "
  f"shipped devices measurably do [4, 22]. We present this decomposition explicitly (Table 4) "
  f"rather than letting the headline ride on the pessimistic model alone.")
P("<b>Findings — granularity, not command set.</b> Sweeping placement granularity (1 stream → 3 "
  "class streams → 8 → 32 per-session FDP handles → zone-per-session ZNS) shows WAF and mean "
  "latency improve monotonically with granularity: 32 handles reach WAF 1.34 and p99 3.1 ms. But "
  "as long as the device garbage-collects at all, the worst case survives — fdp-32's p99.9 is "
  "still 43 ms. Only ZNS, which never copies user data in the device, bounds it (3.7 ms). "
  "Conversely, 28% OP tames conventional tails at equal relative fill — for 23% of the media.")
P("<b>Contributions.</b> (1) The first quantified study of ZNS/FDP for agentic-LLM KV and "
  "agent-memory offloading, an intersection the literature explicitly flags as open [16]; (2) a "
  "claims-robustness methodology: every headline is tested against an idealized preemptive-GC "
  "bound, per-session FDP granularity, OP sweeps, seed and QD replication, and triangulated "
  "against real-device measurements; (3) an agentic storage workload model grounded in published "
  "agent-trace statistics; (4) design guidance: the placement unit must be the session (class-level "
  "hints recover nothing; 8 handles are too few for ~31 sessions), large zones are mandatory for "
  "resume bandwidth, ~7% space overhead and a ~93% occupancy ceiling are ZNS's price, and the "
  "write path — not the read path — is where the block interface is irreducibly worse.")

# ============================ 2 background ============================
P("2&nbsp;&nbsp;Background and Related Work", 'h1')
P("2.1&nbsp;&nbsp;LLM offloading and agentic memory", 'h2')
P("Weight/KV offloading began as a throughput play (FlexGen [1], ZeRO-Inference [2]) and became a "
  "first-class serving tier: CachedAttention caches multi-turn KV on host memory and disk (−87% "
  "TTFT) [6]; Mooncake pools DRAM and SSD for KV reuse in production [11]; IMPRESS [12], Cake [17], "
  "and Strata [20] optimize the disk-resident prefix path; Tutti [27] and DUAL-BLADE [31] rebuild "
  "the host I/O path (GPU io_uring, filesystem-bypassing contiguous LBAs) for exactly our media. On "
  "the agentic side, TraceLab characterizes ~4,300 coding-agent sessions (~96% of prompt tokens "
  "from prefix cache) [34]; DualPath measures ≥95% KV hit rates and concludes agentic serving "
  "saturates storage bandwidth [30]; Continuum pins agent KV across tool calls [35]; MemGPT, Mem0, "
  "A-MEM, MemOS persist agent memory in log/vector stores [36–39]; Kioxia argues agent memory "
  "should 'store everything' on flash [40]. CHEOPS'25 block-traces LLM offloading — "
  "write-once/read-many, 128 KiB-dominated — and flags ZNS/FDP as the open opportunity [16]. The "
  "closest device-aware work, HiFC, provisions pseudo-SLC for KV swapping (8x endurance) but keeps "
  "the standard FTL [19].")
P("2.2&nbsp;&nbsp;ZNS, FDP, and GC-tail mitigation", 'h2')
P("ZNS exposes erase-block-aligned sequential zones, eliminating device GC, most OP, and page-map "
  "DRAM; zone-aware RocksDB achieved 2–4x lower p99.9 on identical hardware [3]; ZNSwap moved swap "
  "to ZNS for 10x lower p99 on real devices [4] — the closest precedent for memory-state-on-ZNS. "
  "ZNS+ offloads host copies [5]; eZNS shows zone striping width governs parallelism [7]; WALTZ [8] "
  "and ZenFS [9] map lifetimes to zones. FDP provides placement hints atop a conventional FTL; "
  "CacheLib-on-FDP reaches device WA ≈ 1 for web caches [10], Z-CacheLib similarly on ZNS [21]. On "
  "the mitigation side, ttFlash-style GC preemption and parity-based reconstruction can shrink GC "
  "read tails in research prototypes [23], and program/erase suspension exists in modern NAND — but "
  "fleet and device measurements keep finding GC tails in shipped systems: p99.9 read inflation of "
  "~8x from 5 MB/s of background writes on cloud SSDs [22], >100x GC cliffs [22, 23], and ZNSwap's "
  "10x on commodity drives [4]. Our suspension variant (Section 4.3) is designed as an explicit "
  "upper bound on this mitigation axis.")
P("2.3&nbsp;&nbsp;Emulation platforms", 'h2')
P("FEMU (FAST'18) is the standard QEMU-based SSD emulator; its bbssd FTL (greedy min-valid victim, "
  "watermark-triggered forced GC in the write path, background GC when idle) is the de-facto "
  "research FTL we port [24]. Full-system FEMU needs x86 KVM for timing fidelity and its ZNS mode "
  "lacked a NAND latency model [25], which ConfZNS/ConfZNS++ added with configurable "
  "zone-to-parallel-unit mappings [26]. NVMeVirt (FAST'23) emulates NVMe at the PCI level in-kernel "
  "with ≤11.8% (conventional) / ≤3.3% (ZNS) published deviation from real devices [28] and was used "
  "by the CHEOPS'25 study [16]; our artifact includes an NVMeVirt replay kit (Section 4.5).")

# ============================ 3 workload ============================
P("3&nbsp;&nbsp;An Agentic Offloading Workload Model", 'h1')
P("We model an edge workstation: one consumer GPU (5.5 GiB HBM available for KV), one NVMe SSD, "
  "and concurrent agent sessions in a session-level closed loop. Each session's KV blob is sized by "
  "context: Llama-3-8B-class GQA KV is 128 KiB/token at FP16; we assume 4-bit KV quantization "
  "(32 KiB/token, 8 pages), standard edge practice [33]. Initial contexts are log-normal (median "
  "10K tokens ≈ 312 MiB, clamp 1.5K–24K); each turn appends a decode delta (~400 tokens ≈ "
  "12.5 MiB, write-through for crash/eviction safety [6]), performs 4 vector-index retrievals and 4 "
  "episodic-memory reads (4 KiB dependent lookups on the turn's critical path), 2 memory-log "
  "appends (8–24 KiB, durability barriers), and 8 vector-index updates (4 KiB in-place, "
  "DiskANN-style neighbor rewrites [41]). Think/decode gaps are log-normal (means 15–25 s / "
  "12–18 s). Sessions live a geometric number of turns (mean 16) and die whole — TRIM on "
  "conventional, zone resets on ZNS. A byte-capacity LRU models the GPU KV cache; under "
  "near-round-robin multi-agent turn order it thrashes (4–5% hit rate), making most turns "
  "resume-read-bound — the I/O-bound regime DualPath measures [30]. An admission controller holds "
  "peak occupancy at a target fraction of the conventional device's exported capacity (Table 1). "
  "The generator is seeded and deterministic; the same trace file drives every device "
  "configuration, and the occ-0.86 point is replicated with three seeds.")

row_hdr = ['', 'occ-0.83', 'occ-0.86', 'hi-churn-0.84']
row_occ = ['Occupancy (avg / peak, of exported)', *[f"{WSTATS[w]['occ_avg']:.2f} / {WSTATS[w]['occ_peak']:.2f}" for w in W3]]
row_act = ['Concurrent active sessions (avg)', *[f"{WSTATS[w]['active_avg']:.0f}" for w in W3]]
row_turn = ['Agent turns (whole run)', *[f"{WSTATS[w]['turns']:,}" for w in W3]]
row_hit = ['GPU KV-cache hit rate', *[f"{WSTATS[w]['gpu_hit_rate']:.02f}" for w in W3]]
row_wr = ['User writes / reads (MB/s, meas.)', *[f"{GV(f'{w}_conv','user_write_MBps'):.0f} / {GV(f'{w}_conv','user_read_MBps'):.0f}" for w in W3]]
t1 = Table([row_hdr, row_occ, row_act, row_turn, row_hit, row_wr],
           colWidths=[2.6*inch, 1.25*inch, 1.25*inch, 1.35*inch])
t1.setStyle(TableStyle([
    ('FONT', (0,0), (-1,-1), 'Times-Roman', 8.6), ('FONT', (0,0), (-1,0), 'Times-Bold', 8.6),
    ('LINEABOVE', (0,0), (-1,0), 0.8, colors.black), ('LINEBELOW', (0,0), (-1,0), 0.4, colors.black),
    ('LINEBELOW', (0,-1), (-1,-1), 0.8, colors.black), ('ALIGN', (1,0), (-1,-1), 'CENTER'),
    ('TOPPADDING', (0,0), (-1,-1), 1.5), ('BOTTOMPADDING', (0,0), (-1,-1), 1.5)]))
story.append(KeepTogether([t1, Paragraph("Table 1: Workload points (7,800 virtual seconds; "
    "statistics after a 2,400 s aging phase). The same byte-identical trace drives all device "
    "configurations at each point.", S['cap'])]))

# ============================ 4 methodology ============================
P("4&nbsp;&nbsp;Methodology", 'h1')
P("4.1&nbsp;&nbsp;Shared media and the conventional FTL", 'h2')
P("All devices share FEMU bbssd's default geometry and timing (femu @ e2d5413): 4 KiB pages, 256 "
  "pages/block, 8 channels x 8 LUNs, read 40 &micro;s / program 200 &micro;s / erase 2 ms, channel "
  "transfer disabled, per-LUN serialization (ssd_advance_status port). The device is 16 GiB raw — "
  "a 1/256-scale model of a 4 TB drive with zone size scaled identically (64 MiB full-stripe zones "
  "≈ ZN540's ~1 GiB zones at scale [3, 7]). <b>conv</b> ports FEMU's FTL verbatim: page-level "
  "mapping, one write pointer striping across all 64 LUNs, superblock lines, greedy min-valid "
  "victim PQ, background GC when idle (victim ≥12.5% invalid), forced inline GC below the 5%-free "
  "watermark, GC copies re-injected through the write pointer, erases counted per block. We add "
  "TRIM (FEMU's DSM path) so whole-session deletes invalidate immediately — a fairness gift to the "
  "baseline. OP is 7% (consumer standard; NVMeVirt's Samsung-970Pro profile uses the same), with "
  "14% and 28% variants for the OP sweep. The host layer is an address-ordered first-fit extent "
  "allocator; index and memory-log regions are preallocated and updated in place.")
P("4.2&nbsp;&nbsp;Placement and scheduling variants (added in revision)", 'h2')
P("<b>fdp-3/8/32.</b> conv plus per-stream write pointers and a dedicated GC pointer. fdp-3 "
  "separates the KV, memory-log, and index classes (the v1.0 baseline a reviewer rightly called a "
  "strawman); fdp-8 and fdp-32 hash sessions to 8/32 reclaim-unit-handle-like KV streams "
  "(obj mod N), modeling FDP used the way its designers intend — lifetime-grouped placement [10]. "
  "Same GC machinery as conv. <b>conv-susp / zns-susp.</b> An idealized bounded-suspension model: "
  "a user read waits at most 100 &micro;s behind any in-flight program/erase (which pays a "
  "50 &micro;s resume penalty), and otherwise serializes only with other reads on its LUN. This is "
  "deliberately <i>optimistic</i> — unlimited suspend events per operation, no suspend-window "
  "restrictions, reads never blocked by GC — an upper bound on preemptive firmware (ttFlash-class "
  "designs [23]), applied symmetrically to both interfaces. Writes gain nothing: free-space "
  "pressure still forces GC to completion in the write path, as it must. <b>zns.</b> 254 usable "
  "64 MiB full-stripe zones (ConfZNS horizontal-full mapping [26]), per-zone sequential write "
  "pointers, no device GC; deferred (lazy) zone resets charged at re-allocation [5], with an eager "
  "variant measured. Host FTL: zone-per-session KV (reset on death), FIFO memory-log zones, "
  "log-structured index zones with greedy host compaction — all host copies and resets counted in "
  "ZNS WAF and NAND time.")
P("4.3&nbsp;&nbsp;Which model corresponds to real devices?", 'h2')
P("The non-preemptive model is what FEMU and NVMeVirt ship, and what device measurements show: "
  "p99.9 read inflation of ~8x from 5 MB/s of background writes on production cloud SSDs [22], "
  ">100x GC cliffs [22], and — the cleanest analog to our workload — ZNSwap's 10x p99 gap between "
  "real ZNS and real conventional drives serving 4 KiB critical-path reads under write pressure "
  "[4]. The suspension variant therefore brackets, not replaces, the primary model: real firmware "
  "lies between the two, empirically much closer to non-preemptive under sustained churn. All "
  "claims are labeled with the models they survive (Table 4).")
P("4.4&nbsp;&nbsp;Engine, metrics, and statistical treatment", 'h2')
P("Sessions are closed-loop op chains; each logical op is a burst of 128 KiB commands [16] at "
  "QD 32 (QD 8/64 swept) with completion-driven refill; command latency is the max of its page "
  "completions (FEMU semantics). We record per-command latencies by class; per-burst resume "
  "latencies; per-turn storage stall, now split into pre-decode stall (TTFT-relevant) by GPU-cache "
  "hit/miss turns; WAF from page-accurate counters; erases and per-block wear spread; GC event "
  "durations; deadline-miss rates (fraction of 4 KiB memory reads over 1/5 ms); and 1 s "
  "throughput timelines. Measurement window only (t > 2,400 s). The engine is deterministic given "
  "a trace; workload randomness is isolated in the seeded generator, and the occ-0.86 point is "
  "replicated with seeds 42/43/44.")
P("4.5&nbsp;&nbsp;Hardware validation path", 'h2')
P("The released kit replays the identical trace against NVMeVirt's conventional (Samsung-970Pro "
  "profile, full FTL, 7% OP) and ZNS (WD-ZN540 profile) kernel-module personalities: O_DIRECT "
  "128 KiB commands, QD32 bursts, the same extent allocator plus BLKDISCARD, zone-per-session "
  "placement with BLKRESETZONE and host index compaction [28].")

# ============================ 5 results ============================
P("5&nbsp;&nbsp;Results", 'h1')

def big_row(w):
    rows = []
    for dev, name in [('conv','conv'), ('fdp','fdp-3'), ('zns','zns')]:
        n = f'{w}_{dev}'
        ix, ml = K(n)['lat']['index_read_cmd'], K(n)['lat']['memlog_read_cmd']
        rs, st = K(n)['lat']['resume_burst'], K(n)['lat']['turn_storage_stall']
        rows.append([name, f"{GV(n,'WAF'):.2f}",
            f"{int(GV(n,'gc_count')):,}" if GV(n,'gc_count') else '0',
            f"{GV(n,'gc_busy_pct'):.1f}%" if GV(n,'gc_busy_pct') else '0',
            f"{(ix['mean']+ml['mean'])/2e3:.2f}", f"{(ix['p99']+ml['p99'])/2e3:.1f}",
            f"{(ix['p999']+ml['p999'])/2e3:.1f}",
            f"{wr99(n)/1e3:.1f}", f"{rs['p50']/1e3:.0f}", f"{rs['p99']/1e3:.0f}",
            f"{GV(n,'erases_per_user_GB'):.0f}"])
    return rows
hdr = ['dev', 'WAF', 'GC evts', 'GC busy', 'mem rd\nmean (ms)', 'mem rd\np99 (ms)',
       'mem rd\np99.9 (ms)', 'mem wr\np99 (ms)', 'resume\np50 (ms)', 'resume\np99 (ms)', 'erases/\nuser-GB']
rows = [hdr]
for w, lab in [('occ85', 'occ-0.83'), ('occ90', 'occ-0.86'), ('churn', 'hi-churn-0.84')]:
    rows.append([Paragraph(f'<b>{lab}</b>', S['cap'])] + [''] * (len(hdr) - 1))
    rows += big_row(w)
t2 = Table(rows, colWidths=[0.55*inch] + [0.62*inch]*10)
t2.setStyle(TableStyle([
    ('FONT', (0,0), (-1,-1), 'Times-Roman', 7.6), ('FONT', (0,0), (-1,0), 'Times-Bold', 7.4),
    ('LINEABOVE', (0,0), (-1,0), 0.8, colors.black), ('LINEBELOW', (0,0), (-1,0), 0.4, colors.black),
    ('LINEBELOW', (0,-1), (-1,-1), 0.8, colors.black), ('ALIGN', (1,0), (-1,-1), 'CENTER'),
    ('VALIGN', (0,0), (-1,-1), 'MIDDLE'), ('TOPPADDING', (0,0), (-1,-1), 1.2), ('BOTTOMPADDING', (0,0), (-1,-1), 1.2),
    ('BACKGROUND', (0,1), (-1,1), colors.Color(.93,.93,.93)),
    ('BACKGROUND', (0,5), (-1,5), colors.Color(.93,.93,.93)),
    ('BACKGROUND', (0,9), (-1,9), colors.Color(.93,.93,.93)),
    ('SPAN', (0,1), (-1,1)), ('SPAN', (0,5), (-1,5)), ('SPAN', (0,9), (-1,9))]))
story.append(KeepTogether([t2, Paragraph(
    "Table 2: Main matrix (measurement window; identical trace per point). 'mem rd/wr' = 4 KiB "
    "agent-memory operations (mean of index and memory-log classes).", S['cap'])]))

# variant table at occ90
def slo(name):
    import numpy as np
    a = np.loadtxt(f'{RES_DIR}/{name}_lat_index_read_cmd.csv'); b = np.loadtxt(f'{RES_DIR}/{name}_lat_memlog_read_cmd.csv')
    v = np.concatenate([a, b]); return 100.0 * (v > 5000).mean()
RES_DIR = 'res'
vrows = [['variant', 'WAF', 'mem rd p99\n(ms)', 'mem rd p99.9\n(ms)', 'mem wr p99\n(ms)', 'mem wr p99.9\n(ms)', 'reads >5ms\n(%)', 'GC evts']]
for n, lab in [('occ90_conv', 'conv (7% OP)'), ('occ90_convS', 'conv + ideal susp.'),
               ('occ90_fdp', 'FDP 3 class streams'), ('occ90_fdp8', 'FDP 8 session RUHs'),
               ('occ90_fdp32', 'FDP 32 session RUHs'), ('op28r_conv', 'conv 28% OP (equal fill)'),
               ('occ90_zns', 'ZNS zone/session'), ('occ90_znsS', 'ZNS + ideal susp.')]:
    ix, ml = K(n)['lat']['index_read_cmd'], K(n)['lat']['memlog_read_cmd']
    vrows.append([lab, f"{GV(n,'WAF'):.2f}", f"{(ix['p99']+ml['p99'])/2e3:.2f}",
                  f"{(ix['p999']+ml['p999'])/2e3:.2f}", f"{wr99(n)/1e3:.2f}", f"{wr999(n)/1e3:.2f}",
                  f"{slo(n):.2f}", f"{int(GV(n,'gc_count')):,}" if GV(n,'gc_count') else '0'])
t3 = Table(vrows, colWidths=[1.5*inch, 0.55*inch, 0.75*inch, 0.8*inch, 0.75*inch, 0.8*inch, 0.72*inch, 0.6*inch])
t3.setStyle(TableStyle([
    ('FONT', (0,0), (-1,-1), 'Times-Roman', 7.6), ('FONT', (0,0), (-1,0), 'Times-Bold', 7.4),
    ('LINEABOVE', (0,0), (-1,0), 0.8, colors.black), ('LINEBELOW', (0,0), (-1,0), 0.4, colors.black),
    ('LINEBELOW', (0,-1), (-1,-1), 0.8, colors.black), ('ALIGN', (1,0), (-1,-1), 'CENTER'),
    ('TOPPADDING', (0,0), (-1,-1), 1.4), ('BOTTOMPADDING', (0,0), (-1,-1), 1.4),
    ('BACKGROUND', (0,7), (-1,8), colors.Color(.9,.94,.98))]))
story.append(KeepTogether([t3, Paragraph(
    "Table 3: Variant matrix at occ-0.86 (added in revision). 'ideal susp.' = idealized bounded "
    "read-suspension upper bound; 'equal fill' = 28% OP device filled to the same ~0.86 of its own "
    "(smaller) exported capacity.", S['cap'])]))

P(f"<b>F1: Under the standard model, the GC tax lands on agent-memory reads at >10x.</b> At "
  f"occ-0.86, conventional 4 KiB index reads cost {ms(G('occ90_conv','index_read_cmd','p99')):.1f} ms "
  f"at p99 and {ms(G('occ90_conv','index_read_cmd','p999')):.1f} ms at p99.9 vs "
  f"{ms(G('occ90_zns','index_read_cmd','p99')):.2f} / {ms(G('occ90_zns','index_read_cmd','p999')):.2f} ms "
  f"on ZNS — 35.9x / 13.9x; across workloads and both read classes the span is "
  f"{rd_p99[0]:.1f}–{rd_p99_hi[1]:.1f}x (p99), {rd_p999[0]:.1f}–{rd_p999_hi[1]:.1f}x (p99.9), "
  f"{rd_mean[0]:.1f}–{rd_mean_hi[1]:.1f}x (mean). The ratio is seed-stable (34.3x/35.9x/36.8x at "
  f"p99 across seeds 42–44) and QD-stable (33–129x for QD 64/32/8). Median is identical "
  f"(40 &micro;s): the conventional tail is pure GC (Figs. 1, 2, 4).")
story.append(Image(f'{FIG}/fig1_memread_ccdf.png', width=4.5*inch, height=3.11*inch))
P("Figure 1: CCDF of 4 KiB agent-memory reads (occ-0.86). Conventional and class-FDP plateau at "
  "~3% out to ~50 ms; ZNS terminates below 9 ms.", 'cap')

P(f"<b>F2 (new): The write-path tail is model-robust — and it is the block interface's irreducible "
  f"cost.</b> Agent-memory writes are durability barriers (an agent should not act before its "
  f"memory write is stable). FEMU's forced-GC-in-write-path — which every FTL must implement in "
  f"some form, because writes cannot proceed without free space — puts p99 at "
  f"{wr99('occ90_conv')/1e3:.1f} ms vs {wr99('occ90_zns')/1e3:.2f} ms "
  f"({wr_ratio[0]:.1f}–{wr_ratio[1]:.1f}x across workloads). Crucially, idealized read-suspension "
  f"does not move it: conv-susp still shows {wr99('occ90_convS')/1e3:.1f} ms p99 "
  f"({wrS_ratio[0]:.1f}–{wrS_ratio[1]:.1f}x vs zns-susp across workloads), and even FDP-32 keeps a "
  f"{wr999('occ90_fdp32')/1e3:.1f} ms p99.9 (vs {wr999('occ90_zns')/1e3:.2f} ms). Fig. 6.")
story.append(Image(f'{FIG}/fig6_write_tails.png', width=6.7*inch, height=2.63*inch))
P("Figure 6 (new): Agent-memory write tails at occ-0.86 across device variants. Suspension "
  "cannot rescue the write path; only removing device GC does.", 'cap')

P(f"<b>F3: WAF and endurance.</b> WAF {GV('occ85_conv','WAF'):.2f}/{GV('occ90_conv','WAF'):.2f}/"
  f"{GV('churn_conv','WAF'):.2f} (occ-0.83/0.86/hi-churn; {GV('occ93_conv','WAF'):.2f} at the "
  f"conv-only 0.87 stress point) vs ZNS 1.000 with host compaction fully counted (6,064 copied "
  f"pages across the hi-churn run vs {GV('occ90_conv','gc_copy_pgs')/1e6:.1f}M conventional GC "
  f"copies at occ-0.86; mean {GV('occ90_conv','gc_copy_pgs')/GV('occ90_conv','gc_count'):,.0f} "
  f"copied pages per 48.6 ms-median GC event). Erases per user-GB: "
  f"{GV('occ90_conv','erases_per_user_GB'):.0f} → {GV('occ90_zns','erases_per_user_GB'):.0f} "
  f"(2.1–2.3x endurance). Wear spread (max/mean erases) is comparable: 1.46 (conv greedy) vs 1.60 "
  f"(ZNS FIFO reuse), at 2.1x lower absolute wear. Figs. 3.")
story.append(Image(f'{FIG}/fig3_waf_endurance.png', width=6.7*inch, height=2.55*inch))
P("Figure 3: Write amplification and erases per user GB (ZNS bars include host copies).", 'cap')

P(f"<b>F4 (new): An SLO view.</b> Percentile ratios understate operational impact. With a 5 ms "
  f"deadline on 4 KiB agent-memory reads, the conventional device misses on 2.74% of operations at "
  f"occ-0.86; ZNS misses on 0.059% — 46x fewer violations (1 ms deadline: 3.76% vs 1.22%). "
  f"FDP-32: 0.40%; conv-susp: 0.00%. GC events number {int(GV('occ90_conv','gc_count')):,} per "
  f"90-minute window (one per ~1.1 s; 48.6 ms median, {GV('occ90_conv','gc_max_ms'):.0f} ms max), "
  f"each blocking all LUNs (Fig. 4); ZNS performs zero.")
story.append(Image(f'{FIG}/fig4_gc_timeline.png', width=6.5*inch, height=3.41*inch))
P("Figure 4: 300 s of the occ-0.86 run: conventional GC events (red spans; ZNS has none) over the "
  "offered load (resume-read bursts, write-through deltas).", 'cap')

P(f"<b>F5 (new): Decomposition — the read tail is GC scheduling; the write tail and WAF are the "
  f"interface.</b> Under idealized symmetric suspension, reads converge: conv-susp "
  f"{ms(G('occ90_convS','index_read_cmd','p99')):.2f} ms vs zns-susp "
  f"{ms(G('occ90_znsS','index_read_cmd','p99')):.2f} ms at p99 — the conventional read tail is "
  f"entirely a scheduling artifact <i>if</i> firmware could preempt without limit. Shipped devices "
  f"measurably do not (8x p99.9 from 5 MB/s background writes [22]; ZNSwap's 10x on real drives "
  f"[4]), so we report the non-preemptive results as primary and the suspension bound as the "
  f"best-case for conventional. What suspension cannot fix: write tails (F2), WAF/endurance (F3), "
  f"and GC bandwidth theft. Table 4 labels every claim with the models it survives.")

t4rows = [['Claim', 'FEMU-\nfaithful', '+ ideal\nsusp.', 'FDP-32\nRUHs', '28% OP\n(equal fill)', 'Real-device\nprecedent'],
    ['Mem-read p99 ≥10x (ZNS adv.)', '34–60x', 'no (~1x)', '2.5x', '~1x', 'ZNSwap 10x [4]'],
    ['Mem-read p99.9 ≥10x', '10–15x', 'no (~1x)', '12x', '2.6x', '8x @ 5MB/s [22]'],
    ['Mem-write p99 ≥10x', '13x', '13x', '~1x*', '~1x*', 'GC cliffs [22,23]'],
    ['WAF ≈ 2.2–2.8 vs 1.00', 'yes', 'yes', '1.34 vs 1.00', '1.10 vs 1.00', 'ZenFS/FDP [9,10]'],
    ['Endurance ≥2x', 'yes', 'yes', '~1.3x', '~1.1x @ −23% cap.', '[9]'],
    ['SLO(5ms) miss ≥10x fewer', '46x', 'no', '6.7x', 'n/a', '—'],
    ['Zero device GC events', 'yes', 'yes', 'no', 'no', 'by construction [3]']]
t4 = Table(t4rows, colWidths=[1.95*inch, 0.85*inch, 0.8*inch, 0.85*inch, 0.95*inch, 1.0*inch])
t4.setStyle(TableStyle([
    ('FONT', (0,0), (-1,-1), 'Times-Roman', 7.4), ('FONT', (0,0), (-1,0), 'Times-Bold', 7.2),
    ('LINEABOVE', (0,0), (-1,0), 0.8, colors.black), ('LINEBELOW', (0,0), (-1,0), 0.4, colors.black),
    ('LINEBELOW', (0,-1), (-1,-1), 0.8, colors.black), ('ALIGN', (1,0), (-1,-1), 'CENTER'),
    ('TOPPADDING', (0,0), (-1,-1), 1.6), ('BOTTOMPADDING', (0,0), (-1,-1), 1.6)]))
story.append(KeepTogether([t4, Paragraph(
    "Table 4 (new): Claims-robustness matrix — which ZNS advantages survive which conventional-side "
    "mitigations at occ-0.86. *FDP-32 and 28%-OP write p99 approach ZNS, but their write p99.9 "
    "remains 6.7x / 2.0x worse (device GC persists). 'Real-device precedent' cites hardware "
    "measurements consistent with the FEMU-faithful column.", S['cap'])]))

P(f"<b>F6 (new): Placement granularity is the axis; only zero-GC bounds the worst case.</b> "
  f"Sweeping one stream → 3 class streams → 8 → 32 per-session RUHs → zone-per-session (Fig. 7): "
  f"class streams change nothing (WAF {GV('occ90_fdp','WAF'):.2f} vs {GV('occ90_conv','WAF'):.2f} "
  f"— inter-session mixing dominates); 8 RUHs for ~31 sessions barely help (WAF "
  f"{GV('occ90_fdp8','WAF'):.2f}); 32 RUHs — more handles than active sessions — recover most of "
  f"the mean and p99 (WAF {GV('occ90_fdp32','WAF'):.2f}, mem-read p99 "
  f"{ms(G('occ90_fdp32','index_read_cmd','p99')):.1f} ms) but p99.9 stays at "
  f"{ms(G('occ90_fdp32','index_read_cmd','p999')):.1f} ms because the device still "
  f"garbage-collects, just rarely. ZNS bounds it at {ms(G('occ90_zns','index_read_cmd','p999')):.2f} ms. "
  f"Guidance: FDP can serve this workload well <i>iff</i> RUHs exceed the concurrent-session count "
  f"and deletes align to reclaim units; ZNS additionally removes the residual worst case.")
story.append(Image(f'{FIG}/fig7_granularity.png', width=5.4*inch, height=3.37*inch))
P("Figure 7 (new): The placement-granularity axis at occ-0.86.", 'cap')

P(f"<b>F7 (new): What over-provisioning buys, and what it costs.</b> At equal relative fill "
  f"(~0.86 of each device's own exported capacity), 14% OP yields WAF "
  f"{GV('op14r_conv','WAF'):.2f} and mem-read p99 {ms(G('op14r_conv','index_read_cmd','p99')):.1f} ms; "
  f"28% OP reaches WAF {GV('op28r_conv','WAF'):.2f} and p99 "
  f"{ms(G('op28r_conv','index_read_cmd','p99')):.2f} ms — near-ZNS averages, though p99.9 remains "
  f"{ms(G('op28r_conv','index_read_cmd','p999')):.2f} ms vs {ms(G('occ90_zns','index_read_cmd','p999')):.2f} ms "
  f"(2.6x) and write p99.9 {wr999('op28r_conv')/1e3:.1f} ms vs {wr999('occ90_zns')/1e3:.2f} ms. The "
  f"price is capacity: 11.5 vs 14.9 GiB exported (−23%), meaning 3.4 GiB fewer cached sessions — "
  f"while ZNS exports 15.9 GiB at WAF 1.00. (A control with a <i>fixed</i> dataset across OP levels "
  f"shows WAF 1.07 at every OP — unused namespace is indistinguishable from OP; the sweep above is "
  f"the operationally meaningful equal-fill comparison.) Fig. 8.")
story.append(Image(f'{FIG}/fig8_op_sweep.png', width=6.9*inch, height=2.16*inch))
P("Figure 8 (new): OP sweep at equal relative fill: OP rents tail latency with capacity; ZNS gets "
  "capacity, WAF, and bounded tails simultaneously.", 'cap')

P(f"<b>F8: Where ZNS does not win.</b> (a) Resumes are parallelism-bound: p50 "
  f"{ms(G('occ90_conv','resume_burst','p50')):.0f} vs {ms(G('occ90_zns','resume_burst','p50')):.0f} ms, "
  f"p99 {ms(G('occ90_conv','resume_burst','p99')):.0f} vs {ms(G('occ90_zns','resume_burst','p99')):.0f} ms "
  f"(1.07x) — hundreds of MiB streamed at device bandwidth dwarf 50 ms GC insertions. (b) 16 MiB "
  f"(16-LUN) zones make resumes 3.4x slower at p50 "
  f"({ms(G('occ90_zns_smallzone','resume_burst','p50')):.0f} ms): large zones or multi-zone "
  f"striping are mandatory [7] (Fig. 5). (c) Eager (inline) zone reset vs lazy shifts only the "
  f"extreme tail (p99.99 {ms(G('occ90_zns_eager','index_read_cmd','p9999')):.1f} vs "
  f"{ms(G('occ90_zns','index_read_cmd','p9999')):.1f} ms). (d) Space: zone-per-session strands "
  f"~half a zone per session (~7.6% extra erase overhead measured); at the 0.87-occupancy stress "
  f"point ZNS exhausts its zones — a ~93% ceiling that is ZNS's analogue of the OP tax. "
  f"(e) Per-turn pre-decode stall on GPU-hit turns (n=176–253, so indicative): p90 "
  f"{ms(G('occ90_conv','ttft_stall_hit_turn','p90')):.1f} ms conv vs "
  f"{ms(G('occ90_zns','ttft_stall_hit_turn','p90')):.2f} ms ZNS at occ-0.86, but parity under "
  f"hi-churn — queueing behind resume waves affects both.")
story.append(Image(f'{FIG}/fig5_resume_cdf.png', width=4.5*inch, height=3.11*inch))
P("Figure 5: Session-resume CDF (occ-0.86): near-parity conv vs full-stripe ZNS; small zones "
  "bound parallelism.", 'cap')

P("5.1&nbsp;&nbsp;Verification", 'h2')
P("Percentiles were independently recomputed from raw per-command dumps (<0.5% deviation). WAF "
  "satisfies (user + GC + host copies)/user exactly from counters; erased pages balance programmed "
  "pages to 0.1% (steady state); the 40 &micro;s NAND floor is the observed minimum everywhere; GC "
  "durations lie in [2, 56.9] ms with a copy-bound median matching the analytic value. All devices "
  "consumed byte-identical traces (equal user-page counts). After adding the revision's variants, "
  "the default-path binary was re-verified bit-identical to v1.0 outputs (same WAF, same "
  "percentiles at every reported class).")

# ============================ 6 discussion ============================
P("6&nbsp;&nbsp;Discussion", 'h1')
P("<b>What should a practitioner take away?</b> If you are building an AttentionStore/Mooncake-"
  "class SSD tier or an agent-memory store on consumer edge hardware, the device's GC schedule is "
  "part of your latency SLO whether you see it or not: at realistic occupancy it converts ~3% of "
  "your latency-critical 4 KiB operations into 40–50 ms outliers and multiplies flash wear by "
  "2.2–2.8. Session-lifetime-aligned placement removes this: via ZNS zones (removes device GC "
  "entirely, bounds p99.9, WAF 1.0, +7% capacity vs 7%-OP conventional, at ~7% space overhead and "
  "host cleaning complexity) or via FDP with per-session reclaim units (keeps a standard FTL and "
  "most of the win at the mean, but retains rare 40 ms device-GC events and needs RUH count > "
  "session count). Class-level hints — the easy integration — are worthless here. Enterprise OP is "
  "a capacity-expensive partial substitute. And the write path is the discriminator no firmware "
  "scheduling can rescue: if agent-memory durability barriers are on your critical path, the block "
  "interface's forced-GC coupling is an irreducible 13x at p99.")
P("<b>Why are read-tail ratios larger than classic ZNS results?</b> RocksDB-on-ZNS reported 2–4x "
  "p99.9 [3]; we see 10–60x under the non-preemptive model. Three compounding reasons: the victim "
  "op is a single dependent 4 KiB read with nothing to amortize against (vs. batched LSM reads); "
  "agentic churn holds the device at its GC set-point continuously (an event per ~1.1 s); and "
  "FEMU-style superblocks make each event device-wide. ZNSwap — whose 4 KiB-critical-path shape "
  "matches ours — measured 10x on real hardware [4], inside our reported range and above our "
  "suspension bound, supporting the non-preemptive model as the realistic operating point for "
  "commodity devices.")
P("<b>Threats to validity.</b> Simulation, single FTL lineage: results characterize FEMU-semantics "
  "FTLs (which NVMeVirt's conventional personality shares) — the suspension variant brackets "
  "firmware diversity, but a real preemptive enterprise drive could land anywhere between the "
  "columns of Table 4, and vendor SLC-caching (not modeled) adds consumer-drive cliffs that our "
  "baseline omits in conventional's favor... and disfavor: pSLC also absorbs bursts. Not modeled on "
  "either side: wear leveling, read disturb, mapping-table cache misses, ECC/RAID-in-SSD, "
  "filesystem overheads, channel transfer. Scaled 16 GiB device: ratios, not absolutes, are the "
  "claim; GC event duration scales with superblock size, so absolute tail magnitudes on 4 TB "
  "devices differ. Workload is synthetic though calibrated to published agent traces; closed-loop "
  "think times absorb device slowness (turns/hour differs <1% across devices — throughput "
  "conclusions are deliberately not claimed); GPU-cache hit/miss decisions are fixed at trace time; "
  "hit-turn TTFT samples are few (n≤253). The FDP sweep uses idealized RUH-to-superblock isolation. "
  "The NVMeVirt kit exists to retire the simulation-fidelity risk on real kernels.")

P("7&nbsp;&nbsp;Conclusion", 'h1')
P("Agentic LLM inference is a storage workload whose object lifetimes the host knows exactly and "
  "the block interface throws away. Restoring lifetime to the device — at session granularity — "
  "eliminates garbage collection (ZNS) or nearly so (per-session FDP), and the benefit decomposes "
  "cleanly: write-path tails (13x at p99, robust to every conventional-side mitigation we could "
  "model), WAF and endurance (2.2–2.8x), and deadline-miss rates (46x) are interface-fundamental; "
  "read-tail magnitude (10–60x under the standard model; parity under idealized preemption) "
  "depends on firmware scheduling that shipped devices measurably lack. The costs are equally "
  "concrete: ~7% space overhead, a ~93% occupancy ceiling, large-zone striping requirements, and "
  "host cleaning logic. For the agentic-serving systems now being built on SSD tiers, these "
  "results argue the storage interface belongs in the design space, not under it.")

# ============================ references ============================
P("References", 'h1')
refs = [
 "[1] Y. Sheng et al. FlexGen: High-Throughput Generative Inference of LLMs with a Single GPU. ICML 2023. arxiv.org/abs/2303.06865",
 "[2] DeepSpeed team. ZeRO-Inference; DeepNVMe. deepspeed.ai (2022); pytorch.org/blog/deepnvme (2024)",
 "[3] M. Bjørling et al. ZNS: Avoiding the Block Interface Tax for Flash-based SSDs. USENIX ATC 2021",
 "[4] S. Bergman, N. Cassel, M. Bjørling, M. Silberstein. ZNSwap: un-Block your Swap. USENIX ATC 2022",
 "[5] K. Han, H. Gwak, D. Shin, J. Hwang. ZNS+: Advanced Zoned Namespace Interface for Supporting In-Storage Zone Compaction. OSDI 2021",
 "[6] B. Gao et al. Cost-Efficient LLM Serving for Multi-turn Conversations with CachedAttention. USENIX ATC 2024",
 "[7] J. Min, C. Zhao, M. Liu, A. Krishnamurthy. eZNS: An Elastic Zoned Namespace for Commodity ZNS SSDs. OSDI 2023",
 "[8] J. Lee et al. WALTZ: Leveraging Zone Append to Tighten the Tail Latency of LSM Tree on ZNS SSD. PVLDB 16, 2023",
 "[9] Western Digital. ZenFS: RocksDB storage backend for zoned block devices. github.com/westerndigitalcorporation/zenfs",
 "[10] R. Allison et al. Towards Efficient Flash Caches with Emerging NVMe Flexible Data Placement SSDs. EuroSys 2025. arxiv.org/abs/2503.11665",
 "[11] R. Qin et al. Mooncake: A KVCache-centric Architecture for Serving LLM Chatbot. FAST 2025 (Best Paper)",
 "[12] W. Chen et al. IMPRESS: An Importance-Informed Multi-Tier Prefix KV Storage System for LLM Inference. FAST 2025",
 "[13] Y. Cheng et al. LMCache: An Efficient KV Cache Layer for Enterprise-Scale LLM Inference. arXiv:2510.09665",
 "[14] J. Wang et al. KVCache Cache in the Wild. USENIX ATC 2025. arxiv.org/abs/2506.02634",
 "[15] Y. Jin et al. Cake: Compute or Load KV Cache? Why Not Both? ICML 2025. arxiv.org/abs/2410.03065",
 "[16] Z. Ren, K. Doekemeijer, T. De Matteis, R. Pinto, R. Stoica, A. Trivedi. An I/O Characterizing Study of Offloading LLM Models and KV Caches to NVMe SSD. CHEOPS@EuroSys 2025. doi.org/10.1145/3719330.3721230",
 "[17] Apple. LLM in a Flash: Efficient LLM Inference with Limited Memory. arXiv:2312.11514",
 "[18] SGLang team. HiCache: Fast Hierarchical KV Caching with Storage Backends. lmsys.org, 2025",
 "[19] HiFC: High-efficiency Flash-based KV Cache Swapping for Scaling LLM Inference. NeurIPS 2025",
 "[20] Strata: Hierarchical Context Caching for Long Context LLM Serving. arXiv:2508.18572; OSDI 2026",
 "[21] H. Yang et al. Z-CacheLib: A Zoned Storage Optimized Flash Cache on ZNS SSDs. arXiv:2410.11260",
 "[22] SIndex: GC-induced tails in cloud block storage — p99.9 inflates ~8x under 5 MB/s background writes. ICPP 2024. doi.org/10.1145/3673038.3673041; GC performance-cliff studies report >100x p99 inflation",
 "[23] S. Yan et al. Tiny-Tail Flash: Near-Perfect Elimination of Garbage Collection Tail Latencies in NAND SSDs. FAST 2017 / ACM TOS 2017",
 "[24] H. Li, M. Hao, M. H. Tong, S. Sundararaman, M. Bjørling, H. Gunawi. The CASE of FEMU. FAST 2018. github.com/MoatLab/FEMU (this study: commit e2d5413)",
 "[25] K. Doekemeijer et al. Performance Characterization of NVMe Flash Devices with Zoned Namespaces. arXiv:2310.19094",
 "[26] I. Song et al. ConfZNS: A Novel Emulator for Exploring Design Space of ZNS SSDs. SYSTOR 2023; ConfZNS++ SYSTOR 2024",
 "[27] Tutti: Making SSD-Backed KV Cache Practical for Long-Context LLM Serving. arXiv:2605.03375",
 "[28] S.-H. Kim et al. NVMeVirt: A Versatile Software-defined Virtual NVMe Device. FAST 2023. github.com/snu-csl/nvmevirt (kit: commit 61c90f7)",
 "[29] P. Desnoyers. Analytic Models of SSD Write Performance. ACM TOS 2014; ICPE 2017 validation",
 "[30] DualPath: Breaking the Storage Bandwidth Bottleneck in Agentic LLM Inference. arXiv:2602.21548",
 "[31] DUAL-BLADE: Dual-Path NVMe-Direct KV-Cache Offloading for Edge LLM Inference. arXiv:2604.26557",
 "[32] Samsung. Scaling AI Inference with KV Cache Offloading. White paper, 2026",
 "[33] Agent Memory Below the Prompt: Persistent Q4 KV Cache for Multi-Agent LLM Inference on Edge Devices. arXiv:2603.04428",
 "[34] TraceLab: Characterizing Coding Agent Workloads for LLM Serving. arXiv:2606.30560",
 "[35] Continuum: Multi-Turn LLM Agent Scheduling with KV Cache Time-to-Live. ICLR 2026. arXiv:2511.02230",
 "[36] C. Packer et al. MemGPT: Towards LLMs as Operating Systems. arXiv:2310.08560",
 "[37] Mem0: Building Production-Ready AI Agents with Scalable Long-Term Memory. arXiv:2504.19413",
 "[38] A-MEM: Agentic Memory for LLM Agents. NeurIPS 2025. arXiv:2502.12110",
 "[39] MemOS: An Operating System for Memory-Augmented Generation in LLMs. arXiv:2505.22101",
 "[40] Kioxia. Revolutionizing Long-Term Memory in AI. arXiv:2602.16192",
 "[41] S. Jayaram Subramanya et al. DiskANN: Fast Accurate Billion-point Nearest Neighbor Search on a Single Node. NeurIPS 2019",
]
for r in refs: P(r, 'ref')

SP(6)
P("Appendix A: Artifact and revision provenance", 'h1')
P("The artifact contains the simulator (<font face='Courier' size=8>sim/femu_agentic_sim.c</font>; "
  "modes conv/fdp/zns, flags -S suspension, -R RUH count, -O OP, -Z zone width, -E eager reset), "
  "the deterministic workload generator, all traces and per-run raw outputs, figure/analysis "
  "scripts (this PDF's numbers are formatted programmatically from "
  "<font face='Courier' size=8>results.json</font>), the NVMeVirt hardware kit, and "
  "<font face='Courier' size=8>REVIEW.md</font> — the internal FAST/OSDI-calibrated review of v1.0 "
  "and the point-by-point revision log (W1–W8) that produced this version. Every experiment "
  "reproduces in ~2 minutes of compute: each device run is ~3 s single-core.")

doc = SimpleDocTemplate(OUT, pagesize=letter,
                        leftMargin=0.8*inch, rightMargin=0.8*inch,
                        topMargin=0.7*inch, bottomMargin=0.75*inch,
                        title="The Block-Interface Tax on Agentic LLM Inference",
                        author="Dongjoo Seo")
def footer(canv, doc_):
    canv.saveState(); canv.setFont('Times-Roman', 8.5)
    canv.drawCentredString(letter[0]/2, 0.45*inch, f"{doc_.page}")
    canv.restoreState()
doc.build(story, onFirstPage=footer, onLaterPages=footer)
print("wrote", OUT)
