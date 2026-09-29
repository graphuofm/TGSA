# Figure sources

Every figure ships with an editable same-named source:

| figure | source | regenerate with |
|---|---|---|
| fig_motivation.pdf | fig_motivation.py | `python3 fig_motivation.py` |
| fig_ingest.pdf | fig_ingest.py | `python3 fig_ingest.py` |
| fig_baselines.pdf | fig_baselines.py | `python3 fig_baselines.py` |
| fig_temporal.pdf | fig_temporal.py | `python3 fig_temporal.py` |
| fig_structural.pdf | fig_structural.py | `python3 fig_structural.py` |
| fig_recovery.pdf | fig_recovery.py | `python3 fig_recovery.py` |
| fig_ablation.pdf | fig_ablation.py | `python3 fig_ablation.py` |
| fig_pipeline.pdf | fig_pipeline.py | `python3 fig_pipeline.py` |
| Fig 2 (architecture) | ../fig_arch.tex (TikZ) | compiles with main.tex |
| Fig 3 (storage formats) | ../fig_format.tex (TikZ) | compiles with main.tex |

Shared palette/rcParams/loaders: figstyle.py. All data comes from
results/ CSVs; nothing is hand-entered except the drawn Fig 1 narrative.
scripts/make_sigmod_figures.py runs all of them.

## Editable drawings (.drawio)

The four hand-drawn figures also ship as same-named diagrams.net files,
so they can be rearranged by hand instead of in code:

    fig_motivation.drawio   fig_arch.drawio
    fig_format.drawio       fig_runclaim.drawio

Open them at app.diagrams.net or in the VS Code Draw.io extension; they
are plain uncompressed XML, so they also diff cleanly. `make_drawio.py`
regenerates all four from scratch, which OVERWRITES hand edits, so run
it only when you want to start over.

What the paper actually compiles is still the TikZ source
(../fig_arch.tex, ../fig_format.tex, ../fig_runclaim.tex) and
fig_motivation.py. After editing a drawio, either export it to PDF here
and switch that figure's \input to \includegraphics, or carry the
change back into the TikZ. The data charts have no drawio: they are
generated from CSVs and belong in code.
