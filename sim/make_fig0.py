#!/usr/bin/env python3
"""Design schematic: lifetime mixing under the block interface vs. Anchor's
session-aligned placement. Column-width figure."""
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle, FancyArrow
plt.rcParams.update({'font.size': 6.2})

fig, axes = plt.subplots(1, 2, figsize=(3.35, 1.95))
SC = ['#c6dbef', '#9ecae1', '#6baed6', '#3182bd']    # sessions A-D
IX, ML = '#fdd0a2', '#a1d99b'

def block(ax, x, y, cells, w=0.155, h=0.10):
    for i, c in enumerate(cells):
        ax.add_patch(Rectangle((x + (i % 4) * w / 4, y + (i // 4) * h / 2),
                               w / 4, h / 2, fc=c, ec='k', lw=0.25))
    ax.add_patch(Rectangle((x, y), w, h, fill=False, ec='k', lw=0.7))

ax = axes[0]
ax.set_title('block interface: FTL mixes lifetimes', fontsize=6.4)
import random
random.seed(7)
pool = SC + [IX, ML]
ys = [0.72, 0.54, 0.36, 0.18]
for r, y in enumerate(ys):
    for c in range(4):
        cells = [random.choice(pool) for _ in range(8)]
        if r == 1 and c == 2:
            cells = ['white' if i % 3 else cells[i] for i in range(8)]
        block(ax, 0.06 + c * 0.24, y, cells)
ax.annotate('GC: copy live pages,\nerase, re-mix', xy=(0.55, 0.30), xytext=(0.28, 0.02),
            fontsize=5.6, arrowprops=dict(arrowstyle='->', lw=0.7))
ax.text(0.06, 0.90, 'superblocks (all dies)', fontsize=5.8)

ax = axes[1]
ax.set_title('Anchor on ZNS: zone per session', fontsize=6.4)
rows = [(SC[0], 'session A'), (SC[1], 'session B'), (SC[2], 'session C'), (SC[3], 'session D (dead)')]
for r, (col, lab) in enumerate(rows):
    y = 0.72 - r * 0.18
    cells = [col] * 8 if r != 3 else ['white'] * 8
    block(axes[1], 0.06, y, cells, w=0.42, h=0.10)
    axes[1].text(0.52, y + 0.03, lab, fontsize=5.6)
block(axes[1], 0.06, 0.72 - 4 * 0.18, [ML] * 8, w=0.42, h=0.10)
axes[1].text(0.52, 0.72 - 4 * 0.18 + 0.03, 'memory log (FIFO)', fontsize=5.6)
axes[1].annotate('reset on\nsession death', xy=(0.27, 0.22), xytext=(0.60, 0.38),
                 fontsize=5.6, arrowprops=dict(arrowstyle='->', lw=0.7))
for ax in axes:
    ax.set_xlim(0, 1); ax.set_ylim(0, 1); ax.axis('off')
plt.tight_layout(pad=0.3)
plt.savefig('figs/fig0_c.png', dpi=300)
print('fig0 done')
