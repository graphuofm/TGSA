// E26: concurrent throughput head-to-head against the in-category baselines.
//
// E19/E20 compared the systems phase by phase (ingest, then query). A
// streaming store's real job is both at once, and E21 measured that for TGSR
// alone. This bench runs one protocol on TGSR and LiveGraph in-process, and
// emits the same protocol's inputs for GraphOne's harness (job 29), so all
// three absorb the identical stream under identical reader pressure:
//
//   1. Preload the first half of the corpus (arrival order).
//   2. W writer threads stream the second half through the store's normal
//      concurrent write path, full speed.
//   3. R reader threads run windowed-neighborhood queries continuously until
//      the writers finish. Every query window closes strictly before the
//      earliest streamed timestamp (settled), so its answer is fixed: each
//      reader checks every answer against a reference computed on a quiescent
//      engine, and a single mismatch fails the run.
//
// Reported: sustained ingest Meps over the streaming phase, queries/s
// completed during that phase, and the mismatch count (must be 0).
// The comparison axis is deliberately throughput on both sides: that is the
// claim under test.
//
// Write paths, chosen as each system's honest concurrent path:
//   TGSR       insert_batch from W threads' run-claimed parallel path.
//   LiveGraph  one transaction per batch per writer thread (its MVCC path;
//              the batch loader is single-writer bulk and cannot be used
//              while readers run).
//   GraphOne   batch_edge (atomic fetch_add into the edge log) from W
//              threads; readers use STALE_MASK static views over the
//              archived adjacency, which settled windows make exact. Its
//              asynchronous archive catch-up is timed separately (job 29).
//
// Modes:
//   prep       write <out>/queries.bin (u64 v, lo, hi triples) and
//              <out>/expected.bin (u64 per query: count + sum ts^dst),
//              plus split info on stderr. Run once per corpus/window.
//   tgsr       run the protocol on TGSR.
//   livegraph  run the protocol on LiveGraph (built only when LiveGraph is).
//
// Usage:
//   bench_e26_concurrent --dataset <bin> --mode prep|tgsr|livegraph
//     --writers W --readers R [--samples N] [--window_frac F] [--batch N]
//     --qdir <dir with queries.bin/expected.bin> --output csv

#include "common.hpp"
#include "barry/engine.hpp"
#include "barry/vertex_index.hpp"

#ifdef E26_HAVE_LIVEGRAPH
#include "livegraph.hpp"
#include <cstring>
#endif

#include <atomic>
#include <climits>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace barry;
using namespace barry::bench;

namespace {

struct Query { uint64_t v, lo, hi; };

std::string qpath(const std::string& d) { return d + "/queries.bin"; }
std::string epath(const std::string& d) { return d + "/expected.bin"; }

bool load_protocol(const std::string& dir, std::vector<Query>& qs,
                   std::vector<uint64_t>& expect) {
    FILE* qf = std::fopen(qpath(dir).c_str(), "rb");
    if (!qf) { std::perror(qpath(dir).c_str()); return false; }
    Query q;
    while (std::fread(&q, sizeof q, 1, qf) == 1) qs.push_back(q);
    std::fclose(qf);
    FILE* ef = std::fopen(epath(dir).c_str(), "rb");
    if (!ef) { std::perror(epath(dir).c_str()); return false; }
    uint64_t h;
    while (std::fread(&h, sizeof h, 1, ef) == 1) expect.push_back(h);
    std::fclose(ef);
    return !qs.empty() && qs.size() == expect.size();
}

GraphConfig make_cfg(const GraphMeta& meta, size_t n) {
    GraphConfig cfg;
    cfg.t_min = meta.t_min;
    cfg.t_max = meta.t_max;
    cfg.num_vertices = meta.v_max + 1;
    cfg.build_reverse_index = false;
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, n * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, n * sizeof(EdgeRef) * 8);
    return cfg;
}

// One checksum formula for every system: count + sum over delivered edges of
// (ts XOR dst). A faster reader that answered a different question cannot
// pass, and a torn read changes the sum.

}  // namespace

int main(int argc, char** argv) {
    Args args(argc, argv);
    const std::string in_path = args.get("dataset");
    const std::string mode    = args.get("mode", "prep");
    const std::string qdir    = args.get("qdir", "");
    const std::string out_csv = args.get("output", "");
    const int    writers      = static_cast<int>(args.getl("writers", 8));
    const int    readers      = static_cast<int>(args.getl("readers", 4));
    const int    samples_in   = static_cast<int>(args.getl("samples", 2000));
    const double window_frac  = args.getd("window_frac", 0.01);
    const size_t batch        = static_cast<size_t>(args.getl("batch", 65536));

    if (in_path.empty() || qdir.empty()) {
        std::fprintf(stderr, "usage: %s --dataset <bin> --mode prep|tgsr|livegraph "
                     "--qdir dir [--writers W --readers R --samples N "
                     "--window_frac F --batch N] --output csv\n", args.prog().c_str());
        return 1;
    }

    EdgeFile ef;
    if (!ef.load(in_path)) return 1;
    const size_t n = ef.count;
    const GraphMeta meta = scan_meta(ef.edges, n);
    const std::string ds = basename_noext(in_path);
    const uint64_t span = (meta.t_max > meta.t_min) ? meta.t_max - meta.t_min : 1;
    const uint64_t window = std::max<uint64_t>(1, static_cast<uint64_t>(span * window_frac));

    const size_t preload = n / 2;
    const size_t stream  = n - preload;
    uint64_t t_split = ULLONG_MAX;
    for (size_t i = preload; i < n; ++i)
        if (ef.edges[i].timestamp < t_split) t_split = ef.edges[i].timestamp;

    // ---------------------------------------------------------------- prep
    if (mode == "prep") {
        std::mt19937_64 rng(0xE26ull);
        std::vector<Query> qs;
        qs.reserve(samples_in);
        for (size_t tries = 0; qs.size() < static_cast<size_t>(samples_in) &&
                               tries < 64ull * samples_in; ++tries) {
            const size_t e = std::uniform_int_distribution<size_t>(0, preload - 1)(rng);
            const uint64_t lo = ef.edges[e].timestamp;
            if (lo + window >= t_split) continue;      // window still open
            qs.push_back({ef.edges[e].src, lo, lo + window});
        }
        if (qs.empty()) {
            std::fprintf(stderr, "[e26-prep] no settled windows at window_frac %g\n",
                         window_frac);
            return 1;
        }

        // Reference answers on a quiescent engine holding exactly the preload.
        Barry b(make_cfg(meta, n));
        b.insert_batch(ef.edges, preload, 8);
        const VertexIndex* vi = b.vertex_index();
        std::vector<TemporalRef> scratch;
        std::vector<uint64_t> expect(qs.size());
        for (size_t i = 0; i < qs.size(); ++i) {
            scratch.clear();
            vi->neighbors_temporal_into(static_cast<uint32_t>(qs[i].v),
                                        qs[i].lo, qs[i].hi, scratch);
            uint64_t h = scratch.size();
            for (const TemporalRef& r : scratch)
                h += r.ts ^ static_cast<uint64_t>(r.peer);
            expect[i] = h;
        }

        FILE* qf = std::fopen(qpath(qdir).c_str(), "wb");
        if (!qf) { std::perror(qpath(qdir).c_str()); return 1; }
        for (const Query& q : qs)
            if (std::fwrite(&q, sizeof q, 1, qf) != 1) { std::perror("fwrite"); return 1; }
        std::fclose(qf);
        FILE* xf = std::fopen(epath(qdir).c_str(), "wb");
        if (!xf) { std::perror(epath(qdir).c_str()); return 1; }
        for (uint64_t h : expect)
            if (std::fwrite(&h, sizeof h, 1, xf) != 1) { std::perror("fwrite"); return 1; }
        std::fclose(xf);

        std::fprintf(stderr,
            "[e26-prep] %s: %zu settled queries, window=%llu, preload=%zu "
            "stream=%zu t_split=%llu\n", ds.c_str(), qs.size(),
            (unsigned long long)window, preload, stream,
            (unsigned long long)t_split);
        return 0;
    }

    // Protocol inputs shared by every measuring mode.
    std::vector<Query> qs;
    std::vector<uint64_t> expect;
    if (!load_protocol(qdir, qs, expect)) {
        std::fprintf(stderr, "[e26] bad or missing protocol files in %s "
                     "(run --mode prep first)\n", qdir.c_str());
        return 1;
    }
    const int samples = static_cast<int>(qs.size());

    CsvWriter w(out_csv,
        {"system","dataset","writers","readers","edges_preloaded","edges_streamed",
         "preload_sec","stream_sec","ingest_Meps","queries","queries_per_sec",
         "q_median_us","q_p95_us","delivered","mismatches","samples"});

    // Shared reader/writer scaffolding: `query` answers one settled query and
    // returns the checksum; `write_range` streams [off, off+m). Both are
    // provided per system below.
    auto run_protocol = [&](const char* sysname,
                            const std::function<uint64_t(const Query&, size_t&)>& query,
                            const std::function<void(size_t, size_t)>& write_batch,
                            double preload_sec) {
        std::atomic<bool> stop{false};
        std::atomic<size_t> mismatches{0};
        std::atomic<size_t> delivered{0};
        std::vector<std::vector<double>> lat(readers);
        std::vector<std::thread> rth;
        for (int r = 0; r < readers; ++r) {
            rth.emplace_back([&, r] {
                std::mt19937_64 lrng(0xBEEFull + r);
                size_t local_del = 0, local_mis = 0;
                lat[r].reserve(1 << 16);
                while (!stop.load(std::memory_order_relaxed)) {
                    const int i = std::uniform_int_distribution<int>(0, samples - 1)(lrng);
                    const double q0 = now_sec();
                    size_t got = 0;
                    const uint64_t h = query(qs[i], got);
                    lat[r].push_back((now_sec() - q0) * 1e6);
                    local_del += got;
                    if (h != expect[i]) ++local_mis;
                }
                delivered.fetch_add(local_del, std::memory_order_relaxed);
                mismatches.fetch_add(local_mis, std::memory_order_relaxed);
            });
        }

        // Writers pull batch indices from a shared counter, so the stream is
        // consumed in arrival order per batch with W concurrent producers.
        std::atomic<size_t> next{preload};
        const double w0 = now_sec();
        std::vector<std::thread> wth;
        for (int t = 0; t < writers; ++t) {
            wth.emplace_back([&] {
                for (;;) {
                    const size_t off = next.fetch_add(batch, std::memory_order_relaxed);
                    if (off >= n) break;
                    write_batch(off, std::min(batch, n - off));
                }
            });
        }
        for (auto& t : wth) t.join();
        const double stream_sec = now_sec() - w0;
        stop.store(true, std::memory_order_relaxed);
        for (auto& t : rth) t.join();

        std::vector<double> all;
        for (auto& v : lat) all.insert(all.end(), v.begin(), v.end());
        const size_t nq = all.size();

        w.row(std::string(sysname), ds, writers, readers,
              (unsigned long long)preload, (unsigned long long)stream,
              preload_sec, stream_sec, stream / stream_sec / 1e6,
              (unsigned long long)nq, nq / stream_sec,
              nq ? median(all) : 0.0, nq ? percentile(all, 0.95) : 0.0,
              (unsigned long long)delivered.load(),
              (unsigned long long)mismatches.load(), samples);
        std::fprintf(stderr,
            "[e26] %-10s W=%d R=%d  stream %6.2fs = %6.2f Meps  "
            "queries=%8zu (%9.0f q/s)  mismatches=%zu\n",
            sysname, writers, readers, stream_sec, stream / stream_sec / 1e6,
            nq, nq / stream_sec, (size_t)mismatches.load());
        return mismatches.load() == 0;
    };

    // ---------------------------------------------------------------- tgsr
    if (mode == "tgsr") {
        Barry b(make_cfg(meta, n));
        const double p0 = now_sec();
        b.insert_batch(ef.edges, preload, writers);
        const double preload_sec = now_sec() - p0;
        const VertexIndex* vi = b.vertex_index();

        thread_local std::vector<TemporalRef> scratch;
        auto query = [&](const Query& q, size_t& got) -> uint64_t {
            scratch.clear();
            vi->neighbors_temporal_into(static_cast<uint32_t>(q.v), q.lo, q.hi, scratch);
            uint64_t h = scratch.size();
            for (const TemporalRef& r : scratch)
                h += r.ts ^ static_cast<uint64_t>(r.peer);
            got = scratch.size();
            return h;
        };
        // Each writer thread hands its batch to the ordinary insert path
        // single-threaded: W concurrent callers, not one caller fanning out,
        // so the concurrency under test is real multi-writer contention.
        auto write_batch = [&](size_t off, size_t m) {
            b.insert_batch(ef.edges + off, m, 1);
        };
        const bool ok = run_protocol("tgsr", query, write_batch, preload_sec);
        if (b.edges_dropped() != 0) {
            std::fprintf(stderr, "[e26] tgsr dropped %zu edges\n", b.edges_dropped());
            return 1;
        }
        return ok ? 0 : 2;
    }

#ifdef E26_HAVE_LIVEGRAPH
    // ----------------------------------------------------------- livegraph
    if (mode == "livegraph") {
        const uint32_t V = meta.v_max + 1;
        lg::Graph g("", "", 1ul << 40, static_cast<lg::vertex_t>(V) + 16);
        {
            auto txn = g.begin_batch_loader();
            for (uint32_t v = 0; v < V; ++v) txn.new_vertex();
            txn.commit();
        }
        auto ts_view = [](const uint64_t& t) {
            return std::string_view(reinterpret_cast<const char*>(&t), sizeof t);
        };
        // Preload through the bulk path (setup, same as E19), stream through
        // the transactional path (the only concurrent-safe one).
        const double p0 = now_sec();
        for (size_t off = 0; off < preload; off += batch) {
            const size_t m = std::min(batch, preload - off);
            auto txn = g.begin_batch_loader();
            for (size_t i = off; i < off + m; ++i) {
                const Edge& e = ef.edges[i];
                txn.put_edge(e.src, 0, e.dst, ts_view(e.timestamp), true);
            }
            txn.commit();
        }
        const double preload_sec = now_sec() - p0;

        auto query = [&](const Query& q, size_t& got) -> uint64_t {
            auto txn = g.begin_read_only_transaction();
            uint64_t h = 0; size_t cnt = 0;
            auto it = txn.get_edges(static_cast<lg::vertex_t>(q.v), 0);
            while (it.valid()) {
                uint64_t ts = 0;
                auto d = it.edge_data();
                if (d.size() >= sizeof(uint64_t)) std::memcpy(&ts, d.data(), sizeof ts);
                if (ts >= q.lo && ts <= q.hi) {
                    h += ts ^ static_cast<uint64_t>(it.dst_id());
                    ++cnt;
                }
                it.next();
            }
            txn.abort();
            got = cnt;
            return h + cnt;
        };
        // Two concurrent write drivings, both reported:
        //
        // arrival: writers take arrival-order chunks, 1K-edge transactions,
        //   retry on abort. This is what the stream is, and it is where
        //   vertex-locked MVCC collides with a time-ordered event stream:
        //   consecutive arrivals share hot sources, so writers spend their
        //   time aborting each other (measured collapse: 1.75 Meps at one
        //   writer to 0.05 at two on askubuntu). A 64K-edge transaction
        //   variant was worse still; both are the store's semantics, not a
        //   harness bug, but we do not stop there.
        //
        // sharded: the collector repartitions the stream by source vertex,
        //   so no two writers ever touch the same adjacency and no
        //   transaction can conflict. This is LiveGraph's best case, bought
        //   by giving up arrival order across writers, a resharding step
        //   TGSR does not need.
        const bool shard = args.getl("lg_shard", 1) != 0;
        std::atomic<size_t> aborts{0};
        auto put_range_txn = [&](size_t c, size_t e_hi) {
            for (;;) {
                auto txn = g.begin_transaction();
                bool ok = true;
                try {
                    for (size_t i = c; i < e_hi; ++i) {
                        const Edge& e = ef.edges[i];
                        txn.put_edge(e.src, 0, e.dst, ts_view(e.timestamp), true);
                    }
                    txn.commit();
                } catch (...) { txn.abort(); ok = false;
                                aborts.fetch_add(1, std::memory_order_relaxed); }
                if (ok) break;
            }
        };
        std::function<void(size_t, size_t)> write_batch;
        if (!shard) {
            write_batch = [&](size_t off, size_t m) {
                constexpr size_t kTxn = 1024;
                for (size_t c = off; c < off + m; c += kTxn)
                    put_range_txn(c, std::min(off + m, c + kTxn));
            };
        } else {
            // The chunk [off, off+m) is filtered per writer by src hash, so
            // the writers cooperatively cover it conflict-free. Each writer
            // buffers its shard of the chunk and commits it as transactions.
            write_batch = [&](size_t off, size_t m) {
                (void)off; (void)m;
            };
        }

        if (shard) {
            // Sharded driving replaces the generic writer pool: writer t owns
            // src % W == t and walks the whole streamed half.
            std::atomic<bool> stop{false};
            std::atomic<size_t> mismatches{0};
            std::atomic<size_t> delivered{0};
            std::vector<std::vector<double>> lat(readers);
            std::vector<std::thread> rth;
            for (int r0 = 0; r0 < readers; ++r0) {
                rth.emplace_back([&, r0] {
                    std::mt19937_64 lrng(0xBEEFull + r0);
                    size_t local_del = 0, local_mis = 0;
                    lat[r0].reserve(1 << 16);
                    while (!stop.load(std::memory_order_relaxed)) {
                        const int i = std::uniform_int_distribution<int>(0, samples - 1)(lrng);
                        const double q0 = now_sec();
                        size_t got = 0;
                        const uint64_t h = query(qs[i], got);
                        lat[r0].push_back((now_sec() - q0) * 1e6);
                        local_del += got;
                        if (h != expect[i]) ++local_mis;
                    }
                    delivered.fetch_add(local_del);
                    mismatches.fetch_add(local_mis);
                });
            }
            const double w0 = now_sec();
            std::vector<std::thread> wth;
            for (int t = 0; t < writers; ++t) {
                wth.emplace_back([&, t] {
                    constexpr size_t kTxn = 1024;
                    size_t buf[kTxn];
                    size_t cnt = 0;
                    auto flush = [&] {
                        if (!cnt) return;
                        for (;;) {
                            auto txn = g.begin_transaction();
                            bool ok = true;
                            try {
                                for (size_t x = 0; x < cnt; ++x) {
                                    const Edge& e = ef.edges[buf[x]];
                                    txn.put_edge(e.src, 0, e.dst,
                                                 ts_view(e.timestamp), true);
                                }
                                txn.commit();
                            } catch (...) { txn.abort(); ok = false;
                                            aborts.fetch_add(1); }
                            if (ok) break;
                        }
                        cnt = 0;
                    };
                    for (size_t i = preload; i < n; ++i) {
                        if (static_cast<int>(ef.edges[i].src % writers) != t) continue;
                        buf[cnt++] = i;
                        if (cnt == kTxn) flush();
                    }
                    flush();
                });
            }
            for (auto& t : wth) t.join();
            const double stream_sec = now_sec() - w0;
            stop.store(true, std::memory_order_relaxed);
            for (auto& t : rth) t.join();

            std::vector<double> all;
            for (auto& v : lat) all.insert(all.end(), v.begin(), v.end());
            const size_t nq = all.size();
            w.row(std::string("livegraph_sharded"), ds, writers, readers,
                  (unsigned long long)preload, (unsigned long long)stream,
                  preload_sec, stream_sec, stream / stream_sec / 1e6,
                  (unsigned long long)nq, nq / stream_sec,
                  nq ? median(all) : 0.0, nq ? percentile(all, 0.95) : 0.0,
                  (unsigned long long)delivered.load(),
                  (unsigned long long)mismatches.load(), samples);
            std::fprintf(stderr,
                "[e26] %-10s W=%d R=%d  stream %6.2fs = %6.2f Meps  "
                "queries=%8zu (%9.0f q/s)  mismatches=%zu aborts=%zu\n",
                "lg_sharded", writers, readers, stream_sec,
                stream / stream_sec / 1e6, nq, nq / stream_sec,
                (size_t)mismatches.load(), (size_t)aborts.load());
            return mismatches.load() == 0 ? 0 : 2;
        }

        const bool ok = run_protocol("livegraph", query, write_batch, preload_sec);
        std::fprintf(stderr, "[e26] livegraph aborts=%zu\n", (size_t)aborts.load());
        return ok ? 0 : 2;
    }
#endif

    std::fprintf(stderr, "unknown or unbuilt mode %s\n", mode.c_str());
    return 1;
}
