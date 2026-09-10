# Results storage convention

**Layout:**

```
results/
├── e1_ingest/
│   └── runs/
│       ├── 2026-04-30T05_overnight_midtier_partial/
│       │   ├── data.csv              ← raw bench rows (one per (dataset×params×trial))
│       │   ├── manifest.json          ← what this sweep was about
│       │   ├── env.json               ← machine/compiler fingerprint
│       │   └── log.txt (optional)     ← stdout of the orchestrator
│       └── 2026-04-30T18_25_custom_smoke/
│           └── ...
├── e2_query/runs/...
├── e3_compute/runs/...
├── e4_scale/runs/...
├── e5_memory/runs/...
├── e6_cache/runs/...
└── figures/                           ← rendered PDFs (overwritten freely)
```

## Three rules

1. **Append-only.** Once a run finishes, never edit its `data.csv`. If you
   need to "re-run" a sweep, that produces a *new* run directory.
2. **Per-run isolation.** Every invocation of `scripts/run_all.py` creates a
   fresh `runs/<label>/` directory. Different invocations cannot collide.
3. **Provenance is mandatory.** Every run has a `manifest.json` recording the
   command, datasets, params, git SHA, and start/end timestamps.

## Run labels

Auto-generated label format: `<UTC_TIMESTAMP>_<TIER>_<NOTE>`

Examples:
- `2026-04-30T05_overnight_midtier_partial`
- `2026-04-30T18_25_custom_smoke`
- `2026-04-30T22_00_full_recover_smalltier_full_algos`

Tier values: `tiny`, `small`, `mid`, `full`, `huge`, `custom`, `default`.

## Reading data

Use `scripts/aggregate.py` instead of touching individual CSVs directly:

```bash
# List runs available for an experiment
python3 scripts/aggregate.py e3_compute --list

# Concatenate all runs into one tall DataFrame
python3 scripts/aggregate.py e3_compute --out merged.csv

# Filter by label substring
python3 scripts/aggregate.py e3_compute --filter midtier --out mid_only.csv

# Skip pre-refactor legacy runs
python3 scripts/aggregate.py e3_compute --no-legacy --out fresh_only.csv
```

Each row in the aggregated DataFrame has extra columns added: `run_label`,
`run_tier`, `run_started`. You can filter on those in pandas.

## Plotting

`scripts/plot_all.py` reads via the aggregator. By default it merges all runs
including legacy. To plot only specific runs:

```bash
# Edit plot_all.py functions to call load("eX", run_filter="...")
```

## Manifest schema (`manifest.json`)

Keys present in every manifest:

| key            | type    | meaning |
|----------------|---------|---------|
| `experiment`   | string  | one of `e1_ingest`/.../`e6_cache` |
| `label`        | string  | unique identifier for this run |
| `tier`         | string  | size-bucket of dataset list |
| `started_utc`  | RFC3339 | when orchestrator launched |
| `ended_utc`    | RFC3339 | when orchestrator finished (or `null` if crashed) |
| `command`      | string  | full argv |
| `git_sha`      | string  | repo HEAD at run time |
| `datasets`     | array   | dataset names included |
| `threads`      | int     | thread count argument |
| `trials`       | int     | trials per data point |
| `timeout`      | int     | per-bench timeout (sec) |
| `note`         | string  | free-form description |
| `status`       | string  | `ok` / `crashed` / `partial` |

Per-experiment extras: `algos`, `systems`, `batches`, `queries`, `samples`,
`thread_grid`, `vidx_modes`, `phases`, `huge_pages`, `perf_events`.

## Pre-refactor data

Runs whose label contains `legacy_` were migrated from the old flat layout
(see `scripts/migrate_to_runs.py`). Their `started_utc` is approximate.

## Adding a new experiment

1. Add a `run_eN(args)` function in `scripts/run_all.py` modelled on
   the existing ones. Use `make_run_dir(...)` to create the per-run dir
   and `finalize_manifest(...)` in a `finally` block.
2. Never call `reset_csv` or write to a fixed path under `results/<exp>/`.
3. The benchmark binary's `--output` flag must point at
   `runs/<label>/data.csv` (returned by `make_run_dir`).
