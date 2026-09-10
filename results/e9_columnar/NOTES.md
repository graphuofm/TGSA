# E9: Barry vs DuckDB (columnar) on identical temporal graphs

_2026-08-21. Same process, same data, same machine. DuckDB v1.5.5 via the C API._

## Why this experiment exists

If a column store sorted by time answers these queries adequately, Barry has no
reason to exist. So both engines run in one process over the same edge array, and
both query classes are reported -- including the one DuckDB wins.

DuckDB is given its best shot: rows appended in timestamp order so zone maps prune
on `ts`, plus an ART index on `src` for the traversal join. Index build time is
charged to DuckDB's load, not hidden.

## Fairness: the queries return identical answers

The first run compared Barry-to-fixpoint against DuckDB capped at 4 hops. Result
sizes differed (13.4 vs 4.9) -- Barry was doing strictly more work and still
winning, which is not a comparison anyone should accept. With hops raised so both
run to fixpoint, result sizes match exactly at every window:

| window_frac | Barry \|R\| | DuckDB \|R\| |
|---|---|---|
| 0.01 | 1.4 | 1.4 |
| 0.05 | 301.1 | 301.1 |
| 0.20 | 2256.4 | 2256.4 |

Every number below is therefore two engines producing the same answer.

## Result 1 -- traversal: Barry wins by 150-700x at every result size

Multi-hop time-respecting traversal (forward taint to fixpoint), sx-mathoverflow,
120 queries across window_frac in {0.02, 0.1, 0.3}, binned by answer size because
the latency distribution is heavy-tailed and a mean would be sampling luck:

| \|R\| | n | Barry median | DuckDB median | speedup |
|---|---|---|---|---|
| 0-1 | 93 | 13.6 us | 7 285 us | 536x |
| 2-10 | 2 | 22.4 us | 16 181 us | 722x |
| 11-100 | 2 | 197 us | 28 881 us | 146x |
| 101-1000 | 3 | 1 597 us | 561 752 us | 352x |
| 1001+ | 20 | 12.0 ms | 2 432 ms | 203x |

The gap is not an artifact of small answers: at \|R\| > 1000 it is still 203x, with
DuckDB taking 2.4 seconds. Each hop costs DuckDB a full join against the edge
table; Barry's cost tracks the frontier.

## Result 2 -- filter: DuckDB is competitive and has the better tail

Time-range filter on wiki-talk-temporal (7.8M edges), identical result sets:

| system | median | p95 |
|---|---|---|
| Barry | 885.7 us | 6 696 us |
| DuckDB | 1 293.4 us | **1 568 us** |

Barry is 1.46x ahead on the median and clearly *behind* on p95. Sequential columnar
scan has the more predictable latency, and this is DuckDB's home turf.

**This result should stay in the paper.** The claim it licenses is precise: a
temporal-first layout buys little for time-range filtering, and buys two to three
orders of magnitude for multi-hop time-respecting traversal, where every hop is a
fresh lookup keyed on the previous hop's vertices and times. A paper that reported
only the traversal win would invite the reviewer to assume the filter case was
hidden.

## Distribution note

Of 120 traversal samples, 93 produced \|R\| <= 1 and 20 produced \|R\| > 1000, with
almost nothing between. Any summary of this workload must be a distribution or a
binned table; means and small-sample medians are actively misleading. An earlier
5-sample run at window_frac=0.05 on wiki-talk reported avg \|R\| = 1.0 purely
because it missed the tail, while a 60-sample run of the same configuration in E7
reported avg \|R\| = 7458.

## Reproduce

```
./build/bench_e9_columnar --dataset data/bin/<ds>.bin --query both \
    --num_samples 40 --window_frac 0.1 --hops 64 --duck_index 1 \
    --threads 8 --trial 0 --output e9.csv --detail e9_detail.csv
```

DuckDB lives in `~/opt/duckdb` (C API) and `~/bin/duckdb` (CLI), installed without
root. CMake probes for it and skips `bench_e9_columnar` when absent, so the normal
build stays dependency-free.


## Update 2026-09-10 -- filter tail closed; concurrency cost recovered

Post publication-protocol (concurrent-read correctness), the acquire loads cost
the filter path 2.3x. A block-publication watermark (finalize verifies a block's
slots and flags it; readers take a no-check bulk path; any append clears the
flag before bumping the count, same proven ordering as the bucket sorted flag)
recovered it: same-seed median 75.1 us vs 71.5 us pre-protocol.

Five-seed wiki-talk filter, current build:
  barry  median 77.1 us   p95 980.8 us  (range 917-1546)
  duckdb median 819.0 us  p95 1105.0 us (range 976-1516)

Median won 10.6x; p95 at parity with overlapping ranges -- the earlier "DuckDB
owns the tail 4x" finding no longer holds and the paper now reports the
progression instead.
