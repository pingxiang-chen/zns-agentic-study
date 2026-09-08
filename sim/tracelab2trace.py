#!/usr/bin/env python3
"""
Drive the device simulator from TraceLab's released multi-session agent
replay workload (uw-syfi/TraceLab, CC BY 4.0), calibrated by its real
Claude Code session trace.

Realism sources (all from the released artifacts):
  - session structure: multi_session_large.csv (48 sessions, 303 rounds,
    per-round prefix/input/output token counts, session arrivals,
    inter-round tool waits);
  - tool-call rate: 2.0 tool events per round measured in the released real
    session (round_trace.jsonl); per-call storage I/O uses our harness's
    measured tool implementations (4 KiB index slot + log append + reads);
  - prefix-cache reality: the real trace shows 89% of input tokens served
    from cache, so the default resume policy is high-hit (full-blob resume
    only on cold start / capacity eviction), with an LRU variant for
    memory-constrained GPUs.

Two device classes: workstation (8 pages/token, Q4 8B-class) and mobile
(3 pages/token, FP16 sub-1B on-device model; fewer concurrent clones,
longer decode gaps), both on the same 16 GiB-scale device.

Sessions are cloned with arrival jitter (documented amplification) to reach
the occupancy target; TraceLab's tool_wait_after_ms paces consecutive rounds
within each cloned session.
"""
import argparse, csv, json, random, sys
from collections import defaultdict

PG = 4096
GiB = 1073741824
TT_PGS = int((16 * GiB) / PG)  # nvmevirt default geometry: 16 GiB raw
S = 1_000_000_000

ap = argparse.ArgumentParser()
ap.add_argument('--csv', default='/tmp/w/TraceLab/replay/examples/multi_session_large.csv')
ap.add_argument('--mode', choices=['workstation', 'mobile'], default='workstation')
ap.add_argument('--resume', choices=['highhit', 'lru'], default='highhit')
ap.add_argument('--occ', type=float, default=0.85)
ap.add_argument('--op-frac', type=float, default=0.07)
ap.add_argument('--horizon', type=float, default=7800.0)
ap.add_argument('--meas-start', type=float, default=2400.0)
ap.add_argument('--tools-per-round', type=float, default=2.0)   # TraceLab-measured
ap.add_argument('--seed', type=int, default=42)
ap.add_argument('--out', default='trace_tl.txt')
ap.add_argument('--stats-out', default=None)
args = ap.parse_args()

PGTOK = 8 if args.mode == 'workstation' else 3
GPU_GIB = 5.5 if args.mode == 'workstation' else 1.5
DECODE_X = 1.0 if args.mode == 'workstation' else 3.0   # slower on-device decode
usable = int((1 - args.op_frac) * TT_PGS)
IX_PGS, ML_PGS = 32768, 65536
target = int(args.occ * usable)
kv_budget = target - IX_PGS - ML_PGS
gpu_pgs = int(GPU_GIB * (1 << 30) / PG)

rng = random.Random(args.seed)
rows = list(csv.DictReader(open(args.csv)))
sess_rounds = defaultdict(list)
for r in rows:
    sess_rounds[r['session_id']].append(r)
for s in sess_rounds:
    sess_rounds[s].sort(key=lambda r: int(r['round_idx']))
base_sessions = list(sess_rounds.values())

# template: per base session, list of rounds with TraceLab token sizes and
# the observed wait after each round.
templates = []
for rounds in base_sessions:
    tpl = []
    for r in rounds:
        newtok = int(r['input_len']) + int(r['output_len'])
        tpl.append(dict(newtok=max(newtok, 16),
                        wait_after=max(float(r['tool_wait_after_ms']) / 1e3, 0.0)))
    templates.append(tpl)

sessions = [dict(t0=0, ops=[(0, 'WIX', 0, 0, IX_PGS, 1), (0, 'WML', 1, 0, ML_PGS, 0)])]
logical = 0; future_growth = 0
ml_cursor = ML_PGS
lru = {}; gpu_used = 0
stats = dict(arrivals=0, deaths=0, blocked=0, rounds=0, resumes=0,
             tool_calls=0, clones=0, evictions=0)
retained = {}
import heapq
heap = []; seqn = [0]
def push(t, kind, sid): heapq.heappush(heap, (t, seqn[0], kind, sid)); seqn[0] += 1
active = {}
push(0, 'arrive', -1)
arrival_gap = 2.0 if args.mode == 'workstation' else 10.0

def lru_insert(sid, pgs):
    global gpu_used
    if sid in lru: gpu_used -= lru.pop(sid)
    lru[sid] = pgs; gpu_used += pgs
    victims = [k for k in lru if k != sid]
    for k in victims:
        if gpu_used <= gpu_pgs: break
        gpu_used -= lru.pop(k)

while heap:
    t, _, kind, sid = heapq.heappop(heap)
    if t >= args.horizon * S: break
    if kind == 'arrive':
        tpl = templates[rng.randrange(len(templates))]
        growth = sum(r['newtok'] for r in tpl) * PGTOK
        if logical - sum(p for _, p in retained.values()) + 0.35 * (future_growth + growth) > kv_budget:
            stats['blocked'] += 1
            push(t + int(5 * S), 'arrive', -1)
            continue
        sid2 = len(sessions)
        obj = 2 + (sid2 - 1)
        sessions.append(dict(t0=t, ops=[]))
        active[sid2] = dict(obj=obj, pgs=0, tpl=tpl, k=0, pre_gap=0)
        future_growth += growth
        stats['arrivals'] += 1; stats['clones'] += 1
        push(t, 'round', sid2)
        push(t + int(rng.expovariate(1.0 / arrival_gap) * S), 'arrive', -1)
    elif kind == 'round':
        st = active.get(sid)
        if st is None: continue
        tpl = st['tpl']
        if st['k'] >= len(tpl):
            # session ends; its KV blob is RETAINED for possible reuse
            # (production KV-pool semantics: Continuum TTLs, CachedAttention
            # multi-turn cache); capacity eviction deletes retained blobs LRU
            retained[sid] = (st['obj'], st['pgs'])
            push(t + 3 * S, 'death', sid)
            continue
        # capacity eviction of retained blobs (LRU by retirement order),
        # with hysteresis headroom for in-flight appends
        while retained and logical > 0.96 * (target - IX_PGS - ML_PGS):
            vsid = next(iter(retained))
            vobj, vpgs = retained.pop(vsid)
            sessions[sid]['ops'].append((0, 'DEL', vobj, 0, 0, 0))
            logical -= vpgs
            stats['evictions'] += 1
        r = tpl[st['k']]; st['k'] += 1
        ops = sessions[sid]['ops']
        round_ops_start = len(ops)
        stats['rounds'] += 1
        pre_gap = st['pre_gap']
        st['pre_gap'] = 0
        first = True
        # resume: cold start always; otherwise only under LRU pressure
        need_resume = (st['pgs'] > 0 and
                       ((args.resume == 'lru' and sid not in lru)))
        if need_resume:
            ops.append((pre_gap, 'RKV', st['obj'], 0, st['pgs'], 1))
            stats['resumes'] += 1
            first = False
        # tool calls at the TraceLab-measured rate, harness-measured I/O.
        # TraceLab's tool_wait_after_ms is applied as the next round's pre-gap.
        ntool = int(args.tools_per_round) + (1 if rng.random() < (args.tools_per_round % 1) else 0)
        for _ in range(ntool):
            stats['tool_calls'] += 1
            tool_gap = int(rng.uniform(0.002, 0.015) * S)
            ops.append((pre_gap if first else tool_gap, 'RIX', 0, rng.randrange(IX_PGS), 1, 1 if first else 0))
            first = False
            back = rng.randrange(1, int(0.55 * ML_PGS))
            ops.append((int(0.01 * S), 'RML', 1, max(0, ml_cursor - back), 1, 0))
            ln = rng.randint(1, 2)
            ops.append((int(0.02 * S), 'WML', 1, ml_cursor, ln, 0)); ml_cursor += ln
            ops.append((int(0.005 * S), 'WIX', 0, rng.randrange(IX_PGS), 1, 0))
        # decode + write-through KV append (real per-round token counts)
        delta = r['newtok'] * PGTOK
        gdec = int(r['newtok'] * 0.02 * DECODE_X * S)      # ~50 tok/s ws, ~17 tok/s mobile
        ops.append(((pre_gap + gdec) if first else gdec, 'WKV', st['obj'], st['pgs'], delta, 1 if first else 0))
        st['pgs'] += delta; logical += delta; future_growth -= delta
        if args.resume == 'lru': lru_insert(sid, st['pgs'])
        # Advance by the emitted tool/decode gaps. The round's start time t
        # already includes pre_gap, so count that wait only once. Device
        # I/O latency is determined later by the simulator.
        round_end = t + sum(op[0] for op in ops[round_ops_start:]) - pre_gap
        wait_after = int(r['wait_after'] * S)
        # next round carries TraceLab's post-round tool/user wait as its first-op gap
        if st['k'] < len(tpl):
            st['pre_gap'] = wait_after
            push(round_end + wait_after, 'round', sid)
        else:
            push(round_end + 2 * S, 'round', sid)
    elif kind == 'death':
        st = active.pop(sid, None)
        if st is None: continue
        # logical stays until capacity eviction of the retained blob
        if args.resume == 'lru' and sid in lru:
            gpu_used -= lru.pop(sid)
        stats['deaths'] += 1

n_objs = 2 + max(0, len(sessions) - 1)
with open(args.out, 'w') as f:
    f.write(f"H {n_objs} {int(args.horizon*S)} {IX_PGS} {ML_PGS} {int(args.meas_start*S)}\n")
    for sid, sdat in enumerate(sessions):
        f.write(f"A {sid} {sdat['t0']}\n")
        for (gap, kindo, obj, off, ln, ts) in sdat['ops']:
            f.write(f"O {gap} {kindo} {obj} {off} {ln} {ts}\n")

out = dict(mode=args.mode, resume=args.resume, pgtok=PGTOK,
           base_sessions=len(templates), occ_target=args.occ, **stats)
print(json.dumps(out, indent=1), file=sys.stderr)
if args.stats_out: json.dump(out, open(args.stats_out, 'w'), indent=1)
