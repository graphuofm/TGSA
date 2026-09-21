#!/usr/bin/env python3
"""fig_recovery.pdf -- regenerate with: python3 fig_recovery.py (edits welcome)."""
from figstyle import *

# =====================================================================
# Fig. recovery: (a) the durability cost spectrum; (b) replay curve to 1.024B
# =====================================================================
def fig_recovery():
    fig, (a, b) = plt.subplots(1, 2, figsize=(7.0, 1.75))
    modes = ['none', 'write_only', 'async_64MB', 'async_4MB', 'sync_4MB', 'fsync_batch']
    mlbl = ['no log', 'write-\nonly', 'async\n64MB', 'async\n4MB', 'sync\n4MB', 'fsync/\nbatch']
    style = {'wiki-talk-temporal': ('#000000', 'o', '-'),
             'cit-Patents': ('#0072B2', 's', '--'),
             'sx-stackoverflow': ('#E69F00', '^', '-.')}
    rows = load('e18_recovery/cost.csv')
    for ds, (col, mk, ls) in style.items():
        ys = []
        for m in modes:
            v = [float(r['ingest_Meps']) for r in rows
                 if r['dataset'] == ds and r['durability'] == m]
            ys.append(med(v) if v else float('nan'))
        a.plot(range(len(modes)), ys, marker=mk, ls=ls, color=col,
               label=SHORT[ds], zorder=3)
    a.set_yscale('log')
    a.set_xticks(range(len(modes))); a.set_xticklabels(mlbl, fontsize=6.4)
    a.set_ylabel('ingest (M events/s)')
    a.legend(frameon=False, fontsize=6.6, loc='upper right', handlelength=1.9,
             borderpad=0.15, labelspacing=0.3)
    a.text(0.02, 0.04, 'bounded by the device, not the engine',
           transform=a.transAxes, fontsize=6.2, style='italic', color='#555555')
    a.set_title('(a) The durability spectrum', loc='left', fontsize=8)

    rows = load('e18_recovery/curve.csv')
    xs = [int(r['edges']) for r in rows]
    wr = [float(r['write_Meps']) for r in rows]
    rp = [float(r['replay_Meps']) for r in rows]
    sec = [float(r['replay_sec']) for r in rows]
    b.plot(xs, rp, marker='o', ls='-', color='#000000', label='replay (recovery)', zorder=4)
    b.plot(xs, wr, marker='s', ls='--', color='#0072B2', label='log write (durable ingest)', zorder=3)
    b.set_xscale('log')
    b.set_ylim(0, 44)
    b.set_xlabel('logged edges (LANL prefix)')
    b.set_ylabel('M events/s')
    b.annotate(f'1.024B edges\nin {sec[-1]:.1f} s', xy=(xs[-1], rp[-1]),
               xytext=(0.97, 0.52), textcoords='axes fraction', ha='right',
               fontsize=6.8, arrowprops=dict(arrowstyle='->', lw=0.7,
                                             color='#333333'))
    b.legend(frameon=False, fontsize=6.6, loc='upper left', handlelength=1.9,
             borderpad=0.15, labelspacing=0.3)
    b.set_title('(b) Recovery is re-ingestion, flat to a billion edges',
                loc='left', fontsize=8)
    fig.tight_layout(pad=0.35, w_pad=1.6)
    fig.savefig(FIG + 'fig_recovery.pdf'); plt.close(fig)


if __name__ == "__main__":
    fig_recovery()
    print("wrote", FIG + "fig_recovery.pdf")
