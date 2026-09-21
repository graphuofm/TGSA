#!/usr/bin/env python3
"""fig_structural.pdf -- regenerate with: python3 fig_structural.py (edits welcome)."""
from figstyle import *

# =====================================================================
# Fig. structural: the concession, priced across corpora and algorithms
# =====================================================================
def fig_structural():
    fig, ax = plt.subplots(figsize=(3.34, 1.88))
    rows = (load('e13_storage/e13_full.csv') + load('e13_storage/e13_lanl.csv')
            + load('e13_storage/e13_more.csv'))
    g = defaultdict(list)
    for r in rows:
        if r['algo'] in ('bfs', 'pr', 'cc') and int(r['edges_delivered']) > 1000:
            g[(r['dataset'], r['algo'], r['engine'])].append(float(r['storage_sec']))
    edges = {'CollegeMsg': 5.98e4, 'email-Eu-core-temporal': 3.32e5, 'act-mooc': 4.12e5,
             'sx-mathoverflow': 5.07e5, 'sx-askubuntu': 9.64e5, 'sx-superuser': 1.44e6,
             'wiki-talk-temporal': 7.83e6, 'cit-Patents': 1.65e7, 'sx-stackoverflow': 6.35e7,
             'soc-LiveJournal1': 6.90e7, 'lanl-auth': 1.05e9}
    series = [('bfs', 'BFS (frontier-driven)', '#000000', 'o', '-'),
              ('pr', 'PageRank (all vertices)', '#0072B2', 's', '--'),
              ('cc', 'label prop. (all vertices)', '#E69F00', '^', '-.')]
    allr = []
    for al, lbl, col, mk, ls in series:
        xs, ys = [], []
        for ds, n in sorted(edges.items(), key=lambda kv: kv[1]):
            if g.get((ds, al, 'csr')) and g.get((ds, al, 'barry')):
                xs.append(n); ys.append(med(g[(ds, al, 'barry')]) / med(g[(ds, al, 'csr')]))
        allr += ys
        ax.plot(xs, ys, marker=mk, ls=ls, color=col, label=lbl, ms=3.6, lw=1.1, zorder=3)
    ax.axhline(1.0, color='#009E73', lw=1.3, ls=(0, (1, 1)), zorder=4)
    ax.text(1.3e9, 0.82, 'topology-only CSR: optimum,\nno ingest path', ha='right',
            fontsize=6.0, color='#007a58', style='italic', va='bottom')
    ax.set_xscale('log')
    ax.set_xlabel('corpus size (edges)')
    ax.set_ylabel('adjacency delivery,\n$\\times$ topology-only CSR')
    ax.set_ylim(0.3, 6.8)
    ax.legend(frameon=False, loc='upper left', fontsize=6.5, handlelength=1.9,
              handletextpad=0.5, borderpad=0.15, labelspacing=0.3)
    ax.annotate(f'{len(allr)} measurements, 11 corpora:\n'
                f'{min(allr):.1f}–{max(allr):.1f}$\\times$, median {sorted(allr)[len(allr)//2]:.1f}$\\times$',
                xy=(0.97, 0.30), xycoords='axes fraction', ha='right', fontsize=6.4)
    fig.tight_layout(pad=0.35)
    fig.savefig(FIG + 'fig_structural.pdf'); plt.close(fig)


if __name__ == "__main__":
    fig_structural()
    print("wrote", FIG + "fig_structural.pdf")
