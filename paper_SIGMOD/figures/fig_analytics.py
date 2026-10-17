#!/usr/bin/env python3
"""fig_analytics.pdf: analytics on the stored graph, two panels on the four
aligned corpora.
(a) The extract-compute-writeback pipeline end to end at the 1% window on
    TGSA, DuckDB, LiveGraph (results/e22_pipeline; medians over trials; the
    compute phase is identical code everywhere).
(b) Whole-graph BFS, time ignored: TGSA and LiveGraph through the identical
    harness BFS (E33 bfs phase), GraphOne through its own BFS kernel (job
    20), and the topology-only static CSR that cannot ingest (E13).
The PageRank / label-propagation share against the CSR is quoted in the
text from scripts/fill_numbers.py."""
from figstyle import *
from collections import defaultdict

PSYS = ['tgsr', 'duckdb', 'livegraph']
BSYS = ['tgsr', 'livegraph', 'csr']   # GraphOne's BFS runs only inside its own harness; not the shared protocol
W = 0.01

def e13_rows():
    rows = []
    for f in ['e13_storage/e13_current.csv', 'e13_storage/e13_lanl.csv',
              'e13_storage/e13_aligned.csv', 'e13_storage/e13_more.csv']:
        rows += try_load(f)
    return rows

def fig_analytics():
    # ---- (a) pipeline
    tot = defaultdict(list)
    for r in try_load('e22_pipeline/e22.csv'):
        if abs(fnum(r['window_frac']) - W) > 1e-9:
            continue
        tot[(sysk(r['system']), r['dataset'])].append(fnum(r['total_sec']))
    # ---- (b) bfs
    e33 = load_e33(); e13 = e13_rows()
    go = try_load('e33_matrix/graphone.csv') + try_load('e20_graphone/graphone.csv')
    def bfs(s, c):
        if s == 'graphone':
            v = [fnum(r['bfs_sec']) for r in go if r['dataset'] == c and r['job'] == '20'
                 and r['config'] == 'weight64' and r.get('bfs_sec')]
            return (med(v), 'own kernel') if v else (None, 'n/a')
        if s == 'csr':
            v = [fnum(r['total_sec']) for r in e13 if r['dataset'] == c
                 and r['algo'] == 'bfs' and r['engine'] == 'csr']
            return (med(v), '') if v else (None, 'n/a')
        r = e33_cell(e33, s, c, 'bfs')
        if r is None or fnum(r['seconds']) == 0:
            return None, 'n/a' if r else ''
        return fnum(r['seconds']), ('did not finish' if 'DNF' in r.get('note', '') else '')

    fig, (a, b) = plt.subplots(2, 1, figsize=(3.4, 2.7))
    grouped_bars(a, CORPORA, PSYS,
                 lambda s, c: (med(tot[(s, c)]) if tot[(s, c)] else None, '' if tot[(s, c)] else 'n/a'),
                 log=True)
    a.set_ylabel('seconds (log)'); panel_label(a, '(a) Analytics pipeline, 1% window')
    grouped_bars(b, CORPORA, BSYS, bfs, log=True)
    b.set_ylabel('BFS seconds (log)'); panel_label(b, '(b) Whole-graph BFS')
    for ax in (a, b):
        ax.tick_params(axis='x', labelsize=6.5)
    handles = legend_handles(['tgsr', 'livegraph', 'duckdb', 'csr'])
    fig.legend(handles=handles, ncol=4, frameon=False, fontsize=6.4,
               loc='lower center', bbox_to_anchor=(0.5, -0.01), handlelength=1.2,
               columnspacing=1.0)
    fig.tight_layout(rect=(0, 0.09, 1, 1), h_pad=1.2)
    fig.savefig(FIG + 'fig_analytics.pdf'); plt.close(fig)


if __name__ == "__main__":
    fig_analytics()
    print("wrote", FIG + "fig_analytics.pdf")
