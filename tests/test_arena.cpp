#include "barry/arena.hpp"
#include "test_common.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace barry;

static void test_alloc_block() {
    Arena a(1ULL << 28);  // 256 MB
    BlockHeader* h = a.allocate_block(16);
    EXPECT_TRUE(h != nullptr);
    EXPECT_EQ(h->capacity, 16u);
    EXPECT_EQ(h->count.load(), 0u);
    EXPECT_TRUE(h->next == nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(h) % alignof(BlockHeader), 0u);
}

static void test_append() {
    Arena a(1ULL << 28);
    BlockHeader* h = a.allocate_block(4);
    Edge e{1, 2, 100, 1.0f};
    EXPECT_EQ(a.try_append(h, e), 0);
    EXPECT_EQ(a.try_append(h, e), 1);
    EXPECT_EQ(a.try_append(h, e), 2);
    EXPECT_EQ(a.try_append(h, e), 3);
    EXPECT_EQ(a.try_append(h, e), -1);  // full

    EXPECT_EQ(edges_of(h)[0].src, 1u);
    EXPECT_EQ(edges_of(h)[0].dst, 2u);
}

static void test_concurrent_alloc() {
    Arena a(1ULL << 30);  // 1 GB
    constexpr int T = 8;
    constexpr int N_PER = 10000;

    std::atomic<int> live{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < T; ++t) {
        ts.emplace_back([&] {
            for (int i = 0; i < N_PER; ++i) {
                BlockHeader* h = a.allocate_block(16);
                if (h) live.fetch_add(1);
            }
        });
    }
    for (auto& th : ts) th.join();

    EXPECT_EQ(live.load(), T * N_PER);
    EXPECT_EQ(a.blocks_allocated(), size_t(T * N_PER));
}

int main() {
    RUN(test_alloc_block);
    RUN(test_append);
    RUN(test_concurrent_alloc);
    std::fprintf(stderr, "test_arena OK\n");
    return 0;
}
