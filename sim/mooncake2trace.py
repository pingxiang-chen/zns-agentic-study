#!/usr/bin/env python3
"""
Replay a Mooncake production trace (FAST '25 release; timestamp ms,
input_length, output_length, hash_ids of 512-token prefix blocks) as a
device-simulator trace of an SSD-resident KV block pool.

Semantics: each hash_id names one immutable 512-token KV block
(512 tokens x 8 pages/token at 4-bit quantization = 4,096 pages = 16 MiB).
On a request: prefix blocks resident in the pool are read (KV restore, the
TTFT-critical path); blocks appearing for the first time are written after
the compute gap (their generation/offload); previously seen but evicted
blocks are recomputed upstream and rewritten. The pool holds a fixed block
budget (occupancy target); LRU eviction deletes whole blocks (TRIM on
conventional, zone reset under Anchor). Real production arrival times drive
the replay; the compute gap is input_length/prefill_rate +
output_length/decode_rate.

Output format is the standard simulator trace. Each request becomes one
engine session (RKV reads at QD, gap, WKV writes, plus DEL ops for the
evictions its inserts caused).
"""
import argparse, json, sys, math

PG = 4096
GiB = 1073741824
TT_PGS = int((32 * GiB) / PG)  # nvmevirt default geometry: 40 GiB raw
S = 1_000_000_000
BLK_TOK = 512
PGS_PER_TOK = 8                 # 32 KiB/token (Q4)
BLK_PGS = BLK_TOK * PGS_PER_TOK # 4096 pages = 16 MiB

ap = argparse.ArgumentParser()
ap.add_argument('--trace', required=True)
ap.add_argument('--occ', type=float, default=0.90)
ap.add_argument('--op-frac', type=float, default=0.07)
ap.add_argument('--prefill-tps', type=float, default=8000.0)
ap.add_argument('--decode-tps', type=float, default=60.0)
ap.add_argument('--meas-frac', type=float, default=0.25)
ap.add_argument('--shard', type=int, default=1,
                help='consistent-hash shard count over conversations (replay shard 0), '
                     'mirroring Mooncake pool sharding; the global system-prefix block is pinned')
ap.add_argument('--out', default='trace_mc.txt')
ap.add_argument('--stats-out', default=None)
args = ap.parse_args()

usable = int((1 - args.op_frac) * TT_PGS)
IX_PGS, ML_PGS = 1, 1                       # unused streams (header requires them)
# 8% headroom absorbs transient overshoot from concurrently in-flight requests
budget_blocks = int(args.occ * usable * 0.92) // BLK_PGS

reqs = [json.loads(l) for l in open(args.trace)]
if args.shard > 1:
    def conv_key(r):
        h = r['hash_ids']
        return h[1] if len(h) > 1 else h[0]
    reqs = [r for r in reqs if (conv_key(r) * 2654435761) % args.shard == 0]
t_end = max(r['timestamp'] for r in reqs) / 1e3
meas_start = int(t_end * args.meas_frac * S)
PINNED = {0}                                   # global system-prefix block: pinned resident

resident = {}          # hash_id -> (cohort_obj, slot)
lru = {}               # cohort_obj -> None (dict order = LRU via re-insert)
cohort_blocks = {}     # cohort_obj -> set(hash_ids still mapped)
COHORT = 4             # blocks per cohort object (4 x 16 MiB = one 64 MiB full-stripe zone)
next_obj = 2
sessions = []
stats = dict(requests=0, blk_reads=0, blk_writes=0, blk_rewrites=0, evictions=0,
             hit_blocks=0, miss_new=0, miss_evicted=0, cohorts=0)
seen_ever = set()

for r in reqs:
    t = int(r['timestamp'] * 1e6)          # ms -> ns
    ids = r['hash_ids']
    ops = []
    first = True
    hits, news, rewrites = [], [], []
    for h in ids:
        if h in resident:
            hits.append(h)
        elif h in seen_ever:
            rewrites.append(h)
        else:
            news.append(h)
        seen_ever.add(h)
    # KV restore of resident prefix blocks (TTFT path)
    for h in hits:
        cobj, slot = resident[h]
        ops.append((0, 'RKV', cobj, slot * BLK_PGS, BLK_PGS, 1 if first else 0))
        first = False
        if cobj in lru: lru.pop(cobj)
        lru[cobj] = None
        stats['blk_reads'] += 1
    stats['hit_blocks'] += len(hits)
    # compute gap: prefill of non-resident input + decode of output
    miss_tok = (len(news) + len(rewrites)) * BLK_TOK
    gap = int((miss_tok / args.prefill_tps + r['output_length'] / args.decode_tps) * S)
    # write newly materialized / recomputed blocks (after the compute gap);
    # blocks inserted by one request pack into 64 MiB cohort objects (one
    # full-stripe zone each); eviction is LRU at cohort granularity, and
    # evictions precede the writes they make room for
    cur_cohort, cur_slot = None, 0
    pinned_cohorts = {resident[h][0] for h in PINNED if h in resident}
    for h in news + rewrites:
        while len(resident) + 1 > budget_blocks:
            victim = None
            for cand in lru:
                if cand not in pinned_cohorts and cand != cur_cohort: victim = cand; break
            if victim is None: break
            lru.pop(victim)
            for hb in cohort_blocks.pop(victim, ()):
                resident.pop(hb, None); stats['evictions'] += 1
            ops.append((0, 'DEL', victim, 0, 0, 0))
        if cur_cohort is None or cur_slot == COHORT:
            cur_cohort = next_obj; next_obj += 1; cur_slot = 0
            cohort_blocks[cur_cohort] = set(); lru[cur_cohort] = None
            stats['cohorts'] += 1
        resident[h] = (cur_cohort, cur_slot)
        cohort_blocks[cur_cohort].add(h)
        ops.append((gap, 'WKV', cur_cohort, cur_slot * BLK_PGS, BLK_PGS, 1 if first else 0))
        cur_slot += 1
        first = False
        gap = 0
        stats['blk_writes'] += 1
        if h in rewrites: stats['blk_rewrites'] += 1
    stats['miss_new'] += len(news); stats['miss_evicted'] += len(rewrites)
    stats['requests'] += 1
    if ops:
        sessions.append(dict(t0=t, ops=ops))

n_objs = next_obj
horizon = int(t_end * S) + 60 * S
with open(args.out, 'w') as f:
    f.write(f"H {n_objs} {horizon} {IX_PGS} {ML_PGS} {meas_start}\n")
    for sid, sdat in enumerate(sessions):
        f.write(f"A {sid} {sdat['t0']}\n")
        for (gap, kind, obj, off, ln, ts) in sdat['ops']:
            f.write(f"O {gap} {kind} {obj} {off} {ln} {ts}\n")

tot = stats['hit_blocks'] + stats['miss_new'] + stats['miss_evicted']
out = dict(trace=args.trace, duration_s=t_end, budget_blocks=budget_blocks,
           occ_target=args.occ, unique_blocks=len(seen_ever),
           hit_rate=stats['hit_blocks'] / max(tot, 1),
           write_GiB=stats['blk_writes'] * BLK_PGS * PG / 2**30,
           read_GiB=stats['blk_reads'] * BLK_PGS * PG / 2**30, **stats)
print(json.dumps(out, indent=1), file=sys.stderr)
if args.stats_out: json.dump(out, open(args.stats_out, 'w'), indent=1)
