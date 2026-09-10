// E9: Barry vs a columnar engine on the same temporal graph, same process, same data.
//
// This is the comparison that decides whether a temporal-first LAYOUT is worth
// anything. A column store sorted by time answers time-RANGE FILTERS extremely
// well -- that is its home turf and we expect to lose there. The claim under test
// is narrower and load-bearing: multi-hop TIME-RESPECTING TRAVERSAL, where every
// hop is a fresh lookup keyed on the previous hop's vertices and times.
//
// Both query classes are measured and both are reported. Showing only the one we
// win would be worthless.
//
// DuckDB is given its best shot: rows appended in timestamp order (so zone maps
// prune), and an optional ART index on src for the traversal join.
//
// Usage:
//   bench_e9_columnar --dataset <bin> --query <filter|traverse|both>
//                     --num_samples N --window_frac F --hops K
//                     --duck_index <0|1> --trial i --output csv

#include "common.hpp"
#include "temporal_csr.hpp"
#include "barry/engine.hpp"
#include "duckdb.h"

#include <memory>
#include <random>
#include <string>

using namespace barry;
using namespace barry::bench;

namespace {

struct Duck {
    duckdb_database   db  = nullptr;
    duckdb_connection con = nullptr;

    bool open() {
        if (duckdb_open(nullptr, &db) == DuckDBError) return false;
        return duckdb_connect(db, &con) != DuckDBError;
    }
    // Runs a statement, aborting loudly on error -- a silently failing baseline
    // would make Barry look good for the wrong reason.
    bool exec(const std::string& sql, int64_t* first_cell = nullptr) {
        duckdb_result r;
        if (duckdb_query(con, sql.c_str(), &r) == DuckDBError) {
            std::fprintf(stderr, "[duckdb] %s\n  SQL: %s\n",
                         duckdb_result_error(&r), sql.c_str());
            duckdb_destroy_result(&r);
            return false;
        }
        if (first_cell) {
            *first_cell = (duckdb_row_count(&r) > 0) ? duckdb_value_int64(&r, 0, 0) : 0;
        }
        duckdb_destroy_result(&r);
        return true;
    }
    ~Duck() {
        if (con) duckdb_disconnect(&con);
        if (db)  duckdb_close(&db);
    }
};

}  // namespace

int main(int argc, char** argv) {
    Args args(argc, argv);
    std::string in_path = args.get("dataset");
    std::string qmode   = args.get("query", "both");
    int    num_samples  = static_cast<int>(args.getl("num_samples", 30));
    double window_frac  = args.getd("window_frac", 0.01);
    int    hops         = static_cast<int>(args.getl("hops", 4));
    bool   duck_index   = args.getl("duck_index", 1) != 0;
    int    trial        = static_cast<int>(args.getl("trial", 0));
    std::string detail_csv = args.get("detail", "");
    int    threads      = static_cast<int>(args.getl("threads", 0));
    std::string out_csv = args.get("output", "");
    if (in_path.empty() || out_csv.empty()) {
        std::fprintf(stderr, "usage: %s --dataset <bin> --query <filter|traverse|both> "
                     "--num_samples N --window_frac F --hops K --duck_index 0|1 "
                     "--trial i --output csv\n", args.prog().c_str());
        return 1;
    }

    EdgeFile ef;
    if (!ef.load(in_path)) return 1;
    GraphMeta meta = scan_meta(ef.edges, ef.count);
    const uint64_t span   = (meta.t_max > meta.t_min) ? meta.t_max - meta.t_min : 1;
    const uint64_t window = std::max<uint64_t>(1, static_cast<uint64_t>(span * window_frac));

    // ---------------------------------------------------------------- Barry
    GraphConfig cfg;
    cfg.t_min = meta.t_min; cfg.t_max = meta.t_max;
    cfg.num_vertices = meta.v_max + 1;
    cfg.build_reverse_index = false;
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, ef.count * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, ef.count * sizeof(EdgeRef) * 8);

    Barry b(cfg);
    double t0 = now_sec();
    b.insert_batch(ef.edges, ef.count, threads);
    b.finalize();   // bulk load then query: compact once, explicitly
    const double barry_load = now_sec() - t0;

    // --------------------------------------------------------------- DuckDB
    // ------------------------------------------------- time-sorted CSR baseline
    // The strongest simple competitor for this query. Build cost is charged.
    TemporalCsr csr;
    t0 = now_sec();
    csr.build(ef.edges, ef.count, cfg.num_vertices);
    const double csr_build = now_sec() - t0;

    Duck duck;
    if (!duck.open()) { std::fprintf(stderr, "duckdb open failed\n"); return 1; }
    if (!duck.exec("CREATE TABLE edges(src UINTEGER, dst UINTEGER, ts UBIGINT, cls FLOAT);"))
        return 1;

    t0 = now_sec();
    {
        duckdb_appender app = nullptr;
        if (duckdb_appender_create(duck.con, nullptr, "edges", &app) == DuckDBError) {
            std::fprintf(stderr, "appender_create failed\n"); return 1;
        }
        // Appending in file order preserves timestamp order for these datasets,
        // which is exactly what lets DuckDB's zone maps prune on ts.
        for (size_t i = 0; i < ef.count; ++i) {
            const Edge& e = ef.edges[i];
            duckdb_appender_begin_row(app);
            duckdb_append_uint32(app, e.src);
            duckdb_append_uint32(app, e.dst);
            duckdb_append_uint64(app, e.timestamp);
            duckdb_append_float(app, e.weight);
            duckdb_appender_end_row(app);
        }
        duckdb_appender_flush(app);
        duckdb_appender_destroy(&app);
    }
    double duck_load = now_sec() - t0;

    if (duck_index) {
        t0 = now_sec();
        if (!duck.exec("CREATE INDEX idx_src ON edges(src);")) return 1;
        duck_load += now_sec() - t0;      // index build is part of the cost of being ready
    }

    std::mt19937_64 rng(0xE9 ^ trial);
    auto rand_ts = [&]() {
        return meta.t_min + std::uniform_int_distribution<uint64_t>(0, span - 1)(rng);
    };
    const VertexIndex* vi = b.vertex_index();
    auto rand_seed = [&]() {
        for (int k = 0; k < 64; ++k) {
            uint32_t v = std::uniform_int_distribution<uint32_t>(0, meta.v_max)(rng);
            if (vi->out_degree(v) > 0) return v;
        }
        return uint32_t{0};
    };

    // Latency on this workload is heavy-tailed: most seeds taint nothing and a few
    // taint a large fraction of the graph, so a mean over a handful of samples is
    // dominated by sampling luck. Emitting one row per query lets the comparison be
    // read as latency-vs-result-size, which is what actually characterises the two
    // engines.
    std::unique_ptr<CsvWriter> detail;
    if (!detail_csv.empty()) {
        detail.reset(new CsvWriter(detail_csv,
            {"dataset","trial","query","sample","seed","window_frac",
             "result_size","barry_us","duckdb_us","speedup"}));
    }

    CsvWriter w(out_csv,
        {"dataset","trial","query","system","num_samples","window_frac","hops",
         "duck_index","median_us","p95_us","mean_us","avg_result","load_sec","total_edges"});

    auto emit = [&](const char* qname, const char* sys,
                    std::vector<double>& lat, double avg_res, double load) {
        w.row(basename_noext(in_path), trial, std::string(qname), std::string(sys),
              num_samples, window_frac, hops, (int)duck_index,
              median(lat), percentile(lat, 0.95), mean(lat), avg_res, load,
              (unsigned long long)ef.count);
        std::fprintf(stderr, "[e9] %-9s %-6s median=%10.1f us  p95=%10.1f us  avg|R|=%9.1f\n",
                     qname, sys, median(lat), percentile(lat, 0.95), avg_res);
    };

    // ------------------------------------------------- Q1: time-range filter
    // DuckDB's home turf. Reported regardless of who wins.
    if (qmode == "filter" || qmode == "both") {
        std::vector<double> lb, ld; double rb = 0, rd = 0;
        for (int i = 0; i < num_samples; ++i) {
            uint64_t lo = rand_ts(), hi = std::min(meta.t_max, lo + window);

            double a = now_sec();
            auto res = b.query_range(lo, hi);
            lb.push_back((now_sec() - a) * 1e6);
            rb += res.size();

            char sql[256];
            std::snprintf(sql, sizeof(sql),
                "SELECT count(*) FROM edges WHERE ts >= %llu AND ts <= %llu;",
                (unsigned long long)lo, (unsigned long long)hi);
            int64_t cnt = 0;
            a = now_sec();
            if (!duck.exec(sql, &cnt)) return 1;
            ld.push_back((now_sec() - a) * 1e6);
            rd += static_cast<double>(cnt);

            if (detail) {
                detail->row(basename_noext(in_path), trial, std::string("filter"), i,
                            (unsigned)0, window_frac, (unsigned long long)res.size(),
                            lb.back(), ld.back(),
                            lb.back() > 0 ? ld.back() / lb.back() : 0.0);
            }
        }
        emit("filter", "barry",  lb, rb / num_samples, barry_load);
        emit("filter", "duckdb", ld, rd / num_samples, duck_load);
    }

    // -------------------------------------------- Q2: time-respecting traversal
    // K hops of frontier expansion. In SQL this is the natural formulation: each
    // hop joins the frontier against edges, constrained to timestamps at or after
    // the time the frontier vertex was reached.
    if (qmode == "traverse" || qmode == "both") {
        std::vector<double> lb, ld, lc; double rb = 0, rd = 0, rc = 0;

        if (!duck.exec("CREATE TABLE frontier(v UINTEGER, t UBIGINT);") ||
            !duck.exec("CREATE TABLE visited(v UINTEGER, t UBIGINT);")) return 1;

        for (int i = 0; i < num_samples; ++i) {
            uint32_t seed = rand_seed();
            uint64_t t_lo = rand_ts();
            uint64_t t_hi = std::min(meta.t_max, t_lo + window);

            // ---- Barry
            double a = now_sec();
            auto res = b.forward_taint(seed, t_lo, t_hi);
            lb.push_back((now_sec() - a) * 1e6);
            rb += res.size();

            // ---- time-sorted CSR
            a = now_sec();
            auto cres = csr.forward_taint(seed, t_lo, t_hi);
            lc.push_back((now_sec() - a) * 1e6);
            rc += cres.size();

            // ---- DuckDB
            char sql[1024];
            a = now_sec();
            if (!duck.exec("DELETE FROM frontier;") || !duck.exec("DELETE FROM visited;")) return 1;
            std::snprintf(sql, sizeof(sql),
                "INSERT INTO frontier VALUES (%u, %llu);",
                seed, (unsigned long long)t_lo);
            if (!duck.exec(sql)) return 1;
            if (!duck.exec("INSERT INTO visited SELECT * FROM frontier;")) return 1;

            int64_t reached = 1;
            for (int h = 0; h < hops; ++h) {
                std::snprintf(sql, sizeof(sql),
                    "CREATE OR REPLACE TEMP TABLE nxt AS "
                    "SELECT e.dst AS v, min(e.ts) AS t "
                    "FROM edges e JOIN frontier f ON e.src = f.v "
                    "WHERE e.ts >= f.t AND e.ts <= %llu "
                    "GROUP BY e.dst;", (unsigned long long)t_hi);
                if (!duck.exec(sql)) return 1;
                // Keep only vertices not already reached at an equal-or-earlier time.
                if (!duck.exec(
                    "CREATE OR REPLACE TEMP TABLE fresh AS "
                    "SELECT n.v, n.t FROM nxt n LEFT JOIN visited s ON n.v = s.v "
                    "WHERE s.v IS NULL OR n.t < s.t;")) return 1;
                int64_t n_fresh = 0;
                if (!duck.exec("SELECT count(*) FROM fresh;", &n_fresh)) return 1;
                if (n_fresh == 0) break;
                if (!duck.exec("INSERT INTO visited SELECT * FROM fresh;") ||
                    !duck.exec("DELETE FROM frontier;") ||
                    !duck.exec("INSERT INTO frontier SELECT * FROM fresh;")) return 1;
            }
            if (!duck.exec("SELECT count(DISTINCT v) FROM visited;", &reached)) return 1;
            ld.push_back((now_sec() - a) * 1e6);
            rd += static_cast<double>(reached);

            if (detail) {
                detail->row(basename_noext(in_path), trial, std::string("traverse"), i,
                            seed, window_frac, (unsigned long long)res.size(),
                            lb.back(), ld.back(),
                            lb.back() > 0 ? ld.back() / lb.back() : 0.0);
            }
        }
        emit("traverse", "barry",   lb, rb / num_samples, barry_load);
        emit("traverse", "tcsr",    lc, rc / num_samples, csr_build);
        emit("traverse", "duckdb",  ld, rd / num_samples, duck_load);
    }

    std::fprintf(stderr,
        "[e9] build/load: barry=%.2fs  tcsr=%.2fs (full sort)  duckdb=%.2fs (index=%d)\n",
        barry_load, csr_build, duck_load, (int)duck_index);
    return 0;
}
