// The dock property module, held to the publication protocol.
//
// Each edge's 16-byte payload is a pure function of (src, dst, ts), so any
// published edge's sidecar bytes can be checked without coordination. The
// test ingests with 8 concurrent writers through both insert paths (run
// claiming on and off), then walks every timeline slot and chained block and
// verifies: every published edge has exactly the payload its identity
// dictates. A torn or lost property write cannot pass.

#include "barry/engine.hpp"
#include "barry/timeline.hpp"
#include "barry/arena.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace barry;

static int failures = 0;
#define EXPECT(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    ++failures; } } while (0)

static constexpr uint32_t kP = 16;

static void expected_prop(const Edge& e, uint8_t out[kP]) {
    uint64_t a = e.timestamp * 0x9E3779B97F4A7C15ull ^ e.src;
    uint64_t b = (uint64_t(e.src) << 32 | e.dst) * 0xC2B2AE3D27D4EB4Full ^ e.timestamp;
    std::memcpy(out, &a, 8);
    std::memcpy(out + 8, &b, 8);
}

int main() {
    const size_t n = 2'000'000;
    std::mt19937_64 rng(0x5109);
    std::vector<Edge> edges(n);
    std::vector<uint8_t> props(n * kP);
    const uint64_t t0 = 1'000'000, span = 1u << 24;
    for (size_t i = 0; i < n; ++i) {
        Edge& e = edges[i];
        // Mostly time-ordered with jitter, the shape run claiming exists for.
        e.timestamp = t0 + (span * i) / n + rng() % 64;
        e.src = static_cast<uint32_t>(rng() % 100'000);
        e.dst = static_cast<uint32_t>(rng() % 100'000);
        e.weight = 1.0f;
        expected_prop(e, &props[i * kP]);
    }

    for (int rc = 1; rc >= 0; --rc) {
        GraphConfig cfg;
        cfg.t_min = t0;
        cfg.t_max = t0 + span + 64;
        cfg.num_vertices = 100'000;
        cfg.run_claim = (rc != 0);
        cfg.edge_property_bytes = kP;
        cfg.arena_reserve_bytes = 1ULL << 30;
        Barry b(cfg);

        b.insert_batch(edges.data(), props.data(), n, 8);
        EXPECT(b.edges_dropped() == 0);
        EXPECT(b.stats().num_edges == n);

        // Walk every slot and chain; check every published edge's payload.
        size_t seen = 0, bad = 0;
        const Timeline& tl = b.timeline();
        for (uint64_t s = 0; s < tl.num_slots(); ++s) {
            for (BlockHeader* blk = tl.load(s); blk; blk = blk->next) {
                const uint32_t cnt = std::min(
                    blk->count.load(std::memory_order_acquire), blk->capacity);
                const Edge* arr = edges_of(blk);
                for (uint32_t k = 0; k < cnt; ++k) {
                    const Edge& e = arr[k];
                    if (e.timestamp == 0) continue;   // unpublished slot
                    uint8_t want[kP];
                    expected_prop(e, want);
                    if (std::memcmp(b.props_of(&e), want, kP) != 0) ++bad;
                    ++seen;
                }
            }
        }
        EXPECT(seen == n);
        EXPECT(bad == 0);
        std::fprintf(stderr, "[test_props] run_claim=%d: %zu edges, %zu bad payloads\n",
                     rc, seen, bad);
    }

    if (failures == 0) std::printf("test_props: all passed\n");
    return failures == 0 ? 0 : 1;
}
