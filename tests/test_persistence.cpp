#include "test_common.hpp"
#include "barry/engine.hpp"
#include "barry/wal.hpp"

#include <cstdio>
#include <unistd.h>
#include <random>
#include <vector>

using namespace barry;

static GraphConfig cfg_small(uint64_t tmax, uint32_t V) {
    GraphConfig c;
    c.t_min = 0; c.t_max = tmax; c.num_vertices = V;
    c.build_reverse_index = true;
    c.arena_reserve_bytes = 1ULL << 26;
    c.vertex_arena_reserve_bytes = 1ULL << 26;
    return c;
}

static std::vector<Edge> random_edges(size_t n, uint32_t V, uint64_t tmax, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint32_t> vd(0, V - 1);
    std::uniform_int_distribution<uint64_t> td(1, tmax - 1);
    std::vector<Edge> es;
    es.reserve(n);
    for (size_t i = 0; i < n; ++i) es.push_back(Edge{vd(rng), vd(rng), td(rng), 1.0f});
    return es;
}

// WAL round-trip: log batches, replay into a fresh engine, and require query
// equivalence -- recovery IS re-ingestion because state is a function of the
// edge multiset.
static void test_wal_roundtrip() {
    const char* path = "/tmp/barry_test.wal";
    std::remove(path);
    const uint32_t V = 300; const uint64_t T = 10000;
    auto es = random_edges(5000, V, T, 42);

    GraphConfig c = cfg_small(T, V);
    Barry a(c);
    {
        Wal wal(path);
        EXPECT_TRUE(wal.ok());
        for (size_t off = 0; off < es.size(); off += 512) {
            size_t m = std::min<size_t>(512, es.size() - off);
            a.insert_batch(es.data() + off, m, 1);
            EXPECT_TRUE(wal.append(es.data() + off, m));
        }
        EXPECT_TRUE(wal.sync());
    }

    Barry b2(c);
    auto st = Wal::replay(path, b2, 1);
    EXPECT_EQ(st.edges, es.size());
    EXPECT_EQ(st.torn_bytes, size_t{0});

    for (uint32_t s = 0; s < 20; ++s) {
        auto ra = a.forward_taint(s, 0, T);
        auto rb = b2.forward_taint(s, 0, T);
        EXPECT_EQ(ra.size(), rb.size());
    }
    EXPECT_EQ(a.query_range(0, T).size(), b2.query_range(0, T).size());
    std::remove(path);
}

// A torn final frame (simulated crash mid-write) is discarded; every complete
// frame before it is recovered.
static void test_wal_torn_tail() {
    const char* path = "/tmp/barry_test_torn.wal";
    std::remove(path);
    const uint32_t V = 100; const uint64_t T = 1000;
    auto es = random_edges(1000, V, T, 7);
    {
        Wal wal(path);
        EXPECT_TRUE(wal.append(es.data(), 600));
        EXPECT_TRUE(wal.append(es.data() + 600, 400));
    }
    // truncate mid-payload of the second frame
    FILE* f = std::fopen(path, "rb+");
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    EXPECT_TRUE(::ftruncate(::fileno(f), sz - 123) == 0);
    std::fclose(f);

    GraphConfig c = cfg_small(T, V);
    Barry b(c);
    auto st = Wal::replay(path, b, 1);
    EXPECT_EQ(st.edges, size_t{600});
    EXPECT_EQ(st.frames, size_t{1});
    EXPECT_TRUE(st.torn_bytes > 0);
    std::remove(path);
}

// flush_before writes exactly the pre-cutoff edges as a loadable segment and
// evicts them; hot-side queries survive, cold segment re-ingests losslessly.
static void test_flush_before_roundtrip() {
    const char* seg = "/tmp/barry_test.seg";
    std::remove(seg);
    const uint32_t V = 200; const uint64_t T = 8000;
    auto es = random_edges(4000, V, T, 99);
    size_t expect_cold = 0;
    const uint64_t cut = T / 2;
    for (const Edge& e : es) if (e.timestamp < cut) ++expect_cold;

    GraphConfig c = cfg_small(T, V);
    Barry hot(c);
    hot.insert_batch(es.data(), es.size(), 1);
    size_t total_before = hot.query_range(0, T).size();

    auto fs = hot.flush_before(cut, seg);
    EXPECT_EQ(fs.edges_flushed, expect_cold);

    // hot side: everything at/after the cut still answers
    EXPECT_EQ(hot.query_range(cut, T).size(), total_before - expect_cold);

    // cold side: segment is a standard .bin; re-ingest and count
    FILE* f = std::fopen(seg, "rb");
    EXPECT_TRUE(f != nullptr);
    std::fseek(f, 0, SEEK_END);
    size_t n = static_cast<size_t>(std::ftell(f)) / sizeof(Edge);
    std::fseek(f, 0, SEEK_SET);
    std::vector<Edge> cold(n);
    EXPECT_EQ(std::fread(cold.data(), sizeof(Edge), n, f), n);
    std::fclose(f);
    EXPECT_EQ(n, expect_cold);
    for (const Edge& e : cold) EXPECT_TRUE(e.timestamp < cut);
    std::remove(seg);
}

int main() {
    std::fprintf(stderr, "test_persistence\n");
    RUN(test_wal_roundtrip);
    RUN(test_wal_torn_tail);
    RUN(test_flush_before_roundtrip);
    std::fprintf(stderr, "test_persistence OK\n");
    return 0;
}
