# E28: cross-system storage consumption (TVA Fig 6a/8a slot)

One table, four systems, same corpora fully ingested. Sources:
TGSR = e16 rss_MB / bytes_per_edge (resident, vertex index and ts cache
included). LiveGraph = e19 rss_after_MB, batch-loader path (its most
compact). RocksDB = e24 rss after volatile ingest incl. Flush.
GraphOne = peak RSS of its own harness (job 20, weight64) measured with
/usr/bin/time, MINUS its in-memory input buffer (24 B/edge), because its
harness slurps the whole corpus before ingesting; that subtraction is an
approximation and is the reason the column is starred.

| corpus | TGSR MB (B/edge) | LiveGraph | GraphOne* | RocksDB |
|---|---|---|---|---|
| askubuntu | 98 (107) | 110 | 92 | 114 |
| wiki-talk | 588 (79) | 761 | 635 | 630 |
| cit-Patents | 1436 (91) | 1938 | 1867 | 2260 |
| tc-cadets | 1606 (75) | 2074 | 1499 | 2259 |

Honest read: TGSR is lowest on the two corpora where its windows are
dense (wiki-talk, cit-Patents), within +-8% of GraphOne's very compact
24 B format elsewhere, 23-29% below LiveGraph, and 30-43% below RocksDB.
We are storage-competitive, not the compactness champion TVA is on its
versioned workload (TVA saves 2.2-4.7x against ITS baselines by
deduplicating versions, a mechanism an immutable event multiset neither
needs nor rewards). The +22% ts-cache share of TGSR's number is priced
separately in e23_ablation.
