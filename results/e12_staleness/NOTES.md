# E12: Rebuild versus stream

_LANL, 1 051 430 459 auth events, 58.0 days, arrival rate 210 events/s._

## What is being compared

A time-sorted CSR beats Barry on traversal latency (E9: 2.7x at high degree), so the
question is not which queries faster but which can be queried against a live stream
at all. The sorted structure has one update path -- rebuild -- and during a rebuild
its answers are not stale, they are **wrong**: silently missing everything that
arrived since.

Protocol: split the trace at `split_frac`; build the CSR on the prefix only, standing
in for "the last rebuild finished this long ago"; let Barry ingest the whole thing
incrementally; ask both identical questions about the live window.

## Result (split_frac = 0.99, i.e. 10 514 305 events arrived since the rebuild)

| | |
|---|---|
| CSR full rebuild | **28.4 s** |
| Barry one-off compaction | **76.3 s** |
| Barry incremental ingest | 10 514 305 events in **3.21 s = 3.28 M events/s** |
| Queries returning an incomplete answer | **68 / 100** |
| Average answer loss | **33.2%** |
| Worst answer loss | **99.5%** |
| Red team events in the unindexed window | 0 / 749 |

The 33.2% figure matches the independent wiki-talk measurement (34.1%), on a dataset
three orders of magnitude larger.

## Two things this experiment got wrong first

1. **split_frac = 0.999 showed almost nothing** (0.17% average loss) and was briefly
   recorded as a negative result. That split leaves only 0.1% of the trace as "newly
   arrived", a window too small for any query to depend on. The effect is real; the
   first split was simply not measuring it.

2. **Barry's ingest rate read 11 241 events/s**, which would have made the whole
   streaming argument collapse. Cause was self-inflicted: `finalize()` had been wired
   into `insert_batch()`, and since it walks the entire index, every batch re-scanned
   all 1.04B refs -- turning O(batch) appends into O(index). Moving compaction off
   the insert path restored 3.28 M events/s, a 292x recovery.

## The uncomfortable number

**Barry's compaction (76.3 s) costs more than a full CSR rebuild (28.4 s).**
Compaction resolves a timestamp per ref by chasing a pointer into the Arena, so its
locality is poor; the CSR build reads the edge array sequentially and sorts. This is
optimisable -- resolve timestamps in Arena order rather than index order -- and is
not done.

It does not change the conclusion, because the two operations sit in different
places:

| | role of the rebuild/compaction |
|---|---|
| time-sorted CSR | **required for correctness** -- without it answers are wrong, and it is blind while running |
| Barry | **optional for speed** -- queries stay correct uncompacted, expansion just degrades to a linear scan |

The claim the paper can make is therefore narrow and true: *Barry never has to stop
being queryable.* It is not that Barry compacts faster.

## Honest framing of what Barry promises

Not "always appendable and always fast to query", but **"always appendable, and fast
to query after a compaction that can be scheduled whenever convenient"**. Those are
different promises and the paper must make the second one.

## Reproduce

```
./build/bench_e12_staleness --dataset /home/jding/dataset/bin/lanl-auth.bin \
    --redteam /home/jding/dataset/bin/lanl-auth.redteam.tsv \
    --split_frac 0.99 --num_queries 100 --window_frac 0.01 --batch 200000 \
    --threads 16 --trial 1 --output e12_lanl.csv
```

Note: an earlier CSV mixed two header schemas (the `barry_compact_sec` column was
added mid-experiment) and was deleted rather than parsed. Regenerate with the command
above.
