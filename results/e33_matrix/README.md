# E33: the aligned comparison matrix

Spec: docs/E33_SPEC.md. Harness: benchmark/bench_e33_matrix.cpp (TGSA, LiveGraph,
RocksDB, DuckDB), scripts/run_graphone.py jobs 20/28/34/35/36 (GraphOne),
bench_e31_stinger / bench_e32_sortledton (ingest only). Queue: scripts/run_revision.sh,
then scripts/run_post_queue.sh and run_post_queue2.sh. Logs: results/revision_logs/.

Files
- e33.csv            every (system, corpus, phase) row of the in-process systems and the
                     Stinger/Sortledton translations; schema in the spec
- e33_graphone.csv   GraphOne rows in the same schema (ingest = Batch Update, compact = Make Graph)
- graphone.csv       the GraphOne driver's native rows
- e31e32.csv         Stinger/Sortledton native rows
- <corpus>.queries   the 1000 shared queries (u64 v, lo, hi), identical for every system
- <corpus>.ref       ground truth per phase (count, checksum); for THEIA the independent
                     tools/e33_ref result (the first engine run had an exhausted vertex
                     index, kept as tc-theia-e3.ref.engine-exhausted)
- <corpus>.prefix    cumulative checksums after k queries, for budget-cut runs
- tgsr_ingest_trials/  TGSA ingestion, three trials, 64K-batch and one-batch protocols

Verification: every full-run baseline cell matches the reference checksum; budget-cut
cells match the k-query prefix; Stinger/Sortledton cannot express q1..q4 (one edge per pair).
Known honest losses: GraphOne is faster on THEIA Q2 (1.5x) and on Q4 for THEIA and LANL
(2.8x, 4.7x); GraphOne ties TGSA ingestion on THEIA under the 64K-batch protocol.
