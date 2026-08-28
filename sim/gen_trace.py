#!/usr/bin/env python3
"""
Agentic-LLM SSD offloading workload generator (edge workstation scenario).

Models N concurrent agent sessions on a resource-constrained box
(single consumer GPU, one 40 GiB-scale emulated NVMe SSD):
  - per-session KV-cache blob on SSD: initial context offload after first
    prefill, per-turn decode deltas appended (write-through), full-blob
    resume read on GPU-cache miss (TTFT-critical), whole-blob delete at
    session end (TRIM / zone reset).
  - agent memory: episodic memory-log appends (WML) + reads of recent
    entries (RML); vector-index 4 KiB in-place updates (WIX, DiskANN-style
    neighbor updates) + random 4 KiB retrieval reads (RIX).
  - GPU KV cache modeled as byte-capacity LRU over sessions -> hit/miss.

Sizing: Llama-3-8B-class GQA KV = 128 KiB/token fp16; Q4-quantized =
32 KiB/token = 8 pages/token (edge practice).

Trace format (consumed by femu_agentic_sim.c and the NVMeVirt replayer):
  H <n_objs> <t_end_ns> <ix_pgs> <ml_pgs> <meas_start_ns>
  A <sid> <t0_ns>
  O <gap_ns> <KIND> <obj> <off_pgs> <len_pgs> <turn_start>
Gaps are think/compute times applied after the previous op completes
(session-level closed loop). Object 0 = vector index, 1 = memory log,
2+k = KV blob of session k.
"""
import argparse, heapq, random, json, math, sys

PG = 4096
GiB = 1073741824
TT_PGS = int((16 * GiB) / PG)
S = 1_000_000_000

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--occ', type=float, default=0.90)
    ap.add_argument('--op-frac', type=float, default=0.07)
    ap.add_argument('--horizon', type=float, default=7800.0)
    ap.add_argument('--meas-start', type=float, default=2400.0)
    ap.add_argument('--seed', type=int, default=42)
    ap.add_argument('--gpu-gib', type=float, default=5.5)
    ap.add_argument('--pgs-per-token', type=int, default=8)
    ap.add_argument('--ctx0-median-tok', type=float, default=6000.0)
    ap.add_argument('--ctx0-sigma', type=float, default=0.6)
    ap.add_argument('--newtok-mean', type=float, default=500.0)
    ap.add_argument('--newtok-sd', type=float, default=150.0)
    ap.add_argument('--turns-mean', type=float, default=28.0)
    ap.add_argument('--turn-gap-mean', type=float, default=25.0)
    ap.add_argument('--decode-gap-mean', type=float, default=18.0)
    ap.add_argument('--mem-reads', type=int, default=4)
    ap.add_argument('--ix-writes', type=int, default=8)
    ap.add_argument('--arrival-mean', type=float, default=3.0)
    ap.add_argument('--growth-reserve', type=float, default=0.35,
                    help='fraction of outstanding future KV growth reserved at admission '
                         '(1.0 = never exceed target; smaller = steady occupancy closer to target)')
    ap.add_argument('--hard-cap', type=float, default=0.95, help='absolute logical cap as fraction of usable')
    ap.add_argument('--hard-reserve', type=float, default=0.35, help='future-growth fraction reserved against hard cap')
    ap.add_argument('--out', default='trace.txt')
    ap.add_argument('--stats-out', default=None)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    usable = int((1 - args.op_frac) * TT_PGS)
    IX_PGS, ML_PGS = 32768, 65536
    target = int(args.occ * usable)
    kv_budget = target - IX_PGS - ML_PGS
    gpu_pgs = int(args.gpu_gib * (1 << 30) / PG)
    horizon = int(args.horizon * S)
    meas_start = int(args.meas_start * S)

    def lgn(mean, sigma):
        mu = math.log(mean) - sigma * sigma / 2.0
        return rng.lognormvariate(mu, sigma)

    sessions = []                 # per sid: dict(t0, ops=[(gap,kind,obj,off,len,ts)])
    logical = 0
    future_growth = 0
    ml_cursor = ML_PGS
    lru = {}                      # sid -> pgs (insertion order = LRU order)
    gpu_used = 0
    stats = dict(arrivals=0, deaths=0, blocked=0, turns=0, miss=0, hit=0,
                 kv_wr_pgs=0, kv_rd_pgs=0, ml_wr_pgs=0, ml_rd=0, ix_wr=0, ix_rd=0)
    occ_samples = []
    active_samples = []

    def lru_insert(sid, pgs):
        nonlocal gpu_used
        if sid in lru:
            gpu_used -= lru.pop(sid)
        lru[sid] = pgs
        gpu_used += pgs
        victims = []
        for k in lru:
            if gpu_used <= gpu_pgs or k == sid:
                continue
            victims.append(k)
            gpu_used -= lru[k]
            if gpu_used <= gpu_pgs:
                break
        for k in victims:
            del lru[k]
    def lru_touch(sid):
        lru[sid] = lru.pop(sid)
    def lru_drop(sid):
        nonlocal gpu_used
        if sid in lru:
            gpu_used -= lru.pop(sid)

    # system session 0: prefill index + memlog at t=0
    sessions.append(dict(t0=0, ops=[(0, 'WIX', 0, 0, IX_PGS, 1), (0, 'WML', 1, 0, ML_PGS, 0)]))

    heap = []
    seqn = [0]
    def push(t, kind, sid):
        heapq.heappush(heap, (t, seqn[0], kind, sid)); seqn[0] += 1

    active = {}
    push(0, 'arrive', -1)

    def io_est(pgs):
        return int((0.0003 + pgs * PG / 3e9) * S)

    while heap:
        t, _, kind, sid = heapq.heappop(heap)
        if t >= horizon:
            break
        if t >= meas_start:
            occ_samples.append(logical + IX_PGS + ML_PGS)
            active_samples.append(len(active))

        if kind == 'arrive':
            ctx0_mean = args.ctx0_median_tok * math.exp(args.ctx0_sigma ** 2 / 2)
            ctx0 = int(min(max(lgn(ctx0_mean, args.ctx0_sigma), 1500), 24000))
            p = 1.0 / max(args.turns_mean - 6, 1)
            g = 0
            while rng.random() > p and g < 200:
                g += 1
            turns = 6 + g
            newtoks = [int(min(max(rng.gauss(args.newtok_mean, args.newtok_sd), 100), 1200))
                       for _ in range(turns)]
            ctx0_pgs = ctx0 * args.pgs_per_token
            growth = sum(nt * args.pgs_per_token for nt in newtoks)
            r = args.growth_reserve
            hard = int(args.hard_cap * usable) - IX_PGS - ML_PGS
            if (logical + r * (future_growth + growth) + ctx0_pgs > kv_budget or
                    logical + args.hard_reserve * (future_growth + growth) + ctx0_pgs > hard):
                stats['blocked'] += 1
                push(t + 5 * S, 'arrive', -1)
                continue
            sid2 = len(sessions)
            obj = 2 + (sid2 - 1)
            sessions.append(dict(t0=t, ops=[(0, 'WKV', obj, 0, ctx0_pgs, 1)]))
            active[sid2] = dict(obj=obj, pgs=ctx0_pgs, newtoks=newtoks, k=0, turns_left=turns)
            logical += ctx0_pgs
            future_growth += growth
            stats['arrivals'] += 1
            stats['kv_wr_pgs'] += ctx0_pgs
            lru_insert(sid2, ctx0_pgs)
            push(t + io_est(ctx0_pgs) + int(lgn(args.turn_gap_mean, 0.5) * S), 'turn', sid2)
            push(t + int(rng.expovariate(1.0 / args.arrival_mean) * S), 'arrive', -1)

        elif kind == 'turn':
            st = active.get(sid)
            if st is None:
                continue
            ops = sessions[sid]['ops']
            gap_pre = int(lgn(args.turn_gap_mean, 0.5) * S)
            est = gap_pre
            first = True
            stats['turns'] += 1
            if sid not in lru:
                stats['miss'] += 1
                ops.append((gap_pre, 'RKV', st['obj'], 0, st['pgs'], 1))
                stats['kv_rd_pgs'] += st['pgs']
                est += io_est(st['pgs'])
                first = False
                lru_insert(sid, st['pgs'])
            else:
                stats['hit'] += 1
                lru_touch(sid)
            for _ in range(args.mem_reads):
                gp = gap_pre if first else int(rng.uniform(0.002, 0.015) * S)
                ops.append((gp, 'RIX', 0, rng.randrange(IX_PGS), 1, 1 if first else 0))
                stats['ix_rd'] += 1
                first = False
                est += io_est(1)
            for _ in range(args.mem_reads):
                back = rng.randrange(1, int(0.55 * ML_PGS))
                off = ml_cursor - back
                if off < 0:
                    continue
                ops.append((int(rng.uniform(0.002, 0.015) * S), 'RML', 1, off, 1, 0))
                stats['ml_rd'] += 1
                est += io_est(1)
            nt = st['newtoks'][st['k']]
            st['k'] += 1
            delta = nt * args.pgs_per_token
            gdec = int(lgn(args.decode_gap_mean, 0.4) * S)
            ops.append((gdec, 'WKV', st['obj'], st['pgs'], delta, 0))
            st['pgs'] += delta
            logical += delta
            future_growth -= delta
            stats['kv_wr_pgs'] += delta
            est += gdec + io_est(delta)
            if sid in lru:
                lru_insert(sid, st['pgs'])
            for _ in range(2):
                ln = rng.randint(2, 6)
                ops.append((int(rng.uniform(0.005, 0.05) * S), 'WML', 1, ml_cursor, ln, 0))
                ml_cursor += ln
                stats['ml_wr_pgs'] += ln
                est += io_est(ln)
            for _ in range(args.ix_writes):
                ops.append((int(rng.uniform(0.001, 0.010) * S), 'WIX', 0, rng.randrange(IX_PGS), 1, 0))
                stats['ix_wr'] += 1
                est += io_est(1)
            st['turns_left'] -= 1
            if st['turns_left'] <= 0:
                ops.append((2 * S, 'DEL', st['obj'], 0, 0, 0))
                push(t + est + 2 * S, 'death', sid)
            else:
                push(t + est + 1 * S, 'turn', sid)

        elif kind == 'death':
            st = active.pop(sid, None)
            if st is None:
                continue
            logical -= st['pgs']
            lru_drop(sid)
            stats['deaths'] += 1

    n_objs = 2 + max(0, len(sessions) - 1)
    with open(args.out, 'w') as f:
        f.write(f"H {n_objs} {horizon} {IX_PGS} {ML_PGS} {meas_start}\n")
        for sid, sdat in enumerate(sessions):
            f.write(f"A {sid} {sdat['t0']}\n")
            for (gap, kindo, obj, off, ln, ts) in sdat['ops']:
                f.write(f"O {gap} {kindo} {obj} {off} {ln} {ts}\n")

    occ_avg = (sum(occ_samples) / len(occ_samples) / usable) if occ_samples else 0
    out_stats = dict(
        sessions=len(sessions) - 1,
        usable_pgs=usable, target_pgs=target, kv_budget_pgs=kv_budget,
        gpu_pgs=gpu_pgs,
        occ_peak=max(occ_samples) / usable if occ_samples else 0,
        occ_avg=occ_avg,
        active_avg=(sum(active_samples) / len(active_samples)) if active_samples else 0,
        gpu_hit_rate=stats['hit'] / max(stats['turns'], 1),
        **stats)
    print(json.dumps(out_stats, indent=1), file=sys.stderr)
    if args.stats_out:
        with open(args.stats_out, 'w') as f:
            json.dump(out_stats, f, indent=1)

if __name__ == '__main__':
    main()
