#!/usr/bin/env python3
"""fig_analysis.pdf: Figure 12, performance analysis of TGSA, one mechanism
per panel, on the four aligned corpora wherever the sweep exists.
(a) run claiming: ingestion against writer threads, with (solid) and
    without (dotted)                              results/e23_ablation/runclaim_*.csv
(b) binary-search threshold: Q2 cost against the threshold
                                                  results/e23_ablation/threshold.csv
(c) inline timestamp: Q2 and Q4 median latency with the 24 B and the 16 B
    reference                                     results/e33_matrix + e23_ablation/notscache_e33
(d) optional modules: ingestion per configuration results/e25_dock/e25.csv
(e) mixed load: ingest and query throughput across writer:reader mixes
                                                  results/e26_concurrent/e26.csv (tgsr rows)
(f) shards: ingestion against shard count at 16 threads
                                                  results/e27_shard/e27.csv
Corpora are told apart by marker; TGSA variants by line style or grey."""
from figstyle import *
from collections import defaultdict
import glob, os, csv
import numpy as np

CMARK = {'lanl-auth': 'o', 'tc-theia-e3': 's', 'tc-cadets-e3': '^', 'wiki-talk-temporal': 'D'}
CGREY = {'lanl-auth': BLUES[0], 'tc-theia-e3': BLUES[1], 'tc-cadets-e3': BLUES[2],
         'wiki-talk-temporal': BLUES[3]}

def fig_analysis():
    fig, axes = plt.subplots(2, 2, figsize=(7.0, 3.05))
    a, c, d, e = axes.flat

    # ---- (a) run claiming
    for fn in glob.glob(R + 'e23_ablation/runclaim_*.csv'):
        rows = list(csv.DictReader(open(fn)))
        if not rows:
            continue
        ds = rows[0]['dataset']
        if ds not in CMARK:
            continue
        for rc, ls in (('1', '-'), ('0', ':')):
            pts = sorted((int(r['threads']), fnum(r['ingest_Meps'])) for r in rows if r['run_claim'] == rc)
            if pts:
                a.plot([p[0] for p in pts], [p[1] for p in pts], marker=CMARK[ds], ls=ls,
                       color=CGREY[ds], ms=3.5, label=f"{SHORT[ds]}" if rc == '1' else None, zorder=3)
    a.set_xscale('log', base=2); a.set_xticks([1, 2, 4, 8]); a.set_xticklabels(['1', '2', '4', '8'])
    a.set_xlabel('writer threads'); a.set_ylabel('M events/s')
    a.legend(frameon=False, fontsize=5.6, loc='upper left', handlelength=1.4, borderpad=0.1,
             title='solid: run claiming on; dotted: off', title_fontsize=5.4)
    panel_label(a, '(a) Run claiming')

    # ---- (c) inline timestamp: 24 B vs 16 B reference, Q2 and Q4 medians
    e33 = load_e33()
    nots = []
    try:
        nots = load('e23_ablation/notscache_e33/e33.csv')
    except FileNotFoundError:
        pass
    xs = np.arange(len(CORPORA)); w = 0.2
    for k, (phase, src, lbl, col, hatch) in enumerate([
            ('q2', e33, 'Q2, 24 B ref', BLUES[0], ''), ('q2', nots, 'Q2, 16 B ref', '#ffffff', '///'),
            ('q4', e33, 'Q4, 24 B ref', BLUES[2], ''), ('q4', nots, 'Q4, 16 B ref', '#ffffff', 'xxx')]):
        ys = []
        for cc in CORPORA:
            r = e33_cell(src, 'tgsr', cc, phase)
            ys.append(fnum(r['median_us']) if r else float('nan'))
        c.bar(xs - 0.4 + w * (k + 0.5), ys, width=w * 0.9, color=col, edgecolor='black',
              linewidth=0.4, hatch=hatch, label=lbl, zorder=3)
    c.set_yscale('log'); c.set_xticks(xs); c.set_xticklabels([SHORT[x] for x in CORPORA], fontsize=6)
    c.set_ylabel('median µs (log)'); c.grid(axis='x', visible=False)
    c.legend(frameon=False, fontsize=5.4, ncol=4, loc='upper center', handlelength=1.1, borderpad=0.1, columnspacing=0.8)
    c.set_ylim(top=c.get_ylim()[1] * 8)
    panel_label(c, '(b) Inline timestamp')

    # ---- (d) modules
    rows = load('e25_dock/e25.csv')
    g = defaultdict(list)
    for r in rows:
        g[(r['dataset'], r['rung'])].append(fnum(r['ingest_Meps']))
    rungs = [('core', 'core'), ('+vindex', '+vertex index'), ('+props16', '+properties'),
             ('+reverse', '+reverse index'), ('+wal', '+log'), ('full', 'all')]
    w = 0.8 / len(rungs)
    greys = BLUES
    for k, (ru, lbl) in enumerate(rungs):
        ys = [med(g[(cc, ru)]) if g[(cc, ru)] else float('nan') for cc in CORPORA]
        d.bar(xs - 0.4 + w * (k + 0.5), ys, width=w * 0.9, color=greys[k], edgecolor='black',
              linewidth=0.4, label=lbl, zorder=3)
    d.set_yscale('log'); d.set_xticks(xs); d.set_xticklabels([SHORT[x] for x in CORPORA], fontsize=6)
    d.set_ylabel('M events/s (log)'); d.grid(axis='x', visible=False)
    d.legend(frameon=False, fontsize=5.2, ncol=3, loc='upper right', handlelength=1.0,
             borderpad=0.1, columnspacing=0.7)
    d.set_ylim(top=d.get_ylim()[1] * 4)
    panel_label(d, '(c) Optional modules')

    # ---- (e) mixed load (tgsr rows of E26)
    rows = [r for r in load('e26_concurrent/e26.csv') if sysk(r['system']) == 'tgsr']
    mixes = ['8:1', '8:4', '4:4', '2:8']
    e2 = e.twinx()
    for ds in CORPORA:
        ing, qps = [], []
        for m in mixes:
            W_, R_ = m.split(':')
            v = [r for r in rows if r['dataset'] == ds and r['writers'] == W_ and r['readers'] == R_]
            ing.append(med([fnum(r['ingest_Meps']) for r in v]) if v else float('nan'))
            qps.append(med([fnum(r['queries_per_sec']) for r in v]) / 1e3 if v else float('nan'))
        e.plot(range(len(mixes)), ing, marker=CMARK[ds], color=CGREY[ds], ms=3.5, label=SHORT[ds], zorder=3)
        e2.plot(range(len(mixes)), qps, marker=CMARK[ds], color=CGREY[ds], ms=3.5, ls='--', zorder=3)
    e.set_xticks(range(len(mixes))); e.set_xticklabels(mixes, fontsize=6.4)
    e.set_xlabel('writers : readers'); e.set_ylabel('M events/s (solid)')
    e2.set_ylabel('K queries/s (dashed)', fontsize=7); e2.set_yscale('log'); e2.grid(False)
    e2.spines['right'].set_visible(True); e2.tick_params(axis='y', labelsize=6.5)
    e.legend(frameon=False, fontsize=5.6, loc='upper right', handlelength=1.4, borderpad=0.1)
    panel_label(e, '(d) Mixed load')

    fig.tight_layout(h_pad=1.0, w_pad=1.0)
    fig.savefig(FIG + 'fig_analysis.pdf'); plt.close(fig)


if __name__ == "__main__":
    fig_analysis()
    print("wrote", FIG + "fig_analysis.pdf")
