// E25: the dock ladder. The core engine (timeline + arena + lock-free append
// + window scans + retention) runs detached at full speed; every further
// capability is a module that docks onto it and pays a measurable price on
// the write path. This bench prices each rung with the same edges, the same
// threads, and a full-count check at every step:
//
//   core        no vertex index, no reverse index, no WAL, no properties.
//               Time-axis operations only. The detached-laptop mode.
//   +vindex     entity-keyed access docks (out-refs).
//   +reverse    backward causal tracing docks (in-refs too).
//   +props8/16  per-edge property payload docks (sidecar under T5).
//   +wal        durability docks under write_only semantics (bytes reach the
//               page cache, no fsync; the contract GraphOne's durable path
//               implements, and E18's write_only rung). The full fsync
//               spectrum is E18 cost's subject, not this ladder's.
//   full        everything at once: vindex + reverse + props16 + wal.
//
// Each rung reports ingest Meps, delta RSS, and where it applies a
// correctness probe: core answers a window count identical to full's, and
// the props rungs verify a sampled payload byte-for-byte.
//
// Usage:
//   bench_e25_dock --dataset <bin> [--threads T] [--batch N] --output csv

#include "common.hpp"
#include "barry/engine.hpp"
#include "barry/timeline.hpp"
#include "barry/arena.hpp"
#include "barry/wal.hpp"

#include <cstring>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

using namespace barry;
using namespace barry::bench;

namespace {

size_t rss_bytes() {
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long size = 0, resident = 0;
    if (std::fscanf(f, "%lu %lu", &size, &resident) != 2) resident = 0;
    std::fclose(f);
    return static_cast<size_t>(resident) * static_cast<size_t>(::sysconf(_SC_PAGESIZE));
}

void fill_prop(const Edge& e, uint8_t* out, uint32_t pb) {
    uint64_t a = e.timestamp * 0x9E3779B97F4A7C15ull ^ e.src;
    for (uint32_t o = 0; o < pb; o += 8) {
        std::memcpy(out + o, &a, std::min<uint32_t>(8, pb - o));
        a = a * 0xC2B2AE3D27D4EB4Full + e.dst;
    }
}

// Count of published edges in [lo, hi], straight off the timeline: the core
// rung has no vertex index, so the probe must be a time-axis operation.
uint64_t window_count(const Barry& b, uint64_t lo, uint64_t hi) {
    return b.query_range(lo, hi).size();
}

}  // namespace

int main(int argc, char** argv) {
    Args args(argc, argv);
    const std::string in_path = args.get("dataset");
    const std::string out_csv = args.get("output", "");
    const int threads = static_cast<int>(args.getl("threads", 8));
    const size_t batch = static_cast<size_t>(args.getl("batch", 65536));

    if (in_path.empty() || out_csv.empty()) {
        std::fprintf(stderr, "usage: %s --dataset <bin> [--threads T] "
                     "[--batch N] --output csv\n", args.prog().c_str());
        return 1;
    }

    EdgeFile ef;
    if (!ef.load(in_path)) return 1;
    const size_t n = ef.count;
    const GraphMeta meta = scan_meta(ef.edges, n);
    const std::string ds = basename_noext(in_path);
    const uint64_t span = (meta.t_max > meta.t_min) ? meta.t_max - meta.t_min : 1;
    const uint64_t probe_lo = meta.t_min + span / 3;
    const uint64_t probe_hi = meta.t_min + span / 2;

    CsvWriter w(out_csv,
        {"system","dataset","rung","threads","edges","ingest_sec","ingest_Meps",
         "rss_MB","slowdown_vs_core","probe_window_count","props_verified"});

    struct Rung {
        const char* name;
        bool vindex, reverse, wal;
        uint32_t props;
    };
    const Rung rungs[] = {
        {"core",     false, false, false, 0},
        {"+vindex",  true,  false, false, 0},
        {"+reverse", true,  true,  false, 0},
        {"+props8",  true,  false, false, 8},
        {"+props16", true,  false, false, 16},
        {"+wal",     true,  false, true,  0},
        {"full",     true,  true,  true,  16},
    };

    // Property payloads prepared outside the timer (a collector hands the
    // engine bytes; synthesizing them is not the engine's cost).
    std::vector<uint8_t> props16(n * 16);
    for (size_t i = 0; i < n; ++i) fill_prop(ef.edges[i], &props16[i * 16], 16);
    std::vector<uint8_t> props8(n * 8);
    for (size_t i = 0; i < n; ++i) std::memcpy(&props8[i * 8], &props16[i * 16], 8);

    double core_meps = 0;
    uint64_t core_probe = 0;

    for (const Rung& r : rungs) {
        GraphConfig cfg;
        cfg.t_min = meta.t_min;
        cfg.t_max = meta.t_max;
        cfg.num_vertices = r.vindex ? meta.v_max + 1 : 0;
        cfg.build_reverse_index = r.reverse;
        cfg.edge_property_bytes = r.props;
        cfg.arena_reserve_bytes = std::max<size_t>(1ULL << 30, n * sizeof(Edge) * 4);
        cfg.vertex_arena_reserve_bytes =
            std::max<size_t>(1ULL << 28, n * sizeof(EdgeRef) * (r.reverse ? 12 : 8));

        const std::string wal_path = "/tmp/e25_dock.wal";
        const size_t rss0 = rss_bytes();
        Barry b(cfg);
        // write_only contract (E18 kind 4): append reaches the page cache,
        // never fsync'd inside the timer; the Wal destructor's fsync falls
        // outside it. The fsync spectrum is E18's subject.
        std::unique_ptr<Wal> wal;
        if (r.wal) wal = std::make_unique<Wal>(wal_path, SIZE_MAX);

        const uint8_t* parr = r.props == 16 ? props16.data()
                            : r.props == 8 ? props8.data() : nullptr;

        const double t0 = now_sec();
        for (size_t off = 0; off < n; off += batch) {
            const size_t m = std::min(batch, n - off);
            if (wal) wal->append(ef.edges + off, m);
            if (parr) b.insert_batch(ef.edges + off,
                                     parr + off * r.props, m, threads);
            else      b.insert_batch(ef.edges + off, m, threads);
        }
        const double ing = now_sec() - t0;
        wal.reset();
        const size_t rss1 = rss_bytes();

        if (b.edges_dropped() != 0 || b.stats().num_edges != n) {
            std::fprintf(stderr, "[e25] COUNT MISMATCH at rung %s\n", r.name);
            return 1;
        }

        // Time-axis probe: identical answer on every rung, core included.
        const uint64_t probe = window_count(b, probe_lo, probe_hi);
        if (std::strcmp(r.name, "core") == 0) core_probe = probe;
        else if (probe != core_probe) {
            std::fprintf(stderr, "[e25] PROBE MISMATCH at rung %s: %llu vs %llu\n",
                         r.name, (unsigned long long)probe,
                         (unsigned long long)core_probe);
            return 1;
        }

        // Props probe: sample published edges, check payload byte for byte.
        int verified = -1;
        if (r.props) {
            verified = 0;
            size_t checked = 0, bad = 0;
            const Timeline& tl = b.timeline();
            for (uint64_t s = 0; s < tl.num_slots() && checked < 100000; s += 7) {
                for (BlockHeader* blk = tl.load(s); blk; blk = blk->next) {
                    const uint32_t cnt = std::min(
                        blk->count.load(std::memory_order_acquire), blk->capacity);
                    const Edge* arr = edges_of(blk);
                    for (uint32_t k = 0; k < cnt; ++k) {
                        if (arr[k].timestamp == 0) continue;
                        uint8_t want[16];
                        fill_prop(arr[k], want, r.props);
                        if (std::memcmp(b.props_of(&arr[k]), want, r.props) != 0) ++bad;
                        ++checked;
                    }
                }
            }
            if (bad) {
                std::fprintf(stderr, "[e25] PROPS MISMATCH at rung %s: %zu bad\n",
                             r.name, bad);
                return 1;
            }
            verified = static_cast<int>(checked);
        }

        const double meps = n / ing / 1e6;
        if (std::strcmp(r.name, "core") == 0) core_meps = meps;
        w.row(std::string("tgsr"), ds, std::string(r.name), threads,
              (unsigned long long)n, ing, meps, (rss1 - rss0) / 1e6,
              core_meps > 0 ? core_meps / meps : 0.0,
              (unsigned long long)probe, verified);
        std::fprintf(stderr,
            "[e25] %-9s %6.2f Meps  (%.2fx vs core)  rss=%7.0f MB  probe=%llu%s\n",
            r.name, meps, core_meps > 0 ? core_meps / meps : 0.0,
            (rss1 - rss0) / 1e6, (unsigned long long)probe,
            verified > 0 ? "  props verified" : "");

        if (r.wal) ::unlink(wal_path.c_str());
    }
    return 0;
}
