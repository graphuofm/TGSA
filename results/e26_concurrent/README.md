# E26: concurrent throughput head-to-head, one protocol, three systems

E19/E20 compared phase by phase; a streaming store's real job is both at
once. One protocol on all three systems (bench_e26_concurrent.cpp; GraphOne
runs it as job 29 in its own harness, baselines_patches/graphone/):

1. preload the first half of the corpus (arrival order);
2. W writer threads stream the second half through each store's own
   concurrent write path at full speed;
3. R reader threads answer windowed-neighborhood queries continuously until
   the writers finish. Every window closes before the earliest streamed
   timestamp (settled), so every answer has one correct checksum, computed
   on a quiescent reference engine and checked per query. **Every recorded
   row has mismatches = 0.**

The comparison axis is throughput on both sides (sustained ingest Meps and
queries/s during the stream), which is the claim under test. Mixes
8:1, 8:4, 4:4, 2:8 on four corpora; `e26.csv` is the source of truth.

## Headline (8 writers : 4 readers)

| corpus | TGSR Meps | GraphOne | LiveGraph sharded | TGSR q/s | GraphOne q/s | LG q/s |
|---|---|---|---|---|---|---|
| askubuntu (0.96M) | 34.3 | 17.9 | 6.2 | 1,553,712 | 393,758 | 493,859 |
| wiki-talk (7.8M) | 61.9 | 15.5 | 9.5 | 182,815 | 61,489 | 46,413 |
| cit-Patents (16.5M) | 26.4 | 18.1 | 4.0 | 16,029,521 | 10,109 | 6,150,192 |
| tc-cadets (22.5M) | 74.0 | 19.2 | 6.9 | 1,300 | 3,287 | 633 |

TGSR sustains 1.5-4x GraphOne's and 4-11x LiveGraph's ingest under the
same reader pressure, while answering more queries per second on three of
four corpora. On cit-Patents the reader gap is three orders of magnitude
(TGSR binary-searches the sorted preload prefix; GraphOne scans whole
neighborhoods). tc-cadets is the reverse case for readers, reported as
found: settled windows there deliver ~13K edges each and TGSR answers them
UNCOMPACTED (streaming appends to the same hot vertices keep clearing the
sorted flag), so its per-query scan is the E19 uncompacted regime while
GraphOne's archived chains are equivalent-and-warmer. Ingest is still 3.9x.

## LiveGraph: two write drivings, both recorded

- **sharded** (its best case): the collector repartitions the stream by
  source vertex so no two writers can conflict. Zero aborts everywhere.
  This resharding step is a requirement TGSR does not have.
- **arrival order** (what the stream is): vertex-locked MVCC collapses.
  askubuntu, 1K-edge transactions: 1.75 Meps at one writer, 0.05 at two,
  0.010 at eight (209K aborts). Recorded on the smallest corpus only; the
  collapse does not need four corpora. An earlier 64K-edge-transaction
  harness was worse still (0.0006 Meps) and was OUR fault; 1K transactions
  with per-chunk retry is the fair driving, and it still collapses,
  because consecutive arrivals share hot sources.

## GraphOne notes

- Rate reported is log-absorbed (its durability contract: write() with no
  fsync, matching E18's write_only rung); the asynchronous archive
  catch-up after the stream is in `archive_catchup_sec` (up to 0.55 s on
  cit-Patents 2:8).
- Its ingest is flat at ~15-19 Meps regardless of writer count on every
  corpus: the archive pipeline, not the log, is the bound.
- **STALE-view observation**: with STALE_MASK static views (the cheap
  reader), settled queries returned wrong answers while the archiver
  extended hot delta chains (tc-cadets 8:4: 150 of 3,276 queries; 4:4: 21).
  All recorded rows therefore use consistent (non-STALE) views, which cost
  about the same here. The observation stands: GraphOne's stale-read path
  is not safe under concurrent archiving.

## TGSR notes

- Readers verify against the reference on every query; writers are W
  independent callers of the ordinary insert path (real multi-writer
  contention, not one caller fanning out).
- tc-cadets q_median is 658 us for the reason above (heavy settled
  windows, uncompacted state); cit-Patents q_median is 0.24 us.
