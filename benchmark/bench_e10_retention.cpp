// E10: Lifecycle management -- what does it cost to forget?
//
// Every long-running telemetry store has a retention policy: keep the last N days
// hot, drop or tier the rest. In an agent-era deployment this is not a background
// chore, it is continuous -- most probes are speculative and short-lived, while a
// small fraction of state must persist.
//
// Retention is a monotone time predicate, which is exactly the predicate class
// Barry's address mapping makes free. Dropping "everything before T" is releasing a
// prefix of the timeline; nothing scans the edge set. A store keyed on entity id
// has no such prefix: eviction must find the rows, wherever they live.
//
// Usage:
//   bench_e10_retention --dataset <bin> --drop_frac 0.5 --threads T
//                       --trial i --output csv

#include "common.hpp"
#include "barry/engine.hpp"
#include "duckdb.h"

#include <string>

using namespace barry;
using namespace barry::bench;

namespace {
struct Duck {
    duckdb_database db = nullptr; duckdb_connection con = nullptr;
    bool open() {
        return duckdb_open(nullptr, &db) != DuckDBError &&
               duckdb_connect(db, &con) != DuckDBError;
    }
    bool exec(const std::string& sql, int64_t* cell = nullptr) {
        duckdb_result r;
        if (duckdb_query(con, sql.c_str(), &r) == DuckDBError) {
            std::fprintf(stderr, "[duckdb] %s\n", duckdb_result_error(&r));
            duckdb_destroy_result(&r); return false;
        }
        if (cell) *cell = duckdb_row_count(&r) ? duckdb_value_int64(&r, 0, 0) : 0;
        duckdb_destroy_result(&r); return true;
    }
    ~Duck() { if (con) duckdb_disconnect(&con); if (db) duckdb_close(&db); }
};
}  // namespace

int main(int argc, char** argv) {
    Args args(argc, argv);
    std::string in_path = args.get("dataset");
    double drop_frac    = args.getd("drop_frac", 0.5);
    int    threads      = static_cast<int>(args.getl("threads", 0));
    int    trial        = static_cast<int>(args.getl("trial", 0));
    std::string out_csv = args.get("output", "");
    if (in_path.empty() || out_csv.empty()) {
        std::fprintf(stderr, "usage: %s --dataset <bin> --drop_frac F --threads T "
                     "--trial i --output csv\n", args.prog().c_str());
        return 1;
    }

    EdgeFile ef;
    if (!ef.load(in_path)) return 1;
    GraphMeta meta = scan_meta(ef.edges, ef.count);
    const uint64_t span   = (meta.t_max > meta.t_min) ? meta.t_max - meta.t_min : 1;
    const uint64_t cutoff = meta.t_min + static_cast<uint64_t>(span * drop_frac);

    size_t expect_dropped = 0;
    for (size_t i = 0; i < ef.count; ++i) if (ef.edges[i].timestamp < cutoff) ++expect_dropped;

    // ---------------------------------------------------------------- Barry
    GraphConfig cfg;
    cfg.t_min = meta.t_min; cfg.t_max = meta.t_max;
    cfg.num_vertices = meta.v_max + 1;
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, ef.count * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, ef.count * sizeof(EdgeRef) * 4);
    Barry b(cfg);
    b.insert_batch(ef.edges, ef.count, threads);

    Barry::EvictStats ev = b.evict_before(cutoff);

    // --------------------------------------------------------------- DuckDB
    Duck duck;
    if (!duck.open()) return 1;
    if (!duck.exec("CREATE TABLE edges(src UINTEGER, dst UINTEGER, ts UBIGINT, cls FLOAT);"))
        return 1;
    {
        duckdb_appender app = nullptr;
        if (duckdb_appender_create(duck.con, nullptr, "edges", &app) == DuckDBError) return 1;
        for (size_t i = 0; i < ef.count; ++i) {
            const Edge& e = ef.edges[i];
            duckdb_appender_begin_row(app);
            duckdb_append_uint32(app, e.src);   duckdb_append_uint32(app, e.dst);
            duckdb_append_uint64(app, e.timestamp); duckdb_append_float(app, e.weight);
            duckdb_appender_end_row(app);
        }
        duckdb_appender_flush(app); duckdb_appender_destroy(&app);
    }

    // Speed is only half the question. The other half is whether the memory comes
    // back: a delete that only marks rows still occupies the machine.
    size_t duck_rss_before = Timeline::process_rss_bytes();

    char sql[256];
    std::snprintf(sql, sizeof(sql), "DELETE FROM edges WHERE ts < %llu;",
                  (unsigned long long)cutoff);
    double d0 = now_sec();
    if (!duck.exec(sql)) return 1;
    double duck_sec = now_sec() - d0;
    size_t duck_rss_after = Timeline::process_rss_bytes();

    // Give DuckDB every chance to actually reclaim, and charge the extra time.
    double vac0 = now_sec();
    duck.exec("CHECKPOINT;");
    duck.exec("PRAGMA memory_limit='1GB';");
    double duck_vacuum_sec = now_sec() - vac0;
    size_t duck_rss_vacuumed = Timeline::process_rss_bytes();

    int64_t remaining = 0;
    duck.exec("SELECT count(*) FROM edges;", &remaining);

    CsvWriter w(out_csv,
        {"dataset","trial","drop_frac","total_edges","edges_dropped",
         "barry_evict_sec","barry_slots_cleared","barry_blocks_released",
         "barry_bytes_spanned_MB","barry_bytes_released_MB",
         "barry_rss_before_MB","barry_rss_after_MB",
         "duckdb_delete_sec","duckdb_vacuum_sec","duckdb_rss_before_MB",
         "duckdb_rss_after_MB","duckdb_rss_vacuumed_MB","duckdb_remaining","speedup"});
    w.row(basename_noext(in_path), trial, drop_frac,
          (unsigned long long)ef.count, (unsigned long long)expect_dropped,
          ev.seconds, (unsigned long long)ev.slots_cleared,
          (unsigned long long)ev.blocks_released,
          ev.bytes_spanned / 1e6, ev.bytes_released / 1e6,
          ev.rss_before / 1e6, ev.rss_after / 1e6,
          duck_sec, duck_vacuum_sec,
          duck_rss_before / 1e6, duck_rss_after / 1e6, duck_rss_vacuumed / 1e6,
          (unsigned long long)remaining,
          ev.seconds > 0 ? duck_sec / ev.seconds : 0.0);

    std::fprintf(stderr,
        "[e10] %-20s drop=%3.0f%%  barry: %.4fs  RSS %5.0f->%5.0f MB (-%.0f)   "
        "duckdb: %.4fs  RSS %5.0f->%5.0f MB (-%.0f)   speed %.0fx\n",
        basename_noext(in_path).c_str(), 100 * drop_frac,
        ev.seconds, ev.rss_before / 1e6, ev.rss_after / 1e6,
        (double)(ev.rss_before - ev.rss_after) / 1e6,
        duck_sec, duck_rss_before / 1e6, duck_rss_vacuumed / 1e6,
        (double)(duck_rss_before > duck_rss_vacuumed ? duck_rss_before - duck_rss_vacuumed : 0) / 1e6,
        ev.seconds > 0 ? duck_sec / ev.seconds : 0.0);
    return 0;
}
