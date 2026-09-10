// E4: Scalability. Identical to E1 but exposes thread count as the primary
// independent variable. Useful for {1,2,4,8,16,20}-thread sweeps.
//
// Usage: bench_e4_scale --dataset <bin> --threads <N> --batch <int>
//                       --trial <i> --output <csv>

#include "common.hpp"
#include "barry/engine.hpp"

using namespace barry;
using namespace barry::bench;

int main(int argc, char** argv) {
    Args args(argc, argv);
    std::string in_path = args.get("dataset");
    long batch          = args.getl("batch", 1000);
    int  threads        = static_cast<int>(args.getl("threads", 1));
    int  trial          = static_cast<int>(args.getl("trial", 0));
    std::string out_csv = args.get("output", "");
    if (in_path.empty() || out_csv.empty()) {
        std::fprintf(stderr, "usage: %s --dataset <bin> --threads <T> "
                     "--batch <N> --trial i --output csv\n",
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
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, ef.count * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, ef.count * sizeof(EdgeRef) * 4);
    cfg.default_block_capacity     = 16;

    Barry b(cfg);

    size_t bulk_n   = static_cast<size_t>(ef.count * 0.9);
    size_t stream_n = ef.count - bulk_n;
    b.insert_batch(ef.edges, bulk_n, threads);  // warm-up untimed

    double t0 = now_sec();
    if (batch <= 0) batch = 1;
    for (size_t i = 0; i < stream_n; i += static_cast<size_t>(batch)) {
        size_t n = std::min<size_t>(static_cast<size_t>(batch), stream_n - i);
        b.insert_batch(ef.edges + bulk_n + i, n, threads);
    }
    double t1 = now_sec();

    double secs = t1 - t0;
    double eps  = stream_n / secs;

    CsvWriter w(out_csv, {"system","dataset","trial","threads","batch",
                          "stream_edges","seconds","throughput_eps"});
    w.row(std::string("barry"), basename_noext(in_path),
          trial, threads, batch,
          (unsigned long long)stream_n, secs, eps);

    std::fprintf(stderr, "[e4] %s threads=%d batch=%ld => %.2f Meps\n",
                 basename_noext(in_path).c_str(), threads, batch, eps / 1e6);
    return 0;
}
