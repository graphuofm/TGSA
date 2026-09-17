// The temporal address space, exercised by a client that is not a graph.
//
// A per-window event counter is about the smallest useful thing one can hang
// on Tmap, and it inherits every clause of the contract without writing any
// of the machinery: O(1) placement (the counter is the slot), concurrent
// multi-writer safety (the slot is an atomic), retention as a prefix release,
// and recovery as replay (counts are a pure function of the appended
// multiset). If those hold here, the contract is engine-independent, which is
// the claim Section 3 of the paper makes.

#include "barry/tmap.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

using barry::Tmap;

static int failures = 0;
#define EXPECT(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    ++failures; } } while (0)

using Counter = std::atomic<uint64_t>;

// A reproducible event stream: timestamps over [t0, t0+span).
static std::vector<uint64_t> make_stream(size_t n, uint64_t t0, uint64_t span,
                                         uint64_t seed) {
    std::vector<uint64_t> ts(n);
    std::mt19937_64 rng(seed);
    for (auto& t : ts) t = t0 + rng() % span;
    return ts;
}

static void ingest(Tmap<Counter>& m, const std::vector<uint64_t>& ts,
                   int threads) {
    std::vector<std::thread> th;
    const size_t per = ts.size() / threads + 1;
    for (int w = 0; w < threads; ++w) {
        th.emplace_back([&, w] {
            const size_t lo = w * per, hi = std::min(ts.size(), lo + per);
            for (size_t i = lo; i < hi; ++i)
                m.slot_for(ts[i])->fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& t : th) t.join();
}

static uint64_t total_in(const Tmap<Counter>& m, uint64_t lo, uint64_t hi) {
    uint64_t sum = 0;
    m.for_range(lo, hi, [&](uint64_t, const Counter& c) {
        sum += c.load(std::memory_order_relaxed);
    });
    return sum;
}

int main() {
    const uint64_t t0 = 1'000'000, span = 1u << 26;
    const uint32_t shift = 12;
    const size_t slots = (span >> shift) + 16;
    const size_t n = 4'000'000;
    auto ts = make_stream(n, t0, span, 0x7714);

    // ---- clause 1 + concurrent multi-writer: every event lands, exactly once
    Tmap<Counter> m(t0, shift, slots);
    ingest(m, ts, 8);
    EXPECT(total_in(m, t0, t0 + span) == n);

    // ---- windowed counts agree with a scalar recount of the raw stream
    const uint64_t wlo = t0 + span / 3, whi = t0 + span / 2;
    // align to window boundaries so both sides count the same slots
    const uint64_t alo = t0 + (((wlo - t0) >> shift) << shift);
    const uint64_t ahi = t0 + ((((whi - t0) >> shift) + 1) << shift) - 1;
    uint64_t expect = 0;
    for (uint64_t t : ts) if (t >= alo && t <= ahi) ++expect;
    EXPECT(total_in(m, alo, ahi) == expect);

    // ---- clause 4: replay reconstructs the region exactly
    Tmap<Counter> r(t0, shift, slots);
    ingest(r, ts, 3);   // different thread count, different interleaving
    bool identical = true;
    for (size_t i = 0; i < slots; ++i) {
        const Counter* a = m.slot_for(t0 + (i << shift));
        const Counter* b = r.slot_for(t0 + (i << shift));
        if (a && b && a->load() != b->load()) { identical = false; break; }
    }
    EXPECT(identical);

    // ---- clause 3: retention releases a prefix and leaves the rest intact
    const uint64_t cut = t0 + span / 2;
    const uint64_t tail_before = total_in(m, cut, t0 + span);
    m.evict_before(cut);
    EXPECT(total_in(m, m.min_valid_time(), t0 + span) == tail_before);
    // the released prefix reads as empty (zero pages), never as garbage
    EXPECT(total_in(m, t0, m.min_valid_time() - 1) == 0);

    if (failures == 0) std::printf("test_tmap: all passed\n");
    return failures == 0 ? 0 : 1;
}
