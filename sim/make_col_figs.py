#!/usr/bin/env python3
"""Column-width (USENIX two-column) versions of all figures -> figs/figN_c.png"""
import json, os, sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
plt.rcParams.update({'font.size': 7, 'axes.titlesize': 7.5, 'axes.labelsize': 7,
                     'xtick.labelsize': 6.3, 'ytick.labelsize': 6.3, 'legend.fontsize': 5.8,
                     'lines.linewidth': 1.3, 'axes.linewidth': 0.6})

RES = sys.argv[1] if len(sys.argv) > 1 else 'res'
FIG = sys.argv[2] if len(sys.argv) > 2 else 'figs'
R = json.load(open(f'{FIG}/results.json'))
def K(n): return R[f'{n}|x'] if f'{n}|x' in R else R[n.replace('_', '|', 1)]
def G(n, c, k): return K(n)['lat'][c][k]
def GV(n, k): return K(n)[k]

COLORS = {'conv': '#c0392b', 'fdp': '#e67e22', 'zns': '#2471a3'}
LABEL = {'conv': 'Conventional (7% OP, TRIM)', 'fdp': 'FDP-like (3 streams)', 'zns': 'ZNS (zone/session)'}
WLABEL = {'occ85': 'occ-0.83', 'occ90': 'occ-0.86', 'churn': 'hi-churn'}
W3 = ['occ85', 'occ90', 'churn']
CW = 3.35  # column width inches

def memread(w, dev):
    a = np.loadtxt(f'{RES}/{w}_{dev}_lat_index_read_cmd.csv')
    b = np.loadtxt(f'{RES}/{w}_{dev}_lat_memlog_read_cmd.csv')
    return np.sort(np.concatenate([a, b]))

# fig1: CCDF
plt.figure(figsize=(CW, 2.55))
for dev in ['conv', 'fdp', 'zns']:
    v = memread('occ90', dev)
    ccdf = np.clip(1.0 - (np.arange(len(v)) + 1) / len(v), 1.0 / len(v), 1)
    plt.loglog(v / 1e3, ccdf, color=COLORS[dev], label=LABEL[dev])
plt.xlabel('4 KiB agent-memory read latency (ms)')
plt.ylabel('CCDF  P[X > x]')
plt.grid(alpha=.3, which='both', lw=0.4)
plt.legend()
plt.tight_layout(pad=0.4); plt.savefig(f'{FIG}/fig1_c.png', dpi=300); plt.close()

# fig2: p99/p999 bars, stacked vertically
fig, axes = plt.subplots(2, 1, figsize=(CW, 3.5), sharex=True)
x = np.arange(len(W3)); wbar = 0.26
for ax, pct, ttl in zip(axes, ['p99', 'p999'], ['p99', 'p99.9']):
    for i, dev in enumerate(['conv', 'fdp', 'zns']):
        vals = [(G(f'{w}_{dev}', 'index_read_cmd', pct) + G(f'{w}_{dev}', 'memlog_read_cmd', pct)) / 2e3 for w in W3]
        bars = ax.bar(x + (i - 1) * wbar, vals, wbar, color=COLORS[dev], label=LABEL[dev])
        for b, v in zip(bars, vals):
            ax.text(b.get_x() + b.get_width() / 2, v * 1.18, f'{v:.2g}', ha='center', fontsize=4.8)
    ax.set_yscale('log'); ax.set_ylabel(f'{ttl} (ms)'); ax.grid(alpha=.3, axis='y', lw=0.4)
axes[0].legend(fontsize=5.2)
axes[1].set_xticks(x); axes[1].set_xticklabels([WLABEL[w] for w in W3])
plt.tight_layout(pad=0.4); plt.savefig(f'{FIG}/fig2_c.png', dpi=300); plt.close()

# fig3: WAF + erases, stacked
fig, axes = plt.subplots(2, 1, figsize=(CW, 3.4))
for i, dev in enumerate(['conv', 'fdp', 'zns']):
    axes[0].bar(x + (i - 1) * wbar, [GV(f'{w}_{dev}', 'WAF') for w in W3], wbar, color=COLORS[dev], label=LABEL[dev])
    axes[1].bar(x + (i - 1) * wbar, [GV(f'{w}_{dev}', 'erases_per_user_GB') for w in W3], wbar, color=COLORS[dev])
axes[0].set_ylabel('WAF'); axes[0].axhline(1.0, color='k', lw=.5, ls='--'); axes[0].legend(fontsize=5.2)
axes[1].set_ylabel('erases / user GB')
for ax in axes:
    ax.set_xticks(x); ax.set_xticklabels([WLABEL[w] for w in W3]); ax.grid(alpha=.3, axis='y', lw=0.4)
plt.tight_layout(pad=0.4); plt.savefig(f'{FIG}/fig3_c.png', dpi=300); plt.close()

# fig4: GC timeline (150 s window)
fig, axes = plt.subplots(2, 1, figsize=(CW, 2.7), sharex=True,
                         gridspec_kw={'height_ratios': [1, 2.2]})
gc = np.genfromtxt(f'{RES}/occ90_conv_gc.csv', delimiter=',', names=True)
t0, t1 = 3050, 3200
sel = (gc['start_s'] >= t0) & (gc['start_s'] <= t1)
for s, e in zip(gc['start_s'][sel], gc['end_s'][sel]):
    axes[0].axvspan(s, e, color='#c0392b', alpha=.6, lw=0)
axes[0].set_yticks([]); axes[0].set_ylabel('conv GC', fontsize=6)
tl = np.genfromtxt(f'{RES}/occ90_conv_timeline.csv', delimiter=',', names=True)
sel2 = (tl['t_s'] >= t0) & (tl['t_s'] <= t1)
axes[1].plot(tl['t_s'][sel2], tl['user_rd_MB'][sel2], color='#555', lw=.6, label='reads (MB/s)')
axes[1].plot(tl['t_s'][sel2], tl['user_wr_MB'][sel2], color='#2471a3', lw=.6, label='writes (MB/s)')
axes[1].set_xlabel('time (s)'); axes[1].set_ylabel('MB/s'); axes[1].legend(); axes[1].grid(alpha=.3, lw=0.4)
plt.tight_layout(pad=0.4); plt.savefig(f'{FIG}/fig4_c.png', dpi=300); plt.close()

# fig5: resume CDF
plt.figure(figsize=(CW, 2.5))
for dev in ['conv', 'zns']:
    v = np.sort(np.loadtxt(f'{RES}/occ90_{dev}_lat_resume_burst.csv'))
    plt.semilogx(v / 1e3, (np.arange(len(v)) + 1) / len(v), color=COLORS[dev], label=LABEL[dev])
v = np.sort(np.loadtxt(f'{RES}/occ90_zns_smallzone_lat_resume_burst.csv'))
plt.semilogx(v / 1e3, (np.arange(len(v)) + 1) / len(v), color='#7d3c98', ls='--', label='ZNS 16 MiB zones')
plt.xlabel('session-resume latency (ms)'); plt.ylabel('CDF')
plt.grid(alpha=.3, which='both', lw=0.4); plt.legend()
plt.tight_layout(pad=0.4); plt.savefig(f'{FIG}/fig5_c.png', dpi=300); plt.close()

# fig6: write tails across variants, stacked
VAR = [('occ90_conv', 'conv'), ('occ90_convS', 'conv+susp'), ('occ90_fdp32', 'fdp-32'),
       ('occ90_zns', 'zns'), ('occ90_znsS', 'zns+susp')]
VC = ['#c0392b', '#e74c3c', '#e67e22', '#2471a3', '#5dade2']
fig, axes = plt.subplots(2, 1, figsize=(CW, 3.3), sharex=True)
xv = np.arange(len(VAR))
for ax, pct, ttl in zip(axes, ['p99', 'p999'], ['p99', 'p99.9']):
    vals = [(G(n, 'index_write_cmd', pct) + G(n, 'memlog_write_cmd', pct)) / 2e3 for n, _ in VAR]
    bars = ax.bar(xv, vals, 0.6, color=VC)
    for b, v in zip(bars, vals):
        ax.text(b.get_x() + b.get_width() / 2, v * 1.12, f'{v:.1f}', ha='center', fontsize=5.2)
    ax.set_yscale('log'); ax.set_ylabel(f'wr {ttl} (ms)'); ax.grid(alpha=.3, axis='y', lw=0.4)
axes[1].set_xticks(xv); axes[1].set_xticklabels([l for _, l in VAR], fontsize=6)
plt.tight_layout(pad=0.4); plt.savefig(f'{FIG}/fig6_c.png', dpi=300); plt.close()

# fig7: granularity axis
AXIS = [('occ90_conv', 'conv'), ('occ90_fdp', 'fdp-3'), ('occ90_fdp8', 'fdp-8'),
        ('occ90_fdp32', 'fdp-32'), ('occ90_zns', 'zns')]
fig, ax1 = plt.subplots(figsize=(CW, 2.7))
xg = np.arange(len(AXIS))
ax1.bar(xg - 0.17, [GV(n, 'WAF') for n, _ in AXIS], 0.34, color='#7f8c8d', label='WAF')
ax1.set_ylabel('WAF'); ax1.axhline(1.0, color='k', lw=.5, ls='--')
ax1.set_xticks(xg); ax1.set_xticklabels([l for _, l in AXIS], fontsize=6.5)
ax2 = ax1.twinx()
ax2.plot(xg, [G(n, 'index_read_cmd', 'p99') / 1e3 for n, _ in AXIS], 'o-', color='#c0392b', ms=3, label='rd p99 (ms)')
ax2.plot(xg, [G(n, 'index_read_cmd', 'p999') / 1e3 for n, _ in AXIS], 's--', color='#2471a3', ms=3, label='rd p99.9 (ms)')
ax2.set_yscale('log'); ax2.set_ylabel('latency (ms)')
h1, l1 = ax1.get_legend_handles_labels(); h2, l2 = ax2.get_legend_handles_labels()
ax1.legend(h1 + h2, l1 + l2, fontsize=5.2, loc='center left')
plt.tight_layout(pad=0.4); plt.savefig(f'{FIG}/fig7_c.png', dpi=300); plt.close()

# fig8: OP sweep, stacked 3 panels
OPS = [('occ90_conv', '7%'), ('op14r_conv', '14%'), ('op28r_conv', '28%'), ('occ90_zns', 'ZNS')]
cs = ['#c0392b', '#d35400', '#e67e22', '#2471a3']
fig, axes = plt.subplots(3, 1, figsize=(CW, 3.7), sharex=True)
xo = np.arange(len(OPS))
axes[0].bar(xo, [GV(n, 'WAF') for n, _ in OPS], 0.6, color=cs); axes[0].set_ylabel('WAF')
axes[1].bar(xo, [G(n, 'index_read_cmd', 'p999') / 1e3 for n, _ in OPS], 0.6, color=cs)
axes[1].set_yscale('log'); axes[1].set_ylabel('rd p99.9 (ms)')
axes[2].bar(xo, [GV(n, 'usable_GiB') for n, _ in OPS], 0.6, color=cs); axes[2].set_ylabel('capacity (GiB)')
axes[2].set_xticks(xo); axes[2].set_xticklabels([l for _, l in OPS])
for ax in axes: ax.grid(alpha=.3, axis='y', lw=0.4)
plt.tight_layout(pad=0.4); plt.savefig(f'{FIG}/fig8_c.png', dpi=300); plt.close()
print('column figures written')
