# E15: Head-to-head with TVA (PVLDB Vol 19, 2026)

_2026-09-10. TVA is the nearest neighbor in the literature -- the most recent
PVLDB temporal graph storage system -- and the standard this paper is measured
against, both in rigor (its complexity table / theorems / SIMD / artifact set
the bar our THEORY.md and artifact deliberately match) and now empirically._

## Building their artifact

The official repository (github.com/Sakuraaa0/TVA) does not build as shipped:
thirdparty submodules are empty directories, `unordered_dense` must be pinned
to v4.5.0 (the code names `ankerl::unordered_dense::v4_5_0`), gflags headers
need an explicit include path, and the CMake tree references six executables
and one library source path that do not exist in the repo. With those repaired
(deps fetched and pinned, missing targets disabled, `src` library path fixed),
their core library builds cleanly under gcc-11/c++20 and our harness links
against it. Repairs are scripted in `baselines/tva/` for reproducibility.

## Comparable surface (defined by TVA's own strengths)

Same process family, same datasets, same RNG seed, identical timestamp
normalization (their packed EdgeBody carries a 48-bit sequence number, so
nanosecond-scale CADETS timestamps are microsecond-normalized on BOTH sides).
Ops: (a) single-threaded ingestion via `put_edge_withTs`; (b) single-vertex
point-in-time snapshot scans via `get_edges_withTs`; (c) full-graph snapshot at
mid-time (the op their NextOffset version-skipping showcases); (d) peak RSS.

## Results

| engine | dataset | edges | ingest (1 thread) | point scan | full snapshot | RSS |
|---|---|---|---|---|---|---|
| barry | wiki-talk | 7.8M | **0.60 s (13.1 Meps)** | 0.2 us | 0.65 s | 0.66 GB |
| barry | CADETS | 22.5M | **1.45 s (15.5 Meps)** | 0.1 us | **217.9 s** | 1.8 GB |
| barry | THEIA | 97.3M | **8.16 s (11.9 Meps)** | 0.4 us | 45.8 s | 7.7 GB |
| tva | wiki-talk | 7.8M | DNF -- see below | -- | -- | >41 GB at cutoff |

TVA ingestion of wiki-talk's 7.8M events: **did not finish in 90 minutes**
(killed at 5,415 s wall, 41.6 GB RSS, 99.9% CPU throughout). Effective
ingestion rate is bounded above by 1,447 events/s against Barry's 13.1M on the
same data and thread count -- a >=9,000x gap -- with >300x the memory per raw
edge byte. The cutoff is generous: their own configuration guidance says
~40 GB DRAM, which the run had already exceeded when killed.

## Reading the result honestly

- **The gap is the data model, not engineering.** TVA stores repeated events on
  one (src,dst) pair as a version chain inside a hopscotch neighborhood with
  bounded-offset guarantees; event streams make version chains as long as pair
  multiplicity (wiki-talk: up to thousands), so maintaining that invariant per
  insert degenerates. Its design assumptions -- few, property-rich versions per
  entity -- are the versioned-entity workload it was built for, and its results
  on that workload are not in question here.
- **Barry's weak op is real and reported.** Full-graph as-of snapshot is
  enumerate-then-reduce for Barry (no native per-neighbor-latest operator):
  217.9 s on CADETS, where nginx-adjacent vertices carry millions of events.
  This is the op TVA's version-skipping is built for, and had its ingestion
  admitted the dataset it would very plausibly win the row.
- **Query surface fact (from their headers, not our claim):**
  `get_edges_withTs` returns one valid version per neighbor -- a snapshot.
  Time-respecting multi-hop traversal (P1/P2) is not expressible over the
  public interface without walking internal version chains.
- 48-bit sequence numbers cannot hold nanosecond epochs; any event-stream use
  needs the normalization both sides applied here.

## Reproduce

TVA side: `baselines/tva/experiment_barry/e15_tva.cc` (build via their tree).
Barry side: `benchmark/bench_e15_barry.cpp`. Same CLI: `<bin> [num_scans]`.
