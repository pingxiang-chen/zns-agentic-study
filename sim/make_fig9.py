#!/usr/bin/env python3
"""Figure 9 (column width): device-model results on the real captured e2e trace."""
import re, sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
plt.rcParams.update({'font.size': 7, 'axes.titlesize': 7.5, 'axes.labelsize': 7,
                     'xtick.labelsize': 6.3, 'ytick.labelsize': 6.3, 'legend.fontsize': 5.8})

RES = 'res'; FIG = 'figs'
def parse(p):
    d = {'lat': {}}
    for line in open(p):
        m = re.match(r'LAT (\S+) n=\d+ sampled=\d+ mean_us=([\d.]+) p50_us=(\d+) p90_us=(\d+) p99_us=(\d+) p999_us=(\d+)', line)
        if m: d['lat'][m.group(1)] = dict(mean=float(m.group(2)), p99=int(m.group(5)), p999=int(m.group(6)))
        else:
            for k, v in re.findall(r'(\w+)=([-\d.]+)', line):
                try: d[k] = float(v)
                except ValueError: pass
    return d
R = {n: parse(f'{RES}/real_{n}_summary.txt') for n in ['conv', 'convS', 'fdp32', 'zns']}

fig, axes = plt.subplots(1, 2, figsize=(3.35, 2.2))
names = ['conv', 'fdp32', 'zns']; cs = ['#c0392b', '#e67e22', '#2471a3']
axes[0].bar(np.arange(3), [R[n]['WAF'] for n in names], 0.6, color=cs)
axes[0].axhline(1.0, color='k', lw=.5, ls='--')
axes[0].set_xticks(np.arange(3)); axes[0].set_xticklabels(['conv', 'fdp-32', 'zns'], fontsize=6.5)
axes[0].set_ylabel('WAF'); axes[0].set_title('WAF', fontsize=7.5)
for i, n in enumerate(names):
    axes[0].text(i, R[n]['WAF'] + 0.03, f"{R[n]['WAF']:.2f}", ha='center', fontsize=5.6)
names2 = ['conv', 'convS', 'zns']; cs2 = ['#c0392b', '#e74c3c', '#2471a3']
rd = [(R[n]['lat']['index_read_cmd']['p99'] + R[n]['lat']['memlog_read_cmd']['p99']) / 2e3 for n in names2]
wr = [(R[n]['lat']['index_write_cmd']['p99'] + R[n]['lat']['memlog_write_cmd']['p99']) / 2e3 for n in names2]
x = np.arange(3)
axes[1].bar(x - 0.18, rd, 0.36, color=cs2, label='read')
axes[1].bar(x + 0.18, wr, 0.36, color=cs2, hatch='//', label='write')
axes[1].set_yscale('log'); axes[1].set_xticks(x)
axes[1].set_xticklabels(['conv', 'conv+susp', 'zns'], fontsize=6)
axes[1].set_title('mem op p99 (ms)', fontsize=7.5); axes[1].legend(fontsize=5.4)
for ax in axes: ax.grid(alpha=.3, axis='y', lw=0.4)
plt.tight_layout(pad=0.4)
plt.savefig(f'{FIG}/fig9_c.png', dpi=300)
print('fig9 done;', {n: R[n]['WAF'] for n in names},
      'rd_p99', [f'{v:.1f}' for v in rd], 'wr_p99', [f'{v:.1f}' for v in wr])
