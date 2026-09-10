# Barry

A temporal-first storage engine for provenance graphs: timestamps map to virtual
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
| `tools/` | dataset converters: LANL (`lanl_to_bin`), DARPA TC CDM (`tc_cdm_to_bin`), provenance query CLI |
| `benchmark/` | E1–E13 drivers (see below) |
| `tests/` | correctness suites incl. the synthetic attack-chain fixture |
| `results/` | per-experiment CSVs + `NOTES.md` (method, findings, negative results) |

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

Each `results/<exp>/NOTES.md` records protocol, headline numbers, and the
negative results kept for the record.

## Datasets

Converters ingest the LANL *Comprehensive Multi-Source Cyber-Security Events*
corpus (public, CC0; red-team ground truth) and DARPA Transparent Computing E3
CDM streams into the packed 20-byte edge format. See `data/SECURITY_DATASETS.md`.
