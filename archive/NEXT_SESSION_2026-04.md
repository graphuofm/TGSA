# Next-session handoff (resume after sleep)

_Started overnight sweep at 2026-04-30 06:40 UTC._

## What's running right now

| Job | PID/log | What it does |
|---|---|---|
| **`overnight.sh`** | `logs/overnight_main.log`, log dir `logs/overnight_20260430_064025/` | 6 phases of experiments on small + mid tier datasets |
| **`try_aeong.sh`** | `logs/try_aeong_main.log` | best-effort AeonG/Memgraph build in conda env `mgbld` |
| **Neo4j 4.4** | port 7687, PID hard-coded in `baselines/neo4j/run/neo4j.pid` | still running, you can stop it with `baselines/neo4j/bin/neo4j stop` |

## Quick "what happened overnight" recipe

```bash
# 1. Are jobs still running or done?
pgrep -af "overnight.sh|try_aeong|run_all|bench_e"

# 2. Per-phase summary
for f in /home/jding/Barry/logs/overnight_20260430_064025/*.log; do
    echo "=== $f ==="
    tail -3 "$f"
done

# 3. Row counts by experiment
wc -l /home/jding/Barry/results/*/e*.csv

# 4. AeonG outcome
cat /home/jding/Barry/baselines/AeonG/BUILD_NOTES.md
```

## Phase order in `overnight.sh`

1. **E5 memory** on small + mid + reddit-1B  (cheap; baseline RSS for paper Table 2)
2. **E3 small-tier** with new algos (BFS+PR+TC+SSSP+CC+KCore) — re-runs after L0 changes
3. **E1 ingest** on mid-tier (cit-Patents 16M, wiki-talk 8M, sx-stackoverflow 63M, soc-LJ 69M)
4. **E2 temporal queries** on mid-tier (Q1..Q6, Q4 sampling fix is in)
5. **E3 mid-tier** Barry vs naive CSR
6. **BYO baselines** (DCSR / PPCSR / BYO_CSR) on mid-tier — output `results/e3_compute/byo_baselines_midtier.csv`

If a phase fails, `overnight.sh` logs `phase N exit: <code>` and continues to the next. Partial data is kept.

## Known data on disk

| File | What |
|---|---|
| `results/e1_ingest/e1.csv` | E1: Barry + Neo4j on small-tier |
| `results/e2_query/e2.csv`  | E2: Barry on small-tier; Neo4j on CollegeMsg only |
| `results/e3_compute/e3.csv` | E3: Barry + naive CSR + BYO_CSR + DCSR + PPCSR (small-tier) |
| `results/e3_compute/e3_v1.csv` | snapshot before L0 optimization (for ablation) |
| `results/e3_compute/byo_baselines.csv` | DCSR / PPCSR / BYO_CSR (small-tier only) |
| `results/e3_compute/byo_baselines_midtier.csv` | DCSR / PPCSR / BYO_CSR (mid-tier, written by Phase 6) |
| `results/e4_scale/e4.csv` | E4: Barry scalability on sx-stackoverflow |
| `results/e5_memory/e5.csv` | E5: Barry memory footprint |
| `results/e6_cache/e6.csv`  | E6: timing on 4KB pages (no huge pages — needs sudo) |

## Paper draft state

- `paper/main.tex` — full 9-section draft (~700 lines)
- `paper/refs.bib` — 28 citations
- Compile via Overleaf with article class (or drop in VLDB/SIGMOD class).
- **Numbers in the abstract / conclusion are from small-tier**. After overnight, update the headline numbers from `results/SUMMARY.md` regenerated via `scripts/plot_all.py` (just runs through the new data).

## Things to do tomorrow

In priority order:

1. **Regenerate `SUMMARY.md` and plots** with overnight data:
   ```bash
   bash -c '
   python3 /home/jding/Barry/scripts/plot_all.py
   # Re-run the SUMMARY.md generator embedded at end of `results/SUMMARY.md`
   '
   ```

2. **Update paper/main.tex Section 5** with:
   - Mid-tier numbers (cit-Patents 16M, sx-stackoverflow 63M)
   - Reddit-1B memory result
   - SSSP / CC / KCore comparisons
   - Q4 with new high-degree sampling

3. **Custom DCSR streaming-ingest driver**:
   The BYO harness only runs static algorithms. To get fair Barry vs DCSR on E1, we need to write a small C++ driver that:
   - Loads our `.bin` file
   - Calls DCSR's `insert_batch()` (the API is in `external/dcsr/include/dcsr1.0/partition.h`)
   - Times batch ingestion at the same batch sizes as `bench_e1_ingest`
   Estimated 3-4h. This is the highest-ROI experimental work remaining.

4. **Write run_e1_dcsr.cc** in `baselines/BYO/benchmarks/run_structures/` patterned on `run_dcsr.cc`. Add a Bazel target. `bazel build` it. Then E1 head-to-head is real.

5. **AeonG**: check `baselines/AeonG/BUILD_NOTES.md`. If it failed at Stage 2 or 3, document the precise error in the paper as "AeonG attempted; build infrastructure assumes root toolchain at `/opt/toolchain-vXYZ`. We were unable to bypass this on a shared evaluation host."

6. **Q3 (earliest_arrival) variance**: median was 1.8 ms—449 s across datasets. Investigate whether high-degree source bias is causing pathological cases.

## What we proved this session

- Storage-overhead ratio: Barry/our_CSR ≈ DCSR/parlay_CSR (both ~1.0-2.5×). **Storage design is competitive.**
- Temporal queries: Barry beats Neo4j by **414,903×** on Q1 (point-in-time). PMA-based stores can't run this query natively.
- L0 ablation: 3-5× internal BFS speedup, **zero** memory cost. Signature-verified across all datasets.
- Ingestion: Barry **561-1650×** faster than Neo4j on small datasets. (Vs DCSR: paper claims 10M ops/s; we hit 13-16M on similar hardware. Same-machine comparison still owed.)
- Infrastructure: DCSR/PPCSR/BYO_CSR all built via Miniconda gcc-11+Bazel without sudo. `barry_bin_to_gbbs` converter works.

## Server state when you log back in

- Neo4j is running on port 7687 (`baselines/neo4j/`); stop with `baselines/neo4j/bin/neo4j stop` if you want the RAM back.
- Conda env `bld` has gcc-11 + Bazel 6 (used by DCSR/PPCSR build).
- Conda env `mgbld` (created by try_aeong.sh) has gcc-12 + memgraph deps, _if_ the AeonG attempt got that far.
- BYO build cache is in `~/.cache/bazel/`; it's about 5 GB. Re-build is fast from cache.
