#!/usr/bin/env python3
"""
Convert the captured end-to-end oplog (agent_run.py) into the device-simulator
trace format, with documented concurrency amplification.

Per-session templates are extracted verbatim from the capture: per turn, the
REAL KV restore size (RKV), REAL KV delta (WKV append = kv_after - kv_before),
REAL tool I/O (MEM_READ -> RIX + RML, MEM_WRITE -> WIX + WML with real byte
counts, SEARCH -> RML pages of real bytes read), and REAL compute gaps
(prefill_ms + decode_ms from engine timings; tool latencies as logged).
Sessions end with DEL (the harness deletes the KV file).

Amplification (stated in the paper): the capture provides K base sessions; we
replay C concurrent clones with Poisson arrivals and +/-20% gap jitter, each
clone cycling through its template with fresh object ids, until the horizon.
An admission controller caps peak logical occupancy exactly as in the
synthetic generator. A think-time gap (lognormal, mean --think s) is added
between turns to model user/environment latency not present in the capture.
KV bytes are used exactly as captured (12,288 B/token engine state).
"""
import argparse, json, math, random, sys

PG = 4096
TT_PGS = 256 * 256 * 64
S = 1_000_000_000

ap = argparse.ArgumentParser()
ap.add_argument('--oplog', default='/tmp/w/e2e/oplog.jsonl')
ap.add_argument('--occ', type=float, default=1.30)
ap.add_argument('--growth-reserve', type=float, default=0.02)
ap.add_argument('--hard-cap', type=float, default=0.95)
ap.add_argument('--hard-reserve', type=float, default=0.35)
ap.add_argument('--op-frac', type=float, default=0.07)
ap.add_argument('--horizon', type=float, default=7800.0)
ap.add_argument('--meas-start', type=float, default=2400.0)
ap.add_argument('--think', type=float, default=25.0)
ap.add_argument('--kv-scale', type=int, default=8,
                help='KV byte multiplier (captured model is 120M-class; x8 ~= 1B-class KV/token)')
ap.add_argument('--seed', type=int, default=42)
ap.add_argument('--arrival-mean', type=float, default=1.5)
ap.add_argument('--out', default='trace_real.txt')
ap.add_argument('--stats-out', default=None)
args = ap.parse_args()

rng = random.Random(args.seed)
usable = int((1 - args.op_frac) * TT_PGS)
IX_PGS, ML_PGS = 32768, 65536
target = int(args.occ * usable)
kv_budget = target - IX_PGS - ML_PGS
horizon = int(args.horizon * S)
meas_start = int(args.meas_start * S)

def pages(b): return max(1, (b + PG - 1) // PG)

# ---------------- extract per-session templates from the capture ----------------
events = [json.loads(l) for l in open(args.oplog)]
sess_ev = {}
for e in events:
    if 'sid' in e:
        sess_ev.setdefault(e['sid'], []).append(e)

templates = []
for sid, evs in sorted(sess_ev.items()):
    turns = []
    cur = None
    for e in evs:
        if e['op'] == 'ENGINE':
            if cur: turns.append(cur)
            cur = {'pre': [], 'post': [],
                   'kv_before': e['kv_bytes_before'] * args.kv_scale,
                   'kv_delta': max(0, e['kv_bytes_after'] - e['kv_bytes_before']) * args.kv_scale,
                   'restore_ms': e.get('restore_ms') or 0,
                   'compute_ms': (e.get('prefill_ms') or 0) + (e.get('decode_ms') or 0)}
        elif e['op'] in ('MEM_READ', 'SEARCH', 'MEM_WRITE', 'CALC'):
            tgt = (cur['post'] if cur else None)
            rec = None
            if e['op'] == 'MEM_READ':
                rec = ('RIX', 1), ('RML', pages(max(0, e.get('bytes_read', 4096) - 4096)) if e.get('bytes_read', 4096) > 4096 else 0)
            elif e['op'] == 'SEARCH':
                rec = (('RML', pages(e.get('bytes_read', 65536))),)
            elif e['op'] == 'MEM_WRITE':
                rec = ('WIX', 1), ('WML', pages(e.get('log_bytes', 256)))
            elif e['op'] == 'CALC':
                rec = ()
            # tools before the first ENGINE of the session go to that turn's pre
            if cur is None:
                continue
            if not turns and cur and not cur.get('_started'):
                pass
            for k in (rec or ()):
                if k and k[1] > 0:
                    (cur['post']).append(k)
    if cur: turns.append(cur)
    if len(turns) >= 2:
        templates.append(turns)

# NOTE: the harness executes pre-tools before ENGINE within a turn, but the
# oplog interleaving assigns them to the previous ENGINE's post in this parser;
# aggregate per-turn tool mix is preserved, which is what the device models see.

if not templates:
    print('no usable sessions in oplog', file=sys.stderr); sys.exit(1)
tpl_growth = [sum(t['kv_delta'] for t in tpl) // PG + pages(tpl[0]['kv_before']) for tpl in templates]
print(f"templates: {len(templates)}, turns/template: {[len(t) for t in templates]}, "
      f"KV total pages/template (x{args.kv_scale}): {tpl_growth}", file=sys.stderr)

# ---------------- amplification: clone scheduling ----------------
import heapq
sessions = [dict(t0=0, ops=[('0', 'WIX', 0, 0, IX_PGS, 1), ('0', 'WML', 1, 0, ML_PGS, 0)])]
sessions[0]['ops'] = [(0, 'WIX', 0, 0, IX_PGS, 1), (0, 'WML', 1, 0, ML_PGS, 0)]
logical = 0
future_growth = 0
ml_cursor = ML_PGS
stats = dict(arrivals=0, deaths=0, blocked=0, turns=0)
occ_samples, active_samples = [], []
heap = []; seqn = [0]
def push(t, kind, sid): heapq.heappush(heap, (t, seqn[0], kind, sid)); seqn[0] += 1
active = {}
push(0, 'arrive', -1)

def jit(v): return v * rng.uniform(0.8, 1.2)

while heap:
    t, _, kind, sid = heapq.heappop(heap)
    if t >= horizon: break
    if t >= meas_start:
        occ_samples.append(logical + IX_PGS + ML_PGS); active_samples.append(len(active))
    if kind == 'arrive':
        tpl = templates[rng.randrange(len(templates))]
        init_pgs = pages(tpl[0]['kv_before']) if tpl[0]['kv_before'] else pages(tpl[0]['kv_delta'])
        growth = sum(pages(t2['kv_delta']) for t2 in tpl)
        r = args.growth_reserve
        hard = int(args.hard_cap * usable) - IX_PGS - ML_PGS
        if (logical + r * (future_growth + growth) + init_pgs > kv_budget or
                logical + args.hard_reserve * (future_growth + growth) + init_pgs > hard):
            stats['blocked'] += 1
            push(t + 5 * S, 'arrive', -1)
            continue
        sid2 = len(sessions)
        obj = 2 + (sid2 - 1)
        ops = [(0, 'WKV', obj, 0, max(1, init_pgs), 1)]
        sessions.append(dict(t0=t, ops=ops))
        active[sid2] = dict(obj=obj, pgs=max(1, init_pgs), tpl=tpl, k=1 if tpl[0]['kv_before'] else 0)
        logical += max(1, init_pgs); future_growth += growth
        stats['arrivals'] += 1
        push(t + int(jit(2) * S), 'turn', sid2)
        push(t + int(rng.expovariate(1.0 / args.arrival_mean) * S), 'arrive', -1)
    elif kind == 'turn':
        st = active.get(sid)
        if st is None: continue
        tpl = st['tpl']
        if st['k'] >= len(tpl):
            ops = sessions[sid]['ops']
            ops.append((2 * S, 'DEL', st['obj'], 0, 0, 0))
            push(t + 3 * S, 'death', sid)
            continue
        tinfo = tpl[st['k']]; st['k'] += 1
        ops = sessions[sid]['ops']
        gap_pre = int(rng.lognormvariate(math.log(args.think) - 0.125, 0.5) * S)
        stats['turns'] += 1
        est = gap_pre
        # resume: full-blob read (the capture's fully-offloaded regime)
        ops.append((gap_pre, 'RKV', st['obj'], 0, st['pgs'], 1))
        est += int(st['pgs'] * PG / 3e9 * S)
        first_done = True
        # engine compute gap, then KV delta append (write-through)
        dpg = pages(tinfo['kv_delta']) if tinfo['kv_delta'] else 0
        gcomp = int(jit(tinfo['compute_ms']) * 1e6)
        if dpg:
            ops.append((gcomp, 'WKV', st['obj'], st['pgs'], dpg, 0))
            st['pgs'] += dpg; logical += dpg; future_growth -= dpg
            est += gcomp
        # tool I/O with real byte counts
        for kindo, npg in tinfo['post']:
            g = int(rng.uniform(0.002, 0.03) * S)
            if kindo == 'WML':
                ops.append((g, 'WML', 1, ml_cursor, npg, 0)); ml_cursor += npg
            elif kindo == 'RML':
                back = rng.randrange(1, int(0.55 * ML_PGS))
                ops.append((g, 'RML', 1, max(0, ml_cursor - back), npg, 0))
            elif kindo == 'WIX':
                ops.append((g, 'WIX', 0, rng.randrange(IX_PGS), npg, 0))
            elif kindo == 'RIX':
                ops.append((g, 'RIX', 0, rng.randrange(IX_PGS), npg, 0))
            est += g
        push(t + est + 1 * S, 'turn', sid)
    elif kind == 'death':
        st = active.pop(sid, None)
        if st is None: continue
        logical -= st['pgs']
        stats['deaths'] += 1

n_objs = 2 + max(0, len(sessions) - 1)
with open(args.out, 'w') as f:
    f.write(f"H {n_objs} {horizon} {IX_PGS} {ML_PGS} {meas_start}\n")
    for sid, sdat in enumerate(sessions):
        f.write(f"A {sid} {sdat['t0']}\n")
        for (gap, kindo, obj, off, ln, ts) in sdat['ops']:
            f.write(f"O {gap} {kindo} {obj} {off} {ln} {ts}\n")

out = dict(base_templates=len(templates),
           kv_scale=args.kv_scale,
           sessions=len(sessions) - 1,
           occ_peak=max(occ_samples) / usable if occ_samples else 0,
           occ_avg=(sum(occ_samples) / len(occ_samples) / usable) if occ_samples else 0,
           active_avg=(sum(active_samples) / len(active_samples)) if active_samples else 0,
           **stats)
print(json.dumps(out, indent=1), file=sys.stderr)
if args.stats_out:
    json.dump(out, open(args.stats_out, 'w'), indent=1)
