#!/usr/bin/env python3
"""Consolidate sim results and produce paper figures."""
import json, re, os, sys, csv
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

RES = sys.argv[1] if len(sys.argv) > 1 else 'res'
FIG = sys.argv[2] if len(sys.argv) > 2 else 'figs'
os.makedirs(FIG, exist_ok=True)

WORKLOADS = ['occ85', 'occ90', 'churn']
DEVICES = ['conv', 'fdp', 'zns']
COLORS = {'conv': '#c0392b', 'fdp': '#e67e22', 'zns': '#2471a3'}
LABEL = {'conv': 'Conventional (7% OP, TRIM)', 'fdp': 'FDP-like (3 streams)', 'zns': 'ZNS (zone/session)'}
WLABEL = {'occ85': 'occ-0.83', 'occ90': 'occ-0.86', 'churn': 'hi-churn-0.84'}

def parse_summary(path):
    d = {'lat': {}}
    for line in open(path):
        if line.startswith('LAT'):
            m = re.match(r'LAT (\S+) n=(\d+) sampled=(\d+) mean_us=([\d.]+) p50_us=(\d+) p90_us=(\d+) p99_us=(\d+) p999_us=(\d+) p9999_us=(\d+) max_us=(\d+)', line)
            if m:
                d['lat'][m.group(1)] = dict(n=int(m.group(2)), mean=float(m.group(4)),
                    p50=int(m.group(5)), p90=int(m.group(6)), p99=int(m.group(7)),
                    p999=int(m.group(8)), p9999=int(m.group(9)), max=int(m.group(10)))
        else:
            for kv in re.findall(r'(\w+)=([-\d.]+)', line):
                try: d[kv[0]] = float(kv[1])
                except ValueError: pass
    return d

R = {}
for w in WORKLOADS:
    for dev in DEVICES:
        p = f'{RES}/{w}_{dev}_summary.txt'
        if os.path.exists(p):
            R[(w, dev)] = parse_summary(p)
import glob as _g
for p in _g.glob(f'{RES}/*_summary.txt'):
    name = os.path.basename(p).replace('_summary.txt', '')
    parts = name.split('_', 1)
    if (parts[0] in WORKLOADS and len(parts) > 1 and parts[1] in DEVICES):
        continue          # already loaded
    R[(name, 'x')] = parse_summary(p)

json.dump({f'{k[0]}|{k[1]}': v for k, v in R.items()}, open(f'{FIG}/results.json', 'w'), indent=1)

# combined agent-memory read latency (index + memlog) per device at occ90
def memread(w, dev):
    a = np.loadtxt(f'{RES}/{w}_{dev}_lat_index_read_cmd.csv')
    b = np.loadtxt(f'{RES}/{w}_{dev}_lat_memlog_read_cmd.csv')
    return np.sort(np.concatenate([a, b]))

# ---------------- Fig 1: CCDF of agent-memory reads (occ90) ----------------
plt.figure(figsize=(5.2, 3.6))
for dev in DEVICES:
    v = memread('occ90', dev)
    ccdf = 1.0 - (np.arange(len(v)) + 1) / len(v)
    ccdf = np.clip(ccdf, 1.0 / len(v), 1)
    plt.loglog(v / 1e3, ccdf, color=COLORS[dev], label=LABEL[dev], lw=1.8)
plt.xlabel('Agent-memory 4 KiB read latency (ms)')
plt.ylabel('CCDF  P[X > x]')
plt.grid(alpha=.3, which='both')
plt.legend(fontsize=8)
plt.title('Agent-memory read tail (occ-0.86)', fontsize=10)
plt.tight_layout(); plt.savefig(f'{FIG}/fig1_memread_ccdf.png', dpi=300); plt.close()

# ---------------- Fig 2: p99/p999 grouped bars across workloads ----------------
fig, axes = plt.subplots(1, 2, figsize=(8.4, 3.3), sharey=True)
for ax, pct in zip(axes, ['p99', 'p999']):
    x = np.arange(len(WORKLOADS)); wbar = 0.26
    for i, dev in enumerate(DEVICES):
        vals = []
        for w in WORKLOADS:
            lat = R[(w, dev)]['lat']
            ix, ml = lat['index_read_cmd'], lat['memlog_read_cmd']
            vals.append((ix[pct] + ml[pct]) / 2 / 1e3)
        bars = ax.bar(x + (i - 1) * wbar, vals, wbar, color=COLORS[dev], label=LABEL[dev])
        for b, v in zip(bars, vals):
            ax.text(b.get_x() + b.get_width() / 2, v * 1.15, f'{v:.2f}', ha='center', fontsize=6.5)
    ax.set_yscale('log'); ax.set_xticks(x); ax.set_xticklabels([WLABEL[w] for w in WORKLOADS], fontsize=8)
    ax.set_title(f'{pct} agent-memory read (ms)', fontsize=10); ax.grid(alpha=.3, axis='y')
axes[0].set_ylabel('latency (ms, log)')
axes[0].legend(fontsize=7.5)
plt.tight_layout(); plt.savefig(f'{FIG}/fig2_tail_bars.png', dpi=300); plt.close()

# ---------------- Fig 3: WAF + erases per user GB ----------------
fig, axes = plt.subplots(1, 2, figsize=(8.4, 3.2))
x = np.arange(len(WORKLOADS)); wbar = 0.26
for i, dev in enumerate(DEVICES):
    axes[0].bar(x + (i - 1) * wbar, [R[(w, dev)]['WAF'] for w in WORKLOADS], wbar, color=COLORS[dev], label=LABEL[dev])
    axes[1].bar(x + (i - 1) * wbar, [R[(w, dev)]['erases_per_user_GB'] for w in WORKLOADS], wbar, color=COLORS[dev])
axes[0].set_ylabel('Write amplification factor'); axes[0].axhline(1.0, color='k', lw=.6, ls='--')
axes[1].set_ylabel('Block erases per user GB (endurance)')
for ax in axes:
    ax.set_xticks(x); ax.set_xticklabels([WLABEL[w] for w in WORKLOADS], fontsize=8); ax.grid(alpha=.3, axis='y')
axes[0].legend(fontsize=7.5)
plt.tight_layout(); plt.savefig(f'{FIG}/fig3_waf_endurance.png', dpi=300); plt.close()

# ---------------- Fig 4: GC activity timeline vs read latency ----------------
fig, axes = plt.subplots(2, 1, figsize=(8.0, 4.2), sharex=True)
gc = np.genfromtxt(f'{RES}/occ90_conv_gc.csv', delimiter=',', names=True)
t0, t1 = 3000, 3300
sel = (gc['start_s'] >= t0) & (gc['start_s'] <= t1)
for s, e in zip(gc['start_s'][sel], gc['end_s'][sel]):
    axes[0].axvspan(s, e, color='#c0392b', alpha=.55, lw=0)
axes[0].set_ylabel('Conv GC\nactive'); axes[0].set_yticks([])
axes[0].set_title(f'GC activity, conventional SSD ({t1-t0}s window, occ-0.86); ZNS has none', fontsize=9)
tl = np.genfromtxt(f'{RES}/occ90_conv_timeline.csv', delimiter=',', names=True)
sel2 = (tl['t_s'] >= t0) & (tl['t_s'] <= t1)
axes[1].plot(tl['t_s'][sel2], tl['user_rd_MB'][sel2], color='#555', lw=.7, label='user reads (MB/s)')
axes[1].plot(tl['t_s'][sel2], tl['user_wr_MB'][sel2], color='#2471a3', lw=.7, label='user writes (MB/s)')
axes[1].set_xlabel('time (s)'); axes[1].set_ylabel('MB/s'); axes[1].legend(fontsize=7.5); axes[1].grid(alpha=.3)
plt.tight_layout(); plt.savefig(f'{FIG}/fig4_gc_timeline.png', dpi=300); plt.close()

# ---------------- Fig 5: resume-read CDF + zone-size sensitivity ----------------
plt.figure(figsize=(5.2, 3.6))
for dev in ['conv', 'zns']:
    v = np.sort(np.loadtxt(f'{RES}/occ90_{dev}_lat_resume_burst.csv'))
    plt.semilogx(v / 1e3, (np.arange(len(v)) + 1) / len(v), color=COLORS[dev], label=LABEL[dev], lw=1.8)
v = np.sort(np.loadtxt(f'{RES}/occ90_zns_smallzone_lat_resume_burst.csv'))
plt.semilogx(v / 1e3, (np.arange(len(v)) + 1) / len(v), color='#7d3c98', ls='--', label='ZNS 16 MiB zones (16-LUN)', lw=1.5)
plt.xlabel('KV-cache session resume latency (ms)'); plt.ylabel('CDF')
plt.grid(alpha=.3, which='both'); plt.legend(fontsize=8)
plt.title('Session resume (full-blob read, occ-0.86)', fontsize=10)
plt.tight_layout(); plt.savefig(f'{FIG}/fig5_resume_cdf.png', dpi=300); plt.close()

# ---------------- results table CSV ----------------
with open(f'{FIG}/results_table.csv', 'w', newline='') as f:
    wr = csv.writer(f)
    wr.writerow(['workload', 'device', 'WAF', 'gc_count', 'gc_busy_s', 'gc_busy_pct', 'gc_max_ms',
                 'erases_per_user_GB', 'wr_MBps', 'rd_MBps', 'turns_per_hour',
                 'memread_mean_us', 'memread_p99_us', 'memread_p999_us', 'memread_max_us',
                 'resume_p50_ms', 'resume_p99_ms', 'stall_p90_ms', 'stall_p99_ms'])
    for w in WORKLOADS:
        for dev in DEVICES:
            d = R[(w, dev)]; L = d['lat']
            ix, ml, rs, st = L['index_read_cmd'], L['memlog_read_cmd'], L['resume_burst'], L['turn_storage_stall']
            wr.writerow([w, dev, f"{d['WAF']:.3f}", int(d['gc_count']), f"{d['gc_busy_s']:.1f}",
                         f"{d['gc_busy_pct']:.2f}", f"{d['gc_max_ms']:.1f}", f"{d['erases_per_user_GB']:.0f}",
                         f"{d['user_write_MBps']:.1f}", f"{d['user_read_MBps']:.1f}", f"{d['turns_per_hour']:.0f}",
                         f"{(ix['mean']+ml['mean'])/2:.1f}", (ix['p99'] + ml['p99']) // 2,
                         (ix['p999'] + ml['p999']) // 2, max(ix['max'], ml['max']),
                         f"{rs['p50']/1e3:.1f}", f"{rs['p99']/1e3:.1f}",
                         f"{st['p90']/1e3:.1f}", f"{st['p99']/1e3:.1f}"])

# ---------------- Fig 6: agent-memory WRITE tails across variants ----------------
VAR = [('occ90_conv', 'conv', '#c0392b'), ('occ90_convS', 'conv + ideal suspension', '#e74c3c'),
       ('occ90_fdp32', 'FDP, 32 session RUHs', '#e67e22'), ('occ90_zns', 'ZNS', '#2471a3'),
       ('occ90_znsS', 'ZNS + ideal suspension', '#5dade2')]
def getlat(name, cls):
    key = (name, 'x') if (name, 'x') in R else tuple(name.split('_', 1))
    return R[key]['lat'][cls]
fig, axes = plt.subplots(1, 2, figsize=(8.4, 3.3), sharey=True)
for ax, pct, ttl in zip(axes, ['p99', 'p999'], ['p99', 'p99.9']):
    x = np.arange(len(VAR))
    vals = [(getlat(n, 'index_write_cmd')[pct] + getlat(n, 'memlog_write_cmd')[pct]) / 2 / 1e3 for n, _, _ in VAR]
    bars = ax.bar(x, vals, 0.62, color=[c for _, _, c in VAR])
    for b, v in zip(bars, vals):
        ax.text(b.get_x() + b.get_width() / 2, v * 1.12, f'{v:.1f}', ha='center', fontsize=7)
    ax.set_yscale('log'); ax.set_xticks(x)
    ax.set_xticklabels([l for _, l, _ in VAR], fontsize=6.6, rotation=18, ha='right')
    ax.set_title(f'{ttl} agent-memory write (ms)', fontsize=10); ax.grid(alpha=.3, axis='y')
axes[0].set_ylabel('latency (ms, log)')
plt.tight_layout(); plt.savefig(f'{FIG}/fig6_write_tails.png', dpi=300); plt.close()

# ---------------- Fig 7: placement-granularity axis ----------------
AXIS = [('occ90_conv', 'conv\n(1 stream)'), ('occ90_fdp', 'FDP\n3 class'), ('occ90_fdp8', 'FDP\n8 RUH'),
        ('occ90_fdp32', 'FDP\n32 RUH'), ('occ90_zns', 'ZNS\nzone/sess')]
fig, ax1 = plt.subplots(figsize=(5.6, 3.5))
x = np.arange(len(AXIS))
def getv(name, k):
    key = (name, 'x') if (name, 'x') in R else tuple(name.split('_', 1))
    return R[key][k]
wafs = [getv(n, 'WAF') for n, _ in AXIS]
ax1.bar(x - 0.17, wafs, 0.34, color='#7f8c8d', label='WAF')
ax1.set_ylabel('Write amplification'); ax1.axhline(1.0, color='k', lw=.6, ls='--')
ax1.set_xticks(x); ax1.set_xticklabels([l for _, l in AXIS], fontsize=7.5)
ax2 = ax1.twinx()
p99 = [getlat(n, 'index_read_cmd')['p99'] / 1e3 for n, _ in AXIS]
p999 = [getlat(n, 'index_read_cmd')['p999'] / 1e3 for n, _ in AXIS]
ax2.plot(x, p99, 'o-', color='#c0392b', label='mem-read p99 (ms)')
ax2.plot(x, p999, 's--', color='#2471a3', label='mem-read p99.9 (ms)')
ax2.set_yscale('log'); ax2.set_ylabel('latency (ms, log)')
h1, l1 = ax1.get_legend_handles_labels(); h2, l2 = ax2.get_legend_handles_labels()
ax1.legend(h1 + h2, l1 + l2, fontsize=7, loc='upper right')
plt.title('Placement granularity: WAF falls, but only ZNS bounds p99.9', fontsize=9.5)
plt.tight_layout(); plt.savefig(f'{FIG}/fig7_granularity.png', dpi=300); plt.close()

# ---------------- Fig 8: OP sweep at equal relative fill ----------------
OPS = [('occ90_conv', '7% OP'), ('op14r_conv', '14% OP'), ('op28r_conv', '28% OP'), ('occ90_zns', 'ZNS')]
fig, axes = plt.subplots(1, 3, figsize=(9.6, 3.0))
x = np.arange(len(OPS)); cs = ['#c0392b', '#d35400', '#e67e22', '#2471a3']
axes[0].bar(x, [getv(n, 'WAF') for n, _ in OPS], 0.6, color=cs); axes[0].set_title('WAF', fontsize=9.5)
axes[1].bar(x, [getlat(n, 'index_read_cmd')['p999'] / 1e3 for n, _ in OPS], 0.6, color=cs)
axes[1].set_yscale('log'); axes[1].set_title('mem-read p99.9 (ms, log)', fontsize=9.5)
axes[2].bar(x, [getv(n, 'usable_GiB') for n, _ in OPS], 0.6, color=cs); axes[2].set_title('exported capacity (GiB)', fontsize=9.5)
for ax in axes:
    ax.set_xticks(x); ax.set_xticklabels([l for _, l in OPS], fontsize=8); ax.grid(alpha=.3, axis='y')
plt.suptitle('Over-provisioning sweep at equal relative fill (~0.86 of each usable)', fontsize=10)
plt.tight_layout(); plt.savefig(f'{FIG}/fig8_op_sweep.png', dpi=300); plt.close()

# ratio summary
print("== conv/zns ratios ==")
for w in WORKLOADS:
    c, z = R[(w, 'conv')], R[(w, 'zns')]
    for cl in ['index_read_cmd', 'memlog_read_cmd']:
        for pct in ['mean', 'p99', 'p999']:
            print(f"{w} {cl} {pct}: {c['lat'][cl][pct]/max(z['lat'][cl][pct],1e-9):.1f}x "
                  f"({c['lat'][cl][pct]/1e3:.2f}ms vs {z['lat'][cl][pct]/1e3:.3f}ms)")
    print(f"{w} WAF: {c['WAF']/z['WAF']:.2f}x  erases/GB: {c['erases_per_user_GB']/z['erases_per_user_GB']:.2f}x")
print("figures written to", FIG)
