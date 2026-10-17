# E33: the aligned comparison matrix (spec shared by every harness)

Purpose: one dataset set, one query set, every system on every cell, so that
each panel of the evaluation compares the same question on the same data.
Every number in the SIGMOD paper's §7 comparison figures comes from this
matrix. Where a system cannot express a query, the cell says so and why.

## Corpora (large to small), binaries in /home/jding/dataset/bin/<name>.bin
| short     | file                | events   | V      |
|-----------|---------------------|----------|--------|
| LANL      | lanl-auth.bin       | 1.05B    | 18K    |
| THEIA     | tc-theia-e3.bin     | 97.3M    | 6.9M   |
| CADETS    | tc-cadets-e3.bin    | 22.5M    | 2.9M   |
| wiki-talk | wiki-talk-temporal.bin | 7.8M  | 1.1M   |
The .bin is an array of packed 20-byte `barry::Edge {u32 src; u32 dst; u64 timestamp; f32 weight}`
(see include/barry/edge.hpp), loaded with `barry::bench::EdgeFile` (benchmark/common.hpp).

## Systems and their ingest path (the system's own fastest documented path)
| system     | ingest path                                                        | threads col |
|------------|--------------------------------------------------------------------|-------------|
| tgsr       | Barry::insert_batch, batch 65536, 8 threads; finalize() timed separately as phase `compact` | 8 |
| livegraph  | begin_batch_loader per 65536-edge batch, put_edge(force_insert=true), ts as 8-byte property (as bench_e19) | 1 |
| rocksdb    | WriteBatch per 65536, WAL off, key = ts:src:dst:seq big-endian 20 B, IncreaseParallelism(8), Flush() inside the timed region (as bench_e24) | 8 |
| duckdb     | appender into edges(src UINTEGER, dst UINTEGER, ts UBIGINT); then CREATE INDEX idx_ts ON edges(ts) and CREATE INDEX idx_src_ts ON edges(src, ts); indexes inside the timed region. DuckDB 1.4 plans every query shape as a parallel sequential scan with zone-map pruning even with index_scan_percentage raised to 1.0; recorded as measured | 8 |
| graphone   | job 20 (weighted 24-byte edge, ts in the weight slot), -t 8; Batch Update = ingest, Make Graph = ingest+archive (scripts/run_graphone.py) | 8 |
| stinger    | bench_e31_stinger, 8 threads (insert-or-update semantics; one edge per pair) | 8 |
| sortledton | bench_e32_sortledton, 8 threads (dedup upstream; one edge per pair) | 8 |

## Query set. Window width w = max(1, (t_max - t_min) / 100). All bounds inclusive.
Samples: N = 1000 edge-uniform draws, identical for every system:
  rng = std::mt19937_64(0xE33ull); for i in 0..N-1: e = uniform_int_distribution<size_t>(0, n-1)(rng);
  v_i = edges[e].src; lo_i = edges[e].timestamp; hi_i = lo_i + w.
The tgsr run writes the list to results/e33_matrix/<short>.queries as N records of
three little-endian u64 {v, lo, hi} (same format bench_e19 --dump_queries writes).

| phase | question | per-query inputs | output and checksum |
|-------|----------|------------------|---------------------|
| q1 | window scan: every event with lo <= ts <= hi | one query: lo = t_max - w, hi = t_max | count; chk = sum over events of (ts ^ ((u64)dst << 32) ^ (u64)src) |
| q2 | windowed neighbors: out-events of v with lo <= ts <= hi | (v_i, lo_i, hi_i) | delivered = total events over all queries; chk = sum of (ts ^ (u64)dst) |
| q3 | time-respecting reachability from v inside [lo, hi]: earliest-arrival label-correcting search. a(v) = lo. Worklist starts with v. Pop u; for every out-event (u -> d, ts) with a(u) <= ts <= hi: if d has no label or ts < a(d) then a(d) = ts and push d. Result: the set of reached d != v with a(d). The fixpoint is unique, so visit order is irrelevant. | (v_i, lo_i, hi_i) | reached = total reached vertices over all queries; chk = sum of ((u64)d ^ a(d)) |
| q4 | as-of neighbors: for each distinct dst among out-events of v with ts <= t, the maximum such ts | (v_i, t = hi_i) | pairs = total distinct dsts over all queries; chk = sum of (((u64)dst << 20) ^ maxts ^ (u64)v) |
| bfs | whole-graph BFS over current adjacency, time ignored, from root r = the vertex with the largest out-degree (lowest id on ties) | one query | chk = FNV-1a over (level+1) for v = 0..V-1 as in bench_e13 run_bfs; levels start at 0 for the root, -1 unreached |

Latency phases (q2, q3, q4): time each query; report median_us, p95_us, total seconds, queries completed.
Throughput phases (ingest, compact, q1, bfs): report seconds.

## Budgets and honesty
- Ingest budget 5400 s. If exceeded, stop, write the row with note=DNF and events = events done.
- Each query phase budget 1800 s by default; the revision queue passes 900 s for the baselines. Time the first 20 queries, project; run as many as fit, report
  samples_done of samples_total. Medians over a random subset are still medians.
- RSS guard: abort a phase with note=rss-guard if /proc/self/statm resident exceeds 150 GB.
- Checksums: the tgsr run writes results/e33_matrix/<short>.ref with one line per phase:
  `phase count checksum`. Every other system's row carries checksum_ok = 1 if both match, 0 if not,
  -1 if the ref is absent. A mismatch is a bug in the harness or a semantic difference; it is
  recorded, never hidden.

## CSV: results/e33_matrix/e33.csv (append; one row per system x corpus x phase)
system,dataset,phase,threads,events,samples_total,samples_done,seconds,rate,median_us,p95_us,count,checksum,checksum_ok,rss_MB,note
- dataset is the .bin basename (lanl-auth, tc-theia-e3, tc-cadets-e3, wiki-talk-temporal).
- rate = events/seconds for ingest, queries/seconds for query phases, 0 otherwise.
- count = events delivered (q1, q2), vertices reached (q3), pairs (q4), 0 for ingest/compact/bfs.
- rss_MB = resident set after the phase minus resident set before the system was constructed.
- note is free text: DNF, rss-guard, n/a (reason), or empty.

## Not expressible (write the row with seconds=0 and note="n/a: <reason>")
- stinger, sortledton: q1..q4 (one edge per (src,dst); the event multiset cannot round-trip).
- graphone: queries run only on the archived adjacency after Make Graph (its documented read path).
