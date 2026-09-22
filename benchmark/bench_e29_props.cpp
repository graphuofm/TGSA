// E29: property-predicate temporal queries (the T-gMark slot).
//
// TVA's property-intensive experiment (its Fig 7j) asks how fast property
// values come back under temporal queries. Our property module is the dock
// sidecar (E25 priced its write path); this bench prices its READ path with
// the same discipline:
//
//   coexist   the same plain window scan on two engines ingested from the
//             same edges, one with the sidecar attached (16 B/edge written),
//             one detached. The sidecar must cost nothing it was not asked
//             for: the scans never touch properties and must time the same.
//   predicate window scans that deliver only edges whose property matches,
//             at selectivities ~1/10/50/100%. The predicate byte lives in
//             the sidecar; the control runs the identical predicate on the
//             edge's own in-record weight field, so the delta prices
//             exactly one thing: the arithmetic-addressed parallel stream
//             against an in-record field.
//
// Every configuration checksums delivered (ts XOR dst XOR tag) against a
// reference computed directly from the input arrays; a mismatch aborts.
// The scan drives the timeline the same way query_range does (slot walk,
// published edges only), because materializing Edge copies would lose the
// addresses the sidecar is keyed on.
//
// Usage:
//   bench_e29_props --dataset <bin> [--windows N] [--window_frac F]
//                   [--threads T] --output csv

#include "common.hpp"
#include "barry/engine.hpp"
#include "barry/timeline.hpp"
#include "barry/arena.hpp"

#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace barry;
using namespace barry::bench;

namespace {

constexpr uint32_t kP = 16;

// prop[0] is the predicate byte: uniform in [0,100). The rest is payload.
inline uint8_t tag_of(const Edge& e) {
    uint64_t h = (e.timestamp * 0x9E3779B97F4A7C15ull) ^ e.src ^
                 (uint64_t(e.dst) << 32);
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDull; h ^= h >> 33;
    return static_cast<uint8_t>(h % 100);
}

void fill_prop(const Edge& e, uint8_t* out) {
    out[0] = tag_of(e);
    uint64_t a = e.timestamp ^ (uint64_t(e.src) << 20) ^ e.dst;
    std::memcpy(out + 1, &a, 8);
    std::memset(out + 9, 0x5A, kP - 9);
}

GraphConfig make_cfg(const GraphMeta& meta, size_t n, bool sidecar) {
    GraphConfig cfg;
    cfg.t_min = meta.t_min;
    cfg.t_max = meta.t_max;
    cfg.num_vertices = meta.v_max + 1;
    cfg.edge_property_bytes = sidecar ? kP : 0;
    cfg.arena_reserve_bytes = std::max<size_t>(1ULL << 30, n * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes =
        std::max<size_t>(1ULL << 28, n * sizeof(EdgeRef) * 8);
    return cfg;
}

// The timeline walk query_range performs, parameterized by a per-edge
// visitor. Returns edges visited.
template <class F>
size_t walk_window(const Barry& b, uint64_t lo, uint64_t hi, F&& f) {
    const Timeline& tl = b.timeline();
    const uint64_t s_lo = b.mapper().map(lo), s_hi = b.mapper().map(hi);
    size_t seen = 0;
    for (uint64_t s = s_lo; s <= s_hi && s < tl.num_slots(); ++s) {
        for (BlockHeader* blk = tl.load(s); blk; blk = blk->next) {
            const uint32_t cnt = std::min(
                blk->count.load(std::memory_order_acquire), blk->capacity);
            const Edge* arr = edges_of(blk);
            for (uint32_t k = 0; k < cnt; ++k) {
                const Edge& e = arr[k];
                if (e.timestamp == 0 || e.timestamp < lo || e.timestamp > hi)
                    continue;
                ++seen;
                f(e);
            }
        }
    }
    return seen;
}

}  // namespace

int main(int argc, char** argv) {
    Args args(argc, argv);
    const std::string in_path = args.get("dataset");
    const std::string out_csv = args.get("output", "");
    const int windows = static_cast<int>(args.getl("windows", 64));
    const double window_frac = args.getd("window_frac", 0.01);
    const int threads = static_cast<int>(args.getl("threads", 8));

    if (in_path.empty() || out_csv.empty()) {
        std::fprintf(stderr, "usage: %s --dataset <bin> [--windows N] "
                     "[--window_frac F] [--threads T] --output csv\n",
                     args.prog().c_str());
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
        {"system","dataset","mode","selectivity_pct","windows","scanned",
         "delivered","total_ms","ns_per_scanned","checksum","checksum_ok"});

    std::vector<uint8_t> props(n * kP);
    for (size_t i = 0; i < n; ++i) fill_prop(ef.edges[i], &props[i * kP]);

    std::mt19937_64 rng(0xE29ull);
    std::vector<uint64_t> wlo(windows);
    for (auto& t : wlo)
        t = meta.t_min + rng() % (span > window ? span - window : 1);

    // Reference answers straight from the input arrays.
    struct Ref { size_t scanned = 0, delivered = 0; uint64_t sum = 0; };
    auto reference = [&](int sel) {
        Ref r;
        for (int q = 0; q < windows; ++q) {
            const uint64_t lo = wlo[q], hi = wlo[q] + window;
            for (size_t i = 0; i < n; ++i) {
                const Edge& e = ef.edges[i];
                const uint64_t ts = e.timestamp ? e.timestamp : 1;
                if (ts < lo || ts > hi) continue;
                ++r.scanned;
                if (tag_of(e) < sel) {
                    ++r.delivered;
                    r.sum += ts ^ e.dst ^ tag_of(e);
                }
            }
        }
        return r;
    };

    // Two engines, same edges: sidecar attached and detached.
    Barry bp(make_cfg(meta, n, true));
    bp.insert_batch(ef.edges, props.data(), n, threads);
    Barry b0(make_cfg(meta, n, false));
    b0.insert_batch(ef.edges, n, threads);
    if (bp.edges_dropped() || b0.edges_dropped()) return 1;

    // ---- coexist: plain scans must not feel the sidecar -----------------
    for (int pass = 0; pass < 2; ++pass) {   // warm, then measure
        for (auto& [name, eng] : {std::pair<const char*, Barry*>{"detached", &b0},
                                  {"attached", &bp}}) {
            uint64_t sum = 0; size_t scanned = 0;
            const double t0 = now_sec();
            for (int q = 0; q < windows; ++q)
                scanned += walk_window(*eng, wlo[q], wlo[q] + window,
                    [&](const Edge& e) { sum += e.timestamp ^ e.dst; });
            const double ms = (now_sec() - t0) * 1e3;
            if (pass == 1) {
                w.row(std::string("tgsr"), ds,
                      std::string("coexist_") + name, 100, windows,
                      (unsigned long long)scanned, (unsigned long long)scanned,
                      ms, scanned ? ms * 1e6 / scanned : 0.0,
                      (unsigned long long)sum, 1);
                std::fprintf(stderr, "[e29] coexist %-8s %8.1f ms  (%zu scanned)\n",
                             name, ms, scanned);
            }
        }
    }

    // ---- predicate: sidecar byte vs in-record field ---------------------
    for (int sel : {1, 10, 50, 100}) {
        const Ref ref = reference(sel);

        // sidecar predicate
        uint64_t sum = 0; size_t delivered = 0, scanned = 0;
        double t0 = now_sec();
        for (int q = 0; q < windows; ++q)
            scanned += walk_window(bp, wlo[q], wlo[q] + window,
                [&](const Edge& e) {
                    const uint8_t* p = bp.props_of(&e);
                    if (p[0] < sel) {
                        ++delivered;
                        sum += (e.timestamp ? e.timestamp : 1) ^ e.dst ^ p[0];
                    }
                });
        double ms = (now_sec() - t0) * 1e3;
        bool ok = (delivered == ref.delivered && sum == ref.sum &&
                   scanned == ref.scanned);
        w.row(std::string("tgsr"), ds, std::string("pred_sidecar"), sel,
              windows, (unsigned long long)scanned,
              (unsigned long long)delivered, ms,
              scanned ? ms * 1e6 / scanned : 0.0,
              (unsigned long long)sum, (int)ok);
        std::fprintf(stderr,
            "[e29] pred sidecar  sel=%3d%%  %8.1f ms  delivered=%zu  %s\n",
            sel, ms, delivered, ok ? "ok" : "CHECKSUM MISMATCH");
        if (!ok) return 1;

        // control: identical predicate recomputed from in-record fields
        // (tag_of reads only the Edge itself), so the only difference from
        // the sidecar run is WHERE the byte comes from.
        sum = 0; delivered = 0;
        t0 = now_sec();
        size_t scanned2 = 0;
        for (int q = 0; q < windows; ++q)
            scanned2 += walk_window(bp, wlo[q], wlo[q] + window,
                [&](const Edge& e) {
                    const uint8_t tag = tag_of(e);
                    if (tag < sel) {
                        ++delivered;
                        sum += (e.timestamp ? e.timestamp : 1) ^ e.dst ^ tag;
                    }
                });
        ms = (now_sec() - t0) * 1e3;
        ok = (delivered == ref.delivered && sum == ref.sum &&
              scanned2 == ref.scanned);
        w.row(std::string("tgsr"), ds, std::string("pred_inrecord"), sel,
              windows, (unsigned long long)scanned2,
              (unsigned long long)delivered, ms,
              scanned2 ? ms * 1e6 / scanned2 : 0.0,
              (unsigned long long)sum, (int)ok);
        std::fprintf(stderr,
            "[e29] pred inrecord sel=%3d%%  %8.1f ms  delivered=%zu  %s\n",
            sel, ms, delivered, ok ? "ok" : "CHECKSUM MISMATCH");
        if (!ok) return 1;
    }
    return 0;
}
