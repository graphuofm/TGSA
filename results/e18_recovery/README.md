# E18: durability, crash injection, recovery replay

Produced by `benchmark/bench_e18_recovery.cpp`. Four modes, four files.

| file | mode | question |
|---|---|---|
| `crash_*.csv` | `--mode crash` | Kill a live writer with SIGKILL. Does replay recover a prefix that is query-equivalent to a reference engine built from exactly those edges? |
| `torn.csv` | `--mode torn` | Truncate the log at a random byte, or flip one byte inside a frame. Does replay stop at the last intact frame and never ingest garbage? |
| `curve.csv` | `--mode curve` | How does replay time scale with log size? This is Theorem 6 ("recovery is re-ingestion") as a number. |
| `cost.csv` | `--mode cost` | What does durability cost on the ingest path, from no log through fsync per batch? |

## Why there are two crash modes

A `SIGKILL` does not lose page cache. Every byte the process handed to `writev`
is still in the file after the process dies, so the crash mode almost never
produces a torn frame: it tests that the *engine* state and the *log* state
agree, which is worth testing, but it does not exercise the length and CRC
checks. Power loss is what truncates a log mid-frame, and the way to test that
path is to inject it. The `torn` mode therefore writes a clean log, then either
truncates it at a uniformly random byte or flips one bit inside a frame, and
checks the recovered prefix against an independently computed frame table.

The contract under test is not "nothing is lost". It is "what is lost is
detected": replay stops at the last intact frame and never ingests a partial or
corrupt batch.

## Two measurement artifacts that were found and fixed

Both produced plausible-looking numbers that were wrong. They are recorded here
because the fix is not obvious from the output.

**1. Forking the writer instead of exec'ing it.** The first version of the crash
mode `fork()`ed the harness. After the parent had built one recovery engine, the
child inherited its page tables and paid copy-on-write faults on every ingest,
which throttled it to a fraction of its real rate. The symptom was that trial 0
recovered 29 log frames and trials 1 through 29 recovered exactly 1 each,
regardless of whether the kill came at 60 ms or 2.4 s. Every trial still passed
its correctness checks, so nothing failed; the durable-throughput figure was
simply meaningless. The writer is now a freshly `execv`'d process, and recovered
volume scales linearly with the kill delay as it should.

**2. Configuring an engine from the corpus when only a prefix is replayed.** The
curve mode replays prefixes of a 1.05-billion-edge corpus. It originally sized
the time mapper from the whole corpus's `(t_min, t_max)`, so a 16-million-edge
prefix, which occupies a short time range, landed in a handful of windows and
the run timed overflow-chain walking rather than recovery. Replay of 16M edges
read 39.98 s (0.40 Meps); configured from the prefix's own range it reads 3.37 s
(4.75 Meps), a factor of 12. Every mode now calls `scan_meta` on the prefix it
actually uses.

The general shape of both: the harness, not the system, was the slow part, and
in both cases the correctness checks stayed green while the performance number
was nonsense. A throughput number that does not change when the experiment's
duration changes is the tell.

## Reading the cost table

`write_only` is there for one reason. GraphOne's durable log path calls
`write()` and leaves `fsync()` commented out (`baselines/graphone/src/sgraph.h`),
which is consistent with its own statement that an unplanned shutdown may lose
recent data and that recovery depends on an upstream backup such as Kafka. Its
"Durable Graph Time" therefore means "reached the page cache". Comparing our
fsync'd numbers against that would flatter us, so both contracts are measured.

## Update after the arena fix

The 256M-edge point of an earlier `curve.csv` read 578.97 s (0.44 Meps) and was
measured against an engine that was silently dropping edges; it is not a
property of replay. With the CAS-loser leak fixed (see the commit "Fix silent
edge loss at scale") replay is flat in log size all the way out:

| edges | log GB | replay s | replay Meps |
|---|---|---|---|
| 1M | 0.02 | 0.06 | 16.20 |
| 4M | 0.08 | 0.17 | 22.96 |
| 16M | 0.32 | 0.49 | 32.87 |
| 64M | 1.28 | 1.80 | 35.54 |
| 256M | 5.12 | 7.11 | 36.02 |
| 1.024B | 20.48 | 28.78 | 35.58 |

The full 1.024-billion-edge LANL corpus recovers in 28.78 seconds. A flat rate
is what "recovery is re-ingestion" should look like when the re-ingestion path
is genuinely O(1) per edge; the earlier decline was the bug, not the theorem.

The cost table also reads differently now that ingestion is faster: the engine
absorbs 63.4 Meps with no log and 30.3 Meps writing a log it never fsyncs, but
only 2.8 to 6.7 Meps with real fsync. Durable ingest is bounded by fsync, not by
the data structure. That is the right frame for the GraphOne comparison, whose
durable path never calls fsync at all: its number belongs beside `write_only`,
not beside `sync_4MB`.
