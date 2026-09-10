// E2: Temporal query latency. After bulk-loading the whole graph, runs the
// six query types Q1-Q6 against a fixed sample of timestamps/sources, and
// reports median latency in microseconds plus result-set size.
//
// Usage:
//   bench_e2_query --dataset <bin> --query <q1|q2|q3|q4|q5|q6>
//                  --num_samples <int> --threads <int>
//                  --trial <int>  --output <csv>

#include "common.hpp"
#include "barry/engine.hpp"

#include <random>
#include <string>

using namespace barry;
using namespace barry::bench;

static uint32_t pick_high_degree(const VertexIndex* vi, uint32_t V, uint32_t scan_max = 65536) {
    uint32_t best = 0, deg = 0;
    uint32_t lim = std::min(V, scan_max);
    for (uint32_t v = 0; v < lim; ++v) {
        uint32_t d = vi->out_degree(v);
        if (d > deg) { deg = d; best = v; }
    }
    return best;
}

int main(int argc, char** argv) {
    Args args(argc, argv);
    std::string in_path = args.get("dataset");
    std::string q       = args.get("query", "q1");
    int  num_samples    = static_cast<int>(args.getl("num_samples", 100));
    int  threads        = static_cast<int>(args.getl("threads", 0));
    int  trial          = static_cast<int>(args.getl("trial", 0));
    std::string out_csv = args.get("output", "");
    if (in_path.empty() || out_csv.empty()) {
        std::fprintf(stderr, "usage: %s --dataset <bin> --query <q1..q6> "
                     "--num_samples N --threads T --trial i --output csv\n",
                     args.prog().c_str());
        return 1;
    }

    EdgeFile ef;
    if (!ef.load(in_path)) return 1;
    GraphMeta meta = scan_meta(ef.edges, ef.count);

    GraphConfig cfg;
    cfg.t_min = meta.t_min;
    cfg.t_max = meta.t_max;
    cfg.shift = 0;
    cfg.timeline_slots = 0;
    cfg.num_vertices = meta.v_max + 1;
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, ef.count * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, ef.count * sizeof(EdgeRef) * 4);
    cfg.default_block_capacity = 16;

    Barry b(cfg);
    b.insert_batch(ef.edges, ef.count, threads);

    std::mt19937_64 rng(0xBA22 ^ trial);
    uint64_t span = (meta.t_max > meta.t_min) ? meta.t_max - meta.t_min : 1;
    auto rand_ts = [&]() {
        uint64_t r = std::uniform_int_distribution<uint64_t>(0, span - 1)(rng);
        return meta.t_min + r;
    };
    uint32_t V = cfg.num_vertices;
    auto rand_v = [&]() {
        return std::uniform_int_distribution<uint32_t>(0, V - 1)(rng);
    };

    std::vector<double> latencies_us;
    std::vector<uint64_t> result_sizes;
    latencies_us.reserve(num_samples);
    result_sizes.reserve(num_samples);

    auto record = [&](double t0, double t1, uint64_t sz) {
        latencies_us.push_back((t1 - t0) * 1e6);
        result_sizes.push_back(sz);
    };

    if (q == "q1") {
        // Point-in-time scan
        for (int i = 0; i < num_samples; ++i) {
            uint64_t ts = rand_ts();
            double t0 = now_sec();
            auto r = b.query_at(ts);
            double t1 = now_sec();
            record(t0, t1, r.size());
        }
    } else if (q == "q2") {
        // Range scan, window = 1% of span
        uint64_t win = std::max<uint64_t>(1, span / 100);
        for (int i = 0; i < num_samples; ++i) {
            uint64_t ts = rand_ts();
            double t0 = now_sec();
            auto r = b.query_range(ts, std::min(ts + win, meta.t_max));
            double t1 = now_sec();
            record(t0, t1, r.size());
        }
    } else if (q == "q3") {
        // Earliest arrival from a high-degree source, full time range
        uint32_t src = pick_high_degree(b.vertex_index(), V);
        for (int i = 0; i < num_samples; ++i) {
            double t0 = now_sec();
            auto ea = b.earliest_arrival(src, meta.t_min, meta.t_max);
            double t1 = now_sec();
            uint64_t reached = 0;
            for (auto v : ea) if (v != ULLONG_MAX) ++reached;
            record(t0, t1, reached);
        }
    } else if (q == "q4") {
        // Temporal 2-hop: pick from top-256 highest-degree vertices so we
        // actually exercise the join path instead of hitting empty buckets.
        uint64_t win = std::max<uint64_t>(1, span / 100);
        std::vector<std::pair<uint32_t,uint32_t>> deg_v;  // (degree, vertex)
        deg_v.reserve(V);
        for (uint32_t v = 0; v < V; ++v) {
            uint32_t d = b.vertex_index()->out_degree(v);
            if (d > 0) deg_v.emplace_back(d, v);
        }
        std::partial_sort(deg_v.begin(),
                          deg_v.begin() + std::min<size_t>(256, deg_v.size()),
                          deg_v.end(),
                          [](auto& a, auto& b){ return a.first > b.first; });
        size_t pool = std::min<size_t>(256, deg_v.size());
        if (pool == 0) {
            std::fprintf(stderr, "[e2] q4 skipped: all vertices have degree 0\n");
            return 0;
        }
        for (int i = 0; i < num_samples; ++i) {
            uint32_t u  = deg_v[rng() % pool].second;
            uint64_t ts = rand_ts();
            uint64_t te = std::min(ts + win, meta.t_max);
            double t0 = now_sec();
            auto h1 = b.vertex_index()->neighbors_temporal(u, ts, te);
            uint64_t cnt = h1.size();
            for (const Edge& e : h1) {
                auto h2 = b.vertex_index()->neighbors_temporal(e.dst, e.timestamp, te);
                cnt += h2.size();
            }
            double t1 = now_sec();
            record(t0, t1, cnt);
        }
    } else if (q == "q5") {
        // Sliding-window weight sum, window = 1% span
        uint64_t win = std::max<uint64_t>(1, span / 100);
        for (int i = 0; i < num_samples; ++i) {
            uint64_t ts = rand_ts();
            double t0 = now_sec();
            double s = b.window_aggregate_sum(ts, std::min(ts + win, meta.t_max));
            double t1 = now_sec();
            record(t0, t1, (uint64_t)s);
        }
    } else if (q == "q6") {
        // Temporal triangle count over window, repeated num_samples times for stability
        uint64_t win = std::max<uint64_t>(1, span / 1000);  // 0.1% span
        uint64_t delta = std::max<uint64_t>(1, win / 4);
        for (int i = 0; i < num_samples; ++i) {
            uint64_t ts = rand_ts();
            double t0 = now_sec();
            uint64_t c = b.temporal_triangle_count(ts, std::min(ts + win, meta.t_max), delta);
            double t1 = now_sec();
            record(t0, t1, c);
        }
    } else {
        std::fprintf(stderr, "unknown query %s\n", q.c_str());
        return 1;
    }

    double med  = median(latencies_us);
    double p95  = percentile(latencies_us, 0.95);
    double mn   = mean(latencies_us);
    double avg_sz = 0;
    for (auto s : result_sizes) avg_sz += s;
    if (!result_sizes.empty()) avg_sz /= result_sizes.size();

    CsvWriter w(out_csv,
        {"system","dataset","trial","threads","query","num_samples",
         "median_us","mean_us","p95_us","avg_result_size","total_edges"});
    w.row(std::string("barry"),
          basename_noext(in_path),
          trial, threads,
          q, num_samples,
          med, mn, p95, avg_sz,
          (unsigned long long)ef.count);

    std::fprintf(stderr, "[e2] %s %s n=%d  median=%.1f us  p95=%.1f us  avg|R|=%.1f\n",
                 basename_noext(in_path).c_str(), q.c_str(), num_samples,
                 med, p95, avg_sz);
    return 0;
}
