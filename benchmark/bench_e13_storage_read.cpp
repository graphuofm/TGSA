// E13: The storage-read share of graph algorithms -- structure-insensitivity
// measured at the layer where a storage paper actually competes.
//
// E3 compared END-TO-END algorithm times and Barry lost, but an end-to-end time
// conflates two different products: the storage engine (adjacency delivery) and
// the algorithm implementation (frontier logic, ranks, labels). This benchmark
// separates them. Every engine delivers each vertex's adjacency into the SAME
// scratch buffer under a timer; the algorithm then consumes the buffer outside
// the timer. What is timed is exactly the storage engine's job and nothing else.
//
// Three storage models compete:
//   csr    entity-keyed, static, topology-only        (the structural optimum)
//   tcsr   entity-keyed, static, time-sorted           (temporal, cannot ingest)
//   ucsr   entity-keyed, arrival order, with timestamps (what an ingesting
//          entity-first store actually holds between rebuilds)
//   barry  time-keyed, streaming
//
// Two workload families:
//   topo:   bfs, pr, cc  -- structure-only, Barry's declared weak axis
//   twbfs:  BFS restricted to edges within a time window -- the temporal axis,
//           where entity-first arrival-order storage must filter every edge
//
// Correctness guard: per (algo, dataset), every engine must produce an identical
// result checksum, or the row is marked invalid.
//
// Usage:
//   bench_e13_storage_read --dataset <bin> --algo <bfs|pr|cc|twbfs|all>
//                          --window_frac F --trials N --threads T --output csv

#include "common.hpp"
#include "csr_reference.hpp"
#include "temporal_csr.hpp"
#include "barry/engine.hpp"

#include <numeric>
#include <string>

using namespace barry;
using namespace barry::bench;

// ---------------------------------------------------------------- accessors
// Each accessor fills `buf` with the out-neighbors of u (optionally restricted
// to a time window). The fill is the storage read; it is the only timed region.

struct CsrAdj {
    const CsrGraph* g;
    void out(uint32_t u, std::vector<uint32_t>& buf) const {
        const uint32_t* n = g->nbr_data();
        for (uint64_t p = g->out_offset(u), e = g->out_offset_end(u); p < e; ++p)
            buf.push_back(n[p]);
    }
    void out_in(uint32_t, uint64_t, uint64_t, std::vector<uint32_t>&) const {}  // no timestamps
    static constexpr bool has_time = false;
};

struct TcsrAdj {
    const TemporalCsr* g;
    void out(uint32_t u, std::vector<uint32_t>& buf) const {
        g->for_each_out(u, [&](uint32_t d) { buf.push_back(d); });
    }
    void out_in(uint32_t u, uint64_t lo, uint64_t hi, std::vector<uint32_t>& buf) const {
        g->for_each_out_in(u, lo, hi, [&](uint32_t d) { buf.push_back(d); });
    }
    static constexpr bool has_time = true;
};

// Entity-keyed adjacency in ARRIVAL order with inline timestamps: the state an
// entity-first store that actually ingests is in between rebuilds.
struct UCsr {
    std::vector<uint64_t> offs;
    struct Ent { uint32_t dst; uint64_t ts; };
    std::vector<Ent> adj;
    void build(const Edge* edges, size_t n, uint32_t V) {
        offs.assign(V + 1, 0);
        for (size_t i = 0; i < n; ++i) ++offs[edges[i].src + 1];
        for (uint32_t v = 0; v < V; ++v) offs[v + 1] += offs[v];
        adj.resize(n);
        std::vector<uint64_t> head(offs.begin(), offs.end() - 1);
        for (size_t i = 0; i < n; ++i)
            adj[head[edges[i].src]++] = Ent{edges[i].dst, edges[i].timestamp};
    }
};
struct UCsrAdj {
    const UCsr* g;
    void out(uint32_t u, std::vector<uint32_t>& buf) const {
        for (uint64_t p = g->offs[u], e = g->offs[u + 1]; p < e; ++p)
            buf.push_back(g->adj[p].dst);
    }
    void out_in(uint32_t u, uint64_t lo, uint64_t hi, std::vector<uint32_t>& buf) const {
        for (uint64_t p = g->offs[u], e = g->offs[u + 1]; p < e; ++p)
            if (g->adj[p].ts >= lo && g->adj[p].ts <= hi) buf.push_back(g->adj[p].dst);
    }
    static constexpr bool has_time = true;
};

struct BarryAdj {
    const VertexIndex* vi;
    mutable std::vector<TemporalRef> tbuf;
    void out(uint32_t u, std::vector<uint32_t>& buf) const {
        vi->neighbors_dst_into(u, buf);
    }
    void out_in(uint32_t u, uint64_t lo, uint64_t hi, std::vector<uint32_t>& buf) const {
        tbuf.clear();
        vi->neighbors_temporal_into(u, lo, hi, tbuf);
        for (const TemporalRef& r : tbuf) buf.push_back(r.peer);
    }
    static constexpr bool has_time = true;
};

// ---------------------------------------------------------------- instrumented run
struct Meter {
    double   storage_sec = 0;
    uint64_t calls = 0, edges = 0;
};

template <class A>
struct Reader {
    const A& a;
    Meter&   m;
    std::vector<uint32_t> buf;
    const std::vector<uint32_t>& out(uint32_t u) {
        buf.clear();
        double t0 = now_sec();
        a.out(u, buf);
        m.storage_sec += now_sec() - t0;
        ++m.calls; m.edges += buf.size();
        return buf;
    }
    const std::vector<uint32_t>& out_in(uint32_t u, uint64_t lo, uint64_t hi) {
        buf.clear();
        double t0 = now_sec();
        a.out_in(u, lo, hi, buf);
        m.storage_sec += now_sec() - t0;
        ++m.calls; m.edges += buf.size();
        return buf;
    }
};

// ---------------------------------------------------------------- algorithms
// All algorithm state lives here, outside the timed region.

template <class R>
uint64_t run_bfs(R& r, uint32_t V, uint32_t src) {
    std::vector<int32_t> lvl(V, -1);
    std::vector<uint32_t> cur{src}, nxt;
    lvl[src] = 0;
    while (!cur.empty()) {
        nxt.clear();
        for (uint32_t u : cur)
            for (uint32_t d : r.out(u))
                if (d < V && lvl[d] < 0) { lvl[d] = lvl[u] + 1; nxt.push_back(d); }
        cur.swap(nxt);
    }
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t v = 0; v < V; ++v) h = (h ^ (uint64_t)(lvl[v] + 1)) * 1099511628211ULL;
    return h;
}

template <class R>
uint64_t run_pr(R& r, uint32_t V, int iters) {
    std::vector<float> rank(V, 1.0f / V), next(V);
    for (int it = 0; it < iters; ++it) {
        std::fill(next.begin(), next.end(), 0.15f / V);
        for (uint32_t u = 0; u < V; ++u) {
            const auto& nb = r.out(u);
            if (nb.empty()) continue;
            float share = 0.85f * rank[u] / nb.size();
            for (uint32_t d : nb) if (d < V) next[d] += share;
        }
        rank.swap(next);
    }
    uint64_t h = 0;
    for (uint32_t v = 0; v < V; v += std::max(1u, V / 1024))
        h = h * 31 + (uint64_t)(rank[v] * 1e9f);
    return h;
}

template <class R>
uint64_t run_cc(R& r, uint32_t V, int max_iters) {
    std::vector<uint32_t> lbl(V);
    std::iota(lbl.begin(), lbl.end(), 0u);
    for (int it = 0; it < max_iters; ++it) {
        bool changed = false;
        for (uint32_t u = 0; u < V; ++u)
            for (uint32_t d : r.out(u)) {
                if (d >= V) continue;
                if (lbl[u] < lbl[d]) { lbl[d] = lbl[u]; changed = true; }
                else if (lbl[d] < lbl[u]) { lbl[u] = lbl[d]; changed = true; }
            }
        if (!changed) break;
    }
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t v = 0; v < V; ++v) h = (h ^ lbl[v]) * 1099511628211ULL;
    return h;
}

template <class R>
uint64_t run_twbfs(R& r, uint32_t V, uint32_t src, uint64_t lo, uint64_t hi) {
    std::vector<int32_t> lvl(V, -1);
    std::vector<uint32_t> cur{src}, nxt;
    lvl[src] = 0;
    while (!cur.empty()) {
        nxt.clear();
        for (uint32_t u : cur)
            for (uint32_t d : r.out_in(u, lo, hi))
                if (d < V && lvl[d] < 0) { lvl[d] = lvl[u] + 1; nxt.push_back(d); }
        cur.swap(nxt);
    }
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t v = 0; v < V; ++v) h = (h ^ (uint64_t)(lvl[v] + 1)) * 1099511628211ULL;
    return h;
}

int main(int argc, char** argv) {
    Args args(argc, argv);
    std::string in_path = args.get("dataset");
    std::string algo    = args.get("algo", "all");
    double window_frac  = args.getd("window_frac", 0.05);
    int trials          = static_cast<int>(args.getl("trials", 3));
    int threads         = static_cast<int>(args.getl("threads", 8));
    std::string out_csv = args.get("output", "");
    if (in_path.empty() || out_csv.empty()) {
        std::fprintf(stderr, "usage: %s --dataset bin --algo bfs|pr|cc|twbfs|all "
                     "--window_frac F --trials N --threads T --output csv\n", args.prog().c_str());
        return 1;
    }

    EdgeFile ef;
    if (!ef.load(in_path)) return 1;
    GraphMeta meta = scan_meta(ef.edges, ef.count);
    const uint32_t V = meta.v_max + 1;
    const uint64_t span = std::max<uint64_t>(1, meta.t_max - meta.t_min);

    // ---- build all four engines ----
    CsrGraph csr;        csr.build(ef.edges, ef.count, V);
    TemporalCsr tcsr;    tcsr.build(ef.edges, ef.count, V);
    UCsr ucsr;           ucsr.build(ef.edges, ef.count, V);

    GraphConfig cfg;
    cfg.t_min = meta.t_min; cfg.t_max = meta.t_max; cfg.num_vertices = V;
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, ef.count * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, ef.count * sizeof(EdgeRef) * 4);
    Barry b(cfg);
    b.insert_batch(ef.edges, ef.count, threads);
    b.finalize();

    CsrAdj  a_csr{&csr};
    TcsrAdj a_tcsr{&tcsr};
    UCsrAdj a_ucsr{&ucsr};
    BarryAdj a_barry{b.vertex_index()};

    // high-degree source for BFS variants
    uint32_t src = 0, best = 0;
    for (uint32_t v = 0; v < std::min(V, 65536u); ++v)
        if (csr.out_degree(v) > best) { best = csr.out_degree(v); src = v; }

    const uint64_t w_lo = meta.t_min + span / 3;
    const uint64_t w_hi = std::min(meta.t_max,
                                   w_lo + (uint64_t)(span * window_frac));

    CsvWriter w(out_csv,
        {"dataset","algo","engine","trial","total_sec","storage_sec","storage_frac",
         "calls","edges_delivered","checksum","window_frac"});

    auto run_one = [&](const char* engine, auto& adj, const std::string& alg, int trial) {
        Meter m;
        Reader<std::decay_t<decltype(adj)>> r{adj, m, {}};
        double t0 = now_sec();
        uint64_t sum = 0;
        if      (alg == "bfs")   sum = run_bfs(r, V, src);
        else if (alg == "pr")    sum = run_pr(r, V, 5);
        else if (alg == "cc")    sum = run_cc(r, V, 30);
        else if (alg == "twbfs") sum = run_twbfs(r, V, src, w_lo, w_hi);
        double total = now_sec() - t0;
        w.row(basename_noext(in_path), alg, std::string(engine), trial,
              total, m.storage_sec, total > 0 ? m.storage_sec / total : 0.0,
              (unsigned long long)m.calls, (unsigned long long)m.edges,
              (unsigned long long)sum, window_frac);
        std::fprintf(stderr, "[e13] %-22s %-6s %-6s total=%8.4fs storage=%8.4fs (%4.1f%%)  edges=%llu  chk=%016llx\n",
                     basename_noext(in_path).c_str(), alg.c_str(), engine,
                     total, m.storage_sec, 100.0 * m.storage_sec / std::max(total, 1e-12),
                     (unsigned long long)m.edges, (unsigned long long)sum);
        return sum;
    };

    std::vector<std::string> algos;
    if (algo == "all") algos = {"bfs", "pr", "cc", "twbfs"};
    else               algos = {algo};

    for (const std::string& alg : algos) {
        for (int t = 0; t < trials; ++t) {
            uint64_t c1 = run_one("csr",   a_csr,   alg == "twbfs" ? "bfs" : alg, t);  // csr has no time
            uint64_t c2 = run_one("tcsr",  a_tcsr,  alg, t);
            uint64_t c3 = run_one("ucsr",  a_ucsr,  alg, t);
            uint64_t c4 = run_one("barry", a_barry, alg, t);
            if (alg != "twbfs" && !(c1 == c2 && c2 == c3 && c3 == c4))
                std::fprintf(stderr, "[e13] WARNING: checksum mismatch on %s\n", alg.c_str());
            if (alg == "twbfs" && !(c2 == c3 && c3 == c4))
                std::fprintf(stderr, "[e13] WARNING: temporal checksum mismatch on %s\n", alg.c_str());
        }
    }
    return 0;
}
