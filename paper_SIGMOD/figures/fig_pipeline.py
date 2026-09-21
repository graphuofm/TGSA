#!/usr/bin/env python3
"""fig_pipeline.pdf -- regenerate with: python3 fig_pipeline.py (edits welcome)."""
from figstyle import *

# =====================================================================
# Fig. pipeline: E22 extract -> compute -> writeback, three systems
# =====================================================================
def fig_pipeline():
    fig, (a, b) = plt.subplots(1, 2, figsize=(7.0, 1.7))
    rows = load('e22_pipeline/e22.csv')
    ds = 'wiki-talk-temporal'
    sysstyle = [('gnomon', '#000000', 'o', '-', 'TGSR'),
                ('duckdb', C['duckdb'], 'v', (0, (3, 1, 1, 1)), 'DuckDB'),
                ('livegraph', C['lg'], '^', '-.', 'LiveGraph')]
    for s, col, mk, ls, lbl in sysstyle:
        pts = sorted([(float(r['window_frac']), float(r['total_sec']))
                      for r in rows if r['system'] == s and r['dataset'] == ds])
        a.plot([p[0] for p in pts], [p[1] for p in pts], marker=mk, ls=ls,
               color=col, label=lbl, zorder=3)
    a.set_xscale('log'); a.set_yscale('log')
    a.set_xlabel('window (fraction of span)')
    a.set_ylabel('extract+compute+\nwriteback (s)')
    a.legend(frameon=False, fontsize=6.4, loc='upper left', handlelength=1.9,
             borderpad=0.15, labelspacing=0.3)
    a.set_title('(a) Windowed analytics pipeline (wiki-talk)', loc='left',
                fontsize=8)

    # (b) extract share of the pipeline: where each system's time goes
    for s, col, mk, ls, lbl in sysstyle:
        pts = sorted([(float(r['window_frac']), 100*float(r['extract_share']))
                      for r in rows if r['system'] == s and r['dataset'] == ds])
        b.plot([p[0] for p in pts], [p[1] for p in pts], marker=mk, ls=ls,
               color=col, label=lbl, zorder=3)
    b.set_xscale('log'); b.set_ylim(0, 105)
    b.set_xlabel('window (fraction of span)')
    b.set_ylabel('extraction share of\npipeline time (%)')
    b.text(0.03, 0.06, 'extraction dominates their loop, not ours',
           transform=b.transAxes, fontsize=6.2, style='italic', color='#555555')
    b.set_title('(b) Where the pipeline time goes', loc='left', fontsize=8)
    fig.tight_layout(pad=0.35, w_pad=1.6)
    fig.savefig(FIG + 'fig_pipeline.pdf'); plt.close(fig)


if __name__ == "__main__":
    fig_pipeline()
    print("wrote", FIG + "fig_pipeline.pdf")
