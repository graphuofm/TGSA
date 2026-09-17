// E23: the ablation sweeps behind TGSR's design decisions, one mode per knob.
// TVA's Fig. 11 is the template: every mechanism gets a figure that prices it
// off as well as on, against the same reseeded workload.
//
//   --mode tau        Granularity target sweep. For each target occupancy tau,
//                     the shift is selected from the REAL event count (not the
//                     constructor's 1M default), the realized mean occupancy is
//                     measured by walking the timeline, and the Theorem bound
//                     mu in [tau/2, 4*tau) is checked as a column, not assumed.
//                     Also reports ingest rate and windowed-query cost per tau.
//
//   --mode threshold  Sorted-bucket binary-search threshold sweep (the knob the
//                     cit-Patents regression exposed). bs_min = 1 means always
//                     binary-search; a huge value means always scan.
//
//   --mode simd       AVX2 boundary-scan on/off across window widths from an
//                     eighth of a slot to 64 slots. The gather-based path is
//                     suspected of losing outright; this sweep decides its
//                     default across corpora and widths rather than from one
//                     micro-benchmark.
//
// Usage:
//   bench_e23_ablation --dataset <bin> --mode <tau|threshold|simd>
//                      [--samples N] [--threads T] --output csv

#include "common.hpp"
#include "barry/engine.hpp"
#include "barry/vertex_index.hpp"

#include <random>
#include <string>
#include <vector>

using namespace barry;
using namespace barry::bench;

namespace {

GraphConfig base_cfg(const GraphMeta& meta, size_t n) {
    GraphConfig cfg;
    cfg.t_min = meta.t_min;
    cfg.t_max = meta.t_max;
    cfg.num_vertices = meta.v_max + 1;
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, n * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, n * sizeof(EdgeRef) * 8);
    return cfg;
}

// Occupied windows, counted rather than estimated: the Theorem-3 check needs
// the realized mu, and walking the timeline head array is cheap (pointer per
// slot, and untouched pages fault in zeroed anyway).
size_t occupied_windows(const Barry& b) {
    size_t occ = 0;
    for (uint64_t i = 0; i < b.timeline().num_slots(); ++i)
        if (b.timeline().load(i) != nullptr) ++occ;
    return occ;
}

}  // namespace

int main(int argc, char** argv) {
    Args args(argc, argv);
    const std::string in_path = args.get("dataset");
    const std::string mode    = args.get("mode", "tau");
    const std::string out_csv = args.get("output", "");
    const int samples = static_cast<int>(args.getl("samples", 200000));
    const int threads = static_cast<int>(args.getl("threads", 8));

    if (in_path.empty() || out_csv.empty()) {
        std::fprintf(stderr, "usage: %s --dataset <bin> --mode <tau|threshold|simd> "
                     "[--samples N] [--threads T] --output csv\n", args.prog().c_str());
        return 1;
    }

    EdgeFile ef;
    if (!ef.load(in_path)) return 1;
    const size_t n = ef.count;
    const GraphMeta meta = scan_meta(ef.edges, n);
    const std::string ds = basename_noext(in_path);
    const uint64_t span = (meta.t_max > meta.t_min) ? meta.t_max - meta.t_min : 1;

    // ------------------------------------------------------------------ tau
    if (mode == "tau") {
        CsvWriter w(out_csv,
            {"system","dataset","tau","shift","windows_occupied","mu",
             "bound_lo","bound_hi","bound_holds","ingest_Meps",
             "window_query_ms","delivered"});
        for (uint32_t tau : {16u, 32u, 64u, 128u, 256u, 512u}) {
            GraphConfig cfg = base_cfg(meta, n);
            cfg.target_avg_edges_per_window = tau;
            cfg.shift = TimeMapper::auto_select_shift_from_stats(
                n, meta.t_min, meta.t_max, tau);
            Barry b(cfg);
            double t0 = now_sec();
            b.insert_batch(ef.edges, n, threads);
            const double ing = now_sec() - t0;
            b.finalize();

            const size_t occ = occupied_windows(b);
            const double mu = occ ? static_cast<double>(n) / occ : 0.0;
            // Theorem 3 bounds the mean over ALL windows in the selected span;
            // empty windows cost nothing (Theorem 2) but pull the realized
            // occupied-window mean above it, so the honest check is against
            // the theorem's own denominator.
            const uint64_t W_all = (span >> cfg.shift) + 1;
            const double mu_thm = static_cast<double>(n) / W_all;
            const bool holds = (mu_thm >= tau / 2.0) && (mu_thm < 4.0 * tau);

            std::mt19937_64 rng(0xE23ull);
            const VertexIndex* vi = b.vertex_index();
            std::vector<TemporalRef> sc;
            const uint64_t wwin = std::max<uint64_t>(1, span / 100);
            double q0 = now_sec(); size_t delivered = 0;
            for (int i = 0; i < samples; ++i) {
                const Edge& e = ef.edges[rng() % n];
                sc.clear();
                vi->neighbors_temporal_into(e.src, e.timestamp, e.timestamp + wwin, sc);
                delivered += sc.size();
            }
            const double qms = (now_sec() - q0) * 1e3;

            w.row(std::string("tgsr"), ds, tau, cfg.shift,
                  (unsigned long long)occ, mu_thm, tau / 2.0, 4.0 * tau,
                  (int)holds, n / ing / 1e6, qms, (unsigned long long)delivered);
            std::fprintf(stderr,
                "[e23-tau] tau=%-4u shift=%-2u mu=%8.1f in[%5.1f,%6.1f) holds=%d  "
                "ingest=%6.2f Meps  query=%8.1f ms\n",
                tau, cfg.shift, mu_thm, tau / 2.0, 4.0 * tau, (int)holds,
                n / ing / 1e6, qms);
        }
        return 0;
    }

    // ------------------------------------------------------------- runclaim
    // The run-claiming A/B: same edges, same thread counts, the switch the
    // only difference. Both sides must land every edge (edges_dropped == 0
    // and total counts equal) before the rates mean anything.
    if (mode == "runclaim") {
        CsvWriter w(out_csv,
            {"system","dataset","run_claim","threads","edges",
             "ingest_sec","ingest_Meps"});
        for (int th : {1, 2, 4, 8}) {
            for (int rc : {1, 0}) {
                GraphConfig cfg = base_cfg(meta, n);
                cfg.run_claim = (rc != 0);
                Barry b(cfg);
                double t0 = now_sec();
                b.insert_batch(ef.edges, n, th);
                const double ing = now_sec() - t0;
                const Barry::Stats st = b.stats();
                if (st.edges_dropped != 0 || st.num_edges != n) {
                    std::fprintf(stderr, "[e23-runclaim] COUNT MISMATCH rc=%d "
                                 "th=%d inserted=%zu dropped=%zu\n", rc, th,
                                 st.num_edges, st.edges_dropped);
                    return 1;
                }
                w.row(std::string("tgsr"), ds, rc, th,
                      (unsigned long long)n, ing, n / ing / 1e6);
                std::fprintf(stderr,
                    "[e23-runclaim] run_claim=%d threads=%d  %6.2f Meps\n",
                    rc, th, n / ing / 1e6);
            }
        }
        return 0;
    }

    // ------------------------------------------------------------ threshold
    if (mode == "threshold") {
        CsvWriter w(out_csv,
            {"system","dataset","bs_min","expansions","total_ms","delivered"});
        for (uint32_t bs : {1u, 8u, 16u, 32u, 64u, 128u, 256u, 1000000000u}) {
            GraphConfig cfg = base_cfg(meta, n);
            cfg.binary_search_min = bs;
            Barry b(cfg);
            b.insert_batch(ef.edges, n, threads);
            b.finalize();
            std::mt19937_64 rng(0xE23ull);
            const VertexIndex* vi = b.vertex_index();
            std::vector<TemporalRef> sc;
            const uint64_t wwin = std::max<uint64_t>(1, span / 100);
            double t0 = now_sec(); size_t delivered = 0;
            for (int i = 0; i < samples; ++i) {
                const Edge& e = ef.edges[rng() % n];
                sc.clear();
                vi->neighbors_temporal_into(e.src, e.timestamp, e.timestamp + wwin, sc);
                delivered += sc.size();
            }
            const double ms = (now_sec() - t0) * 1e3;
            w.row(std::string("tgsr"), ds, bs, samples, ms,
                  (unsigned long long)delivered);
            std::fprintf(stderr, "[e23-thresh] bs_min=%-10u %9.1f ms  delivered=%llu\n",
                         bs, ms, (unsigned long long)delivered);
        }
        return 0;
    }

    // ----------------------------------------------------------------- simd
    if (mode == "simd") {
        CsvWriter w(out_csv,
            {"system","dataset","simd","window_slots","queries","total_ms",
             "sum_result"});
        // Same engine data both ways; the switch is read at construction, so
        // build one engine per setting on identical input.
        for (int simd = 1; simd >= 0; --simd) {
            GraphConfig cfg = base_cfg(meta, n);
            cfg.simd_scan = (simd != 0);
            Barry b(cfg);
            b.insert_batch(ef.edges, n, threads);
            b.finalize();
            const uint32_t shift = b.mapper().shift();
            for (double slots : {0.125, 0.5, 2.0, 8.0, 64.0}) {
                const uint64_t wwin = std::max<uint64_t>(
                    1, static_cast<uint64_t>((1ull << shift) * slots));
                std::mt19937_64 rng(0xE23ull);
                double t0 = now_sec(); size_t sum = 0;
                const int q = 2000;
                for (int i = 0; i < q; ++i) {
                    const uint64_t lo = ef.edges[rng() % n].timestamp;
                    sum += b.query_range(lo, lo + wwin).size();
                }
                const double ms = (now_sec() - t0) * 1e3;
                w.row(std::string("tgsr"), ds, simd, slots, q, ms,
                      (unsigned long long)sum);
                std::fprintf(stderr,
                    "[e23-simd] simd=%d width=%6.3f slots  %8.1f ms  sum|R|=%llu\n",
                    simd, slots, ms, (unsigned long long)sum);
            }
        }
        return 0;
    }

    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 1;
}
