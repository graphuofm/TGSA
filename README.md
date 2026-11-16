# TGSA (formerly Barry)

A time-keyed storage engine for provenance graphs: timestamps map to virtual
addresses by a constant-time shift, demand paging absorbs sparsity, ingestion is
an O(1) lock-free append, and time-respecting traversal (forward taint, backward
causality tracing) runs in microseconds against a stream that never stops.

Formal analysis (complexity table, five theorems with proofs): [`docs/THEORY.md`](docs/THEORY.md).

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8
cd build && ctest          # 6 correctness suites
```

No mandatory dependencies beyond a C++17 compiler. `bench_e9_columnar` /
`bench_e10_retention` additionally need the DuckDB C API at `~/opt/duckdb`
(auto-detected; skipped when absent). AVX2 is used when the CPU has it.

## Layout

| | |
|---|---|
| `include/barry/`, `src/` | engine: time mapper, timeline, arena, vertex index, retention |
| `src/algorithms/` | BFS, PageRank, TC, BC, SSSP, CC, k-core, earliest-arrival, temporal triangles, forward/backward taint |
| `tools/` | dataset converters: LANL (`lanl_to_bin`), DARPA TC CDM (`tc_cdm_to_bin`), LDBC, ground-truth reference (`e33_ref`), provenance query CLI |
| `benchmark/` | experiment drivers E1 to E35 (see below); `reduction_policies.hpp` is shared by E8/E14 |
| `tests/` | correctness suites incl. the synthetic attack-chain fixture |
| `results/` | per-experiment CSVs + `NOTES.md` / `README.md` (method, findings, negative results) |
| `scripts/` | experiment queues, ground-truth resolution, figure regeneration, Overleaf flattening |
| `data/` | small tracked ground-truth files (CADETS, THEIA); bulky corpora live in `~/dataset/bin` |
| `paper_SIGMOD/` | SIGMOD 2027 submission (paper 2388): `main.tex`, figure sources, `submission/` holds the uploaded PDF and Overleaf zip (untracked) |
| `paper_USENIX/` | USENIX Security draft: skeleton and `PLAN.md` evidence map |
| `docs/` | theory, plans, writing rules, experiment specs; `docs/references/` holds reference PDFs (untracked) |
| `archive/` | superseded papers (early draft, FAST short paper) and April 2026 run logs |
| `baselines/` (untracked) | competitor builds; `baselines_patches/` holds our patches to them |

## Experiments

| # | Question | Driver |
|---|---|---|
| E7 | provenance traversal latency; dependency explosion | `bench_e7_provenance` |
| E8 | does lossy log reduction destroy attack evidence? | `bench_e8_attack` |
| E9 | traversal & filter vs DuckDB and a time-sorted CSR | `bench_e9_columnar` |
| E10 | retention cost (`evict_before`) | `bench_e10_retention` |
| E11 | sustained triage: trace every alert | `bench_e11_sweep` |
| E12 | rebuild-vs-stream: answers lost during a static rebuild | `bench_e12_staleness` |
| E13 | storage-read share of graph algorithms across 4 layouts | `bench_e13_storage_read` |
| E14 | attack-chain recall on DARPA TC (CADETS, THEIA) under reduction | `bench_e14_tc` |
| E18 | durability cost, crash/corruption injection, replay | `bench_e18_recovery` |
| E19-E32 | baselines (LiveGraph, GraphOne, DuckDB, RocksDB, Stinger, Sortledton), ablations, modules, sharding | see `results/` |
| E33 | aligned 7-system x 4-corpus x Q1..Q4 matrix (SIGMOD paper) | `bench_e33_matrix`, `scripts/run_e33.py` |
| E34 | lossless-short vs reduced-long under a storage budget | `bench_e34_budget` |
| E35 | Elasticsearch / Kafka baseline | `bench_e35_siem.py` |

Each `results/<exp>/NOTES.md` records protocol, headline numbers, and the
negative results kept for the record.

## Datasets

Converters ingest the LANL *Comprehensive Multi-Source Cyber-Security Events*
corpus (public, CC0; red-team ground truth) and DARPA Transparent Computing E3
CDM streams into the packed 20-byte edge format. See `data/SECURITY_DATASETS.md`.
