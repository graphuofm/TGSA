# E31/E32: Stinger and Sortledton (completing the streaming category)

Both systems key edges on (src,dst): a repeated pair is an in-place
update, not a new event, so an event multiset cannot round-trip through
either and windowed-delivery comparisons would compare different
questions. TVA compares against them on current-graph operations only;
so do we. Axis: events processed per second through each system's own
concurrent write path (8 threads), delta RSS, and the final unique
directed pair count verified against ground truth (every row: ok).

| corpus | events | unique pairs | Stinger Meps | Sortledton Mpairs/s | TGSR Meps (E16, multiset) |
|---|---|---|---|---|---|
| askubuntu | 0.96M | 0.60M | 3.12 | 2.62 | 30.1 |
| wiki-talk | 7.8M | 3.3M | **0.29** | 2.02 | 66.9 |
| cit-Patents | 16.5M | 16.5M | 5.02 | 3.57 | 24.5 |
| tc-cadets | 22.5M | 4.0M | **0.02** (913 s) | 1.17 | 80.1 |
| LDBC-SNB SF10 | 65.7M | 65.7M | 0.19 (345 s, 5.7 GB) | 3.23 | 58.1 |

Findings, honestly attributed:

1. **The (src,dst)-keyed update cliff.** Stinger is credible where pair
   multiplicity is ~1 (cit-Patents 5.0 Meps) and collapses three orders
   of magnitude where the stream repeats pairs (wiki-talk 0.29, CADETS
   0.02 Meps): each repeated event must first FIND the edge it updates
   inside its neighborhood. This is the thesis in one number: a
   structural-era edge model meeting an event stream.
2. **Sortledton** holds 1.2-3.6 Mpairs/s throughout (its MVCC per-edge
   transactions bound it), 7-68x behind TGSR on the same streams.
   Driving notes: its plain insert throws on existing edges and its
   insert_or_update path corrupted the heap under repeated pairs, so
   pairs are deduplicated upstream (the shape its own evaluation uses)
   and the dedup cost is charged to the collector, not to Sortledton.
3. Stinger's edge-block pool must be sized from the pair count, not its
   nv-based default (LDBC SF10 exhausted the default pool); recorded in
   the harness. Even with the pool sized and multiplicity exactly 1,
   Stinger runs LDBC SF10 at 0.19 Meps (345 s, 5.7 GB): hub
   neighborhoods (large forums) make even first-insert edge-existence
   searches long. TGSR ingests the same stream at 58.1 Meps.

Harnesses: benchmark/bench_e31_stinger.cpp, bench_e32_sortledton.cpp.
Semantics caveats live in the harness headers and in 7.2's prose.
