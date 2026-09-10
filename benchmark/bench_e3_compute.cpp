// E3: Graph computation. Times BFS / PageRank / Triangle Counting under both
// Barry and a static CSR reference. We then normalize Barry runtime to CSR in
// the plotting step.
//
// Usage:
//   bench_e3_compute --dataset <bin> --algo <bfs|pr|tc> --system <barry|csr>
//                    --threads <N> --trial <i> --output <csv>

#include "common.hpp"
#include "csr_reference.hpp"
#include "barry/engine.hpp"

#include <random>

using namespace barry;
using namespace barry::bench;

int main(int argc, char** argv) {
    Args args(argc, argv);
    std::string in_path = args.get("dataset");
    std::string algo    = args.get("algo", "bfs");
    std::string sys     = args.get("system", "barry");
    int threads         = static_cast<int>(args.getl("threads", 0));
    int trial           = static_cast<int>(args.getl("trial", 0));
    std::string out_csv = args.get("output", "");
    if (in_path.empty() || out_csv.empty()) {
        std::fprintf(stderr, "usage: %s --dataset <bin> --algo <bfs|pr|tc> "
                     "--system <barry|csr> --threads N --trial i --output csv\n",
                     args.prog().c_str());
        return 1;
    }

    EdgeFile ef;
    if (!ef.load(in_path)) return 1;
    GraphMeta meta = scan_meta(ef.edges, ef.count);

    double load_secs = 0;
    double run_secs  = 0;
    uint64_t signature = 0;  // arbitrary integer summary so the compiler doesn't optimize calls away

    if (sys == "barry") {
        GraphConfig cfg;
        cfg.t_min = meta.t_min; cfg.t_max = meta.t_max;
        cfg.shift = 0; cfg.timeline_slots = 0;
        cfg.num_vertices = meta.v_max + 1;
        cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, ef.count * sizeof(Edge) * 4);
        cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, ef.count * sizeof(EdgeRef) * 4);
        cfg.default_block_capacity     = 16;

        Barry b(cfg);
        double l0 = now_sec();
        b.insert_batch(ef.edges, ef.count, threads);
        load_secs = now_sec() - l0;

        // Pick a high-degree source for BFS.
        uint32_t src = 0, deg = 0;
        for (uint32_t v = 0; v < std::min<uint32_t>(65536, cfg.num_vertices); ++v) {
            uint32_t d = b.vertex_index()->out_degree(v);
            if (d > deg) { deg = d; src = v; }
        }

        double r0 = now_sec();
        if (algo == "bfs") {
            auto level = b.bfs(src);
            for (auto l : level) if (l >= 0) signature += 1;
        } else if (algo == "pr") {
            auto pr = b.pagerank(20, 0.85f);
            double s = 0; for (auto x : pr) s += x;
            signature = (uint64_t)(s * 1e6);
        } else if (algo == "tc") {
            signature = b.triangle_count();
        } else if (algo == "sssp") {
            auto d = b.sssp(src);
            uint64_t finite = 0;
            for (auto x : d) if (std::isfinite(x)) ++finite;
            signature = finite;
        } else if (algo == "cc") {
            auto c = b.connected_components();
            // Distinct component-ids = number of components.
            std::vector<int32_t> ids = c;
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            signature = ids.size();
        } else if (algo == "kcore") {
            auto k = b.kcore();
            uint32_t mx = 0; for (auto x : k) if (x > mx) mx = x;
            signature = mx;
        } else {
            std::fprintf(stderr, "unknown algo %s\n", algo.c_str()); return 1;
        }
        run_secs = now_sec() - r0;

    } else if (sys == "csr") {
        CsrGraph g;
        double l0 = now_sec();
        g.build(ef.edges, ef.count, meta.v_max + 1);
        load_secs = now_sec() - l0;

        uint32_t src = 0, deg = 0;
        for (uint32_t v = 0; v < std::min<uint32_t>(65536, g.num_vertices()); ++v) {
            uint32_t d = g.out_degree(v);
            if (d > deg) { deg = d; src = v; }
        }

        double r0 = now_sec();
        if (algo == "bfs") {
            auto level = g.bfs(src);
            for (auto l : level) if (l >= 0) signature += 1;
        } else if (algo == "pr") {
            auto pr = g.pagerank();
            double s = 0; for (auto x : pr) s += x;
            signature = (uint64_t)(s * 1e6);
        } else if (algo == "tc") {
            signature = g.triangle_count();
        } else if (algo == "sssp") {
            // CSR has no weights; use uniform 1.0 (degenerates to BFS).
            std::vector<float> w(ef.count, 1.0f);
            auto d = g.sssp(src, w);
            uint64_t finite = 0;
            for (auto x : d) if (std::isfinite(x)) ++finite;
            signature = finite;
        } else if (algo == "cc") {
            auto c = g.connected_components();
            std::vector<int32_t> ids = c;
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            signature = ids.size();
        } else if (algo == "kcore") {
            auto k = g.kcore();
            uint32_t mx = 0; for (auto x : k) if (x > mx) mx = x;
            signature = mx;
        } else {
            std::fprintf(stderr, "unknown algo %s\n", algo.c_str()); return 1;
        }
        run_secs = now_sec() - r0;
    } else {
        std::fprintf(stderr, "unknown system %s\n", sys.c_str()); return 1;
    }

    CsvWriter w(out_csv,
        {"system","dataset","trial","threads","algo",
         "load_seconds","run_seconds","signature","total_edges"});
    w.row(sys,
          basename_noext(in_path),
          trial, threads, algo,
          load_secs, run_secs,
          (unsigned long long)signature,
          (unsigned long long)ef.count);

    std::fprintf(stderr, "[e3] %s/%s on %s: load=%.3fs run=%.3fs sig=%llu\n",
                 sys.c_str(), algo.c_str(), basename_noext(in_path).c_str(),
                 load_secs, run_secs, (unsigned long long)signature);
    return 0;
}
