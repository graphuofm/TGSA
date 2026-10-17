// E33: the aligned comparison matrix, one harness for every in-process system.
//
// One dataset set, one query set, every system on every cell. The four
// queries and the BFS are written once, over a four-primitive store interface
//
//     scan(lo, hi)        every event with lo <= ts <= hi              (q1)
//     out_in(v, lo, hi)   out-events of v with lo <= ts <= hi          (q2, q3)
//     asof(v, t)          per distinct dst of v, the latest ts <= t    (q4)
//     out_all(v)          every out-neighbour of v, time ignored       (bfs)
//
// so a system's number is the cost of its primitives and nothing else, and
// its answer is checksummed against the TGSR reference on identical questions
// (docs/E33_SPEC.md fixes the formulas, the sample RNG and the CSV schema).
//
// One system per process: memory is isolated, and a DNF cannot take the other
// systems down with it. Budgets and the RSS guard are checked cooperatively
// per batch, per query and per expansion; a watchdog thread backstops them
// when the main thread is inside a call it cannot interrupt (an index build,
// a flush, a compaction pass), writing the row from a live snapshot and
// exiting with status 4.
//
// Usage:
//   bench_e33_matrix --dataset <bin> --system tgsr|livegraph|rocksdb|duckdb
//                    [--phases ingest,compact,q1,q2,q3,q4,bfs] [--limit N]
//                    [--batch 65536] [--samples 1000] [--threads 8]
//                    [--ingest_budget_sec 5400] [--phase_budget_sec 1800]
//                    [--rss_guard_gb 150] [--duckdb_threads T]
//                    [--rocksdb_dir DIR] --outdir results/e33_matrix

#include "common.hpp"
#include "barry/engine.hpp"
#include "barry/vertex_index.hpp"

#ifdef E33_HAVE_LIVEGRAPH
#include "livegraph.hpp"
#endif
#ifdef E33_HAVE_ROCKSDB
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>
#endif
#ifdef E33_HAVE_DUCKDB
#include "duckdb.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <sys/stat.h>

using namespace barry;
using namespace barry::bench;

namespace {

size_t rss_bytes() {
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long size = 0, resident = 0;
    if (std::fscanf(f, "%lu %lu", &size, &resident) != 2) resident = 0;
    std::fclose(f);
    return static_cast<size_t>(resident) * static_cast<size_t>(::sysconf(_SC_PAGESIZE));
}

bool file_exists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}

[[noreturn]] void die(const std::string& msg) {
    std::fprintf(stderr, "[e33] fatal: %s\n", msg.c_str());
    std::exit(1);
}

struct Query { uint64_t v, lo, hi; };

// Per peer keep the latest timestamp: sort by (peer asc, ts desc) and take the
// first of each run. This is the same flat reduction TGSR's as-of operator
// applies internally, so the q4 row compares the stores' delivery and not two
// different reductions.
void keep_latest(std::vector<TemporalRef>& out, size_t base) {
    auto lo = out.begin() + static_cast<std::ptrdiff_t>(base);
    std::sort(lo, out.end(), [](const TemporalRef& a, const TemporalRef& b) {
        return a.peer != b.peer ? a.peer < b.peer : a.ts > b.ts;
    });
    auto w = lo;
    for (auto r = lo; r != out.end(); ++r)
        if (r == lo || r->peer != (w - 1)->peer) *w++ = *r;
    out.erase(w, out.end());
}

// ----------------------------------------------------------- store interface
// The four read primitives plus the ingest hooks. Every query below is written
// once against this; nothing system-specific lives outside the Store classes.
struct Store {
    virtual ~Store() = default;
    virtual const char* name() const = 0;
    virtual int threads() const = 0;

    // One arriving batch of m events; `off` is the global index of e[0].
    virtual void ingest_batch(const Edge* e, size_t m, size_t off) = 0;
    // Whatever the system needs before reads are fast (flush, index build);
    // timed inside the ingest figure, as the spec requires.
    virtual void ingest_finish() {}
    virtual std::string ingest_note() { return ""; }

    virtual bool has_compact() const { return false; }
    virtual void compact() {}
    virtual const char* compact_note() const { return "n/a: no separate compaction step"; }

    // Read session: a read-only transaction or a reusable iterator held
    // across every query phase.
    virtual void begin_read() {}
    virtual void end_read() {}

    virtual void scan(uint64_t lo, uint64_t hi, std::vector<Edge>& out) = 0;
    virtual void out_in(uint32_t v, uint64_t lo, uint64_t hi,
                        std::vector<TemporalRef>& out) = 0;
    virtual void out_all(uint32_t v, std::vector<uint32_t>& dsts) = 0;
    virtual void asof(uint32_t v, uint64_t t, std::vector<TemporalRef>& out) = 0;
};

// ------------------------------------------------------------------- TGSR
struct TgsrStore final : Store {
    std::unique_ptr<Barry> b_;
    const VertexIndex* vi_ = nullptr;
    int th_;

    TgsrStore(const GraphMeta& meta, uint32_t V, size_t n, int th) : th_(th) {
        GraphConfig cfg;
        cfg.t_min = meta.t_min;
        cfg.t_max = meta.t_max;
        cfg.num_vertices = V;
        cfg.build_reverse_index = false;
        cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, n * sizeof(Edge) * 4);
        // The bucket arena holds one reference per edge, doubled by chain
        // growth, plus a first bucket per vertex and the buckets abandoned by
        // CAS losers on hot vertices. n x 24 x 4 ran out on THEIA (6.9M
        // vertices) and refs were silently refused; reserve from n AND V.
        // The reservation is MAP_NORESERVE virtual space, so size is free.
        cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28,
            n * sizeof(EdgeRef) * 8 + static_cast<size_t>(V) * 512);
        b_.reset(new Barry(cfg));
        vi_ = b_->vertex_index();
    }
    const char* name() const override { return "tgsr"; }
    int threads() const override { return th_; }
    void ingest_batch(const Edge* e, size_t m, size_t) override { b_->insert_batch(e, m, th_); }
    std::string ingest_note() override {
        const size_t d = b_->edges_dropped();
        const size_t r = vi_ ? vi_->refused_refs() : 0;
        if (r) {
            std::fprintf(stderr, "[e33] FATAL: vertex index refused %zu refs (reservation exhausted); "
                         "this run is not a result\n", r);
            std::exit(5);
        }
        if (!d) return "";
        return "edges_dropped=" + std::to_string(d);
    }
    bool has_compact() const override { return true; }
    void compact() override { b_->finalize(); }
    void scan(uint64_t lo, uint64_t hi, std::vector<Edge>& out) override {
        out = b_->query_range(lo, hi);
    }
    void out_in(uint32_t v, uint64_t lo, uint64_t hi, std::vector<TemporalRef>& out) override {
        vi_->neighbors_temporal_into(v, lo, hi, out);
    }
    void out_all(uint32_t v, std::vector<uint32_t>& dsts) override {
        vi_->neighbors_dst_into(v, dsts);
    }
    void asof(uint32_t v, uint64_t t, std::vector<TemporalRef>& out) override {
        vi_->latest_neighbors_into(v, t, out);
    }
};

// -------------------------------------------------------------- LiveGraph
#ifdef E33_HAVE_LIVEGRAPH
inline std::string_view ts_view(const uint64_t& t) {
    return std::string_view(reinterpret_cast<const char*>(&t), sizeof(uint64_t));
}
inline uint64_t ts_of(std::string_view d) {
    uint64_t ts = 0;
    if (d.size() >= sizeof(uint64_t)) std::memcpy(&ts, d.data(), sizeof(uint64_t));
    return ts;
}

struct LiveGraphStore final : Store {
    lg::Graph g_;
    uint32_t V_;
    // lg::Transaction is neither copyable nor movable (const unique_ptr
    // member); direct-initialising from the prvalue is the one way to keep
    // one alive across a whole query phase.
    std::unique_ptr<lg::Transaction> rtx_;

    explicit LiveGraphStore(uint32_t V)
        : g_("", "", 1ul << 40, static_cast<lg::vertex_t>(V) + 16), V_(V) {
        // Vertex creation is setup, not stream cost (as bench_e19): timed and
        // printed, not blended into ingest.
        const double t0 = now_sec();
        auto txn = g_.begin_batch_loader();
        for (uint32_t v = 0; v < V; ++v) txn.new_vertex();
        txn.commit();
        std::fprintf(stderr, "[e33] livegraph: %u vertices created in %.2f s (setup, untimed)\n",
                     V, now_sec() - t0);
    }
    const char* name() const override { return "livegraph"; }
    int threads() const override { return 1; }
    void ingest_batch(const Edge* e, size_t m, size_t) override {
        auto txn = g_.begin_batch_loader();
        for (size_t i = 0; i < m; ++i)
            txn.put_edge(e[i].src, 0, e[i].dst, ts_view(e[i].timestamp), /*force_insert=*/true);
        txn.commit();
    }
    void begin_read() override {
        rtx_.reset(new lg::Transaction(g_.begin_read_only_transaction()));
    }
    void end_read() override {
        if (rtx_) { rtx_->abort(); rtx_.reset(); }
    }
    void scan(uint64_t lo, uint64_t hi, std::vector<Edge>& out) override {
        // Entity-keyed: the only way to a time window is through every vertex.
        for (uint32_t v = 0; v < V_; ++v) {
            auto it = rtx_->get_edges(v, 0);
            while (it.valid()) {
                const uint64_t ts = ts_of(it.edge_data());
                if (ts >= lo && ts <= hi)
                    out.push_back(Edge{v, static_cast<uint32_t>(it.dst_id()), ts, 0.0f});
                it.next();
            }
        }
    }
    void out_in(uint32_t v, uint64_t lo, uint64_t hi, std::vector<TemporalRef>& out) override {
        auto it = rtx_->get_edges(v, 0);
        while (it.valid()) {
            const uint64_t ts = ts_of(it.edge_data());
            if (ts >= lo && ts <= hi) out.push_back(TemporalRef{static_cast<uint32_t>(it.dst_id()), ts});
            it.next();
        }
    }
    void out_all(uint32_t v, std::vector<uint32_t>& dsts) override {
        auto it = rtx_->get_edges(v, 0);
        while (it.valid()) { dsts.push_back(static_cast<uint32_t>(it.dst_id())); it.next(); }
    }
    void asof(uint32_t v, uint64_t t, std::vector<TemporalRef>& out) override {
        const size_t base = out.size();
        auto it = rtx_->get_edges(v, 0);
        while (it.valid()) {
            const uint64_t ts = ts_of(it.edge_data());
            if (ts <= t) out.push_back(TemporalRef{static_cast<uint32_t>(it.dst_id()), ts});
            it.next();
        }
        keep_latest(out, base);
    }
};
#endif

// ---------------------------------------------------------------- RocksDB
#ifdef E33_HAVE_ROCKSDB
inline uint64_t be64(uint64_t v) { return __builtin_bswap64(v); }
inline uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }

// Time-encoded key (bench_e24, TVA's own RocksDB schema): lexicographic order
// is time order. The monotone seq suffix keeps identical (ts,src,dst) events
// distinct, since the workload is a multiset and a KV Put overwrites.
struct RKey {
    uint64_t ts_be;
    uint32_t src_be, dst_be, seq_be;
} __attribute__((packed));
static_assert(sizeof(RKey) == 20, "time-encoded key is 20 bytes");

inline rocksdb::Slice key_slice(const RKey& k) {
    return rocksdb::Slice(reinterpret_cast<const char*>(&k), sizeof(RKey));
}

struct RocksStore final : Store {
    rocksdb::DB* db_ = nullptr;
    std::string dir_;
    int th_;
    std::unique_ptr<rocksdb::Iterator> it_;

    RocksStore(const std::string& dir, int th) : dir_(dir), th_(th) {
        rocksdb::DestroyDB(dir_, rocksdb::Options());
        rocksdb::Options opt;
        opt.create_if_missing = true;
        opt.compression = rocksdb::kNoCompression;
        opt.IncreaseParallelism(th);
        opt.OptimizeLevelStyleCompaction();
        auto st = rocksdb::DB::Open(opt, dir_, &db_);
        if (!st.ok()) die("rocksdb open: " + st.ToString());
    }
    ~RocksStore() override {
        it_.reset();
        delete db_;
        rocksdb::DestroyDB(dir_, rocksdb::Options());
    }
    const char* name() const override { return "rocksdb"; }
    int threads() const override { return th_; }
    const char* compact_note() const override { return "n/a: Flush() inside ingest"; }
    void ingest_batch(const Edge* e, size_t m, size_t off) override {
        rocksdb::WriteBatch wb;
        for (size_t i = 0; i < m; ++i) {
            const RKey k{be64(e[i].timestamp), be32(e[i].src), be32(e[i].dst),
                         be32(static_cast<uint32_t>(off + i))};
            wb.Put(key_slice(k), rocksdb::Slice(reinterpret_cast<const char*>(&e[i].weight), 4));
        }
        rocksdb::WriteOptions wo;
        wo.disableWAL = true;
        auto st = db_->Write(wo, &wb);
        if (!st.ok()) die("rocksdb write: " + st.ToString());
    }
    void ingest_finish() override { db_->Flush(rocksdb::FlushOptions()); }
    void begin_read() override { it_.reset(db_->NewIterator(rocksdb::ReadOptions())); }
    void end_read() override { it_.reset(); }

    // Seek to lo, walk to hi. The time axis is the only access path the schema
    // has, so every per-vertex question is this scan plus a filter on src.
    template <class F> void range(uint64_t lo, uint64_t hi, F&& f) {
        const RKey k0{be64(lo), 0, 0, 0};
        for (it_->Seek(key_slice(k0)); it_->Valid(); it_->Next()) {
            const RKey* k = reinterpret_cast<const RKey*>(it_->key().data());
            const uint64_t ts = __builtin_bswap64(k->ts_be);
            if (ts > hi) break;
            f(__builtin_bswap32(k->src_be), __builtin_bswap32(k->dst_be), ts);
        }
    }
    void scan(uint64_t lo, uint64_t hi, std::vector<Edge>& out) override {
        range(lo, hi, [&](uint32_t s, uint32_t d, uint64_t ts) { out.push_back(Edge{s, d, ts, 0.0f}); });
    }
    void out_in(uint32_t v, uint64_t lo, uint64_t hi, std::vector<TemporalRef>& out) override {
        range(lo, hi, [&](uint32_t s, uint32_t d, uint64_t ts) { if (s == v) out.push_back(TemporalRef{d, ts}); });
    }
    void out_all(uint32_t v, std::vector<uint32_t>& dsts) override {
        range(0, UINT64_MAX, [&](uint32_t s, uint32_t d, uint64_t) { if (s == v) dsts.push_back(d); });
    }
    void asof(uint32_t v, uint64_t t, std::vector<TemporalRef>& out) override {
        const size_t base = out.size();
        range(0, t, [&](uint32_t s, uint32_t d, uint64_t ts) { if (s == v) out.push_back(TemporalRef{d, ts}); });
        keep_latest(out, base);
    }
};
#endif

// ----------------------------------------------------------------- DuckDB
#ifdef E33_HAVE_DUCKDB
struct DuckStore final : Store {
    duckdb_database db_ = nullptr;
    duckdb_connection con_ = nullptr;
    duckdb_appender app_ = nullptr;
    duckdb_prepared_statement ps_out_in_ = nullptr, ps_asof_ = nullptr, ps_out_all_ = nullptr;
    int th_;
    // Opt-in, off by default: extra statements run after the spec's two
    // indexes (e.g. a single-column index on src), so a variant can be priced
    // without editing the harness. The ingest row's note records it.
    std::string extra_sql_;

    DuckStore(int th, std::string extra_sql) : th_(th), extra_sql_(std::move(extra_sql)) {
        if (duckdb_open(nullptr, &db_) == DuckDBError) die("duckdb_open");
        if (duckdb_connect(db_, &con_) == DuckDBError) die("duckdb_connect");
        exec("SET threads TO " + std::to_string(th));
        exec("CREATE TABLE edges(src UINTEGER, dst UINTEGER, ts UBIGINT)");
    }
    ~DuckStore() override {
        for (auto* ps : {&ps_out_in_, &ps_asof_, &ps_out_all_})
            if (*ps) duckdb_destroy_prepare(ps);
        if (app_) duckdb_appender_destroy(&app_);
        duckdb_disconnect(&con_);
        duckdb_close(&db_);
    }
    const char* name() const override { return "duckdb"; }
    int threads() const override { return th_; }
    const char* compact_note() const override { return "n/a: index build inside ingest"; }

    void exec(const std::string& sql) {
        duckdb_result r;
        if (duckdb_query(con_, sql.c_str(), &r) == DuckDBError) {
            std::string m = std::string("duckdb: ") + duckdb_result_error(&r) + " [" + sql + "]";
            duckdb_destroy_result(&r);
            die(m);
        }
        duckdb_destroy_result(&r);
    }
    void prep(duckdb_prepared_statement& ps, const char* sql) {
        if (duckdb_prepare(con_, sql, &ps) == DuckDBError)
            die(std::string("duckdb prepare: ") + duckdb_prepare_error(ps) + " [" + sql + "]");
    }
    // Walk a result chunk by chunk through the vector interface, which is
    // DuckDB's fast path for reading results out of the C API.
    template <class F> static void each_chunk(duckdb_result& r, F&& f) {
        for (;;) {
            duckdb_data_chunk c = duckdb_fetch_chunk(r);
            if (!c) break;
            const idx_t rows = duckdb_data_chunk_get_size(c);
            if (rows) f(c, rows);
            duckdb_destroy_data_chunk(&c);
        }
    }
    template <class F> void run_prepared(duckdb_prepared_statement ps, F&& f) {
        duckdb_result r;
        if (duckdb_execute_prepared(ps, &r) == DuckDBError) {
            std::string m = std::string("duckdb execute: ") + duckdb_result_error(&r);
            duckdb_destroy_result(&r);
            die(m);
        }
        each_chunk(r, f);
        duckdb_destroy_result(&r);
    }

    void ingest_batch(const Edge* e, size_t m, size_t) override {
        if (!app_ && duckdb_appender_create(con_, nullptr, "edges", &app_) == DuckDBError)
            die("duckdb appender create");
        for (size_t i = 0; i < m; ++i) {
            duckdb_appender_begin_row(app_);
            duckdb_append_uint32(app_, e[i].src);
            duckdb_append_uint32(app_, e[i].dst);
            duckdb_append_uint64(app_, e[i].timestamp);
            duckdb_appender_end_row(app_);
        }
        duckdb_appender_flush(app_);
    }
    void ingest_finish() override {
        if (app_) { duckdb_appender_flush(app_); duckdb_appender_destroy(&app_); app_ = nullptr; }
        exec("CREATE INDEX idx_ts ON edges(ts)");
        exec("CREATE INDEX idx_src_ts ON edges(src, ts)");
        for (const std::string& sql : split_sql(extra_sql_)) exec(sql);
    }
    static std::vector<std::string> split_sql(const std::string& s) {
        std::vector<std::string> out; std::string cur;
        for (char c : s) { if (c == ';') { if (!cur.empty()) out.push_back(cur); cur.clear(); } else cur += c; }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }
    std::string ingest_note() override { return extra_sql_.empty() ? "" : "extra_sql: " + extra_sql_; }
    void begin_read() override {
        prep(ps_out_in_,  "SELECT dst, ts FROM edges WHERE src = ? AND ts BETWEEN ? AND ?");
        prep(ps_asof_,    "SELECT dst, max(ts) FROM edges WHERE src = ? AND ts <= ? GROUP BY dst");
        prep(ps_out_all_, "SELECT DISTINCT dst FROM edges WHERE src = ?");
    }
    // --duckdb_explain: print the physical plans so the report can say whether
    // the ART indexes the spec prescribes are the access path or decoration.
    void explain(uint64_t lo, uint64_t hi) {
        char q[4][300];
        std::snprintf(q[0], 300, "SELECT src, dst, ts FROM edges WHERE ts BETWEEN %llu AND %llu", (unsigned long long)lo, (unsigned long long)hi);
        std::snprintf(q[1], 300, "SELECT dst, ts FROM edges WHERE src = 1 AND ts BETWEEN %llu AND %llu", (unsigned long long)lo, (unsigned long long)hi);
        std::snprintf(q[2], 300, "SELECT dst, max(ts) FROM edges WHERE src = 1 AND ts <= %llu GROUP BY dst", (unsigned long long)hi);
        std::snprintf(q[3], 300, "SELECT DISTINCT dst FROM edges WHERE src = 1");
        for (auto& sql : q) {
            duckdb_result r;
            const std::string ex = std::string("EXPLAIN ") + sql;
            if (duckdb_query(con_, ex.c_str(), &r) == DuckDBError) { duckdb_destroy_result(&r); continue; }
            std::fprintf(stderr, "[e33] duckdb EXPLAIN %s\n", sql);
            each_chunk(r, [&](duckdb_data_chunk c, idx_t rows) {
                for (idx_t i = 0; i < rows; ++i) {
                    duckdb_string_t* col = static_cast<duckdb_string_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(c, 1)));
                    const char* p = duckdb_string_is_inlined(col[i]) ? col[i].value.inlined.inlined : col[i].value.pointer.ptr;
                    std::fprintf(stderr, "%.*s\n", (int)duckdb_string_t_length(col[i]), p);
                }
            });
            duckdb_destroy_result(&r);
        }
    }
    void scan(uint64_t lo, uint64_t hi, std::vector<Edge>& out) override {
        char sql[256];
        std::snprintf(sql, sizeof(sql),
                      "SELECT src, dst, ts FROM edges WHERE ts BETWEEN %llu AND %llu",
                      (unsigned long long)lo, (unsigned long long)hi);
        duckdb_result r;
        if (duckdb_query(con_, sql, &r) == DuckDBError) {
            std::string m = std::string("duckdb scan: ") + duckdb_result_error(&r);
            duckdb_destroy_result(&r);
            die(m);
        }
        each_chunk(r, [&](duckdb_data_chunk c, idx_t rows) {
            const auto* s  = static_cast<const uint32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(c, 0)));
            const auto* d  = static_cast<const uint32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(c, 1)));
            const auto* ts = static_cast<const uint64_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(c, 2)));
            for (idx_t i = 0; i < rows; ++i) out.push_back(Edge{s[i], d[i], ts[i], 0.0f});
        });
        duckdb_destroy_result(&r);
    }
    void out_in(uint32_t v, uint64_t lo, uint64_t hi, std::vector<TemporalRef>& out) override {
        duckdb_bind_uint32(ps_out_in_, 1, v);
        duckdb_bind_uint64(ps_out_in_, 2, lo);
        duckdb_bind_uint64(ps_out_in_, 3, hi);
        run_prepared(ps_out_in_, [&](duckdb_data_chunk c, idx_t rows) {
            const auto* d  = static_cast<const uint32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(c, 0)));
            const auto* ts = static_cast<const uint64_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(c, 1)));
            for (idx_t i = 0; i < rows; ++i) out.push_back(TemporalRef{d[i], ts[i]});
        });
    }
    void out_all(uint32_t v, std::vector<uint32_t>& dsts) override {
        duckdb_bind_uint32(ps_out_all_, 1, v);
        run_prepared(ps_out_all_, [&](duckdb_data_chunk c, idx_t rows) {
            const auto* d = static_cast<const uint32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(c, 0)));
            dsts.insert(dsts.end(), d, d + rows);
        });
    }
    void asof(uint32_t v, uint64_t t, std::vector<TemporalRef>& out) override {
        duckdb_bind_uint32(ps_asof_, 1, v);
        duckdb_bind_uint64(ps_asof_, 2, t);
        run_prepared(ps_asof_, [&](duckdb_data_chunk c, idx_t rows) {
            const auto* d  = static_cast<const uint32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(c, 0)));
            const auto* ts = static_cast<const uint64_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(c, 1)));
            for (idx_t i = 0; i < rows; ++i) out.push_back(TemporalRef{d[i], ts[i]});
        });
    }
};
#endif

// ------------------------------------------------------- budgets and guard
// Cooperative first: the main thread polls `tripped()` per batch, per query
// and per expansion. The watchdog only writes the row itself when the main
// thread has not come back `grace_sec` after the trip, i.e. it is inside a
// call that cannot be interrupted.
struct Guard {
    std::atomic<bool>        active{false};
    std::atomic<double>      deadline{0.0};
    std::atomic<const char*> reason{nullptr};
    size_t                   rss_limit = 0;
    double                   grace_sec = 90.0;
    // Live snapshot for a hard stop.
    const char*              phase = "";
    double                   t_start = 0;
    size_t                   samples_total = 0;
    std::atomic<size_t>      done{0}, count{0}, events{0};
    std::atomic<uint64_t>    chk{0};
    std::function<void(const char*)> hard_stop;

    void arm(const char* ph, double budget, size_t total) {
        phase = ph; t_start = now_sec(); samples_total = total;
        done = 0; count = 0; chk = 0;
        reason.store(nullptr);
        deadline.store(budget > 0 ? t_start + budget : 0.0);
        active.store(true);
    }
    void disarm() { active.store(false); reason.store(nullptr); }
    const char* tripped() const { return reason.load(std::memory_order_relaxed); }

    void run() {
        double tripped_at = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (!active.load()) { tripped_at = 0; continue; }
            const double now = now_sec();
            const char* r = reason.load();
            if (!r) {
                if (rss_limit && rss_bytes() > rss_limit) r = "rss-guard";
                else if (deadline.load() > 0 && now > deadline.load()) r = "DNF";
                if (r) { reason.store(r); tripped_at = now; }
            } else if (tripped_at > 0 && now - tripped_at > grace_sec && active.load()) {
                hard_stop(r);
            }
        }
    }
};

// ------------------------------------------------------------------ rows
struct Reporter {
    CsvWriter csv;
    std::mutex mu;
    std::string system, ds;
    int threads;
    size_t rss0 = 0;

    Reporter(const std::string& path, const std::string& sys, const std::string& dsn, int th)
        : csv(path, {"system","dataset","phase","threads","events","samples_total",
                     "samples_done","seconds","rate","median_us","p95_us","count",
                     "checksum","checksum_ok","rss_MB","note"}),
          system(sys), ds(dsn), threads(th) {}

    void emit(const char* phase, size_t events, size_t st, size_t sd, double sec,
              double rate, double med, double p95, uint64_t count, uint64_t chk,
              int ok, const std::string& note) {
        std::lock_guard<std::mutex> g(mu);
        const size_t now = rss_bytes();
        const double rss_mb = (now > rss0 ? now - rss0 : 0) / 1e6;
        std::string safe = note;                 // the CSV is unquoted: no commas in free text
        std::replace(safe.begin(), safe.end(), ',', ';');
        csv.row(system, ds, std::string(phase), threads,
                (unsigned long long)events, (unsigned long long)st, (unsigned long long)sd,
                sec, rate, med, p95, (unsigned long long)count, (unsigned long long)chk,
                ok, rss_mb, safe);
    }
};

// ------------------------------------------------------- reference files
// <ds>.ref     one line per phase: `phase count checksum` (the spec's file).
// <ds>.prefix  one line per completed query of a latency phase:
//              `phase i count checksum`, cumulative after i queries, so a run
//              that stopped on its budget is still checked on what it did.
struct RefData {
    std::map<std::string, std::pair<uint64_t,uint64_t>> full;
    std::map<std::string, std::vector<std::pair<uint64_t,uint64_t>>> prefix;
};

RefData load_ref(const std::string& ref_path, const std::string& prefix_path) {
    RefData r;
    if (std::ifstream f{ref_path}) {
        std::string ph; unsigned long long c, k;
        while (f >> ph >> c >> k) r.full[ph] = {c, k};
    }
    if (std::ifstream f{prefix_path}) {
        std::string ph; unsigned long long i, c, k;
        while (f >> ph >> i >> c >> k) {
            auto& v = r.prefix[ph];
            if (v.size() < i) v.resize(i, {0, 0});
            if (i > 0) v[i - 1] = {c, k};
        }
    }
    return r;
}

void merge_ref_line(const std::string& path, const std::string& phase, uint64_t count, uint64_t chk) {
    std::vector<std::string> keep;
    if (std::ifstream f{path}) {
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream is(line);
            std::string ph; is >> ph;
            if (ph != phase && !ph.empty()) keep.push_back(line);
        }
    }
    std::ofstream o(path, std::ios::trunc);
    for (auto& l : keep) o << l << '\n';
    o << phase << ' ' << count << ' ' << chk << '\n';
}

void merge_prefix(const std::string& path, const std::string& phase,
                  const std::vector<std::pair<uint64_t,uint64_t>>& cum) {
    std::vector<std::string> keep;
    if (std::ifstream f{path}) {
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream is(line);
            std::string ph; is >> ph;
            if (ph != phase && !ph.empty()) keep.push_back(line);
        }
    }
    std::ofstream o(path, std::ios::trunc);
    for (auto& l : keep) o << l << '\n';
    for (size_t i = 0; i < cum.size(); ++i)
        o << phase << ' ' << (i + 1) << ' ' << cum[i].first << ' ' << cum[i].second << '\n';
}

// ------------------------------------------------------- latency phases
struct LatResult {
    size_t done = 0;
    double seconds = 0;
    std::vector<double> lat;
    uint64_t count = 0, chk = 0;
    std::vector<std::pair<uint64_t,uint64_t>> cum;
    std::string note;
};

// Time each query. After the first 20, project; run only as many as fit the
// budget and say so. The watchdog deadline catches a projection that was
// optimistic. Medians over the prefix of a random list are still medians.
// `one` returns false when the budget tripped inside the query; that query
// is then neither folded nor counted, so the prefix check stays exact.
template <class F>
LatResult run_latency(Guard& g, const std::vector<Query>& qs, double budget, F&& one) {
    LatResult r;
    const size_t N = qs.size();
    r.lat.reserve(N); r.cum.reserve(N);
    size_t n_run = N;
    const double t0 = now_sec();
    for (size_t i = 0; i < n_run; ++i) {
        if (const char* why = g.tripped()) { r.note = why; break; }
        const double q0 = now_sec();
        if (!one(qs[i], r.count, r.chk)) { r.note = g.tripped() ? g.tripped() : "aborted"; break; }
        r.lat.push_back((now_sec() - q0) * 1e6);
        r.cum.emplace_back(r.count, r.chk);
        ++r.done;
        g.done.store(r.done, std::memory_order_relaxed);
        g.count.store(r.count, std::memory_order_relaxed);
        g.chk.store(r.chk, std::memory_order_relaxed);
        if (i + 1 == 20 && N > 20 && budget > 0) {
            const double per = (now_sec() - t0) / 20.0;
            const double projected = per * static_cast<double>(N);
            if (projected > budget) {
                n_run = std::min(N, std::max<size_t>(20, static_cast<size_t>(budget / per)));
                char b[160];
                std::snprintf(b, sizeof(b), "budget: projected %.0f s for %zu queries; running %zu",
                              projected, N, n_run);
                r.note = b;
            }
        }
    }
    r.seconds = now_sec() - t0;
    return r;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) { if (c == sep) { if (!cur.empty()) out.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    Args args(argc, argv);
    const std::string in_path = args.get("dataset");
    const std::string system  = args.get("system");
    const std::string outdir  = args.get("outdir", "results/e33_matrix");
    const std::string phases_s = args.get("phases", "ingest,compact,q1,q2,q3,q4,bfs");
    const size_t limit   = static_cast<size_t>(args.getl("limit", 0));
    const size_t batch   = static_cast<size_t>(args.getl("batch", 65536));
    const size_t samples = static_cast<size_t>(args.getl("samples", 1000));
    const int    threads = static_cast<int>(args.getl("threads", 8));
    const double ingest_budget = args.getd("ingest_budget_sec", 5400);
    const double phase_budget  = args.getd("phase_budget_sec", 1800);
    const double rss_guard_gb  = args.getd("rss_guard_gb", 150);
    const int    duckdb_threads = static_cast<int>(args.getl("duckdb_threads", threads));
    const double guard_grace    = args.getd("guard_grace_sec", 90.0);
    const bool   duckdb_explain = args.getl("duckdb_explain", 0) != 0;

    // common.hpp's Args gives every --flag the next token as its value, so a
    // bare switch silently eats the flag after it. Refuse that outright.
    for (const char* k : {"dataset","system","outdir","phases","limit","batch","samples","threads",
                          "ingest_budget_sec","phase_budget_sec","rss_guard_gb","duckdb_threads",
                          "guard_grace_sec","duckdb_explain","duckdb_extra_sql","rocksdb_dir"}) {
        const std::string v = args.get(k);
        if (v.rfind("--", 0) == 0) {
            std::fprintf(stderr, "[e33] --%s took '%s' as its value; every flag needs a value "
                         "(write --duckdb_explain 1)\n", k, v.c_str());
            return 1;
        }
    }

    static const std::vector<std::string> kPhases = {"ingest","compact","q1","q2","q3","q4","bfs"};
    std::set<std::string> want;
    for (auto& p : split(phases_s, ',')) {
        if (std::find(kPhases.begin(), kPhases.end(), p) == kPhases.end()) {
            std::fprintf(stderr, "unknown phase '%s'\n", p.c_str()); return 1;
        }
        want.insert(p);
    }
    const bool is_tgsr = (system == "tgsr");
    if (in_path.empty() || system.empty() || want.empty()) {
        std::fprintf(stderr,
            "usage: %s --dataset <bin> --system tgsr|livegraph|rocksdb|duckdb "
            "[--phases ingest,compact,q1,q2,q3,q4,bfs] [--limit N] [--batch 65536] "
            "[--samples 1000] [--threads 8] [--ingest_budget_sec 5400] "
            "[--phase_budget_sec 1800] [--rss_guard_gb 150] [--duckdb_threads T] "
            "[--duckdb_extra_sql \"SQL;SQL\"] [--duckdb_explain 1] [--rocksdb_dir DIR] "
            "[--guard_grace_sec 90] --outdir results/e33_matrix\n", args.prog().c_str());
        return 1;
    }
    // Refuse early for a system this binary was not built with.
    const bool compiled =
        is_tgsr
#ifdef E33_HAVE_LIVEGRAPH
        || system == "livegraph"
#endif
#ifdef E33_HAVE_ROCKSDB
        || system == "rocksdb"
#endif
#ifdef E33_HAVE_DUCKDB
        || system == "duckdb"
#endif
        ;
    if (!compiled) {
        const bool known = system == "livegraph" || system == "rocksdb" || system == "duckdb";
        std::fprintf(stderr, known
            ? "[e33] system '%s' is not compiled into this binary (its library was not found at configure time); exit 2\n"
            : "[e33] unknown system '%s' (tgsr|livegraph|rocksdb|duckdb); exit 2\n", system.c_str());
        return 2;
    }

    ::mkdir(outdir.c_str(), 0775);
    EdgeFile ef;
    if (!ef.load(in_path)) return 1;
    const size_t n = std::min(limit ? limit : ef.count, ef.count);
    const GraphMeta meta = scan_meta(ef.edges, n);
    const std::string ds = basename_noext(in_path);
    const uint32_t V = meta.v_max + 1;
    const uint64_t span = (meta.t_max > meta.t_min) ? meta.t_max - meta.t_min : 0;
    const uint64_t w = std::max<uint64_t>(1, span / 100);
    const uint64_t q1_hi = meta.t_max;
    const uint64_t q1_lo = (meta.t_max >= w) ? meta.t_max - w : 0;

    // ---------------------------------------------------------------- samples
    // Edge-uniform draws with the spec's RNG, byte-identical across systems
    // through the .queries file: whoever runs first writes it, everyone after
    // reads it. The in-process list is still generated and compared, so a
    // file produced by another harness is checked rather than trusted.
    const std::string qpath = outdir + "/" + ds + ".queries";
    std::vector<Query> gen(samples);
    {
        std::mt19937_64 rng(0xE33ull);
        for (size_t i = 0; i < samples; ++i) {
            const size_t e = std::uniform_int_distribution<size_t>(0, n - 1)(rng);
            gen[i] = Query{ef.edges[e].src, ef.edges[e].timestamp, ef.edges[e].timestamp + w};
        }
    }
    std::vector<Query> qs;
    if (file_exists(qpath)) {
        FILE* qf = std::fopen(qpath.c_str(), "rb");
        if (!qf) { std::perror(qpath.c_str()); return 1; }
        uint64_t rec[3];
        while (std::fread(rec, sizeof(rec), 1, qf) == 1) qs.push_back(Query{rec[0], rec[1], rec[2]});
        std::fclose(qf);
        const bool same = qs.size() == gen.size() &&
            std::equal(qs.begin(), qs.end(), gen.begin(), [](const Query& a, const Query& b) {
                return a.v == b.v && a.lo == b.lo && a.hi == b.hi; });
        std::fprintf(stderr, "[e33] %zu queries read from %s (%s the in-process generation)\n",
                     qs.size(), qpath.c_str(), same ? "identical to" : "DIFFERENT from");
    } else {
        qs = gen;
        if (is_tgsr) {
            FILE* qf = std::fopen(qpath.c_str(), "wb");
            if (!qf) { std::perror(qpath.c_str()); return 1; }
            for (const Query& q : qs) {
                const uint64_t rec[3] = {q.v, q.lo, q.hi};
                if (std::fwrite(rec, sizeof(rec), 1, qf) != 1) { std::perror("fwrite"); return 1; }
            }
            std::fclose(qf);
            std::fprintf(stderr, "[e33] wrote %zu queries to %s\n", qs.size(), qpath.c_str());
        } else {
            std::fprintf(stderr, "[e33] warning: %s absent; using the in-process list (run tgsr first)\n",
                         qpath.c_str());
        }
    }
    const size_t N = qs.size();

    // BFS root: largest out-degree in the event multiset, lowest id on ties.
    uint32_t root = 0;
    {
        std::vector<uint32_t> deg(V, 0);
        for (size_t i = 0; i < n; ++i) ++deg[ef.edges[i].src];
        uint32_t best = 0;
        for (uint32_t v = 0; v < V; ++v) if (deg[v] > best) { best = deg[v]; root = v; }
        std::fprintf(stderr, "[e33] %s: n=%zu V=%u t=[%llu,%llu] w=%llu bfs_root=%u (deg %u)\n",
                     ds.c_str(), n, V, (unsigned long long)meta.t_min,
                     (unsigned long long)meta.t_max, (unsigned long long)w, root, best);
    }

    const std::string ref_path = outdir + "/" + ds + ".ref";
    const std::string prefix_path = outdir + "/" + ds + ".prefix";
    const RefData ref = is_tgsr ? RefData{} : load_ref(ref_path, prefix_path);
    if (!is_tgsr && ref.full.empty())
        std::fprintf(stderr, "[e33] warning: no reference at %s; checksum_ok will be -1\n", ref_path.c_str());

    // ------------------------------------------------------------ the store
    // Everything that touches the input array is above this line, so the RSS
    // delta is the store and not the page cache filling behind it.
    int th = threads;
    if (system == "livegraph") th = 1;
    if (system == "duckdb") th = duckdb_threads;
    Reporter rep(outdir + "/e33.csv", system, ds, th);
    rep.rss0 = rss_bytes();

    std::unique_ptr<Store> store;
    if (is_tgsr) store.reset(new TgsrStore(meta, V, n, threads));
#ifdef E33_HAVE_LIVEGRAPH
    else if (system == "livegraph") store.reset(new LiveGraphStore(V));
#endif
#ifdef E33_HAVE_ROCKSDB
    else if (system == "rocksdb")
        store.reset(new RocksStore(args.get("rocksdb_dir", "/home/jding/dataset/e33_rocksdb_" + ds), threads));
#endif
#ifdef E33_HAVE_DUCKDB
    else if (system == "duckdb") store.reset(new DuckStore(duckdb_threads, args.get("duckdb_extra_sql", "")));
#endif
    if (!store) return 2;

    Guard guard;
    guard.rss_limit = static_cast<size_t>(rss_guard_gb * 1e9);
    guard.grace_sec = guard_grace;
    guard.hard_stop = [&](const char* why) {
        const double sec = now_sec() - guard.t_start;
        const size_t done = guard.done.load();
        std::string note = std::string(why) + " (hard stop: main thread unresponsive)";
        const size_t ev = std::string(guard.phase) == "ingest" ? guard.events.load() : n;
        rep.emit(guard.phase, ev, guard.samples_total, done, sec,
                 done && sec > 0 ? done / sec : 0.0, 0.0, 0.0,
                 guard.count.load(), guard.chk.load(), -1, note);
        std::fprintf(stderr, "[e33] %s %s %-7s %s after %.1f s; exiting\n",
                     system.c_str(), ds.c_str(), guard.phase, note.c_str(), sec);
        std::_Exit(4);
    };
    std::thread([&guard] { guard.run(); }).detach();

    // Compare against the reference (or write it, for tgsr). Returns checksum_ok.
    auto settle = [&](const std::string& phase, size_t done, size_t total, uint64_t count,
                      uint64_t chk, const std::vector<std::pair<uint64_t,uint64_t>>* cum,
                      std::string& note, const char*& verdict) -> int {
        if (is_tgsr) {
            if (cum && !cum->empty()) merge_prefix(prefix_path, phase, *cum);
            if (done == total) { merge_ref_line(ref_path, phase, count, chk); verdict = "REF"; return 1; }
            verdict = "partial (no ref written)";
            return -1;
        }
        if (done == total) {
            auto it = ref.full.find(phase);
            if (it == ref.full.end()) { verdict = "no-ref"; return -1; }
            const bool ok = it->second.first == count && it->second.second == chk;
            verdict = ok ? "MATCH" : "MISMATCH";
            return ok ? 1 : 0;
        }
        auto it = ref.prefix.find(phase);
        if (it == ref.prefix.end() || done == 0 || it->second.size() < done) { verdict = "no-ref (partial)"; return -1; }
        const bool ok = it->second[done - 1].first == count && it->second[done - 1].second == chk;
        verdict = ok ? "MATCH (prefix)" : "MISMATCH (prefix)";
        if (!note.empty()) note += "; ";
        note += "checked against the " + std::to_string(done) + "-query prefix";
        return ok ? 1 : 0;
    };

    bool loaded = false;        // ingest completed
    std::string skip_reason;    // set when later phases cannot run

    // ================================================================ ingest
    // Always runs (nothing is queryable otherwise); the row is written when
    // the phase was asked for.
    {
        guard.arm("ingest", ingest_budget, 0);
        const double t0 = now_sec();
        size_t done = 0;
        const char* why = nullptr;
        const size_t step = batch ? batch : n;   // --batch 0: the whole corpus in one call
        for (size_t off = 0; off < n; off += step) {
            if ((why = guard.tripped())) break;
            const size_t m = std::min(step, n - off);
            store->ingest_batch(ef.edges + off, m, off);
            done += m;
            guard.events.store(done, std::memory_order_relaxed);
        }
        if (!why) store->ingest_finish();
        const double sec = now_sec() - t0;
        std::string note = store->ingest_note();
        if (why) {
            note = note.empty() ? std::string(why) : std::string(why) + "; " + note;
            skip_reason = std::string("skipped: ingest ") + why;
        } else {
            loaded = true;
            if (const char* late = guard.tripped()) {
                // The finish step ran past the deadline but completed: report
                // it as what it is rather than as a DNF.
                if (!note.empty()) note += "; ";
                note += std::string("over budget (") + late + " at finish)";
            }
        }
        guard.disarm();
        if (want.count("ingest"))
            rep.emit("ingest", done, 0, 0, sec, sec > 0 ? done / sec : 0.0, 0, 0, 0, 0, -1, note);
        std::fprintf(stderr, "[e33] %-9s %-18s ingest   %10zu events %9.2f s %7.3f Meps  rss %7.0f MB  %s\n",
                     system.c_str(), ds.c_str(), done, sec, sec > 0 ? done / sec / 1e6 : 0.0,
                     (rss_bytes() - rep.rss0) / 1e6, note.c_str());
    }

    // =============================================================== compact
    if (want.count("compact")) {
        if (!loaded) {
            rep.emit("compact", n, 0, 0, 0.0, 0, 0, 0, 0, 0, -1, skip_reason);
        } else if (!store->has_compact()) {
            rep.emit("compact", n, 0, 0, 0.0, 0, 0, 0, 0, 0, -1, store->compact_note());
            std::fprintf(stderr, "[e33] %-9s %-18s compact  %s\n", system.c_str(), ds.c_str(), store->compact_note());
        } else {
            guard.arm("compact", phase_budget, 0);
            const double t0 = now_sec();
            store->compact();
            const double sec = now_sec() - t0;
            const char* late = guard.tripped();
            guard.disarm();
            rep.emit("compact", n, 0, 0, sec, 0, 0, 0, 0, 0, -1, late ? std::string("over budget (") + late + ")" : "");
            std::fprintf(stderr, "[e33] %-9s %-18s compact  %9.2f s  rss %7.0f MB\n",
                         system.c_str(), ds.c_str(), sec, (rss_bytes() - rep.rss0) / 1e6);
        }
    }

    const bool any_query = want.count("q1") || want.count("q2") || want.count("q3") ||
                           want.count("q4") || want.count("bfs");
    if (any_query && !loaded) {
        for (const char* ph : {"q1", "q2", "q3", "q4", "bfs"})
            if (want.count(ph)) rep.emit(ph, n, 0, 0, 0.0, 0, 0, 0, 0, 0, -1, skip_reason);
        return 0;
    }
    if (any_query) store->begin_read();
#ifdef E33_HAVE_DUCKDB
    if (any_query && duckdb_explain)
        if (auto* d = dynamic_cast<DuckStore*>(store.get())) d->explain(q1_lo, q1_hi);
#endif

    // ==================================================================== q1
    if (want.count("q1")) {
        guard.arm("q1", phase_budget, 1);
        std::vector<Edge> buf;
        const double t0 = now_sec();
        store->scan(q1_lo, q1_hi, buf);
        const double sec = now_sec() - t0;
        uint64_t chk = 0;
        for (const Edge& e : buf)
            chk += e.timestamp ^ (static_cast<uint64_t>(e.dst) << 32) ^ static_cast<uint64_t>(e.src);
        const uint64_t count = buf.size();
        guard.disarm();
        std::string note;
        const char* verdict = "";
        const int ok = settle("q1", 1, 1, count, chk, nullptr, note, verdict);
        rep.emit("q1", n, 1, 1, sec, 0, 0, 0, count, chk, ok, note);
        std::fprintf(stderr, "[e33] %-9s %-18s q1       %10llu events %9.3f s  (%.1f ms)  chk %llu  %s\n",
                     system.c_str(), ds.c_str(), (unsigned long long)count, sec, sec * 1e3,
                     (unsigned long long)chk, verdict);
    }

    // The three latency phases share one driver; only the per-query lambda
    // differs, and each lambda is one call into the primitive interface plus
    // the spec's fold.
    auto latency_phase = [&](const char* ph, auto&& one) {
        guard.arm(ph, phase_budget, N);
        LatResult r = run_latency(guard, qs, phase_budget, one);
        guard.disarm();
        const char* verdict = "";
        const int ok = settle(ph, r.done, N, r.count, r.chk, &r.cum, r.note, verdict);
        const double med = median(r.lat), p95 = percentile(r.lat, 0.95);
        rep.emit(ph, n, N, r.done, r.seconds, r.seconds > 0 ? r.done / r.seconds : 0.0,
                 med, p95, r.count, r.chk, ok, r.note);
        std::fprintf(stderr, "[e33] %-9s %-18s %-8s %5zu/%zu q %9.3f s  median %10.1f us  p95 %10.1f us  count %llu  chk %llu  %s%s%s\n",
                     system.c_str(), ds.c_str(), ph, r.done, N, r.seconds, med, p95,
                     (unsigned long long)r.count, (unsigned long long)r.chk, verdict,
                     r.note.empty() ? "" : "  note: ", r.note.c_str());
    };

    // ==================================================================== q2
    if (want.count("q2")) {
        std::vector<TemporalRef> buf;
        latency_phase("q2", [&](const Query& q, uint64_t& count, uint64_t& chk) {
            buf.clear();
            store->out_in(static_cast<uint32_t>(q.v), q.lo, q.hi, buf);
            count += buf.size();
            for (const TemporalRef& r : buf) chk += r.ts ^ static_cast<uint64_t>(r.peer);
            return true;
        });
    }

    // ==================================================================== q3
    // Earliest-arrival reachability inside [lo, hi]: a(v) = lo; an out-event
    // (u -> d, ts) is usable when a(u) <= ts <= hi and improves d when d is
    // unlabeled or ts < a(d). The fixpoint is unique, so the visit order is
    // free; vertices are popped in increasing a() order (Dijkstra-style, stale
    // heap entries skipped), so each reached neighbourhood is scanned once per
    // query. This is the same loop as GraphOne's job 35
    // (baselines/graphone/analytics/e33_queries.h); the two must agree on the
    // algorithm, not only on the fixpoint, for the latencies to be comparable.
    if (want.count("q3")) {
        std::vector<TemporalRef> buf;
        std::unordered_map<uint32_t, uint64_t> label;
        using Item = std::pair<uint64_t, uint32_t>;                      // (a(u), u)
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
        label.reserve(1 << 12);
        latency_phase("q3", [&](const Query& q, uint64_t& count, uint64_t& chk) {
            const uint32_t v = static_cast<uint32_t>(q.v);
            label.clear();
            label.emplace(v, q.lo);
            heap = {};
            heap.emplace(q.lo, v);
            size_t steps = 0;
            while (!heap.empty()) {
                const Item top = heap.top();
                heap.pop();
                const uint64_t au = top.first;
                const uint32_t u = top.second;
                if (au != label.find(u)->second) continue;              // superseded by a smaller label
                buf.clear();
                store->out_in(u, au, q.hi, buf);
                for (const TemporalRef& r : buf) {
                    auto it = label.find(r.peer);
                    if (it == label.end()) { label.emplace(r.peer, r.ts); heap.emplace(r.ts, r.peer); }
                    else if (r.ts < it->second) { it->second = r.ts; heap.emplace(r.ts, r.peer); }
                }
                if ((++steps & 255) == 0 && guard.tripped()) return false;   // budget: abandon the search
            }
            for (const auto& [d, a] : label)
                if (d != v) { ++count; chk += static_cast<uint64_t>(d) ^ a; }
            return true;
        });
    }

    // ==================================================================== q4
    if (want.count("q4")) {
        std::vector<TemporalRef> buf;
        latency_phase("q4", [&](const Query& q, uint64_t& count, uint64_t& chk) {
            const uint32_t v = static_cast<uint32_t>(q.v);
            buf.clear();
            store->asof(v, q.hi, buf);
            count += buf.size();
            for (const TemporalRef& r : buf)
                chk += (static_cast<uint64_t>(r.peer) << 20) ^ r.ts ^ static_cast<uint64_t>(v);
            return true;
        });
    }

    // =================================================================== bfs
    if (want.count("bfs")) {
        guard.arm("bfs", phase_budget, 1);
        std::vector<int32_t> lvl(V, -1);
        std::vector<uint32_t> cur{root}, nxt, dsts;
        lvl[root] = 0;
        const char* why = nullptr;
        size_t expanded = 0, reached = 1;
        const double t0 = now_sec();
        while (!cur.empty() && !why) {
            nxt.clear();
            for (uint32_t u : cur) {
                dsts.clear();
                store->out_all(u, dsts);
                for (uint32_t d : dsts)
                    if (d < V && lvl[d] < 0) { lvl[d] = lvl[u] + 1; nxt.push_back(d); ++reached; }
                if ((++expanded & 255) == 0 && (why = guard.tripped())) break;
            }
            cur.swap(nxt);
        }
        const double sec = now_sec() - t0;
        guard.disarm();
        uint64_t chk = 0;
        std::string note;
        const char* verdict = "";
        int ok = -1;
        if (why) {
            note = std::string(why) + ": expanded " + std::to_string(expanded) + ", reached " +
                   std::to_string(reached) + " of " + std::to_string(V);
            verdict = "incomplete";
        } else {
            uint64_t h = 1469598103934665603ULL;
            for (uint32_t v = 0; v < V; ++v) h = (h ^ static_cast<uint64_t>(lvl[v] + 1)) * 1099511628211ULL;
            chk = h;
            ok = settle("bfs", 1, 1, 0, chk, nullptr, note, verdict);
        }
        rep.emit("bfs", n, 1, why ? 0 : 1, sec, 0, 0, 0, 0, chk, ok, note);
        std::fprintf(stderr, "[e33] %-9s %-18s bfs      reached %zu/%u %9.3f s  chk %llu  %s%s%s\n",
                     system.c_str(), ds.c_str(), reached, V, sec, (unsigned long long)chk, verdict,
                     note.empty() ? "" : "  note: ", note.c_str());
    }

    if (any_query) store->end_read();
    store.reset();
    return 0;
}
