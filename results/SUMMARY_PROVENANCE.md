# Barry on provenance workloads -- consolidated results

_Last updated 2026-09-10. Supersedes SUMMARY.md, which covers the earlier
structural-graph framing (BFS/PageRank/KCore vs DCSR) that this work moved away from._

## What is being claimed

Provenance graphs -- system audit streams and LLM-agent execution traces -- are a
workload existing storage engines are systematically mismatched to: the query axis
is time and causality rather than vertex id, arrival is continuous and small-batch,
and the data must be retained for months while almost never being read. The field's
current answer to the resulting storage pressure is to throw data away. This work
argues the layout should change instead, and measures what each alternative costs.

## Datasets

| dataset | scale | ground truth | role |
|---|---|---|---|
| **LANL cyber1** (Kent 2015) | 1 051 430 459 auth events, 17 666 hosts, 58.0 days, 0 out-of-order, avg degree 59 517 | **749 red team events, all resolved** | primary |
| DARPA TC E3 | CADETS/THEIA, CDM | attack report | pipeline built, download quota-blocked |
| SNAP temporal graphs | 60K-646M edges | none | proxies, scale |

Converters: `tools/lanl_to_bin.cpp`, `tools/tc_cdm_to_bin.cpp` (both C++, streaming).
Validation fixture: `tests/fixtures/cdm_attack_chain.json`.

## Results that hold

### E8 -- lossy reduction deletes attack evidence; the standard metric is blind to it

Across CPR reduction from 1x to 131x on LANL, node-level recall is **97.7% at every
single setting**, while attack-edge survival falls **100% -> 55.9%**. Forty-four
percent of the red team's evidence is deleted with no movement in the metric the
field uses to validate reduction. Random dropping is far worse (90% drop -> 8.6%
survival). Full table in `e8_attack/NOTES.md`.

This nearly went the other way: a pilot using node recall showed 8.4x reduction
costing nothing and would have concluded reduction is free.

### E8b -- there is no usable investigation window

| window | duration | tainted | % of net | recall | latency |
|---|---|---|---|---|---|
| 0.0001 | 8.4 min | 12 | 0.1% | 3.6% | 667 us |
| 0.001 | 1.4 h | 45 | 0.3% | 7.9% | 6.5 ms |
| 0.01 | 14 h | 95 | 0.5% | 10.2% | 72 ms |
| 0.05 | 2.9 d | 808 | 4.6% | 22.6% | 435 ms |
| 0.2 | 11.6 d | 2 774 | 15.7% | 88.9% | 2.55 s |
| 1.0 | 58 d | 5 121 | 29.0% | 97.7% | 17.1 s |

An acceptable horizon misses the attack; a horizon with useful recall implicates a
sixth of the network. Real APT dwell time is weeks, i.e. exactly where the answer
stops being actionable.

### E9 -- traversal versus filter, against DuckDB and a time-sorted CSR

Same process, same data, answers verified identical at every window.

- **Traversal**: Barry beats DuckDB **150-700x** across all result sizes (203x even
  at \|R\| > 1000, where DuckDB takes 2.43 s). Each hop costs a relational engine a
  full join; that is a data-model limit, not an engineering one.
- **Filter**: after vectorizing the range scan (AVX2 + interior-window bulk copy
  + header-only reserve prepass), Barry wins the median **14x** (71.5 us vs
  1.01 ms); DuckDB keeps the better tail (p95 1.50 ms vs 2.30 ms). Both reported.
- **vs time-sorted CSR**: Barry is **2.7x slower** at high degree, 8-11x at low
  degree. The CSR wins query latency and cannot ingest a stream at all.

### E11 -- what the speed makes affordable

All **749 real red team alerts** traced in **46.5 s** (median 24.6 us, p95 353 ms).
Roughly 10 minutes of one core for 10 000 alerts/day, against 6.7 hours for the
columnar path. The workflow changes from "trace what an analyst picked" to "trace
everything".

Backward tracing implicates **2 313 hosts on average per alert (13% of the
network)**. Both directions explode; speed is necessary, not sufficient.

### E13 -- the storage-read share of graph algorithms

Four layouts serve identical executions (checksum-verified); only adjacency
delivery is timed. Structure-only algorithms cost Barry ~2x vs the static
topology optimum (E3's end-to-end 5-15x was mostly algorithm implementation,
not layout). Payload delivery costs 4-5x (arena resolve; inlining rejected as
the +50%-memory trade). Time-windowed traversal ordering flips with degree:
ucsr wins at degree 20, parity at 337, LANL (59K) pending.

### Durability -- recovery is re-ingestion (Theorem 6)

WAL with CRC frames + torn-tail discard; asynchronous writer overlaps log I/O:
**4.2M events/s durable** (64MB group commit) vs 7.4M volatile -- disk-bandwidth
bound (156MB/s), not engine-bound. `flush_before(t)` tiers a cold time prefix
to a loadable segment and releases its pages, fsync'd before eviction. 3
persistence suites pass incl. crash simulation.

### Concurrent-read correctness -- claim upgraded to tested property

"Never stops being queryable" had an unverified hole: count-then-payload writes
could expose torn edges to concurrent readers. Fixed by per-slot release
publication (timestamp/block-pointer written last; sorted flag cleared before
count bump, making the binary-search path provably safe). Stress test: 1,395
concurrent read iterations, 0 violations; at-scale soak on wiki-talk: 46K
queries / 50.5M edges validated during 8-writer ingest, 0 violations, exact
final count. LANL-scale soak pending.

### E12 -- a static index does not return a stale answer, it returns a wrong one

Rebuilding is a sorted CSR's only update path. Queries issued against the
pre-rebuild snapshot lost **34.1% of the true answer on average, 96.7% at worst**
(wiki-talk, 150 queries). LANL-scale run not yet completed.

## Results that did not hold, kept for the record

- **E10 retention**: Barry evicts 5-29x faster than `DELETE WHERE ts < cutoff`, but
  the hypothesis behind the experiment -- that DuckDB would mark rows without
  reclaiming memory -- is **false**. DuckDB reclaims as much or more. A nightly
  retention pass costing 0.5 s instead of 0.018 s is not a pain point. Supporting
  material at best; the version that could carry an argument (eviction concurrent
  with queries, high-frequency eviction) is unmeasured.
- **EdgeRef timestamp cache**: +50% vertex-index memory for ~15% traversal time.
  Reverted.
- **Binary-search expansion**: worth 2.5x at avg degree 337, ~0 at low degree, and
  only 10% on LANL, where an unbounded horizon puts nearly every edge in range so
  there is nothing to skip. Kept -- it is free -- but it is not a headline.

## Known costs and limits

- LANL load: **255 s, 96 GB RSS** for 1.05B edges.
- Reverse index (needed for backward tracing) roughly doubles the vertex index:
  RSS +39% to +58%.
- Vertex index is ~half of RSS, which is in tension with the "zero-index" framing.
- Query latency loses to a static sorted CSR; the compensating claim is streaming
  ingestion, quantified by E12.
- Latency distributions on this workload are extremely heavy-tailed (median 24.6 us
  vs p95 353 ms on LANL). **Never report a mean or a small-sample median here.**

## Positioning

Each competitor fails a different requirement, measured:

| | streaming ingest | multi-hop temporal traversal | lossless |
|---|---|---|---|
| time-sorted CSR | no (E12: wrong answers) | yes, 2.7x faster | yes |
| columnar / LSM | yes | no (E9: 150-700x slower) | yes |
| reduction-based provenance systems | yes | yes | no (E8: 44% evidence lost) |
| **Barry** | yes | yes | yes |
