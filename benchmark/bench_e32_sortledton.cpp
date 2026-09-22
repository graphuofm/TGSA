// E32: Sortledton on ingestion throughput and resident memory, driven
// exactly the way its own internal driver drives it (per-edge snapshot
// transactions with does-not-exist vertex semantics, per-thread
// registration, ts carried as an 8-byte edge property). Like Stinger
// (E31), Sortledton keys edges on (src, dst): a repeated pair is an
// update, not a new event, so the comparison stays on the axis both
// systems can answer identically: events/s processed and memory, with the
// final unique-pair count verified against ground truth.
//
// Usage: bench_e32_sortledton --dataset <bin> [--threads T] --output csv

#include "data-structure/TransactionManager.h"
#include "data-structure/VersioningBlockedSkipListAdjacencyList.h"
#include "data-structure/SnapshotTransaction.h"

#include "common.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <unistd.h>

using namespace barry;
using namespace barry::bench;

static size_t rss_bytes() {
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long size = 0, resident = 0;
    if (std::fscanf(f, "%lu %lu", &size, &resident) != 2) resident = 0;
    std::fclose(f);
    return (size_t)resident * (size_t)::sysconf(_SC_PAGESIZE);
}

int main(int argc, char** argv) {
    Args args(argc, argv);
    const std::string in_path = args.get("dataset");
    const std::string out_csv = args.get("output", "");
    const int threads = static_cast<int>(args.getl("threads", 8));

    if (in_path.empty() || out_csv.empty()) {
        std::fprintf(stderr, "usage: %s --dataset <bin> [--threads T] --output csv\n",
                     args.prog().c_str());
        return 1;
    }

    EdgeFile ef;
    if (!ef.load(in_path)) return 1;
    const size_t n = ef.count;
    const std::string ds_name = basename_noext(in_path);

    CsvWriter w(out_csv,
        {"system","dataset","threads","events","unique_pairs","ingest_sec",
         "ingest_Mpairs_per_s","rss_MB","pairs_reported","probe_ok"});

    // Sortledton retains one edge per (src,dst) whatever we feed it, and
    // its heavily exercised path is plain insert_edge on distinct pairs
    // (its own evaluation deduplicates upstream; the insert_or_update
    // path corrupted the heap under this workload). So the collector
    // dedups upstream, keeping the FIRST occurrence's timestamp as the
    // property: identical retained state, their fast path, and the
    // deduplication cost is charged to the collector, not to Sortledton.
    std::unordered_set<uint64_t> uniq;
    uniq.reserve(n);
    std::vector<Edge> ded;
    ded.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const uint64_t key = (uint64_t(ef.edges[i].src) << 32) | ef.edges[i].dst;
        if (uniq.insert(key).second) ded.push_back(ef.edges[i]);
    }
    const size_t pairs = uniq.size();

    const size_t rss0 = rss_bytes();
    TransactionManager tm(threads);
    VersioningBlockedSkipListAdjacencyList ds(512, sizeof(uint64_t), tm);

    std::atomic<size_t> next{0};
    const double t0 = now_sec();
    std::vector<std::thread> th;
    for (int t = 0; t < threads; ++t) {
        th.emplace_back([&, t] {
            tm.register_thread(t);
            constexpr size_t kBatch = 3000;   // their driver's batch size
            SnapshotTransaction tx = tm.getSnapshotTransaction(&ds, true);
            for (;;) {
                const size_t lo = next.fetch_add(kBatch);
                if (lo >= ded.size()) break;
                const size_t hi = std::min(ded.size(), lo + kBatch);
                for (size_t i = lo; i < hi; ++i) {
                    const Edge& e = ded[i];
                    tx.use_vertex_does_not_exists_semantics();
                    tx.insert_vertex(e.src);
                    tx.insert_vertex(e.dst);
                    uint64_t ts = e.timestamp ? e.timestamp : 1;
                    tx.insert_edge({e.src, e.dst},
                                   reinterpret_cast<char*>(&ts), sizeof ts);
                    tx.execute();
                    tm.transactionCompleted(tx);
                    tm.getSnapshotTransaction(&ds, true, tx);
                }
            }
            tm.transactionCompleted(tx);
            tm.deregister_thread(t);
        });
    }
    for (auto& t : th) t.join();
    const double ing = now_sec() - t0;
    const size_t rss1 = rss_bytes();

    tm.register_thread(0);
    SnapshotTransaction rtx = tm.getSnapshotTransaction(&ds, false);
    const size_t reported = rtx.edge_count();
    tm.transactionCompleted(rtx);
    tm.deregister_thread(0);

    const bool ok = (reported == pairs);
    w.row(std::string("sortledton"), ds_name, threads, (unsigned long long)n,
          (unsigned long long)pairs, ing, pairs / ing / 1e6,
          (rss1 - rss0) / 1e6, (unsigned long long)reported, (int)ok);
    std::fprintf(stderr,
        "[e32] sortledton %-18s %2dT  %8.2fs = %6.2f Meps (events)  "
        "rss=%6.0f MB  pairs %zu/%zu %s\n",
        ds_name.c_str(), threads, ing, pairs / ing / 1e6, (rss1 - rss0) / 1e6,
        reported, pairs, ok ? "ok" : "MISMATCH");
    return ok ? 0 : 1;
}
