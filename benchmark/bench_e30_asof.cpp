// E30: the as-of snapshot row, revisited with a real operator.
//
// TVA's home operation is "each neighbor's latest version at time t". Our
// old answer was enumerate-then-reduce (collect every event <= t, hash-map
// the max per neighbor), which cost 217.9 s on CADETS in E15 and was
// reported as a loss. This bench measures three paths in one harness, all
// checksum-verified against ground truth computed from the input arrays:
//
//   old      E15's exact method: neighbors_temporal_into + unordered_map,
//            one thread.
//   new      VertexIndex::latest_neighbors_into (in-place filter + flat
//            sort dedup, no materialized hash), one thread.
//   new_mt   the same operator across vertices in parallel (8 threads),
//            which the per-vertex operator makes embarrassingly parallel.
//
// The honest remainder stays stated: the operator inspects O(d_le_t) refs
// per vertex where a version-grouped store inspects O(distinct); E30
// prices how much of the 217.9 s was that asymptotic gap and how much was
// our own enumerate-then-reduce overhead.
//
// Usage: bench_e30_asof --dataset <bin> [--threads T] --output csv

#include "common.hpp"
#include "barry/engine.hpp"
#include "barry/vertex_index.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef BARRY_HAVE_OPENMP
#include <omp.h>
#endif

using namespace barry;
using namespace barry::bench;

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
    // Mid-time snapshot, the same shape as E15's full-graph scan.
    const uint64_t t_asof = meta.t_min + (meta.t_max - meta.t_min) / 2;

    CsvWriter w(out_csv,
        {"system","dataset","path","threads","t_asof","pairs","checksum",
         "seconds","checksum_ok"});

    // ---- ground truth from the input arrays --------------------------
    // Sort (src, dst, ts); the last entry of each (src, dst) run with
    // ts <= t is that pair's latest version.
    struct Rec { uint32_t s, d; uint64_t t; };
    std::vector<Rec> recs;
    recs.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const Edge& e = ef.edges[i];
        const uint64_t ts = e.timestamp ? e.timestamp : 1;
        if (ts <= t_asof) recs.push_back({e.src, e.dst, ts});
    }
    std::sort(recs.begin(), recs.end(), [](const Rec& a, const Rec& b) {
        if (a.s != b.s) return a.s < b.s;
        if (a.d != b.d) return a.d < b.d;
        return a.t < b.t;
    });
    uint64_t ref_pairs = 0, ref_sum = 0;
    for (size_t i = 0; i < recs.size(); ++i) {
        if (i + 1 < recs.size() && recs[i].s == recs[i + 1].s &&
            recs[i].d == recs[i + 1].d)
            continue;                       // not the latest of its pair
        ++ref_pairs;
        ref_sum += (uint64_t)recs[i].s ^ ((uint64_t)recs[i].d << 20) ^ recs[i].t;
    }
    std::fprintf(stderr, "[e30] %s ground truth: %llu pairs at t=%llu\n",
                 ds.c_str(), (unsigned long long)ref_pairs,
                 (unsigned long long)t_asof);

    // ---- engine -------------------------------------------------------
    GraphConfig cfg;
    cfg.t_min = meta.t_min; cfg.t_max = meta.t_max; cfg.num_vertices = V;
    cfg.arena_reserve_bytes = std::max<size_t>(1ULL << 30, n * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes =
        std::max<size_t>(1ULL << 28, n * sizeof(EdgeRef) * 8);
    Barry b(cfg);
    b.insert_batch(ef.edges, n, threads);
    b.finalize();
    const VertexIndex* vi = b.vertex_index();

    auto report = [&](const char* path, int th, uint64_t pairs, uint64_t sum,
                      double sec) {
        const bool ok = (pairs == ref_pairs && sum == ref_sum);
        w.row(std::string("tgsr"), ds, std::string(path), th,
              (unsigned long long)t_asof, (unsigned long long)pairs,
              (unsigned long long)sum, sec, (int)ok);
        std::fprintf(stderr, "[e30] %-7s t=%d  %8.3f s  pairs=%llu  %s\n",
                     path, th, sec, (unsigned long long)pairs,
                     ok ? "ok" : "CHECKSUM MISMATCH");
        return ok;
    };

    // ---- old: E15's enumerate-then-reduce, one thread ------------------
    {
        std::vector<TemporalRef> buf;
        std::unordered_map<uint32_t, uint64_t> latest;
        uint64_t pairs = 0, sum = 0;
        const double t0 = now_sec();
        for (uint32_t v = 0; v < V; ++v) {
            buf.clear(); latest.clear();
            vi->neighbors_temporal_into(v, 1, t_asof, buf);
            for (const TemporalRef& r : buf) {
                auto it = latest.find(r.peer);
                if (it == latest.end() || r.ts > it->second) latest[r.peer] = r.ts;
            }
            pairs += latest.size();
            for (auto& [peer, ts] : latest)
                sum += (uint64_t)v ^ ((uint64_t)peer << 20) ^ ts;
        }
        if (!report("old", 1, pairs, sum, now_sec() - t0)) return 1;
    }

    // ---- new: the operator, one thread ---------------------------------
    {
        std::vector<TemporalRef> out;
        uint64_t pairs = 0, sum = 0;
        const double t0 = now_sec();
        for (uint32_t v = 0; v < V; ++v) {
            out.clear();
            vi->latest_neighbors_into(v, t_asof, out);
            pairs += out.size();
            for (const TemporalRef& r : out)
                sum += (uint64_t)v ^ ((uint64_t)r.peer << 20) ^ r.ts;
        }
        if (!report("new", 1, pairs, sum, now_sec() - t0)) return 1;
    }

    // ---- new_mt: the operator across vertices --------------------------
    {
        uint64_t pairs = 0, sum = 0;
        const double t0 = now_sec();
#ifdef BARRY_HAVE_OPENMP
        #pragma omp parallel num_threads(threads) reduction(+:pairs, sum)
        {
            std::vector<TemporalRef> out;
            #pragma omp for schedule(dynamic, 1024)
            for (uint32_t v = 0; v < V; ++v) {
                out.clear();
                vi->latest_neighbors_into(v, t_asof, out);
                pairs += out.size();
                for (const TemporalRef& r : out)
                    sum += (uint64_t)v ^ ((uint64_t)r.peer << 20) ^ r.ts;
            }
        }
#else
        for (uint32_t v = 0; v < V; ++v) {
            std::vector<TemporalRef> out;
            vi->latest_neighbors_into(v, t_asof, out);
            pairs += out.size();
            for (const TemporalRef& r : out)
                sum += (uint64_t)v ^ ((uint64_t)r.peer << 20) ^ r.ts;
        }
#endif
        if (!report("new_mt", threads, pairs, sum, now_sec() - t0)) return 1;
    }
    return 0;
}
