// E5: Memory footprint. Loads the whole graph and reports virtual + RSS +
// Barry-internal sizes vs raw edge bytes.
//
// Usage: bench_e5_memory --dataset <bin> --trial <i> --output <csv>

#include "common.hpp"
#include "barry/engine.hpp"

using namespace barry;
using namespace barry::bench;

int main(int argc, char** argv) {
    Args args(argc, argv);
    std::string in_path = args.get("dataset");
    int  trial          = static_cast<int>(args.getl("trial", 0));
    int  threads        = static_cast<int>(args.getl("threads", 0));
    bool with_vidx      = args.get("vidx", "1") != "0";
    std::string out_csv = args.get("output", "");
    if (in_path.empty() || out_csv.empty()) {
        std::fprintf(stderr, "usage: %s --dataset <bin> --trial i --output csv "
                     "[--threads N] [--vidx 0/1]\n", args.prog().c_str());
        return 1;
    }

    EdgeFile ef;
    if (!ef.load(in_path)) return 1;
    GraphMeta meta = scan_meta(ef.edges, ef.count);

    size_t rss_before = Timeline::process_rss_bytes();

    GraphConfig cfg;
    cfg.t_min = meta.t_min; cfg.t_max = meta.t_max;
    cfg.shift = 0; cfg.timeline_slots = 0;
    cfg.num_vertices = with_vidx ? meta.v_max + 1 : 0;
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, ef.count * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, ef.count * sizeof(EdgeRef) * 4);
    cfg.default_block_capacity     = 16;

    Barry b(cfg);
    b.insert_batch(ef.edges, ef.count, threads);

    auto s = b.stats();
    size_t raw_bytes = ef.count * sizeof(Edge);

    CsvWriter w(out_csv,
        {"system","dataset","trial","vidx","total_edges",
         "raw_bytes","arena_used_bytes","arena_reserved_bytes",
         "timeline_virtual_bytes","vertex_idx_bytes",
         "rss_before_bytes","rss_after_bytes","rss_delta_bytes"});
    w.row(std::string("barry"), basename_noext(in_path),
          trial, with_vidx ? 1 : 0,
          (unsigned long long)ef.count,
          (unsigned long long)raw_bytes,
          (unsigned long long)s.bytes_arena_used,
          (unsigned long long)s.bytes_arena_reserved,
          (unsigned long long)s.bytes_timeline_virtual,
          (unsigned long long)s.bytes_vertex_index,
          (unsigned long long)rss_before,
          (unsigned long long)s.bytes_rss,
          (unsigned long long)(s.bytes_rss > rss_before ? s.bytes_rss - rss_before : 0));

    std::fprintf(stderr,
        "[e5] %s edges=%zu raw=%.1fMB arena=%.1fMB vidx=%.1fMB rss=%.1fMB\n",
        basename_noext(in_path).c_str(), ef.count, raw_bytes/1e6,
        s.bytes_arena_used/1e6, s.bytes_vertex_index/1e6, s.bytes_rss/1e6);
    return 0;
}
