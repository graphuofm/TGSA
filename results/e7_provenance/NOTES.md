# E7: Provenance traversal (P1 backward trace / P2 forward taint)

_First pilot run, 2026-08-21. Machine: same lab host as E1-E6._

New in this experiment:
- `VertexIndex` reverse (in-edge) index, opt-in via `GraphConfig::build_reverse_index`
- `Barry::backward_trace()` (P1) and `Barry::forward_taint()` (P2) in `src/algorithms/taint.cpp`
- Correctness suite `tests/test_taint.cpp` (6 tests incl. forward/backward duality and
  exact agreement with the dense `earliest_arrival` path)

## Finding 1 -- sparse output turns an O(V) query into an O(|result|) query

`earliest_arrival` allocates a dense `vector<uint64_t>(V)` on every call. The sparse
`forward_taint` returns only reached vertices. Same results (verified vertex-by-vertex
in `test_sparse_matches_dense_earliest_arrival`), median latency at window_frac=0.01:

| dataset | V | sparse (p2) | dense (p2d) | speedup |
|---|---|---|---|---|
| CollegeMsg | 1.9K | 0.7 us | 0.7 us | 1.0x |
| sx-mathoverflow | 24.8K | 0.5 us | 6.9 us | 14x |
| sx-askubuntu | 159K | 0.5 us | 54.8 us | 110x |
| wiki-talk-temporal | 1.14M | 0.5 us | 636.9 us | 1274x |
| cit-Patents | 3.77M | 1.3 us | 14314.4 us | 11011x |

This is almost certainly the cause of the Q3 latency pathology recorded in
NEXT_SESSION.md item 6 (median 1.8 ms - 449 s across datasets): the dense allocation
made every temporal-reachability query pay for the whole vertex space.

## Finding 2 -- dependency explosion is real and heavy-tailed

Forward taint from a single seed, sweeping the horizon as a fraction of the time span:

| window_frac | email-Eu-core | sx-askubuntu | wiki-talk |
|---|---|---|---|
| 0.001 | 0.15% | 0.0006% | 0.0001% |
| 0.01  | 4.57% | 0.0006% | 0.036% |
| 0.05  | 30.1% | 0.19%  | 0.65% |
| 0.2   | 45.4% | 2.82%  | 2.56% |
| 0.5   | 56.1% | 9.92%  | 15.6% |
| 1.0   | 56.9% | 17.1%  | 25.2% |

(percent of vertices tainted, mean over 60 random seeds)

The distribution is extremely skewed: at window_frac=1.0 on wiki-talk the MEDIAN query
is 6.4 us while p95 is 904 ms. Most seeds taint almost nothing; a few taint a quarter
of the graph. Report this as a distribution, never as a mean.

Motivation payload: whole-node rollback after a confirmed compromise would discard
17-57% of the system state. Version-granularity, availability-first recovery is not an
optimization, it is the only tractable option.

## Finding 3 -- reverse index cost

| dataset | vidx off/on (MB) | RSS off/on (MB) | RSS overhead |
|---|---|---|---|
| sx-askubuntu | 35.7 / 69.8 | 89.2 / 123.9 | +39% |
| wiki-talk-temporal | 213.3 / 513.5 | 531.9 / 831.1 | +56% |
| cit-Patents | 543.6 / 1227.3 | 1267.4 / 1999.9 | +58% |

vidx roughly doubles (each edge gets a second 16-byte EdgeRef). Opt-in, so P2-only
workloads do not pay it.

## Caveats

- SNAP social/citation graphs are a PROXY for provenance graphs, not the real thing.
  They validate the primitives; they do not validate the application claim.
- cit-Patents shows ~0% taint: it is a citation DAG where edges point to older work, so
  time-respecting forward paths are rare. It is a poor provenance proxy -- keep it for
  the scalability story only.
- `forward_taint` / `backward_trace` use `unordered_map` for visited state. An epoch-
  stamped dense array would cut constant factors; not yet done.
- No node-version model yet (P4). Currently a vertex is a single identity across all
  time.

## Reproduce

```
cd build && make -j8 bench_e7_provenance
./build/bench_e7_provenance --dataset data/bin/<ds>.bin --query <p1|p2|p2d> \
    --num_samples 100 --window_frac 0.01 --threads 8 --trial 0 --output out.csv
```
