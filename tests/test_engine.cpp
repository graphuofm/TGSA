#include "barry/engine.hpp"
#include "test_common.hpp"

#include <algorithm>
#include <atomic>
#include <climits>
#include <thread>
#include <vector>

using namespace barry;

static GraphConfig small_cfg(uint32_t V = 16) {
    GraphConfig c;
    c.t_min = 0;
    c.t_max = 1ULL << 20;
    c.shift = 4;                     // window = 16
    c.timeline_slots = 1ULL << 16;
    c.arena_reserve_bytes = 1ULL << 28;
    c.num_vertices = V;
    c.vertex_arena_reserve_bytes = 1ULL << 24;
    c.default_block_capacity = 4;
    return c;
}

static void test_insert_query_roundtrip() {
    Barry b(small_cfg());
    b.insert({1, 2, 100, 1.0f});
    b.insert({2, 3, 100, 1.5f});
    b.insert({3, 4, 200, 2.0f});

    auto at_100 = b.query_at(100);
    EXPECT_EQ(at_100.size(), 2u);

    auto rng = b.query_range(50, 150);
    EXPECT_EQ(rng.size(), 2u);

    auto rng2 = b.query_range(50, 250);
    EXPECT_EQ(rng2.size(), 3u);
}

static void test_block_overflow_chain() {
    GraphConfig c = small_cfg();
    c.default_block_capacity = 2;
    Barry b(c);
    // 5 edges into the same window  -> 3+ blocks chained
    for (int i = 0; i < 5; ++i) b.insert({0, uint32_t(i+1), 5, float(i)});
    auto rng = b.query_range(0, 10);
    EXPECT_EQ(rng.size(), 5u);
}

static void test_concurrent_insert_no_loss() {
    Barry b(small_cfg(32));
    constexpr int T = 4;
    constexpr int N_PER = 5000;

    std::vector<std::thread> ts;
    for (int t = 0; t < T; ++t) {
        ts.emplace_back([&, t] {
            for (int i = 0; i < N_PER; ++i) {
                // ts >= 1: timestamp 0 is the unpublished-slot sentinel and is
                // rejected by insert() by contract.
                Edge e{uint32_t(t), uint32_t(t + 1),
                       uint64_t(i) + 1, 1.0f};
                b.insert(e);
            }
        });
    }
    for (auto& th : ts) th.join();

    auto all = b.query_range(0, N_PER + 1);
    EXPECT_EQ(all.size(), size_t(T * N_PER));
    EXPECT_EQ(b.stats().num_edges, size_t(T * N_PER));
}

static void test_remove_tombstone() {
    Barry b(small_cfg());
    b.insert({1, 2, 100, 1.0f});
    b.insert({3, 4, 100, 1.0f});

    EXPECT_EQ(b.query_at(100).size(), 2u);
    b.remove(1, 2, 100);
    EXPECT_EQ(b.query_at(100).size(), 1u);
}

static void test_window_aggregate() {
    Barry b(small_cfg());
    b.insert({1, 2, 10, 1.0f});
    b.insert({2, 3, 20, 2.0f});
    b.insert({3, 4, 30, 4.0f});
    EXPECT_EQ(b.window_aggregate_sum(0, 25), 3.0);
    EXPECT_EQ(b.window_aggregate_sum(0, 35), 7.0);
}

static void test_bfs_line_graph() {
    Barry b(small_cfg(16));
    for (uint32_t i = 0; i < 10; ++i)
        b.insert({i, i + 1, uint64_t(i) + 1, 1.0f});   // ts >= 1 by contract

    auto level = b.bfs(0);
    for (int i = 0; i < 11; ++i) {
        EXPECT_EQ(level[i], i);
    }
    // Vertex 11 is unreachable in a 16-vertex graph with edges 0->1..9->10
    EXPECT_EQ(level[11], -1);
}

static void test_earliest_arrival_time_respecting() {
    Barry b(small_cfg(16));
    // Edge 0->1 at t=10, then 1->2 at t=5: cannot reach 2 from 0 via time-respecting path.
    b.insert({0, 1, 10, 1.0f});
    b.insert({1, 2, 5,  1.0f});
    auto ea = b.earliest_arrival(0, 0, 100);
    EXPECT_EQ(ea[1], 10u);
    EXPECT_TRUE(ea[2] == ULLONG_MAX);

    // Add 1->2 at t=20 -> reachable at 20.
    b.insert({1, 2, 20, 1.0f});
    auto ea2 = b.earliest_arrival(0, 0, 100);
    EXPECT_EQ(ea2[2], 20u);
}

static void test_temporal_triangle_delta() {
    Barry b(small_cfg(16));
    // Triangle: 0->1 at t=1, 1->2 at t=2, 0->2 at t=3.
    b.insert({0, 1, 1, 1.0f});
    b.insert({1, 2, 2, 1.0f});
    b.insert({0, 2, 3, 1.0f});

    EXPECT_EQ(b.temporal_triangle_count(0, 100, 10), 1u);
    EXPECT_EQ(b.temporal_triangle_count(0, 100, 1), 0u);
}

int main() {
    RUN(test_insert_query_roundtrip);
    RUN(test_block_overflow_chain);
    RUN(test_concurrent_insert_no_loss);
    RUN(test_remove_tombstone);
    RUN(test_window_aggregate);
    RUN(test_bfs_line_graph);
    RUN(test_earliest_arrival_time_respecting);
    RUN(test_temporal_triangle_delta);
    std::fprintf(stderr, "test_engine OK\n");
    return 0;
}
