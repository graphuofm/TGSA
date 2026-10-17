#!/usr/bin/env python3
"""fig_recovery.pdf: ingestion under each durability policy on the four
aligned corpora, with RocksDB's write-ahead log as the reference. The
replay curve (recovery is re-ingestion) is quoted in the text.
Sources: results/e18_recovery/cost.csv, curve.csv; results/e24_rocksdb/e24.csv."""
from figstyle import *
from collections import defaultdict
import numpy as np

MODES = [('none', 'no log'), ('write_only', 'write only'), ('async_64MB', 'async 64 MB'),
         ('async_4MB', 'async 4 MB'), ('sync_4MB', 'sync 4 MB'), ('fsync_batch', 'fsync / batch')]
GREYS = BLUES

def fig_recovery():
    cost = load('e18_recovery/cost.csv')
    e24 = load('e24_rocksdb/e24.csv')
    g = defaultdict(list)
    for r in cost:
        g[(r['dataset'], r['durability'])].append(fnum(r['ingest_Meps']))
    wal = defaultdict(list)
    for r in e24:
        if r['system'] == 'rocksdb' and r['phase'] == 'ingest' and r['param'] == 'wal':
            wal[r['dataset']].append(fnum(r['Meps_or_ms']))
    fig, a = plt.subplots(1, 1, figsize=(3.4, 2.15))
    xs = np.arange(len(CORPORA)); n = len(MODES) + 1; w = 0.8 / n
    for k, (m, lbl) in enumerate(MODES):
        ys = [med(g[(c, m)]) if g[(c, m)] else float('nan') for c in CORPORA]
        pos = xs - 0.4 + w * (k + 0.5)
        a.bar(pos, ys, width=w * 0.9, color=GREYS[k], edgecolor='black', linewidth=0.4,
              label='TGSA, ' + lbl, zorder=3)
        for p, y in zip(pos, ys):
            if y == y:
                a.text(p, y, f'{y:.0f}' if y >= 10 else f'{y:.1f}', rotation=90, ha='center',
                       va='bottom', fontsize=4.8)
            else:
                a.text(p, 0.02, 'n/a', rotation=90, ha='center', va='bottom', fontsize=5,
                       color=GREY, transform=a.get_xaxis_transform())
    ys = [med(wal[c]) if wal[c] else float('nan') for c in CORPORA]
    pos = xs - 0.4 + w * (len(MODES) + 0.5)
    a.bar(pos, ys, width=w * 0.9, **bar_kw('rocksdb'), label='RocksDB, WAL on', zorder=3)
    for p, y in zip(pos, ys):
        if y == y:
            a.text(p, y, f'{y:.1f}', rotation=90, ha='center', va='bottom', fontsize=4.8)
        else:
            a.text(p, 0.02, 'n/a', rotation=90, ha='center', va='bottom', fontsize=5,
                   color=GREY, transform=a.get_xaxis_transform())
    a.set_yscale('log'); a.set_xticks(xs)
    a.set_xticklabels([corpus_label(c) for c in CORPORA], fontsize=6.5)
    a.set_ylabel('M events/s (log)'); a.grid(axis='x', visible=False)
    a.legend(frameon=False, fontsize=5.4, ncol=2, loc='upper center',
             bbox_to_anchor=(0.5, 1.0), handlelength=1.0, columnspacing=0.7, borderpad=0.1)
    a.set_ylim(top=a.get_ylim()[1] * 40)

    fig.tight_layout()
    fig.savefig(FIG + 'fig_recovery.pdf'); plt.close(fig)


if __name__ == "__main__":
    fig_recovery()
    print("wrote", FIG + "fig_recovery.pdf")
