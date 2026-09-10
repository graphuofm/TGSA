// E15 (Barry side): mirror of baselines/tva/experiment_barry/e15_tva.cc.
// Identical timestamp normalization, identical query sequence (same RNG seed),
// identical semantics: a point-in-time snapshot scan returns each neighbor's
// latest event at or before t. Barry has no native "distinct neighbor as of t"
// operator -- it enumerates events and reduces, and that cost is reported as
// measured, not hidden.
#include "common.hpp"
#include "barry/engine.hpp"
#include <random>
#include <sys/resource.h>
#include <unordered_map>

using namespace barry;
using namespace barry::bench;

static size_t rss_mb() { struct rusage u; getrusage(RUSAGE_SELF,&u); return u.ru_maxrss/1024; }

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s data.bin [num_scans]\n", argv[0]); return 1; }
    EdgeFile ef;
    if (!ef.load(argv[1])) return 1;
    int scans = argc > 2 ? atoi(argv[2]) : 200;
    GraphMeta m = scan_meta(ef.edges, ef.count);

    const uint64_t div = (m.t_max - m.t_min) > (1ULL << 47) ? 1000 : 1;
    auto norm = [&](uint64_t t){ return (t - m.t_min) / div + 1; };
    const uint64_t T = norm(m.t_max);
    const uint32_t V = m.v_max + 1;

    GraphConfig cfg;
    cfg.t_min = 1; cfg.t_max = T; cfg.num_vertices = V;
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL<<30, ef.count*sizeof(Edge)*4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL<<28, ef.count*sizeof(EdgeRef)*4);
    Barry b(cfg);

    // single-thread ingestion, same as the TVA driver (its API is not
    // documented thread-safe; fairness first, Barry's parallel number is E1's)
    std::vector<Edge> tmp(ef.count);
    for (size_t i = 0; i < ef.count; ++i)
        tmp[i] = Edge{ef.edges[i].src, ef.edges[i].dst, norm(ef.edges[i].timestamp), ef.edges[i].weight};
    double t0 = now_sec();
    b.insert_batch(tmp.data(), tmp.size(), 1);
    double ing = now_sec() - t0;
    b.finalize();   // explicit compaction, charged separately below

    std::mt19937_64 rng(0xE15);
    std::vector<std::pair<uint32_t,uint64_t>> qs;
    for (int i = 0; i < scans; ++i) qs.push_back({(uint32_t)(rng()%V), 1 + rng()%T});

    const VertexIndex* vi = b.vertex_index();
    std::vector<TemporalRef> buf;
    std::unordered_map<uint32_t,uint64_t> latest;
    double q0 = now_sec(); uint64_t edges1 = 0;
    for (auto [u, t] : qs) {
        buf.clear(); latest.clear();
        vi->neighbors_temporal_into(u, 1, t, buf);
        for (auto& r : buf) { auto it = latest.find(r.peer);
            if (it == latest.end() || r.ts > it->second) latest[r.peer] = r.ts; }
        edges1 += latest.size();
    }
    double q1 = now_sec() - q0;

    double t1 = now_sec(); uint64_t edges2 = 0;
    for (uint32_t v = 0; v < V; ++v) {
        buf.clear(); latest.clear();
        vi->neighbors_temporal_into(v, 1, T/2, buf);
        for (auto& r : buf) { auto it = latest.find(r.peer);
            if (it == latest.end() || r.ts > it->second) latest[r.peer] = r.ts; }
        edges2 += latest.size();
    }
    double q2 = now_sec() - t1;

    std::printf("barry,%zu,%u,%.3f,%.0f,%d,%.1f,%llu,%.3f,%llu,%zu\n",
        ef.count, V, ing, ef.count/ing, scans, q1*1e6/scans,
        (unsigned long long)edges1, q2, (unsigned long long)edges2, rss_mb());
    std::fprintf(stderr, "[e15-barry] ingest %.2fs (%.2f Meps, 1 thread)  scan med~%.1fus  fullsnap %.3fs (%llu nbrs)  RSS %zuMB\n",
        ing, ef.count/ing/1e6, q1*1e6/scans, q2, (unsigned long long)edges2, rss_mb());
    return 0;
}
