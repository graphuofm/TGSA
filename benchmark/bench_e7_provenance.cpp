// E7: Provenance traversal latency -- the query class that motivates Barry's
// temporal-first layout.
//
//   p1  backward_trace  : causal ancestors of an alert (BackTracker semantics)
//   p2  forward_taint   : impact set of a contamination seed (sparse output)
//   p2d forward_taint   : same query answered by the dense earliest_arrival
//                         path, kept as an internal ablation baseline
//
// Besides latency this reports the taint fraction |result| / V, which is the
// dependency-explosion statistic that justifies version-granularity recovery.
//
// Usage:
//   bench_e7_provenance --dataset <bin> --query <p1|p2|p2d>
//                       --num_samples N --window_frac 0.01 --max_nodes 0
//                       --threads T --trial i --output csv

#include "common.hpp"
#include "barry/engine.hpp"

#include <random>
#include <string>

using namespace barry;
using namespace barry::bench;

int main(int argc, char** argv) {
    Args args(argc, argv);
    std::string in_path = args.get("dataset");
    std::string q       = args.get("query", "p2");
    int    num_samples  = static_cast<int>(args.getl("num_samples", 100));
    double window_frac  = args.getd("window_frac", 0.01);
    size_t max_nodes    = static_cast<size_t>(args.getl("max_nodes", 0));
    int    threads      = static_cast<int>(args.getl("threads", 0));
    int    trial        = static_cast<int>(args.getl("trial", 0));
    std::string out_csv = args.get("output", "");
    if (in_path.empty() || out_csv.empty()) {
        std::fprintf(stderr, "usage: %s --dataset <bin> --query <p1|p2|p2d> "
                     "--num_samples N --window_frac F --max_nodes M "
                     "--threads T --trial i --output csv\n", args.prog().c_str());
        return 1;
    }

    EdgeFile ef;
    if (!ef.load(in_path)) return 1;
    GraphMeta meta = scan_meta(ef.edges, ef.count);

    GraphConfig cfg;
    cfg.t_min = meta.t_min;
    cfg.t_max = meta.t_max;
    cfg.num_vertices = meta.v_max + 1;
    cfg.build_reverse_index = (q == "p1");   // only pay for it when p1 needs it
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, ef.count * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, ef.count * sizeof(EdgeRef) * 8);

    Barry b(cfg);
    double load_t0 = now_sec();
    b.insert_batch(ef.edges, ef.count, threads);
    b.finalize();   // bulk load then query: compact once, explicitly
    double load_sec = now_sec() - load_t0;

    Barry::Stats st = b.stats();
    const VertexIndex* vi = b.vertex_index();
    const uint32_t V = cfg.num_vertices;
    const uint64_t span = (meta.t_max > meta.t_min) ? meta.t_max - meta.t_min : 1;
    const uint64_t window = std::max<uint64_t>(1, static_cast<uint64_t>(span * window_frac));

    std::mt19937_64 rng(0xB4227 ^ trial);
    auto rand_v  = [&]() { return std::uniform_int_distribution<uint32_t>(0, V - 1)(rng); };
    auto rand_ts = [&]() {
        return meta.t_min + std::uniform_int_distribution<uint64_t>(0, span - 1)(rng);
    };

    // Pick a seed that actually has an incident edge in the right direction,
    // otherwise most samples measure an empty traversal.
    auto pick_seed = [&](bool need_out) {
        for (int tries = 0; tries < 64; ++tries) {
            uint32_t v = rand_v();
            uint32_t d = need_out ? vi->out_degree(v) : vi->in_degree(v);
            if (d > 0) return v;
        }
        return rand_v();
    };

    std::vector<double>   latencies_us;
    std::vector<double>   sizes;
    latencies_us.reserve(num_samples);
    sizes.reserve(num_samples);

    for (int i = 0; i < num_samples; ++i) {
        uint64_t ts = rand_ts();
        size_t   sz = 0;
        double   t0, t1;

        if (q == "p1") {
            uint32_t alert = pick_seed(/*need_out=*/false);
            uint64_t t_lo  = (ts > meta.t_min + window) ? ts - window : meta.t_min;
            t0 = now_sec();
            auto r = b.backward_trace(alert, ts, t_lo, max_nodes);
            t1 = now_sec();
            sz = r.size();
        } else if (q == "p2") {
            uint32_t seed = pick_seed(/*need_out=*/true);
            uint64_t t_hi = std::min(meta.t_max, ts + window);
            t0 = now_sec();
            auto r = b.forward_taint(seed, ts, t_hi, max_nodes);
            t1 = now_sec();
            sz = r.size();
        } else if (q == "p2d") {
            uint32_t seed = pick_seed(/*need_out=*/true);
            uint64_t t_hi = std::min(meta.t_max, ts + window);
            t0 = now_sec();
            auto ea = b.earliest_arrival(seed, ts, t_hi);
            // Count reached vertices so the two paths do comparable work.
            for (uint64_t x : ea) if (x != ULLONG_MAX) ++sz;
            t1 = now_sec();
        } else {
            std::fprintf(stderr, "unknown query %s\n", q.c_str());
            return 1;
        }

        latencies_us.push_back((t1 - t0) * 1e6);
        sizes.push_back(static_cast<double>(sz));
    }

    double med   = median(latencies_us);
    double p95   = percentile(latencies_us, 0.95);
    double p99   = percentile(latencies_us, 0.99);
    double mn    = mean(latencies_us);
    double avgsz = mean(sizes);
    double maxsz = sizes.empty() ? 0 : *std::max_element(sizes.begin(), sizes.end());

    CsvWriter w(out_csv,
        {"system","dataset","trial","threads","query","num_samples","window_frac",
         "median_us","mean_us","p95_us","p99_us",
         "avg_result_size","max_result_size","taint_frac_avg","taint_frac_max",
         "num_vertices","total_edges","load_sec",
         "reverse_index","vidx_MB","arena_MB","rss_MB"});
    w.row(std::string("barry"), basename_noext(in_path), trial, threads,
          q, num_samples, window_frac,
          med, mn, p95, p99,
          avgsz, maxsz, avgsz / V, maxsz / V,
          (unsigned long long)V, (unsigned long long)ef.count, load_sec,
          (int)cfg.build_reverse_index,
          st.bytes_vertex_index / 1e6, st.bytes_arena_used / 1e6, st.bytes_rss / 1e6);

    std::fprintf(stderr,
        "[e7] %-22s %-3s  median=%9.1f us  p95=%10.1f us  avg|R|=%10.1f  "
        "taint=%.4f%% of V\n",
        basename_noext(in_path).c_str(), q.c_str(), med, p95, avgsz,
        100.0 * avgsz / V);
    return 0;
}
