# Figures of the SIGMOD paper

Every `fig_*.pdf` has a same-named generator next to it. `figstyle.py` holds
the one color legend of the paper (systems vs paths; identical hex values to
the `\definecolor` block in `main.tex`), the loaders for the aligned matrix
(`results/e33_matrix`, spec in `docs/E33_SPEC.md`), and the grouped-bar helper
that gives every evaluation figure the same layout: one group per corpus,
large to small (LANL, THEIA, CADETS, wiki-talk), one bar per system.

| file | figure | source |
|---|---|---|
| fig_motivation.py | 1, the CADETS intrusion | drawn from the published narrative |
| ../fig_overview.tex | 2, engine components | TikZ |
| ../fig_arch.tex | 3, architecture with Q1/Q2 drawn on the structures, plus the byte formats | TikZ |
| fig_ingest.py | 4, ingestion, ingestion under readers, memory | e33_matrix, e31/e32, e26 |
| fig_queries.py | 5, Q1..Q4 grid | e33_matrix |
| fig_analytics.py | 6, analytics pipeline and whole-graph BFS | e22_pipeline, e33_matrix, e13 |
| fig_recovery.py | 7, durability cost | e18_recovery, e24_rocksdb |
| fig_analysis.py | 8, performance analysis (four panels) | e23, e25, e26, notscache_e33 |

`../fig_runclaim.tex` (run claiming) is kept in the repository but no longer in the paper (cut for length; its effect is Figure 8(a)).

Regenerate everything with `python3 ../../scripts/make_sigmod_figures.py`, or
one figure with `python3 fig_X.py`. `make_drawio.py` writes the editable
draw.io sources for the four drawings. Rules: no text below 7pt at final
size, no `\tiny` in TikZ, a missing cell is marked "n/a" or "did not finish",
never dropped.
