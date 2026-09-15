# E21: streaming behaviour

Produced by `benchmark/bench_e21_streaming.cpp`. A bulk-load throughput number
says nothing about a store that has to absorb a stream while queries run against
it, so each mode here asks a question that number cannot answer.

| file | mode | question |
|---|---|---|
| `concurrent.csv` | `--mode concurrent` | What do readers and writers cost each other, and does a settled time window keep returning the same answer while the stream continues? |
| `visibility.csv` | `--mode visibility` | How long after a write returns can a reader see the edge? |
| `steady_lanl_*.csv` | `--mode steady` | Does the ingest rate depend on how much has already been ingested? |
| `batchsize.csv` | `--mode batchsize` | Rate against arrival batch size and writer count, as a grid. |
| `disorder.csv` | `--mode disorder` | Rate and correctness when arrivals are shuffled, which is what a real collector delivers. |
| `retention.csv` | `--mode retention` | Unbounded stream, bounded history: does memory reach a steady state? |

## The settled-window invariant

The concurrent mode's correctness check is not "the answers look plausible". It
is: a query whose window closes strictly before the earliest edge the streaming
phase will deliver has an answer that is fixed before the phase starts, so any
later disagreement is a concurrency bug. Readers re-issue those queries
continuously while writers append, and compare against answers computed on a
quiescent engine.

Choosing those windows correctly matters. The first version drew queries from
the preloaded half of the corpus and reported 862 mismatches. The windows were
23 days wide, so any window opening within 23 days of the preload/stream split
legitimately received new edges: the queries were wrong, not the engine. The
harness now requires `lo + window < t_split` where `t_split` is the minimum
timestamp of the streamed half.

## What these experiments found

The disorder sweep produced a result that made no sense on its face: shuffling
the arrival order made ingestion *faster* on tc-cadets, 6.13 against 7.55 Meps.
That only holds if time-ordered arrival is the pathological case, and it is.
Every writer thread lands in the same time window at the same instant, so one
block counter and one cache line serialise the engine. The batch-size grid
confirmed it directly: one writer beat eight by 2.7x on wiki-talk, and the
apparently fast small-batch numbers at higher thread counts were serial
execution in disguise, since a 256-edge batch is a single chunk under
`schedule(static, 1024)`.

The fix follows from the diagnosis. Consecutive arrivals nearly always share a
window, so `insert_batch` now claims a whole run of same-window edges with one
counter bump. On wiki-talk the best configuration went from 14.5 Meps (one
thread, because eight were worse) to 52.8 Meps (eight threads).

The trade is real and is recorded rather than buried: a fully shuffled arrival
order produces runs of length one and falls back to the per-edge path, which is
marginally slower than before the change. In-order arrival, which is what a
collector delivers, is roughly three times better. Correctness is unchanged at
every shuffle window: the final state is identical to the in-order build.

The steady mode is what exposed the arena leak described in
`results/e18_recovery/README.md`: ingestion was flat to 144M edges and then
collapsed while resident memory stopped growing, which is the signature of a
store that is dropping data rather than slowing down.

## Transparent huge pages: a measured negative

`--thp 1` requests `MADV_HUGEPAGE` on the dense arenas. It was added while
chasing the scale collapse and kept only because the measurement is worth
having. On 240M LANL edges it costs 28% of ingest throughput (2.81 against 3.89
Meps) and 69% more resident memory (15.9 against 9.4 GB). The default is off.
The timeline is never included regardless, because its sparsity is the space
bound in Theorem 2 and 2 MB pages would make one touched window dirty 2 MB
instead of 4 KB.
