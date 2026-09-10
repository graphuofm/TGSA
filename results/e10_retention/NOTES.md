# E10: Retention / lifecycle cost -- a weaker leg than expected

_2026-08-21. Barry `evict_before()` vs DuckDB `DELETE WHERE ts < cutoff`._

## The idea

Retention is a monotone time predicate, the predicate class Barry's address mapping
makes free: dropping "everything before T" releases a prefix of the timeline without
scanning the edge set. New engine support was added for this
(`Barry::evict_before()`, `src/retention.cpp`), which releases physical pages with
MADV_DONTNEED after merging block ranges into whole pages.

## Results

| graph | drop | Barry time | Barry RSS | DuckDB time | DuckDB RSS | speedup |
|---|---|---|---|---|---|---|
| wiki-talk | 50% | 1.1 ms | 544 -> 537 MB | 5.8 ms | 809 -> 612 MB | 5x |
| wiki-talk | 90% | 15.3 ms | 543 -> 420 MB | 192 ms | 691 -> 525 MB | 13x |
| cit-Patents | 50% | 12.6 ms | 1280 -> 1097 MB | 306 ms | 1637 -> 1373 MB | 24x |
| cit-Patents | 90% | 18.0 ms | 1282 -> 951 MB | 527 ms | 1491 -> 1030 MB | 29x |

## Honest assessment: do not headline this

Two things went against the hypothesis.

1. **The predicted failure mode did not occur.** The expectation was that DuckDB's
   DELETE would only mark rows and leave memory occupied. It does not -- DuckDB
   reclaims as much as Barry does, sometimes more (-461 MB vs -331 MB on cit-Patents
   at 90%). The "memory does not come back" argument is simply false and must not
   appear in the paper.

2. **The remaining win is speed on an infrequent operation.** 5-29x is real, but a
   daily retention pass costing 0.5 s instead of 0.018 s is not a pain point anyone
   has.

## Where the lifecycle argument could still be real

The framing that survives is not batch retention but *continuous, fine-grained*
lifecycle: an agent-era store where most state is speculative and short-lived while
a small fraction must persist, so eviction runs constantly rather than nightly. Two
things would have to be measured to make that claim, and neither is done:

- **Eviction concurrent with queries.** A timeline-prefix release should not block
  readers on other time ranges, whereas a table-wide DELETE should. This is the
  structural difference, and it is untested.
- **Eviction at high frequency**, where a 5-29x per-operation gap compounds.

Until those exist, E10 is a supporting result at best: it demonstrates the
capability and costs little to keep, but it does not carry an argument.
