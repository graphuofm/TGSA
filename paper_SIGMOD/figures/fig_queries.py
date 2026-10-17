#!/usr/bin/env python3
"""fig_queries.pdf: Figure 7. Rows Q1..Q4, columns the four aligned corpora,
one bar per system that can express the query. Within a row the y scale is
shared across the columns. Source: results/e33_matrix (spec in
docs/E33_SPEC.md)."""
from figstyle import *
import numpy as np

QSYS = ['tgsr', 'graphone', 'livegraph', 'rocksdb', 'duckdb']
ROWS = [('q1', 'Q1 window scan', 'ms'),
        ('q2', 'Q2 neighbors', 'median µs'),
        ('q3', 'Q3 traversal', 'median µs'),
        ('q4', 'Q4 as-of', 'median µs')]

def fig_queries():
    rows = load_e33()
    fig, axes = plt.subplots(4, 4, figsize=(7.0, 3.9), sharey='row')
    for i, (phase, title, unit) in enumerate(ROWS):
        vals = {}
        for j, c in enumerate(CORPORA):
            for s in QSYS:
                r = e33_cell(rows, s, c, phase)
                if r is None:
                    vals[(s, c)] = (None, '')
                    continue
                note = r.get('note', '')
                if note.startswith('n/a') or (fnum(r['seconds']) == 0 and fnum(r['median_us']) == 0):
                    vals[(s, c)] = (None, 'n/a')
                    continue
                if phase == 'q1':
                    y = fnum(r['seconds']) * 1e3
                else:
                    y = fnum(r['median_us'])
                tag = ''
                done, tot = fnum(r.get('samples_done')), fnum(r.get('samples_total'))
                if tot and done < tot:
                    tag = f'{int(done)}/{int(tot)}'
                if 'DNF' in note:
                    tag = 'did not finish'
                vals[(s, c)] = (y, tag)
        for j, c in enumerate(CORPORA):
            ax = axes[i][j]
            n = len(QSYS); width = 0.8 / n
            for k, s in enumerate(QSYS):
                y, tag = vals[(s, c)]
                x = -0.4 + width * (k + 0.5)
                if y:
                    ax.bar(x, y, width=width * 0.9, **bar_kw(s), zorder=3)
                    if tag:
                        ax.text(x, y, tag, rotation=90, ha='center', va='bottom',
                                fontsize=4.8)
                elif tag:
                    ax.text(x, 0.02, tag, rotation=90, ha='center', va='bottom',
                            fontsize=5.0, color=GREY, transform=ax.get_xaxis_transform())
            ax.set_yscale('log')
            ax.set_xlim(-0.5, 0.5)
            ax.set_xticks([])
            ax.grid(axis='x', visible=False)
            ax.tick_params(axis='y', labelsize=6.5)
            if i == 0:
                ax.set_title(f"{SHORT[c]} ({EVENTS[c]} events)", fontsize=7.5, pad=4)
            if j == 0:
                ax.set_ylabel(f"{title}\n({unit})", fontsize=6.2, labelpad=2)
    fig.legend(handles=legend_handles(QSYS), ncol=5, frameon=False, fontsize=7,
               loc='lower center', bbox_to_anchor=(0.5, -0.005), handlelength=1.4)
    fig.tight_layout(rect=(0, 0.05, 1, 1), h_pad=0.4, w_pad=0.5)
    fig.savefig(FIG + 'fig_queries.pdf'); plt.close(fig)


if __name__ == "__main__":
    fig_queries()
    print("wrote", FIG + "fig_queries.pdf")
