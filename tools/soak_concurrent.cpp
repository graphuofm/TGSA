// Concurrency soak at dataset scale: readers hammer range and taint queries
// with validity checks WHILE writers ingest the full corpus. This is the
// at-scale counterpart of tests/test_concurrent.cpp -- same invariants, real
// data volume, many threads, long runtime.
//
//   barry_soak --dataset <bin> --writers 12 --readers 4 [--batch 100000]
//
// Exit 0 iff zero invariant violations and the final edge count is exact.

#include "barry/engine.hpp"
#include "../benchmark/common.hpp"

#include <atomic>
#include <random>
#include <thread>
#include <vector>

using namespace barry;
using namespace barry::bench;

int main(int argc, char** argv) {
    Args args(argc, argv);
    std::string in_path = args.get("dataset");
    int writers  = static_cast<int>(args.getl("writers", 12));
    int readers  = static_cast<int>(args.getl("readers", 4));
    size_t batch = static_cast<size_t>(args.getl("batch", 100000));
    if (in_path.empty()) { std::fprintf(stderr, "--dataset required\n"); return 2; }

    EdgeFile ef;
    if (!ef.load(in_path)) return 2;
    GraphMeta meta = scan_meta(ef.edges, ef.count);
    const uint32_t V = meta.v_max + 1;

    GraphConfig cfg;
    cfg.t_min = meta.t_min; cfg.t_max = meta.t_max; cfg.num_vertices = V;
    cfg.build_reverse_index = true;
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, ef.count * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, ef.count * sizeof(EdgeRef) * 8);
    Barry b(cfg);

    std::atomic<bool> done{false};
    std::atomic<uint64_t> q_done{0}, violations{0}, edges_seen{0};

    std::vector<std::thread> rs;
    for (int r = 0; r < readers; ++r) {
        rs.emplace_back([&, r] {
            std::mt19937_64 rng(77 + r);
            const uint64_t span = std::max<uint64_t>(1, meta.t_max - meta.t_min);
            std::uniform_int_distribution<uint64_t> tq(meta.t_min, meta.t_max - 1);
            std::uniform_int_distribution<uint32_t> vq(0, V - 1);
            while (!done.load(std::memory_order_acquire)) {
                uint64_t lo = tq(rng);
                uint64_t hi = std::min(meta.t_max, lo + span / 1000);
                for (const Edge& e : b.query_range(lo, hi)) {
                    if (e.timestamp < lo || e.timestamp > hi || e.timestamp == 0 ||
                        e.src >= V || e.dst >= V)
                        violations.fetch_add(1);
                    edges_seen.fetch_add(1);
                }
                auto t = b.forward_taint(vq(rng), lo, hi);
                for (const auto& n : t)
                    if (n.vertex >= V || n.time == 0 || n.time > hi ||
                        (n.time < lo))
                        violations.fetch_add(1);
                q_done.fetch_add(1);
            }
        });
    }

    double t0 = now_sec();
    std::vector<std::thread> ws;
    const size_t slice = ef.count / writers;
    for (int w = 0; w < writers; ++w) {
        ws.emplace_back([&, w] {
            size_t off = static_cast<size_t>(w) * slice;
            size_t n = (w == writers - 1) ? ef.count - off : slice;
            for (size_t i = 0; i < n; i += batch)
                b.insert_batch(ef.edges + off + i, std::min(batch, n - i), 1);
        });
    }
    for (auto& t : ws) t.join();
    double ingest_sec = now_sec() - t0;
    done.store(true, std::memory_order_release);
    for (auto& t : rs) t.join();

    size_t final_edges = b.query_range(meta.t_min, meta.t_max).size();
    bool count_ok = (final_edges == ef.count);
    std::printf("[soak] %s: %zu edges ingested in %.1fs (%.2f Meps) under %d writers\n"
                "       %llu reader queries, %llu edges validated, %llu VIOLATIONS\n"
                "       final count %zu vs expected %zu -> %s\n",
                basename_noext(in_path).c_str(), ef.count, ingest_sec,
                ef.count / ingest_sec / 1e6, writers,
                (unsigned long long)q_done.load(),
                (unsigned long long)edges_seen.load(),
                (unsigned long long)violations.load(),
                final_edges, ef.count, count_ok ? "EXACT" : "MISMATCH");
    return (violations.load() == 0 && count_ok) ? 0 : 1;
}
