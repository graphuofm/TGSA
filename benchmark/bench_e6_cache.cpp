// E6: Cache & TLB behavior. Two phases:
//   (1) ingest burst (90% bulk warm-up + 10% streamed at batch=1k)
//   (2) range scan (1% windows around random midpoints)
// Run this binary inside `perf stat -x , -e ...` so the stderr metrics get
// aligned with the CSV row written here. Python wrapper handles that.
//
// Usage: bench_e6_cache --dataset <bin> --phase <ingest|scan>
//                       --huge_pages <0|1> --threads <N>
//                       --trial <i> --output <csv>

#include "common.hpp"
#include "barry/engine.hpp"

#include <random>

using namespace barry;
using namespace barry::bench;

int main(int argc, char** argv) {
    Args args(argc, argv);
    std::string in_path  = args.get("dataset");
    std::string phase    = args.get("phase", "ingest");
    int  threads         = static_cast<int>(args.getl("threads", 0));
    int  trial           = static_cast<int>(args.getl("trial", 0));
    bool huge_pages      = args.get("huge_pages", "0") != "0";
    int  num_scans       = static_cast<int>(args.getl("num_scans", 200));
    std::string out_csv  = args.get("output", "");
    if (in_path.empty() || out_csv.empty()) {
        std::fprintf(stderr, "usage: %s --dataset <bin> --phase <ingest|scan> "
                     "--huge_pages 0/1 --threads N --trial i --output csv\n",
                     args.prog().c_str());
        return 1;
    }

    EdgeFile ef;
    if (!ef.load(in_path)) return 1;
    GraphMeta meta = scan_meta(ef.edges, ef.count);

    GraphConfig cfg;
    cfg.t_min = meta.t_min; cfg.t_max = meta.t_max;
    cfg.shift = 0; cfg.timeline_slots = 0;
    cfg.num_vertices = meta.v_max + 1;
    cfg.use_huge_pages = huge_pages;
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, ef.count * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, ef.count * sizeof(EdgeRef) * 4);
    cfg.default_block_capacity     = 16;

    Barry b(cfg);
    size_t op_count = 0;
    double secs = 0;

    if (phase == "ingest") {
        size_t bulk_n   = static_cast<size_t>(ef.count * 0.9);
        size_t stream_n = ef.count - bulk_n;
        b.insert_batch(ef.edges, bulk_n, threads);  // warm-up untimed

        double t0 = now_sec();
        b.insert_batch(ef.edges + bulk_n, stream_n, threads);
        secs = now_sec() - t0;
        op_count = stream_n;
    } else if (phase == "scan") {
        // Bulk-load entire graph first (untimed), then time num_scans range scans.
        b.insert_batch(ef.edges, ef.count, threads);

        std::mt19937_64 rng(0xCA22 ^ trial);
        uint64_t span = (meta.t_max > meta.t_min) ? meta.t_max - meta.t_min : 1;
        uint64_t win  = std::max<uint64_t>(1, span / 100);

        double t0 = now_sec();
        uint64_t total_hits = 0;
        for (int i = 0; i < num_scans; ++i) {
            uint64_t r = std::uniform_int_distribution<uint64_t>(0, span - 1)(rng);
            uint64_t ts = meta.t_min + r;
            auto v = b.query_range(ts, std::min(ts + win, meta.t_max));
            total_hits += v.size();
        }
        secs = now_sec() - t0;
        op_count = total_hits;
    } else {
        std::fprintf(stderr, "unknown phase %s\n", phase.c_str()); return 1;
    }

    auto s = b.stats();

    CsvWriter w(out_csv,
        {"system","dataset","trial","threads","phase","huge_pages",
         "ops","seconds","ops_per_sec","arena_used_bytes","rss_bytes"});
    w.row(std::string("barry"), basename_noext(in_path),
          trial, threads, phase, huge_pages ? 1 : 0,
          (unsigned long long)op_count, secs,
          op_count / (secs > 0 ? secs : 1.0),
          (unsigned long long)s.bytes_arena_used,
          (unsigned long long)s.bytes_rss);

    std::fprintf(stderr, "[e6] %s phase=%s hp=%d  ops=%zu  %.3fs\n",
                 basename_noext(in_path).c_str(), phase.c_str(),
                 huge_pages ? 1 : 0, op_count, secs);
    return 0;
}
