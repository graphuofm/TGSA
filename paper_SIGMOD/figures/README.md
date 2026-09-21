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
