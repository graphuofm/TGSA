# E13: The storage-read share of graph algorithms

_2026-09-10. Four storage layouts serve identical algorithm executions; only
adjacency delivery is timed. Correctness guard: per (algo, dataset), all engines
must produce identical result checksums -- every reported row passed._

## Why this experiment exists

E3 compared end-to-end algorithm times and Barry lost 5-15x on structural
workloads. An end-to-end time conflates the storage engine (adjacency delivery)
with the algorithm implementation (frontier logic, rank arrays). E13 separates
them: every engine fills the same scratch buffer under a timer; the algorithm
consumes it outside the timer. What is timed is the storage engine's job.

Engines: `csr` (entity-keyed, topology-only -- the structural optimum), `tcsr`
(entity-keyed, per-vertex time-sorted, cannot ingest), `ucsr` (entity-keyed,
arrival order with inline timestamps -- what an ingesting entity-first store
holds between rebuilds), `barry`.

## Finding 1 -- the structural concession is ~2x at the storage layer, not 5-15x

sx-mathoverflow pilot (storage_sec, single trial):

| algo | csr | tcsr | ucsr | barry | barry/csr |
|---|---|---|---|---|---|
| bfs | 1.5 ms | 2.4 ms | 2.5 ms | 3.1 ms | 2.1x |
| pr  | 5.7 ms | 5.8 ms | 6.1 ms | 9.7 ms | 1.7x |
| cc  | 3.5 ms | 3.5 ms | 3.6 ms | 6.8 ms | 1.9x |

The E3 losses were mostly algorithm-implementation (dense arrays, allocation
patterns), not layout. At the layer where a storage paper competes, the
structure-insensitivity cost is bounded around 2x -- paid once, for streaming.

## Finding 2 -- time-windowed traversal ordering flips with degree

twbfs (BFS restricted to a time window): at mean degree 20, `ucsr` beats barry
on pure contiguity (0.7 vs 1.3 ms); at degree 337 barry has caught it (0.7 vs
0.7 ms at wf=0.05; ahead at wf=0.005) because sorted-bucket pruning skips what
ucsr must scan. `tcsr` leads throughout and cannot ingest. LANL (degree 59,517)
run pending -- the expected completion of the crossover curve.

## Finding 3 -- payload resolution costs 4-5x, and we know why

`bfsw` demands (dst, weight) at every expansion. csr/tcsr store the weight
inline; barry resolves it through the Arena (pointer chase per edge):

| | csr | tcsr | barry |
|---|---|---|---|
| storage_sec | 1.7 ms | 1.6 ms | 7.3-9.3 ms |

Topology-only delivery costs barry ~2x; payload delivery ~4-5x. Inlining the
weight into EdgeRef would repeat the rejected timestamp-cache trade (+50% index
memory, see e9 NOTES). Accepted and reported, not hidden.

Method note: true SSSP is semantically empty on these corpora (SNAP weights
uniformly 1.0 -> Dijkstra degenerates to BFS; LANL weights are event-class
codes, not costs). bfsw isolates the only storage-relevant difference -- weight
delivery -- without pretending shortest-path semantics.

## Reproduce

```
./build/bench_e13_storage_read --dataset data/bin/<ds>.bin --algo all \
    --window_frac 0.05 --trials 3 --threads 8 --output e13.csv
```

Full sweep CSV: `e13_full.csv` (5 datasets x 3 windows x 3 trials).
