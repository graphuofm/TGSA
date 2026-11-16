# E35: the SIEM default and the pipeline default as provenance stores

The question a security reviewer asks: why not keep the unreduced provenance log in
Elasticsearch (the SIEM default) or in Kafka (the pipeline default)? E35 runs both on the
E33 corpora, with the E33 query set, window width and reference checksums, so every
number here lines up with `results/e33_matrix/e33.csv`.

Harness: `benchmark/bench_e35_siem.py` (Python orchestrates; the data path is the systems
themselves) and `benchmark/e35_kafka/E35Producer.java`, `E35Consumer.java` (Kafka's own
Java client). Run: `python3 benchmark/bench_e35_siem.py --datasets tc-cadets-e3,wiki-talk-temporal`
(default phases, 1800 s latency budget, load gate 4). The async-durability ingest row was
added after the main run (`--phases es-ingest-async`).

Files
- `e35.csv`          one row per (system, corpus, phase), E33 schema
- `e35_extra.json`   index settings, store/log sizes, CPU use, ES-reported `took`, Q3 call counts
- `config/`          the exact configuration files used (below)
- `logs/`            the orchestrator log, including every load-gate wait

## Versions and setup

| | version | setup |
|---|---|---|
| Elasticsearch | 8.19.23 (build 6940e499, 2026-09-30), Lucene 9.12.2, bundled OpenJDK 27 | tarball, single node, security/ML/monitoring/geoip off, heap `-Xms8g -Xmx8g`, `path.data=/home/jding/dataset/es_data` (`config/elasticsearch.yml`, `config/es-heap.options`) |
| Kafka | 3.9.2 (Scala 2.13), KRaft combined broker+controller, metadata 3.9-IV0, OpenJDK 11.0.27 | tarball, `config/kraft/server.properties` with `log.dirs=/home/jding/dataset/kafka_data`, `num.partitions=8`, loopback listeners, `log.retention.hours=-1` (`config/kafka-e35-server.properties`); broker heap 4 GB |
| Machine | 20 cores, 188 GB RAM, rotational disk (`/dev/sda`) | shared with other experiments; every timed phase started only with the 1-min load below 4 |

Tarballs checked against the vendor SHA-512 files. Elasticsearch warns that
`vm.max_map_count` (65530) is below its production check (262144); there is no root to
raise it, and with a loopback-only bind the bootstrap checks are warnings. No mmap failure
occurred.

## Method

Elasticsearch. Mapping `src`, `dst` keyword; `ts` long (both corpora fit a signed 64-bit
value; long keeps the raw timestamp of both, ns for CADETS and seconds for wiki-talk, and
range filters on it are exact; `date_nanos` would store the same long for CADETS); `w` float; `dynamic: strict`; 4 primary
shards, 0 replicas; `refresh_interval=-1` during the bulk load. Ingest: 8 client processes,
each owning a contiguous eighth of the .bin, `_bulk` batches of 5,000 docs over a kept-alive
connection; timed from a barrier to the last acknowledged batch, zero rejected items.
`compact` = restore the default refresh interval, `_refresh`, `_flush`; then the primary
store size. Queries run from one client, sequentially, caches cleared before each phase:
- Q1: `range` on ts over the trailing 1% window. `q1_count` is `_count`; `q1` retrieves every
  hit (src, dst, ts as docvalue fields) through a point-in-time + `search_after` (10,000 hits
  per page; Elasticsearch recommends PIT + `search_after` over scroll for deep pagination).
- Q2: `bool.filter[term src, range ts]` per sampled (v, lo, hi): `q2` retrieves every hit
  through PIT + `search_after`; `q2_count` is `_count` only (no events delivered).
- Q3: not expressible as one query. Implemented client side: the E33 earliest-arrival
  search, one Q2 call per expanded vertex, popped in increasing arrival time.
- Q4: `composite` aggregation on `terms(dst)` (10,000 buckets per page) under the same
  filter with `ts <= t`. `q4` uses a `max(ts)` sub-aggregation; `q4_exact` uses
  `top_hits(size 1, sort ts desc)`.

Kafka. Topic with 8 partitions, replication 1, no retention limit. Producer: 8 threads,
one `KafkaProducer` each, null keys (default sticky partitioner), 20-byte binary values
(the raw record), `acks=1`, every other setting at its default (`linger.ms=0`,
`batch.size=16384`, compression none); idempotence is turned off because it needs
`acks=all`. `ingest_tuned` repeats with `linger.ms=50`, `batch.size=1 MiB`. Log size =
bytes of the `.log` segment files. Queries: Kafka has no index, so a query is a consume of
the whole topic (8 consumer threads, one per partition, manual assignment, from offset 0 to
the end offsets taken at start) with the filter applied client side. `q1` filters the
window; `q2` answers all 1,000 queries in one consume (hash of v); `q2_single` is one query
on its own, the honest per-query cost.

Verification. All 1,000 E33 queries (`results/e33_matrix/<corpus>.queries`) are used. Every
count and checksum is checked against `results/e33_matrix/<corpus>.ref`, or against the
k-query prefix (`.prefix`) for a budget-cut phase. All rows match, with one recorded
exception: `q4` on CADETS has the right pair count but a different checksum, because
Elasticsearch's `max` aggregation returns a double and the CADETS ns timestamps (~1.5e18)
exceed 2^53; `q4_exact` (top_hits) matches exactly. On wiki-talk (seconds) both match.

## Results

Ingest and footprint (TGSA from `results/e33_matrix/e33.csv`, 8 threads, batch 65,536;
TGSA's `compact` = finalize, reported in parentheses as ingest+compact):

| corpus | system | ingest events/s | on disk (B/event) | process RSS |
|---|---|---|---|---|
| CADETS (22.5M) | TGSA | 60.3M (21.1M with compact) | in memory | 1,182 MB (store) |
| | Elasticsearch, default durability | 83.9K | 1.19 GiB (56.9) | 9.8 GB (8 GB pre-touched heap) |
| | Elasticsearch, async translog | ASYNC_C_RATE | ASYNC_C_STORE | |
| | Kafka, producer defaults | 6.84M | 0.59 GiB (28.0) | 1.8 GB (4 GB heap) |
| | Kafka, linger 50 ms, batch 1 MiB | 8.48M | 0.60 GiB (28.7) | |
| wiki-talk (7.8M) | TGSA | 54.2M (23.2M with compact) | in memory | 461 MB (store) |
| | Elasticsearch, default durability | 87.2K | 0.67 GiB (91.4) | 10.8 GB |
| | Elasticsearch, async translog | ASYNC_W_RATE | ASYNC_W_STORE | |
| | Kafka, producer defaults | 4.95M | 0.20 GiB (28.0) | 3.2 GB |
| | Kafka, linger 50 ms, batch 1 MiB | 4.80M | 0.21 GiB (28.7) | |

The raw records are 20 B/event. Elasticsearch's default `translog.durability=request`
fsyncs before every bulk response; on this rotational disk that, not indexing, sets the
rate (2.1 of 20 cores busy, client formatting at most 4 s of the 269 s). The async row
fsyncs every 5 s, the durability Kafka's default (no fsync, page cache) and TGSA's
in-memory ingest also give. The wiki-talk index shrank to 454 MB after background merges;
the table reports the size right after flush.

Queries (median / p95 per query; seconds for whole-phase rows):

| corpus | query | TGSA | Elasticsearch | Kafka (full consume) |
|---|---|---|---|---|
| CADETS | Q1 window, every event (310,976) | 1.01 ms | 5.18 s (`_count` alone: 10.5 ms) | 1.19 s |
| | Q2 windowed neighbors, every event | 23.4 / 160 µs | 74.3 / 666 ms (server `took` 29 / 363 ms) | 1.21 s per query; 1.80 s for all 1,000 in one consume |
| | Q2, count only | | 4.74 / 6.31 ms | |
| | Q3 reachability | 469 / 1,974 µs, 1,000/1,000 | 1.97 / 27.2 s client-side, 337/1,000 in budget; median 332 Q2 calls per query (p95 6,916, max 13,713), median depth 4 hops (max 97) | n/a: no index; each hop wave is another full consume |
| | Q4 as-of neighbors | 2.81 / 88.2 ms | 85.7 / 1,511 ms `max` (checksum off: double); 224 / 5,314 ms exact | not run: no index (one full consume, as Q2) |
| wiki-talk | Q1 window, every event (283,276) | 0.88 ms | 5.47 s (`_count`: 4.6 ms) | 0.66 s |
| | Q2 windowed neighbors, every event | 1.67 / 42.9 µs | 5.39 / 151 ms (server `took` 2 / 78 ms) | 0.77 s per query; 0.69 s for all 1,000 |
| | Q2, count only | | 3.45 / 4.70 ms | |
| | Q3 reachability | 12.6 / 22.6 ms, 1,000/1,000 | 71.2 / 156.5 s client-side, 23/1,000 in budget; median 19,984 Q2 calls per query (max 45,845), median depth 15 (max 22) | n/a |
| | Q4 as-of neighbors | 22.5 / 4,449 µs | 6.09 / 395 ms `max`; 12.7 / 1,346 ms exact | not run |

Elasticsearch latencies are client-measured: HTTP, JSON decoding and paging included, one
query at a time, as every E33 latency phase. The server-reported `took` is given where it
matters; even that alone is three orders of magnitude above TGSA on Q2. Budget-cut Q3 cells
follow the E33 rule (time the first 20, project, run what fits 1800 s, a 1.25x deadline);
their medians are over the first k queries of the random list. RSS is not comparable
across the columns: TGSA's is the store's resident delta, the others are whole-JVM resident
sets dominated by the configured heap.

## Interpretation

INTERPRETATION

## Data directories

Recorded at cleanup and then deleted: CLEANUP_SIZES.
Both services stopped.
