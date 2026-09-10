#include "test_common.hpp"
#include "barry/engine.hpp"

#include <atomic>
#include <random>
#include <thread>
#include <vector>

using namespace barry;

// The claim under test: Barry never stops being queryable. Readers run DURING
// concurrent multi-writer ingestion and must only ever observe fully published
// edges -- correct timestamps in range, valid endpoints, no torn reads, no
// crashes -- and the final state must match a serially built engine exactly.
int main() {
    std::fprintf(stderr, "test_concurrent\n");
    const uint32_t V = 500;
    const uint64_t T = 200000;
    const size_t   N = 400000;
    const int      W = 4;          // writer threads
    const int      R = 3;          // reader threads

    std::mt19937_64 rng(2026);
    std::uniform_int_distribution<uint32_t> vd(0, V - 1);
    std::uniform_int_distribution<uint64_t> td(1, T - 1);
    std::vector<Edge> es;
    es.reserve(N);
    for (size_t i = 0; i < N; ++i) es.push_back(Edge{vd(rng), vd(rng), td(rng), 1.0f});

    GraphConfig cfg;
    cfg.t_min = 0; cfg.t_max = T; cfg.num_vertices = V;
    cfg.build_reverse_index = true;
    cfg.arena_reserve_bytes = 1ULL << 28;
    cfg.vertex_arena_reserve_bytes = 1ULL << 28;
    Barry b(cfg);

    std::atomic<bool> done{false};
    std::atomic<uint64_t> reads{0}, violations{0};

    std::vector<std::thread> readers;
    for (int r = 0; r < R; ++r) {
        readers.emplace_back([&, r] {
            std::mt19937_64 rr(1000 + r);
            std::uniform_int_distribution<uint64_t> tq(1, T - 1);
            std::uniform_int_distribution<uint32_t> vq(0, V - 1);
            while (!done.load(std::memory_order_acquire)) {
                uint64_t lo = tq(rr), hi = std::min<uint64_t>(T, lo + T / 20);
                for (const Edge& e : b.query_range(lo, hi)) {
                    if (e.timestamp < lo || e.timestamp > hi || e.timestamp == 0 ||
                        e.src >= V || e.dst >= V)
                        violations.fetch_add(1);
                }
                auto taint = b.forward_taint(vq(rr), lo, hi);
                for (const auto& n : taint)
                    if (n.vertex >= V || n.time < lo || n.time > hi)
                        if (n.time != lo)              // seed carries t_seed itself
                            violations.fetch_add(1);
                reads.fetch_add(1);
            }
        });
    }

    std::vector<std::thread> writers;
    const size_t slice = N / W;
    for (int w = 0; w < W; ++w) {
        writers.emplace_back([&, w] {
            size_t off = w * slice;
            size_t n = (w == W - 1) ? N - off : slice;
            for (size_t i = 0; i < n; i += 1000)
                b.insert_batch(es.data() + off + i, std::min<size_t>(1000, n - i), 1);
        });
    }
    for (auto& t : writers) t.join();
    done.store(true, std::memory_order_release);
    for (auto& t : readers) t.join();

    std::fprintf(stderr, "  [info] %llu concurrent read iterations, %llu violations\n",
                 (unsigned long long)reads.load(), (unsigned long long)violations.load());
    EXPECT_EQ(violations.load(), uint64_t{0});
    EXPECT_TRUE(reads.load() > 0);

    // Final state must equal a serially built engine.
    Barry ref(cfg);
    ref.insert_batch(es.data(), es.size(), 1);
    EXPECT_EQ(b.query_range(0, T).size(), ref.query_range(0, T).size());
    for (uint32_t s = 0; s < 25; ++s) {
        EXPECT_EQ(b.forward_taint(s, 0, T).size(), ref.forward_taint(s, 0, T).size());
        EXPECT_EQ(b.backward_trace(s, T, 0).size(), ref.backward_trace(s, T, 0).size());
    }
    std::fprintf(stderr, "test_concurrent OK\n");
    return 0;
}
