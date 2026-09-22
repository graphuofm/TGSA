# E30: the as-of snapshot row, revisited

E15 reported full-graph as-of snapshot (each neighbor's latest edge at
time t) as our honest loss: 217.9 s on CADETS via enumerate-then-reduce.
E30 adds a real operator (VertexIndex::latest_neighbors_into: filter refs
to ts <= t in place with sorted-bucket early exit, then flat sort-dedup,
no hash map, no materialized TemporalRef flood) and measures three paths
in one harness, all checksummed against ground truth computed from the
input arrays.

| corpus | old (E15 method, 1T) | new operator (1T) | new operator (8T) |
|---|---|---|---|
| wiki-talk | 0.607 s | 0.039 s (15.6x) | 0.0056 s (109x) |
| tc-cadets | 218.99 s | 0.442 s (496x) | 0.273 s (803x) |

All six rows checksum-identical to ground truth (2,009,624 pairs on
CADETS at mid-time).

Honest reading: the 217.9 s was almost entirely OUR enumerate-then-reduce
harness (per-event unordered_map churn on million-event neighborhoods),
not the layout. The operator still inspects O(d_le_t) references per
vertex where a version-grouped store like TVA inspects O(distinct
neighbors); that asymptotic gap remains and is stated, but on real
corpora the row moves from a 217.9 s concession to 0.27 s.

Bench: benchmark/bench_e30_asof.cpp. Operator shipped in the engine
(include/barry/vertex_index.hpp), covered by the existing test suite
build.
