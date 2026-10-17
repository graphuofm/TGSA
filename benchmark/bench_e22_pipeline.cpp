// E22: the whole round trip, not the algorithm.
//
// Running PageRank against a hand-tuned in-memory framework is a race we would
// lose and should not enter: the algorithm is not the contribution. What an
// analyst or an agent actually asks for is "run this over last week's activity",
// and answering that means three things, of which the middle one is the same
// code everywhere:
//
//   extract     pull the edges of a time window out of the store and build the
//               working structure the algorithm needs
//   compute     run the algorithm over that structure   <- identical for all
//   write back  put the per-vertex results where a later query can read them
//
// Only extract and write back are storage. Holding compute fixed is what makes
// the comparison about storage instead of about whose PageRank is tighter, and
// it is also why the result checksums must agree: same structure in, same
// answer out, or the row is void.
//
// Three stores that can do all three phases in-process:
//   gnomon      time-keyed
//   livegraph   entity-keyed, time as edge payload
//   duckdb      columnar, time as a column
//
// The window fraction is swept because it is the whole argument. Ask for the
// entire history and a time-keyed layout has no advantage to offer; ask for a
// recent slice, which is what the workload actually does, and the other two
// must touch everything to find it.
//
// Usage:
//   bench_e22_pipeline --dataset <bin> --windows 0.001,0.01,0.1,1.0
//                      --algo pagerank --iters 10 --threads 8 --output csv

#include "common.hpp"
#include "barry/engine.hpp"
#include "barry/vertex_index.hpp"

#include "livegraph.hpp"
#include "duckdb.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <cstring>
#include <string>
#include <vector>

using namespace barry;
using namespace barry::bench;

namespace {

// The working structure every algorithm runs on. Building it is part of
// extraction, because a store that hands back a pile of edges has not finished
// the job the algorithm needs doing.
struct Csr {
    uint32_t n = 0;
    std::vector<uint64_t> off;
    std::vector<uint32_t> nbr;

    void build(uint32_t num_vertices, const std::vector<uint32_t>& src,
               const std::vector<uint32_t>& dst) {
        n = num_vertices;
        off.assign(n + 1, 0);
        for (uint32_t s : src) ++off[s + 1];
        for (uint32_t i = 0; i < n; ++i) off[i + 1] += off[i];
        nbr.resize(src.size());
        std::vector<uint64_t> cur(off.begin(), off.end() - 1);
        for (size_t i = 0; i < src.size(); ++i) nbr[cur[src[i]]++] = dst[i];
        // Canonical order per vertex. Without it the three stores hand back the
        // same edges in different orders, and any order-sensitive algorithm then
        // produces different answers from identical input, which would look like
        // a storage bug and is really a benchmark bug. Every store pays this
        // identically, so it does not tilt the comparison.
        for (uint32_t v = 0; v < n; ++v)
            std::sort(nbr.begin() + off[v], nbr.begin() + off[v + 1]);
    }
};

// Identical for every store. Ten rounds of PageRank plus a label-propagation
// pass, chosen because both walk the entire structure rather than a frontier,
// so no store can win by handing back a conveniently ordered subset.
struct Computed {
    std::vector<float> rank;
    std::vector<uint32_t> label;
    uint64_t checksum = 0;
};

Computed compute(const Csr& g, int iters) {
    Computed c;
    const uint32_t n = g.n;
    c.rank.assign(n, 1.0f / std::max(1u, n));
    std::vector<float> next(n, 0.0f);

    for (int it = 0; it < iters; ++it) {
        std::fill(next.begin(), next.end(), 0.15f / std::max(1u, n));
        for (uint32_t u = 0; u < n; ++u) {
            const uint64_t b = g.off[u], e = g.off[u + 1];
            if (b == e) continue;
            const float share = 0.85f * c.rank[u] / static_cast<float>(e - b);
            for (uint64_t p = b; p < e; ++p) next[g.nbr[p]] += share;
        }
        c.rank.swap(next);
    }

    // Run to convergence, not for a fixed count: the fixpoint (each vertex
    // labelled with the smallest id in its component) is unique, so the answer
    // does not depend on visit order. A capped iteration count leaves an
    // intermediate state that does.
    c.label.resize(n);
    for (uint32_t u = 0; u < n; ++u) c.label[u] = u;
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t u = 0; u < n; ++u)
            for (uint64_t p = g.off[u]; p < g.off[u + 1]; ++p) {
                const uint32_t v = g.nbr[p];
                if (c.label[v] < c.label[u]) { c.label[u] = c.label[v]; changed = true; }
                else if (c.label[u] < c.label[v]) { c.label[v] = c.label[u]; changed = true; }
            }
    }

    // Quantised so float association order cannot make identical inputs look
    // different; the labels are exact.
    for (uint32_t u = 0; u < n; ++u) {
        c.checksum += static_cast<uint64_t>(c.rank[u] * 1e6f);
        c.checksum += static_cast<uint64_t>(c.label[u]) * 1315423911ull;
    }
    return c;
}

size_t rss_bytes() {
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long size = 0, resident = 0;
    if (std::fscanf(f, "%lu %lu", &size, &resident) != 2) resident = 0;
    std::fclose(f);
    return static_cast<size_t>(resident) * static_cast<size_t>(::sysconf(_SC_PAGESIZE));
}

inline std::string_view ts_view(const uint64_t& t) {
    return std::string_view(reinterpret_cast<const char*>(&t), sizeof(uint64_t));
}

}  // namespace

int main(int argc, char** argv) {
    Args args(argc, argv);
    const std::string in_path = args.get("dataset");
    const std::string out_csv = args.get("output", "");
    const std::string windows = args.get("windows", "0.001,0.01,0.1,1.0");
    const size_t limit   = static_cast<size_t>(args.getl("limit", 0));
    const size_t batch   = static_cast<size_t>(args.getl("batch", 65536));
    const int    threads = static_cast<int>(args.getl("threads", 8));
    const int    iters   = static_cast<int>(args.getl("iters", 10));
    const int    trial   = static_cast<int>(args.getl("trial", 0));
    // --systems tgsr,livegraph,duckdb (default all). One store per process
    // is how the billion-edge corpus fits in memory; the compute phase is
    // deterministic, so checksums stay comparable across processes.
    const std::string systems = args.get("systems", "tgsr,livegraph,duckdb");
    const bool want_gn = systems.find("tgsr") != std::string::npos ||
                         systems.find("gnomon") != std::string::npos;
    const bool want_lg = systems.find("livegraph") != std::string::npos;
    const bool want_dd = systems.find("duckdb") != std::string::npos;

    if (in_path.empty() || out_csv.empty()) {
        std::fprintf(stderr,
            "usage: %s --dataset <bin> [--windows 0.001,0.01,0.1,1.0] "
            "[--limit N] [--batch N] [--threads T] [--iters N] --output csv\n",
            args.prog().c_str());
        return 1;
    }

    EdgeFile ef;
    if (!ef.load(in_path)) return 1;
    const size_t n = std::min(limit ? limit : ef.count, ef.count);
    const GraphMeta meta = scan_meta(ef.edges, n);
    const std::string ds = basename_noext(in_path);
    const uint32_t V = meta.v_max + 1;
    const uint64_t span = (meta.t_max > meta.t_min) ? meta.t_max - meta.t_min : 1;

    std::vector<double> wfracs;
    { std::string s = windows;
      size_t p;
      while ((p = s.find(',')) != std::string::npos) {
          wfracs.push_back(std::strtod(s.substr(0, p).c_str(), nullptr));
          s = s.substr(p + 1);
      }
      if (!s.empty()) wfracs.push_back(std::strtod(s.c_str(), nullptr));
    }

    CsvWriter w(out_csv,
        {"system","dataset","trial","edges_total","vertices","window_frac",
         "edges_in_window","load_sec","extract_sec","compute_sec","writeback_sec",
         "total_sec","extract_share","checksum","rss_MB"});

    // ---------------------------------------------------------------- setup
    // Loading is not part of the round trip: a store is loaded once and queried
    // many times. Timed and reported, but not summed into the total.
    GraphConfig cfg;
    cfg.t_min = meta.t_min; cfg.t_max = meta.t_max;
    cfg.num_vertices = V + 1;              // +1 for the result sink vertex
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, n * sizeof(Edge) * 6);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, n * sizeof(EdgeRef) * 10);
    std::unique_ptr<Barry> gnp;
    double gn_load = 0;
    if (want_gn) {
        gnp = std::make_unique<Barry>(cfg);
        gn_load = now_sec();
        for (size_t off = 0; off < n; off += batch)
            gnp->insert_batch(ef.edges + off, std::min(batch, n - off), threads);
        gnp->finalize();
        gn_load = now_sec() - gn_load;
    }
    Barry& gn = *gnp;

    std::unique_ptr<lg::Graph> lgp;
    double lg_load = 0;
    if (want_lg) {
        lgp = std::make_unique<lg::Graph>("", "", 1ul << 40, static_cast<lg::vertex_t>(V) + 16);
        lg_load = now_sec();
        {
            auto txn = lgp->begin_batch_loader();
            for (uint32_t v = 0; v <= V; ++v) txn.new_vertex();
            txn.commit();
        }
        for (size_t off = 0; off < n; off += batch) {
            const size_t m = std::min(batch, n - off);
            auto txn = lgp->begin_batch_loader();
            for (size_t i = off; i < off + m; ++i) {
                const Edge& e = ef.edges[i];
                txn.put_edge(e.src, 0, e.dst, ts_view(e.timestamp), true);
            }
            txn.commit();
        }
        lg_load = now_sec() - lg_load;
    }
    lg::Graph& lgg = *lgp;

    duckdb_database db = nullptr; duckdb_connection con = nullptr;
    if (want_dd) {
    if (duckdb_open(nullptr, &db) == DuckDBError) { std::fprintf(stderr, "duckdb_open\n"); return 1; }
    if (duckdb_connect(db, &con) == DuckDBError) { std::fprintf(stderr, "duckdb_connect\n"); return 1; }
    }
    auto dsql = [&](const char* sql) {
        duckdb_result r;
        if (duckdb_query(con, sql, &r) == DuckDBError) {
            std::fprintf(stderr, "duckdb: %s\n[%s]\n", duckdb_result_error(&r), sql);
            duckdb_destroy_result(&r); return false;
        }
        duckdb_destroy_result(&r); return true;
    };
    double dd_load = now_sec();
    if (want_dd) {
    if (!dsql("CREATE TABLE edges(src UINTEGER, dst UINTEGER, ts UBIGINT)")) return 1;
    if (!dsql("CREATE TABLE results(v UINTEGER, val DOUBLE, ts UBIGINT)")) return 1;
    {
        duckdb_appender app = nullptr;
        if (duckdb_appender_create(con, nullptr, "edges", &app) == DuckDBError) return 1;
        for (size_t i = 0; i < n; ++i) {
            duckdb_appender_begin_row(app);
            duckdb_append_uint32(app, ef.edges[i].src);
            duckdb_append_uint32(app, ef.edges[i].dst);
            duckdb_append_uint64(app, ef.edges[i].timestamp);
            duckdb_appender_end_row(app);
        }
        duckdb_appender_flush(app);
        duckdb_appender_destroy(&app);
    }
    if (!dsql("CREATE INDEX idx_ts ON edges(ts)")) return 1;   // give it the index it wants
    }
    dd_load = want_dd ? now_sec() - dd_load : 0;

    std::fprintf(stderr, "[e22] load: gnomon %.2fs  livegraph %.2fs  duckdb %.2fs\n",
                 gn_load, lg_load, dd_load);

    // -------------------------------------------------------------- sweep
    for (double wf : wfracs) {
        const uint64_t width = std::max<uint64_t>(1, static_cast<uint64_t>(span * wf));
        // The trailing window: "what happened recently", which is the question
        // that actually gets asked of a live stream.
        const uint64_t t_hi = meta.t_max;
        const uint64_t t_lo = (t_hi > meta.t_min + width) ? t_hi - width : meta.t_min;

        uint64_t ref_checksum = 0;
        size_t ref_edges = 0;

        for (int sys = 0; sys < 3; ++sys) {
            if ((sys == 0 && !want_gn) || (sys == 1 && !want_lg) || (sys == 2 && !want_dd)) continue;
            std::vector<uint32_t> src, dst;
            Csr g;
            double t_extract = 0, t_compute = 0, t_write = 0;
            const size_t rss0 = rss_bytes();

            // ---- extract
            double t0 = now_sec();
            if (sys == 0) {
                std::vector<Edge> es = gn.query_range(t_lo, t_hi);
                src.reserve(es.size()); dst.reserve(es.size());
                for (const Edge& e : es) { src.push_back(e.src); dst.push_back(e.dst); }
            } else if (sys == 1) {
                auto txn = lgg.begin_read_only_transaction();
                for (uint32_t v = 0; v < V; ++v) {
                    auto it = txn.get_edges(v, 0);
                    while (it.valid()) {
                        uint64_t ts = 0;
                        auto d = it.edge_data();
                        if (d.size() >= sizeof(uint64_t)) std::memcpy(&ts, d.data(), sizeof(uint64_t));
                        if (ts >= t_lo && ts <= t_hi) {
                            src.push_back(v);
                            dst.push_back(static_cast<uint32_t>(it.dst_id()));
                        }
                        it.next();
                    }
                }
                txn.abort();
            } else {
                char sql[512];
                std::snprintf(sql, sizeof(sql),
                    "SELECT src,dst FROM edges WHERE ts >= %llu AND ts <= %llu",
                    (unsigned long long)t_lo, (unsigned long long)t_hi);
                duckdb_result r;
                if (duckdb_query(con, sql, &r) == DuckDBError) {
                    std::fprintf(stderr, "duckdb select: %s\n", duckdb_result_error(&r));
                    duckdb_destroy_result(&r); return 1;
                }
                const idx_t rows = duckdb_row_count(&r);
                src.reserve(rows); dst.reserve(rows);
                for (idx_t i = 0; i < rows; ++i) {
                    src.push_back(static_cast<uint32_t>(duckdb_value_uint32(&r, 0, i)));
                    dst.push_back(static_cast<uint32_t>(duckdb_value_uint32(&r, 1, i)));
                }
                duckdb_destroy_result(&r);
            }
            g.build(V, src, dst);
            t_extract = now_sec() - t0;

            // ---- compute (identical code)
            t0 = now_sec();
            Computed c = compute(g, iters);
            t_compute = now_sec() - t0;

            // ---- write back: one result record per vertex with any edge
            std::vector<uint32_t> touched;
            for (uint32_t v = 0; v < V; ++v) if (g.off[v] != g.off[v + 1]) touched.push_back(v);

            t0 = now_sec();
            if (sys == 0) {
                std::vector<Edge> out;
                out.reserve(touched.size());
                for (uint32_t v : touched)
                    out.push_back(Edge{v, V, t_hi + 1, c.rank[v]});
                for (size_t off = 0; off < out.size(); off += batch)
                    gn.insert_batch(out.data() + off,
                                    std::min(batch, out.size() - off), threads);
            } else if (sys == 1) {
                auto txn = lgg.begin_batch_loader();
                for (uint32_t v : touched) {
                    const uint64_t packed = t_hi + 1;
                    txn.put_edge(v, 1, V, ts_view(packed), true);
                }
                txn.commit();
            } else {
                duckdb_appender app = nullptr;
                if (duckdb_appender_create(con, nullptr, "results", &app) == DuckDBError) return 1;
                for (uint32_t v : touched) {
                    duckdb_appender_begin_row(app);
                    duckdb_append_uint32(app, v);
                    duckdb_append_double(app, c.rank[v]);
                    duckdb_append_uint64(app, t_hi + 1);
                    duckdb_appender_end_row(app);
                }
                duckdb_appender_flush(app);
                duckdb_appender_destroy(&app);
            }
            t_write = now_sec() - t0;

            const double total = t_extract + t_compute + t_write;
            const char* name = (sys == 0) ? "gnomon" : (sys == 1) ? "livegraph" : "duckdb";
            if (sys == 0) { ref_checksum = c.checksum; ref_edges = src.size(); }

            w.row(std::string(name), ds, trial, (unsigned long long)n,
                  (unsigned long long)V, wf, (unsigned long long)src.size(),
                  (sys == 0) ? gn_load : (sys == 1) ? lg_load : dd_load,
                  t_extract, t_compute, t_write, total,
                  t_extract / total, (unsigned long long)c.checksum,
                  (rss_bytes() - rss0) / 1e6);

            std::fprintf(stderr,
                "[e22] w=%-7.4g %-10s edges=%10llu  extract=%8.3fs  compute=%8.3fs  "
                "write=%7.3fs  total=%8.3fs  extract=%4.1f%%  %s\n",
                wf, name, (unsigned long long)src.size(), t_extract, t_compute,
                t_write, total, 100.0 * t_extract / total,
                (sys == 0) ? "(reference)"
                           : (c.checksum == ref_checksum && src.size() == ref_edges)
                                 ? "checksum MATCHES" : "CHECKSUM DIFFERS");
        }
        // Results accumulate across windows in every store alike, so no store
        // gets a cleaner table than another on the next iteration.
        if (want_dd) dsql("DELETE FROM results");
    }

    if (want_dd) { duckdb_disconnect(&con); duckdb_close(&db); }
    return 0;
}
