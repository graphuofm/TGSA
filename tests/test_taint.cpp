#include "test_common.hpp"
#include "barry/engine.hpp"

#include <algorithm>
#include <climits>
#include <map>
#include <random>
#include <vector>

using namespace barry;

static GraphConfig small_cfg(uint64_t t_min, uint64_t t_max, uint32_t V) {
    GraphConfig cfg;
    cfg.t_min = t_min;
    cfg.t_max = t_max;
    cfg.shift = 0;
    cfg.num_vertices = V;
    cfg.build_reverse_index = true;
    cfg.arena_reserve_bytes        = 1ULL << 26;
    cfg.vertex_arena_reserve_bytes = 1ULL << 26;
    return cfg;
}

static std::map<uint32_t, uint64_t> as_map(const std::vector<Barry::TaintNode>& r) {
    std::map<uint32_t, uint64_t> m;
    for (const auto& n : r) m[n.vertex] = n.time;
    return m;
}

// Chain 0->1@10 ->2@20 ->3@30, plus a decoy 1->4@5 that fires BEFORE vertex 1
// is reachable and therefore must not carry taint forward.
static void test_forward_taint_is_time_respecting() {
    GraphConfig cfg = small_cfg(0, 100, 5);
    Barry b(cfg);
    b.insert(Edge{0, 1, 10, 1.0f});
    b.insert(Edge{1, 2, 20, 1.0f});
    b.insert(Edge{2, 3, 30, 1.0f});
    b.insert(Edge{1, 4,  5, 1.0f});   // too early to be reached from 0

    auto m = as_map(b.forward_taint(0, 0, 100));
    EXPECT_EQ(m.size(), size_t{4});
    EXPECT_EQ(m[0], uint64_t{0});
    EXPECT_EQ(m[1], uint64_t{10});
    EXPECT_EQ(m[2], uint64_t{20});
    EXPECT_EQ(m[3], uint64_t{30});
    EXPECT_TRUE(m.count(4) == 0);     // decoy edge fired before taint arrived
}

// The horizon must clip propagation.
static void test_forward_taint_respects_horizon() {
    GraphConfig cfg = small_cfg(0, 100, 5);
    Barry b(cfg);
    b.insert(Edge{0, 1, 10, 1.0f});
    b.insert(Edge{1, 2, 20, 1.0f});
    b.insert(Edge{2, 3, 30, 1.0f});

    auto m = as_map(b.forward_taint(0, 0, 25));
    EXPECT_EQ(m.size(), size_t{3});   // 0, 1, 2 -- edge to 3 is past the horizon
    EXPECT_TRUE(m.count(3) == 0);
}

// Backward trace walks in-edges with non-increasing timestamps.
static void test_backward_trace_is_time_respecting() {
    GraphConfig cfg = small_cfg(0, 100, 5);
    Barry b(cfg);
    b.insert(Edge{0, 1, 10, 1.0f});
    b.insert(Edge{1, 2, 20, 1.0f});
    b.insert(Edge{2, 3, 30, 1.0f});
    b.insert(Edge{1, 4,  5, 1.0f});

    auto m = as_map(b.backward_trace(3, 100, 0));
    EXPECT_EQ(m.size(), size_t{4});
    EXPECT_EQ(m[3], uint64_t{100});
    EXPECT_EQ(m[2], uint64_t{30});
    EXPECT_EQ(m[1], uint64_t{20});
    EXPECT_EQ(m[0], uint64_t{10});

    // Tracing back from 4: the 0->1 edge at t=10 is AFTER 1->4 at t=5, so 0 is
    // not a causal ancestor of 4.
    auto m4 = as_map(b.backward_trace(4, 100, 0));
    EXPECT_EQ(m4.size(), size_t{2});
    EXPECT_EQ(m4[4], uint64_t{100});
    EXPECT_EQ(m4[1], uint64_t{5});
    EXPECT_TRUE(m4.count(0) == 0);
}

// Backward trace needs the reverse index; without it the query returns empty
// rather than silently answering something wrong.
static void test_backward_trace_requires_reverse_index() {
    GraphConfig cfg = small_cfg(0, 100, 5);
    cfg.build_reverse_index = false;
    Barry b(cfg);
    b.insert(Edge{0, 1, 10, 1.0f});
    EXPECT_TRUE(b.backward_trace(1, 100, 0).empty());
    EXPECT_TRUE(!b.forward_taint(0, 0, 100).empty());   // forward still works
}

// Duality on a random graph: if forward taint from s reaches u at time t_u,
// then tracing back from (u, t_u) must reach s.
static void test_forward_backward_duality() {
    const uint32_t V = 200;
    const uint64_t T = 5000;
    GraphConfig cfg = small_cfg(0, T, V);
    Barry b(cfg);

    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<uint32_t> vd(0, V - 1);
    std::uniform_int_distribution<uint64_t> td(1, T - 1);
    for (int i = 0; i < 4000; ++i) {
        b.insert(Edge{vd(rng), vd(rng), td(rng), 1.0f});
    }

    int checked = 0;
    for (uint32_t s = 0; s < 25; ++s) {
        auto fwd = b.forward_taint(s, 0, T);
        for (const auto& n : fwd) {
            if (n.vertex == s) continue;
            auto back = b.backward_trace(n.vertex, n.time, 0);
            bool found = false;
            for (const auto& m : back) if (m.vertex == s) { found = true; break; }
            EXPECT_TRUE(found);
            ++checked;
            if (checked > 3000) return;   // keep the test fast
        }
    }
    EXPECT_TRUE(checked > 0);
}

// The sparse forward_taint must agree with the dense earliest_arrival path.
static void test_sparse_matches_dense_earliest_arrival() {
    const uint32_t V = 300;
    const uint64_t T = 8000;
    GraphConfig cfg = small_cfg(0, T, V);
    Barry b(cfg);

    std::mt19937_64 rng(999);
    std::uniform_int_distribution<uint32_t> vd(0, V - 1);
    std::uniform_int_distribution<uint64_t> td(1, T - 1);
    for (int i = 0; i < 6000; ++i) {
        b.insert(Edge{vd(rng), vd(rng), td(rng), 1.0f});
    }

    for (uint32_t s = 0; s < 10; ++s) {
        auto sparse = as_map(b.forward_taint(s, 0, T));
        auto dense  = b.earliest_arrival(s, 0, T);
        size_t dense_reached = 0;
        for (uint32_t v = 0; v < V; ++v) {
            if (dense[v] == ULLONG_MAX) {
                EXPECT_TRUE(sparse.count(v) == 0);
            } else {
                ++dense_reached;
                EXPECT_TRUE(sparse.count(v) == 1);
                EXPECT_EQ(sparse[v], dense[v]);
            }
        }
        EXPECT_EQ(sparse.size(), dense_reached);
    }
}

int main() {
    std::fprintf(stderr, "test_taint\n");
    RUN(test_forward_taint_is_time_respecting);
    RUN(test_forward_taint_respects_horizon);
    RUN(test_backward_trace_is_time_respecting);
    RUN(test_backward_trace_requires_reverse_index);
    RUN(test_forward_backward_duality);
    RUN(test_sparse_matches_dense_earliest_arrival);
    std::fprintf(stderr, "test_taint OK\n");
    return 0;
}
