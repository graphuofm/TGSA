# E23: ablation sweeps (TVA Fig. 11 parity)

One mechanism per sweep, identical reseeded workloads on both sides of every
switch, delivered results checked identical before any timing is compared.
CSVs in this directory are the source of truth.

## tau: the granularity bound, checked rather than assumed (`tau.csv`)

For each target occupancy tau the shift is selected from the real event count,
and the realized mean occupancy mu = E / W over the selected span is computed
by walking the timeline. Theorem 3 claims mu in [tau/2, 4·tau).

Result: **the bound holds at every tau on every corpus tested** (18/18 rows,
`bound_holds=1`). Ingest improves mildly with coarser windows (fewer block
installs); query cost is flat across two orders of magnitude of tau. The knob
is safe to leave at its default.

## Binary-search threshold (`threshold.csv`)

bs_min = 1 always binary-searches a sorted bucket; 10^9 never does.

Shape: **a wide plateau, then a cliff**. Any threshold from 1 to 256 is within
noise of any other; removing binary search entirely costs 2.8x on wiki-talk
(678 -> 1910 ms) and ~2.4x on tc-cadets. The mechanism is what matters, the
knob's exact value does not — the same shape TVA reports for its migration
threshold. Default stays 32.

## SIMD boundary scan (`simd.csv`): a measured negative, defaulted off

Four corpora x five window widths (1/8 slot to 64 slots), same engine data,
same queries, identical result sums both ways. **The gather-based AVX2 path
never won once.** Worst case 1.52x slower than scalar (tc-cadets, 1/8-slot
windows, 41.3 vs 27.1 ms); converges to parity at 64-slot widths where the
comparison-free interior copy dominates and the vector path idles.

Cause: `_mm256_i64gather_epi64` at a 20-byte stride is slower than scalar
loads on this microarchitecture, and a range scan only ever vector-filters its
~2 boundary slots. Where the path operates it loses; where it doesn't matter
it idles. `simd_scan` now defaults to false (the THP treatment: shipped off,
knob kept, numbers here). A shuffle-based reformulation over 160-byte groups
is the only plausible rescue and is not currently justified by the share of
query time boundary scans represent.

## Read/write mix (`../e21_streaming/rw_ratio.csv`)

Writers:readers from 8:1 to 1:8 on wiki-talk, settled-window mismatches zero
at every mix. The trade is smooth and monotone:

| W:R | ingest Meps | queries/s | read p99 (us) |
|-----|------------|-----------|----------------|
| 8:1 | 44.2 | 36,799 | 611 |
| 8:4 | 34.5 | 149,568 | 544 |
| 8:8 | 29.6 | 257,347 | 620 |
| 4:8 | 38.6 | 319,155 | 483 |
| 2:8 | 27.8 | 470,180 | 327 |
| 1:8 | 19.5 | 565,769 | 247 |

Read tail latency *improves* as writers thin out, as it should.

## Timestamp cache in EdgeRef (`notscache_e19.csv`, `notscache_e13.csv`)

Compile-time A/B: the shipping 24-byte ref against the 16-byte ablation build
(`build_notscache/`, all suites pass). Same corpora, same seeds, checksums
match LiveGraph in both builds.

Query side (ns per delivered edge, 16B -> 24B):

| corpus | uncompacted | compacted | RSS |
|---|---|---|---|
| wiki-talk | 118.3 -> 21.5 (5.5x) | 10.0 -> 3.5 (2.9x) | 373 -> 454 MB |
| tc-cadets | 404.6 -> 152.5 (2.7x) | 6.2 -> 3.0 (2.1x) | 963 -> 1168 MB |
| cit-Patents | 724.4 -> 336.6 (2.2x) | 546.5 -> 327.6 (1.7x) | 934 -> 1172 MB |

Structural side (adjacency delivery vs topology-only CSR, common
corpus/algorithm set): median concession 2.06x at 16B refs against 2.25x at
24B. The cached timestamp costs roughly nine percent on the axis that never
reads it and buys 2-5x on the axis the engine exists for. Ingest rate is
unchanged either way.
