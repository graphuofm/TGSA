// E27: scale-out readiness, measured on one box.
//
// A reviewer's fair question about a single-node engine is whether the
// design shards. This bench answers it without pretending to be a
// distributed system: S independent engine shards in one process, no
// shared state between them, edges routed by a pure function, queries
// answered by routing (single shard) or scatter-gather (all shards),
// results checksum-verified against an unsharded reference. What is
// measured is the design's parallel decomposition; networks, replication,
// and partial failure are future work and say so in the paper.
//
// Two partitioning axes, because they fail differently:
//
//   by_src   shard = hash(src) % S. Entity-keyed queries route to one
//            shard; window scans scatter-gather. Every shard sees the
//            whole time range, so a time-ordered stream keeps ALL shards
//            busy: this is the axis that scales ingest.
//   by_time  shard owns a contiguous 1/S slice of the time range. Window
//            queries route; entity queries scatter-gather.
//
// Two ingest modes, because they answer different questions:
//
//   live     one global stream in arrival order; each batch is routed to
//            its shard(s) as it arrives. by_src spreads every batch over
//            all shards; by_time hands nearly every batch to the one
//            shard that owns the current time slice, so only one shard is
//            hot at a time. This is the mode that says which axis a live
//            deployment must shard on.
//   replay   the corpus is pre-partitioned and every shard loads its
//            slice concurrently, which is exactly recovery (each node
//            replays its own log) and bulk backfill. by_time is expected
//            to scale HERE even though it cannot scale live.
//
// Thread budget is fixed (default 16) and divided among shards, so the
// curve is scale-out of the design, not just more cores.
//
// Usage:
//   bench_e27_shard --dataset <bin> [--shards 1,2,4,8] [--threads_total 16]
//                   [--samples N] [--window_frac F] --output csv

#include "common.hpp"
#include "barry/engine.hpp"
#include "barry/vertex_index.hpp"

#include <atomic>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace barry;
using namespace barry::bench;

namespace {

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

inline uint32_t src_shard(uint32_t src, int S) {
    // Cheap avalanche so consecutive vertex ids spread.
    uint32_t h = src * 0x9E3779B1u;
    return (h >> 16) % S;
}

}  // namespace

int main(int argc, char** argv) {
    Args args(argc, argv);
    const std::string in_path = args.get("dataset");
    const std::string out_csv = args.get("output", "");
    const std::string shard_list = args.get("shards", "1,2,4,8");
    const int threads_total = static_cast<int>(args.getl("threads_total", 16));
    const int samples = static_cast<int>(args.getl("samples", 2000));
    const double window_frac = args.getd("window_frac", 0.01);

    if (in_path.empty() || out_csv.empty()) {
        std::fprintf(stderr, "usage: %s --dataset <bin> [--shards 1,2,4] "
                     "[--threads_total N] --output csv\n", args.prog().c_str());
        return 1;
    }

    EdgeFile ef;
    if (!ef.load(in_path)) return 1;
    const size_t n = ef.count;
    const GraphMeta meta = scan_meta(ef.edges, n);
    const std::string ds = basename_noext(in_path);
    const uint64_t span = (meta.t_max > meta.t_min) ? meta.t_max - meta.t_min : 1;
    const uint64_t window = std::max<uint64_t>(1, static_cast<uint64_t>(span * window_frac));

    CsvWriter w(out_csv,
        {"system","dataset","partition","ingest_mode","shards",
         "threads_per_shard","edges","ingest_sec","ingest_Meps",
         "speedup_vs_1","nbr_ms","nbr_route","scan_ms","scan_route",
         "delivered_nbr","checksum_nbr","scan_count","checksum_ok"});

    // Query workloads, fixed across every configuration.
    std::mt19937_64 rng(0xE27ull);
    std::vector<uint32_t> qv(samples);
    std::vector<uint64_t> qlo(samples);
    for (int i = 0; i < samples; ++i) {
        const size_t e = std::uniform_int_distribution<size_t>(0, n - 1)(rng);
        qv[i] = ef.edges[e].src;
        qlo[i] = ef.edges[e].timestamp;
    }
    std::vector<uint64_t> slo(64);
    for (auto& t : slo)
        t = meta.t_min + rng() % (span > window ? span - window : 1);

    // Unsharded reference answers.
    uint64_t ref_nbr_sum = 0, ref_nbr_cnt = 0, ref_scan_cnt = 0;
    {
        Barry b(make_cfg(meta, n));
        b.insert_batch(ef.edges, n, threads_total);
        b.finalize();
        const VertexIndex* vi = b.vertex_index();
        std::vector<TemporalRef> sc;
        for (int i = 0; i < samples; ++i) {
            sc.clear();
            vi->neighbors_temporal_into(qv[i], qlo[i], qlo[i] + window, sc);
            ref_nbr_cnt += sc.size();
            for (const TemporalRef& r : sc)
                ref_nbr_sum += r.ts ^ static_cast<uint64_t>(r.peer);
        }
        for (uint64_t t : slo) ref_scan_cnt += b.query_range(t, t + window).size();
    }

    struct Part { const char* name; bool by_time; };
    const Part parts[] = {{"by_src", false}, {"by_time", true}};

    std::vector<int> shard_counts;
    for (size_t p = 0; p < shard_list.size();) {
        size_t q = shard_list.find(',', p);
        if (q == std::string::npos) q = shard_list.size();
        shard_counts.push_back(std::stoi(shard_list.substr(p, q - p)));
        p = q + 1;
    }

    struct Mode { const char* name; bool replay; };
    const Mode modes[] = {{"live", false}, {"replay", true}};

    for (const Part& part : parts) {
      for (const Mode& mode : modes) {
        double meps1 = 0;
        for (int S : shard_counts) {
            const int tps = std::max(1, threads_total / S);
            const uint64_t slice = span / S + 1;
            auto shard_of = [&](const Edge& e) {
                const int s = part.by_time
                    ? static_cast<int>((e.timestamp - meta.t_min) / slice)
                    : static_cast<int>(src_shard(e.src, S));
                return std::min(s, S - 1);
            };

            // Routing happens outside the timer in both modes: a
            // collector's partitioner is upstream of every store in a real
            // deployment and identical work for any sharded system. What
            // differs is WHEN a shard's edges become available: replay
            // pre-partitions the whole corpus; live keeps arrival order and
            // routes batch by batch.
            std::vector<std::vector<Edge>> routed(S);
            if (mode.replay) {
                for (auto& v : routed) v.reserve(n / S + 16);
                for (size_t i = 0; i < n; ++i)
                    routed[shard_of(ef.edges[i])].push_back(ef.edges[i]);
            }

            std::vector<std::unique_ptr<Barry>> shards;
            for (int s = 0; s < S; ++s)
                shards.push_back(std::make_unique<Barry>(make_cfg(meta, n)));

            constexpr size_t kBatch = 65536;
            double ing = 0;
            if (mode.replay) {
                // Every shard loads its slice concurrently: recovery and
                // bulk backfill semantics.
                const double t0 = now_sec();
                std::vector<std::thread> th;
                for (int s = 0; s < S; ++s) {
                    th.emplace_back([&, s] {
                        const auto& v = routed[s];
                        for (size_t off = 0; off < v.size(); off += kBatch)
                            shards[s]->insert_batch(v.data() + off,
                                std::min(kBatch, v.size() - off), tps);
                    });
                }
                for (auto& t : th) t.join();
                ing = now_sec() - t0;
            } else {
                // Live: the global stream advances one arrival-order batch
                // at a time; every shard with work in the current batch
                // runs concurrently, but no shard may run ahead of the
                // stream. Under by_time nearly every batch lands on one
                // shard, and that serialization is the measurement. The
                // per-batch bucketing is the collector's router and is
                // prepared outside the timer, as in replay mode.
                const size_t nbatches = (n + kBatch - 1) / kBatch;
                std::vector<std::vector<std::vector<Edge>>> buckets(nbatches);
                for (size_t bi = 0; bi < nbatches; ++bi) {
                    buckets[bi].resize(S);
                    const size_t off = bi * kBatch;
                    const size_t m = std::min(kBatch, n - off);
                    for (size_t i = off; i < off + m; ++i)
                        buckets[bi][shard_of(ef.edges[i])].push_back(ef.edges[i]);
                }
                const double t0 = now_sec();
                for (size_t bi = 0; bi < nbatches; ++bi) {
                    std::vector<std::thread> th;
                    for (int s = 0; s < S; ++s) {
                        if (buckets[bi][s].empty()) continue;
                        th.emplace_back([&, bi, s] {
                            shards[s]->insert_batch(buckets[bi][s].data(),
                                                    buckets[bi][s].size(), tps);
                        });
                    }
                    for (auto& t : th) t.join();
                }
                ing = now_sec() - t0;
            }
            size_t total = 0, dropped = 0;
            for (auto& b : shards) {
                total += b->stats().num_edges;
                dropped += b->edges_dropped();
            }
            if (total != n || dropped != 0) {
                std::fprintf(stderr, "[e27] COUNT MISMATCH %s %s S=%d\n",
                             part.name, mode.name, S);
                return 1;
            }
            for (auto& b : shards) b->finalize();

            // Entity expansion: route on by_src, scatter-gather on by_time.
            uint64_t nbr_sum = 0, nbr_cnt = 0;
            double q0 = now_sec();
            {
                std::vector<TemporalRef> sc;
                for (int i = 0; i < samples; ++i) {
                    const uint64_t lo = qlo[i], hi = qlo[i] + window;
                    if (!part.by_time) {
                        const VertexIndex* vi =
                            shards[src_shard(qv[i], S)]->vertex_index();
                        sc.clear();
                        vi->neighbors_temporal_into(qv[i], lo, hi, sc);
                        nbr_cnt += sc.size();
                        for (const TemporalRef& r : sc)
                            nbr_sum += r.ts ^ static_cast<uint64_t>(r.peer);
                    } else {
                        for (int s = 0; s < S; ++s) {
                            sc.clear();
                            shards[s]->vertex_index()->
                                neighbors_temporal_into(qv[i], lo, hi, sc);
                            nbr_cnt += sc.size();
                            for (const TemporalRef& r : sc)
                                nbr_sum += r.ts ^ static_cast<uint64_t>(r.peer);
                        }
                    }
                }
            }
            const double nbr_ms = (now_sec() - q0) * 1e3;

            // Window scan: route on by_time, scatter-gather on by_src.
            uint64_t scan_cnt = 0;
            q0 = now_sec();
            for (uint64_t t : slo) {
                const uint64_t lo = t, hi = t + window;
                if (part.by_time) {
                    const int s0 = std::min<int>(S - 1, (lo - meta.t_min) / slice);
                    const int s1 = std::min<int>(S - 1, (hi - meta.t_min) / slice);
                    for (int s = s0; s <= s1; ++s)
                        scan_cnt += shards[s]->query_range(lo, hi).size();
                } else {
                    for (int s = 0; s < S; ++s)
                        scan_cnt += shards[s]->query_range(lo, hi).size();
                }
            }
            const double scan_ms = (now_sec() - q0) * 1e3;

            const bool ok = (nbr_sum == ref_nbr_sum && nbr_cnt == ref_nbr_cnt &&
                             scan_cnt == ref_scan_cnt);
            if (!ok) {
                std::fprintf(stderr, "[e27] CHECKSUM MISMATCH %s %s S=%d\n",
                             part.name, mode.name, S);
                return 1;
            }
            const double meps = n / ing / 1e6;
            if (S == shard_counts.front()) meps1 = meps;

            w.row(std::string("tgsr"), ds, std::string(part.name),
                  std::string(mode.name), S, tps,
                  (unsigned long long)n, ing, meps, meps / meps1,
                  nbr_ms, std::string(part.by_time ? "scatter" : "route"),
                  scan_ms, std::string(part.by_time ? "route" : "scatter"),
                  (unsigned long long)nbr_cnt, (unsigned long long)nbr_sum,
                  (unsigned long long)scan_cnt, (int)ok);
            std::fprintf(stderr,
                "[e27] %-7s %-6s S=%d (%2d thr/shard)  ingest %6.2f Meps (%.2fx)  "
                "nbr %8.1f ms (%s)  scan %8.1f ms (%s)  checksums ok\n",
                part.name, mode.name, S, tps, meps, meps / meps1, nbr_ms,
                part.by_time ? "scatter" : "route", scan_ms,
                part.by_time ? "route" : "scatter");
        }
      }
    }
    return 0;
}
