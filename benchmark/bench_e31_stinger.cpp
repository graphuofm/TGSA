// E31: Stinger (the classic streaming-graph store) on the axis the user
// fights on: ingestion throughput and resident memory. TVA compares against
// Stinger on current-graph operations only, and so do we, for a semantic
// reason this bench states rather than hides: Stinger keys edges on
// (type, src, dst) and treats a repeated pair as an UPDATE (weight and
// recent-timestamp overwrite), so an event multiset cannot round-trip
// through it and windowed delivery comparisons would be comparing different
// questions. What can be compared exactly:
//
//   ingest   all N events pushed through stinger_insert_edge from T threads
//            (its documented parallel path). Rate is events processed/s,
//            which for Stinger means insert-or-update.
//   probe    final unique directed (src,dst) pair count, computed by the
//            harness from the input and read back from Stinger's own
//            structure (stinger_total_edges); a mismatch aborts.
//   memory   delta RSS after ingest.
//
// TGSR rows for the same corpora come from E16 (it ingests the multiset,
// which is strictly more work per event).
//
// Usage: bench_e31_stinger --dataset <bin> [--threads T] --output csv

extern "C" {
#include "stinger_core/stinger.h"
}

#include "common.hpp"

#include <unordered_set>
#include <string>
#include <vector>
#include <unistd.h>

#ifdef BARRY_HAVE_OPENMP
#include <omp.h>
#endif

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
    const GraphMeta meta = scan_meta(ef.edges, n);
    const std::string ds = basename_noext(in_path);
    const uint32_t V = meta.v_max + 1;

    CsvWriter w(out_csv,
        {"system","dataset","threads","events","unique_pairs","ingest_sec",
         "ingest_Meps","rss_MB","pairs_reported","probe_ok"});

    // Ground truth: unique directed pairs.
    std::unordered_set<uint64_t> uniq;
    uniq.reserve(n);
    for (size_t i = 0; i < n; ++i)
        uniq.insert((uint64_t(ef.edges[i].src) << 32) | ef.edges[i].dst);
    const size_t pairs = uniq.size();

    struct stinger_config_t cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.nv = V + 16;
    cfg.nebs = 0;              // let stinger size edge blocks from nv
    cfg.netypes = 1;
    cfg.nvtypes = 1;
    cfg.no_map_none_etype = 1;
    cfg.no_map_none_vtype = 1;
    cfg.no_resize = 0;

    const size_t rss0 = rss_bytes();
    struct stinger* S = stinger_new_full(&cfg);
    if (!S) { std::fprintf(stderr, "[e31] stinger_new_full failed\n"); return 1; }

    const double t0 = now_sec();
#ifdef BARRY_HAVE_OPENMP
    #pragma omp parallel for num_threads(threads) schedule(static, 4096)
#endif
    for (size_t i = 0; i < n; ++i) {
        const Edge& e = ef.edges[i];
        stinger_insert_edge(S, 0, e.src, e.dst, 1,
                            (int64_t)(e.timestamp ? e.timestamp : 1));
    }
    const double ing = now_sec() - t0;
    const size_t rss1 = rss_bytes();

    const int64_t reported = stinger_total_edges(S);
    const bool ok = (reported == (int64_t)pairs);
    w.row(std::string("stinger"), ds, threads, (unsigned long long)n,
          (unsigned long long)pairs, ing, n / ing / 1e6,
          (rss1 - rss0) / 1e6, (long long)reported, (int)ok);
    std::fprintf(stderr,
        "[e31] stinger %-18s %2dT  %8.2fs = %6.2f Meps (events)  "
        "rss=%6.0f MB  pairs %lld/%zu %s\n",
        ds.c_str(), threads, ing, n / ing / 1e6, (rss1 - rss0) / 1e6,
        (long long)reported, pairs, ok ? "ok" : "MISMATCH");
    stinger_free_all(S);
    return ok ? 0 : 1;
}
