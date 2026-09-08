#!/usr/bin/env python3
"""
Replay ID-less production LLM request traces (Azure LLM inference 2023,
BurstGPT) as a request-level KV persistence pool.

These traces provide real arrival processes and real context/generated token
counts but no session or prefix identifiers, so the honest storage mapping is
the request-level tier: each request's KV (context + generated tokens) is
written through to the pool as one blob after its prefill/decode gap, blobs
are retained for potential reuse, and oldest-ready capacity eviction deletes
them whole (TRIM / zone reset). Deletes stay in their object's session so
replay cannot issue them before the write completes. This is a write-dominant
churn workload: exactly
the write-once pattern CHEOPS'25 measured for LLM offloading, driven here by
production arrivals and sizes. No linkage means no resume reads; the metrics
are write-path tails, WAF, and cleaning behavior.

Formats:
  azure:    TIMESTAMP (datetime), ContextTokens, GeneratedTokens
  burstgpt: Timestamp (s), Model, Request tokens, Response tokens, ...
"""
import argparse, csv, json, sys
from datetime import datetime

PG = 4096
GiB = 1073741824
TT_PGS = int((8 * GiB) / PG)  # pool-budget reference capacity, not device discovery
S = 1_000_000_000
PGTOK = 8

ap = argparse.ArgumentParser()
ap.add_argument('--csv', required=True)
ap.add_argument('--format', choices=['azure', 'burstgpt'], required=True)
ap.add_argument('--occ', type=float, default=0.85)
ap.add_argument('--max-resident-requests', type=int, default=0,
                help='retained KV request limit in addition to the byte budget; 0 disables')
ap.add_argument('--op-frac', type=float, default=0.07)
ap.add_argument('--window-s', type=float, default=7200.0, help='replay window from trace start')
ap.add_argument('--skip-s', type=float, default=0.0)
ap.add_argument('--max-ctx-tok', type=int, default=32000)
ap.add_argument('--prefill-tps', type=float, default=8000.0)
ap.add_argument('--decode-tps', type=float, default=60.0)
ap.add_argument('--meas-frac', type=float, default=0.3)
ap.add_argument('--out', default='trace_rp.txt')
ap.add_argument('--stats-out', default=None)
args = ap.parse_args()
if args.max_resident_requests < 0:
    ap.error('--max-resident-requests must be nonnegative')
if not 0 < args.occ <= 1:
    ap.error('--occ must be greater than 0 and at most 1')
if not 0 <= args.op_frac < 1:
    ap.error('--op-frac must be at least 0 and less than 1')
if args.prefill_tps <= 0 or args.decode_tps <= 0:
    ap.error('--prefill-tps and --decode-tps must be positive')

usable = int((1 - args.op_frac) * TT_PGS)
IX_PGS, ML_PGS = 1, 1
budget = int(args.occ * usable * 0.94)

reqs = []
with open(args.csv) as f:
    rd = csv.DictReader(f)
    t0 = None
    for r in rd:
        if args.format == 'azure':
            ts = datetime.fromisoformat(r['TIMESTAMP'][:26])
            if t0 is None: t0 = ts
            t = (ts - t0).total_seconds()
            ctx, gen = int(r['ContextTokens']), int(r['GeneratedTokens'])
        else:
            t = float(r['Timestamp'])
            if t0 is None: t0 = t
            t -= t0
            ctx, gen = int(r['Request tokens']), int(r['Response tokens'])
        if t < args.skip_s: continue
        if t - args.skip_s > args.window_s: break
        tok = min(ctx + gen, args.max_ctx_tok)
        if tok < 16: continue
        reqs.append((t - args.skip_s, tok, ctx, gen))

sessions = [dict(t0=0, ops=[(0, 'WIX', 0, 0, IX_PGS, 1), (0, 'WML', 1, 0, ML_PGS, 0)])]
resident = {}          # obj -> (pgs, session, write-ready time), oldest ready first
logical = 0
next_obj = 2
stats = dict(requests=0, evictions=0, write_pgs=0,
             peak_resident_requests=0, peak_resident_pgs=0)
ready = []

for (t, tok, ctx, gen) in reqs:
    pgs = tok * PGTOK
    if pgs > budget: continue
    gap = int((ctx / args.prefill_tps + gen / args.decode_tps) * S)
    obj = next_obj; next_obj += 1
    sdat = dict(t0=int(t * S), ops=[(gap, 'WKV', obj, 0, pgs, 1)])
    sessions.append(sdat)
    ready.append((sdat['t0'] + gap, obj, pgs, sdat))
    stats['requests'] += 1; stats['write_pgs'] += pgs

# Account for residency when KV becomes ready, not while it is computing.
# Keep each eviction in the victim's session: the replayer waits for its WKV
# to finish before starting the retention gap, even if storage falls behind.
# The caps describe the planned ready-time workload; I/O delays can temporarily
# overlap evictions and new writes across sessions.
for ready_ns, obj, pgs, sdat in sorted(ready, key=lambda r: (r[0], r[1])):
    while resident and (logical + pgs > budget or
                        (args.max_resident_requests and
                         len(resident) >= args.max_resident_requests)):
        vobj = next(iter(resident))
        vpgs, victim, victim_ready_ns = resident.pop(vobj)
        logical -= vpgs
        victim['ops'].append((ready_ns - victim_ready_ns, 'DEL', vobj, 0, 0, 0))
        stats['evictions'] += 1
    resident[obj] = (pgs, sdat, ready_ns)
    logical += pgs
    stats['peak_resident_requests'] = max(stats['peak_resident_requests'], len(resident))
    stats['peak_resident_pgs'] = max(stats['peak_resident_pgs'], logical)

horizon = int((reqs[-1][0] + 120) * S) if reqs else S
if ready:
    horizon = max(horizon, max(r[0] for r in ready) + 1)
meas_start = int(horizon * args.meas_frac)
with open(args.out, 'w') as f:
    f.write(f"H {next_obj} {horizon} {IX_PGS} {ML_PGS} {meas_start}\n")
    for sid, sdat in enumerate(sessions):
        f.write(f"A {sid} {sdat['t0']}\n")
        for (gap, kindo, obj, off, ln, ts) in sdat['ops']:
            f.write(f"O {gap} {kindo} {obj} {off} {ln} {ts}\n")

out = dict(fmt=args.format, csv=args.csv.split('/')[-1], window_s=args.window_s,
           duration_s=reqs[-1][0] if reqs else 0, occ_target=args.occ,
           max_resident_requests=args.max_resident_requests,
           budget_pgs=budget, final_resident_requests=len(resident),
           final_resident_pgs=logical,
           write_GiB=stats['write_pgs'] * PG / 2**30,
           write_MBps=stats['write_pgs'] * PG / max(reqs[-1][0], 1) / 1e6 if reqs else 0,
           **stats)
print(json.dumps(out, indent=1), file=sys.stderr)
if args.stats_out: json.dump(out, open(args.stats_out, 'w'), indent=1)
