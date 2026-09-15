// E19: an in-category baseline. LiveGraph (VLDB'20) is a transactional graph
// store built for exactly the workload shape we target -- continuous updates
// concurrent with analytics -- and it is the comparison a reviewer asks for
// after seeing a columnar engine and a static CSR. Its layout is the other
// choice: adjacency grouped by source vertex, time carried as edge payload.
//
// Two measurements, on the same corpus, the same sampled vertices, the same
// windows, in the same process:
//
//   ingest    Edges per second through each store's normal write path. Both
//             stores are given the vertex count up front, and vertex creation
//             is timed separately because it is setup, not stream cost.
//
//   window    Deliver every edge of a vertex whose timestamp falls in a query
//             window. An entity-keyed store must walk the whole neighborhood
//             and test each edge; a time-keyed one addresses the window first.
//             Reported per delivered edge, since neighborhoods differ by four
//             orders of magnitude across corpora. A checksum over delivered
//             (dst, timestamp) pairs is compared between the two stores, so a
//             faster number that answered a different question cannot pass.
//
// Usage:
//   bench_e19_livegraph --dataset <bin> --limit N --batch N --samples N
//                       --window_frac F --threads T --output csv

#include "common.hpp"
#include "barry/engine.hpp"
#include "barry/vertex_index.hpp"

#include "livegraph.hpp"

#include <algorithm>
#include <random>
#include <string>
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

// Timestamps travel as the edge's property, which is the only place an
// entity-keyed property-graph store can put them.
inline std::string_view ts_view(const uint64_t& t) {
    return std::string_view(reinterpret_cast<const char*>(&t), sizeof(uint64_t));
}

}  // namespace

int main(int argc, char** argv) {
    Args args(argc, argv);
    const std::string in_path = args.get("dataset");
    const std::string out_csv = args.get("output", "");
    const size_t limit      = static_cast<size_t>(args.getl("limit", 0));
    const size_t batch      = static_cast<size_t>(args.getl("batch", 65536));
    const int    samples    = static_cast<int>(args.getl("samples", 2000));
    const double window_frac= args.getd("window_frac", 0.01);
    const int    threads    = static_cast<int>(args.getl("threads", 0));
    const int    trial      = static_cast<int>(args.getl("trial", 0));
    // LiveGraph's transactional path builds per-transaction conflict and block
    // caches, so on the largest corpora it is far slower than its bulk loader.
    // Both are wanted, but the sweep should not stall on one of them.
    const size_t lg_txn_max = static_cast<size_t>(args.getl("lg_txn_max_edges", 25000000));

    if (in_path.empty() || out_csv.empty()) {
        std::fprintf(stderr,
            "usage: %s --dataset <bin> [--limit N] [--batch N] [--samples N] "
            "[--window_frac F] [--threads T] --output csv\n", args.prog().c_str());
        return 1;
    }

    EdgeFile ef;
    if (!ef.load(in_path)) return 1;
    const size_t n = std::min(limit ? limit : ef.count, ef.count);
    const GraphMeta meta = scan_meta(ef.edges, n);
    const std::string ds = basename_noext(in_path);
    const uint32_t V = meta.v_max + 1;
    const uint64_t span = (meta.t_max > meta.t_min) ? meta.t_max - meta.t_min : 1;
    const uint64_t window = std::max<uint64_t>(1, static_cast<uint64_t>(span * window_frac));

    CsvWriter w(out_csv,
        {"system","dataset","trial","edges","vertices","threads","mode",
         "setup_sec","ingest_sec","ingest_Meps","rss_after_MB",
         "window_samples","delivered_edges","window_total_sec",
         "ns_per_delivered_edge","median_query_us","p95_query_us","checksum"});

    // --------------------------------------------------------------- samples
    // Fixed across both stores so the comparison is on identical questions.
    //
    // Sampling is by edge, not by vertex: a uniformly drawn vertex of a sparse
    // corpus has degree near zero and its window is empty, so a vertex-uniform
    // workload would time an empty walk in both stores and measure nothing. An
    // edge-uniform draw puts the query on an active entity at a time when that
    // entity was active, which is both the non-degenerate case and the one a
    // real query workload occupies.
    std::mt19937_64 rng(0xE19ull ^ static_cast<uint64_t>(trial));
    std::vector<uint32_t> qv(samples);
    std::vector<uint64_t> qlo(samples);
    for (int i = 0; i < samples; ++i) {
        const size_t e = std::uniform_int_distribution<size_t>(0, n - 1)(rng);
        qv[i]  = ef.edges[e].src;
        qlo[i] = ef.edges[e].timestamp;
    }

    // Hand the same query list to an out-of-process baseline (GraphOne reads
    // it via GNOMON_QUERY_FILE) so all three systems answer one list and their
    // checksums are directly comparable.
    const std::string dump = args.get("dump_queries");
    if (!dump.empty()) {
        FILE* qf = std::fopen(dump.c_str(), "wb");
        if (!qf) { std::perror(dump.c_str()); return 1; }
        for (int i = 0; i < samples; ++i) {
            const uint64_t rec[3] = {qv[i], qlo[i], qlo[i] + window};
            if (std::fwrite(rec, sizeof(rec), 1, qf) != 1) { std::perror("fwrite"); return 1; }
        }
        std::fclose(qf);
        std::fprintf(stderr, "[e19] wrote %d queries to %s\n", samples, dump.c_str());
    }

    const size_t rss0 = rss_bytes();

    // ======================================================= LiveGraph
    //
    // Two write paths, because picking one invites the obvious objection either
    // way. `batch_loader` is LiveGraph's bulk path: no version chain, no
    // conflict tracking, the fastest it goes. `transaction` is its normal
    // concurrent path, which is what a store absorbing a live stream alongside
    // readers actually runs. Gnomon's single path is compared against both.
    size_t lg_delivered = 0;
    uint64_t lg_sum = 0;
    for (int path = 0; path < 2; ++path) {
        const bool bulk = (path == 0);
        const char* path_name = bulk ? "batch_loader" : "transaction";
        if (!bulk && n > lg_txn_max) {
            std::fprintf(stderr,
                "[e19] livegraph transaction path skipped: %llu edges above "
                "--lg_txn_max_edges %llu\n",
                (unsigned long long)n, (unsigned long long)lg_txn_max);
            continue;
        }

        double setup = 0, ingest = 0, win = 0;
        size_t delivered = 0, rss_after = 0;
        uint64_t sum = 0;
        std::vector<double> lat;
        {
            lg::Graph g("", "", 1ul << 40, static_cast<lg::vertex_t>(V) + 16);

            double t0 = now_sec();
            {   // Vertex creation is setup in both stores; time it, do not blend it in.
                auto txn = g.begin_batch_loader();
                for (uint32_t v = 0; v < V; ++v) txn.new_vertex();
                txn.commit();
            }
            setup = now_sec() - t0;

            // One transaction per arriving batch, which is how a collector drives
            // it. force_insert appends rather than checking for an existing
            // (src,label,dst), giving the multi-edge semantics an event stream
            // needs; without it a repeated pair would overwrite its own history.
            t0 = now_sec();
            for (size_t off = 0; off < n; off += batch) {
                const size_t m = std::min(batch, n - off);
                auto txn = bulk ? g.begin_batch_loader() : g.begin_transaction();
                for (size_t i = off; i < off + m; ++i) {
                    const Edge& e = ef.edges[i];
                    txn.put_edge(e.src, 0, e.dst, ts_view(e.timestamp), /*force_insert=*/true);
                }
                txn.commit();
            }
            ingest = now_sec() - t0;
            rss_after = rss_bytes();

            // Windowed delivery: walk the neighborhood, test every edge.
            lat.reserve(samples);
            auto txn = g.begin_read_only_transaction();
            t0 = now_sec();
            for (int i = 0; i < samples; ++i) {
                const uint64_t lo = qlo[i], hi = qlo[i] + window;
                const double q0 = now_sec();
                auto it = txn.get_edges(qv[i], 0);
                while (it.valid()) {
                    uint64_t ts = 0;
                    auto d = it.edge_data();
                    if (d.size() >= sizeof(uint64_t)) std::memcpy(&ts, d.data(), sizeof(uint64_t));
                    if (ts >= lo && ts <= hi) {
                        sum += ts ^ static_cast<uint64_t>(it.dst_id());
                        ++delivered;
                    }
                    it.next();
                }
                lat.push_back((now_sec() - q0) * 1e6);
            }
            win = now_sec() - t0;
            txn.abort();
        }

        if (bulk) { lg_delivered = delivered; lg_sum = sum; }

        w.row(std::string("livegraph"), ds, trial, (unsigned long long)n,
              (unsigned long long)V, 1, std::string(path_name),
              setup, ingest, n / ingest / 1e6,
              (rss_after - rss0) / 1e6, samples, (unsigned long long)delivered,
              win, delivered ? win * 1e9 / delivered : 0.0,
              median(lat), percentile(lat, 0.95), (unsigned long long)sum);

        std::fprintf(stderr,
            "[e19] livegraph %-13s setup=%6.2fs ingest=%7.2fs (%5.2f Meps) rss=%7.0f MB  "
            "window: %llu edges in %6.3fs = %8.1f ns/edge  median=%8.1f us\n",
            path_name, setup, ingest, n / ingest / 1e6, (rss_after - rss0) / 1e6,
            (unsigned long long)delivered, win,
            delivered ? win * 1e9 / delivered : 0.0, median(lat));
    }

    // ========================================================== Gnomon
    // Same corpus, same samples, same windows, measured the same way.
    for (int t : {1, threads ? threads : 8}) {
        GraphConfig cfg;
        cfg.t_min = meta.t_min;
        cfg.t_max = meta.t_max;
        cfg.num_vertices = V;
        cfg.build_reverse_index = false;
        cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, n * sizeof(Edge) * 4);
        cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, n * sizeof(EdgeRef) * 8);

        const size_t r0 = rss_bytes();
        Barry b(cfg);

        double t0 = now_sec();
        for (size_t off = 0; off < n; off += batch)
            b.insert_batch(ef.edges + off, std::min(batch, n - off), t);
        const double ing = now_sec() - t0;
        const size_t rss1 = rss_bytes();

        const VertexIndex* vi = b.vertex_index();
        std::vector<TemporalRef> scratch;
        auto measure_window = [&](size_t& delivered, uint64_t& sum,
                                  std::vector<double>& lat) {
            delivered = 0; sum = 0;
            lat.clear(); lat.reserve(samples);
            const double s0 = now_sec();
            for (int i = 0; i < samples; ++i) {
                const uint64_t lo = qlo[i], hi = qlo[i] + window;
                const double q0 = now_sec();
                scratch.clear();
                vi->neighbors_temporal_into(qv[i], lo, hi, scratch);
                for (const TemporalRef& r : scratch)
                    sum += r.ts ^ static_cast<uint64_t>(r.peer);
                delivered += scratch.size();
                lat.push_back((now_sec() - q0) * 1e6);
            }
            return now_sec() - s0;
        };

        // Uncompacted is the state that matches LiveGraph's: every edge ever
        // ingested is queryable, nothing was reorganised afterwards. Our
        // contract says answers are already correct here and only expansion is
        // slower, so this is the honest head-to-head; the compacted row below
        // is what a deployment gets once compaction has been scheduled.
        for (int phase = 0; phase < 2; ++phase) {
            double setup = 0;
            if (phase == 1) {
                const double c0 = now_sec();
                b.finalize();
                setup = now_sec() - c0;
            }
            size_t delivered = 0; uint64_t sum = 0;
            std::vector<double> lat;
            const double win = measure_window(delivered, sum, lat);

            w.row(std::string("gnomon"), ds, trial, (unsigned long long)n,
                  (unsigned long long)V, t,
                  std::string(phase ? "compacted" : "uncompacted"),
                  setup, ing, n / ing / 1e6, (rss1 - r0) / 1e6,
                  samples, (unsigned long long)delivered, win,
                  delivered ? win * 1e9 / delivered : 0.0,
                  median(lat), percentile(lat, 0.95), (unsigned long long)sum);

            std::fprintf(stderr,
                "[e19] gnomon t=%-2d %-11s compact=%6.2fs ingest=%7.2fs (%5.2f Meps) "
                "rss=%7.0f MB  window: %llu edges in %6.3fs = %8.1f ns/edge  "
                "median=%8.1f us  %s\n",
                t, phase ? "compacted" : "uncompacted", setup, ing,
                n / ing / 1e6, (rss1 - r0) / 1e6,
                (unsigned long long)delivered, win,
                delivered ? win * 1e9 / delivered : 0.0, median(lat),
                (delivered == lg_delivered && sum == lg_sum)
                    ? "checksum MATCHES livegraph" : "CHECKSUM DIFFERS");
        }
    }
    return 0;
}
