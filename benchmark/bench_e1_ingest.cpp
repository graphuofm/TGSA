// E1: Ingestion throughput.
//
// Loads the first 90% of edges in bulk (untimed warm-up), then times the
// remaining 10% as a stream of fixed-size batches. Reports throughput as
// (stream_edges / total_seconds).
//
// Usage:
//   bench_e1_ingest --dataset <bin> --batch <int> --threads <int>
//                   --trial <int>   --output <csv>

#include "common.hpp"
#include "barry/engine.hpp"

using namespace barry;
using namespace barry::bench;

int main(int argc, char** argv) {
    Args args(argc, argv);
    std::string in_path  = args.get("dataset");
    long batch           = args.getl("batch", 1000);
    int  threads         = static_cast<int>(args.getl("threads", 0));
    int  trial           = static_cast<int>(args.getl("trial", 0));
    std::string out_csv  = args.get("output", "");
    bool with_vidx       = args.get("vidx", "1") != "0";
    bool huge_pages      = args.get("huge_pages", "0") != "0";
    if (in_path.empty() || out_csv.empty()) {
        std::fprintf(stderr,
            "usage: %s --dataset <bin> --batch <N> --threads <T> "
            "--trial <i> --output <csv> [--vidx 0/1] [--huge_pages 0/1]\n",
            args.prog().c_str());
        return 1;
    }

    EdgeFile ef;
    if (!ef.load(in_path)) return 1;
    GraphMeta meta = scan_meta(ef.edges, ef.count);

    size_t bulk_n   = static_cast<size_t>(ef.count * 0.9);
    size_t stream_n = ef.count - bulk_n;

    GraphConfig cfg;
    cfg.t_min = meta.t_min;
    cfg.t_max = meta.t_max;
    cfg.shift = 0;
    cfg.timeline_slots = 0;
    cfg.num_vertices = with_vidx ? meta.v_max + 1 : 0;
    cfg.use_huge_pages = huge_pages;
    cfg.arena_reserve_bytes = std::max<size_t>(1ULL << 30,
                                               ef.count * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28,
                                                      ef.count * sizeof(EdgeRef) * 4);
    cfg.default_block_capacity = 16;

    Barry b(cfg);

    // Bulk load (untimed warm-up).
    b.insert_batch(ef.edges, bulk_n, threads);

    // Time the streaming portion as fixed-size batches.
    double t0 = now_sec();
    if (batch <= 0) batch = 1;
    for (size_t i = 0; i < stream_n; i += static_cast<size_t>(batch)) {
        size_t n = std::min<size_t>(static_cast<size_t>(batch), stream_n - i);
        b.insert_batch(ef.edges + bulk_n + i, n, threads);
    }
    double t1 = now_sec();

    double secs = t1 - t0;
    double throughput = stream_n / secs;

    auto stats = b.stats();

    CsvWriter w(out_csv,
        {"system","dataset","trial","threads","batch","vidx","huge_pages",
         "total_edges","bulk_edges","stream_edges","seconds",
         "throughput_eps","blocks","arena_used_bytes",
         "vertex_idx_bytes","rss_bytes"});
    w.row(std::string("barry"),
          basename_noext(in_path),
          trial,
          threads,
          batch,
          with_vidx ? 1 : 0,
          huge_pages ? 1 : 0,
          (unsigned long long)ef.count,
          (unsigned long long)bulk_n,
          (unsigned long long)stream_n,
          secs,
          throughput,
          (unsigned long long)stats.num_blocks,
          (unsigned long long)stats.bytes_arena_used,
          (unsigned long long)stats.bytes_vertex_index,
          (unsigned long long)stats.bytes_rss);

    std::fprintf(stderr,
        "[e1] dataset=%s batch=%ld threads=%d  stream=%zu in %.3fs => %.2f Meps\n",
        basename_noext(in_path).c_str(), batch, threads,
        stream_n, secs, throughput / 1e6);

    return 0;
}
