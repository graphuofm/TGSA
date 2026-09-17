# E24: RocksDB baseline with a time-encoded key

The strongest KV framing we could give RocksDB (v9.7.4, static lib, no
compression, IncreaseParallelism + OptimizeLevelStyleCompaction): key =
ts(8B BE) : src(4B BE) : dst(4B BE) : seq(4B BE), so a time range is a
contiguous key range and extraction is one Seek plus a sequential scan.
The seq suffix exists because Put dedupes and the workload is a multiset.
Every phase's delivered edge count AND checksum (sum of ts^dst) is
compared against the TGSR reference engine before any timing is kept:
**0 mismatches across all rows** (`e24.csv` is the source of truth).

## Ingest (single writer, Meps; wal row = WriteOptions.disableWAL=false, unsynced)

| corpus | TGSR | RocksDB | RocksDB+WAL | TGSR advantage |
|---|---|---|---|---|
| sx-askubuntu (0.96M) | 21.17 | 1.51 | 1.57 | 14.0x |
| wiki-talk (7.8M) | 55.40 | 2.25 | 2.15 | 24.6x |
| cit-Patents (16.5M) | 22.89 | 2.91 | 3.11 | 7.9x |
| tc-cadets-e3 (22.5M) | 60.28 | 2.88 | 2.73 | 20.9x |

RocksDB Flush() is timed inside the ingest phase, so memtable-to-L0 cost
is not hidden. WAL on/off is within noise because the WAL is unsynced;
this is stated wherever the paper uses these rows.

## Trailing-window extraction (fraction of the time span, ms)

Time-range extraction is the honest strength of the time-encoded key:
one Seek and a scan. RocksDB is competitive in shape (linear in window
width) but ~15-25x slower per delivered edge (iterator + decode
overhead vs pointer-free window walks), e.g. wiki-talk 10% window:
285.5 ms vs 15.9 ms.

## Windowed neighbors of hot vertices (window scan + src filter, ms)

The structural cliff. A KV store keyed on time has no per-vertex path:
neighbors(v) must scan the whole window and filter on src.

| corpus | RocksDB | TGSR | ratio |
|---|---|---|---|
| sx-askubuntu | 2691.9 | 1.74 | 1,545x |
| wiki-talk | 46,678.9 | 10.5 | 4,459x |
| cit-Patents | 39,568.1 | 0.67 | 59,400x |
| tc-cadets-e3 | 64,769.8 | 82.6 | 784x |

The ratio tracks selectivity: cit-Patents delivers 2,330 edges out of a
16.5M-edge scan (worst case for the scan-and-filter plan), tc-cadets
delivers 26.8M (best case). This is the quantified version of "a
time-only key lacks structural awareness": TGSR's vertex index answers
the same query from the entity side while inheriting the same time
addressing.

Bench: `benchmark/bench_e24_rocksdb.cpp` (target `bench_e24_rocksdb`,
requires ROCKSDB_ROOT=baselines/rocksdb_src).
