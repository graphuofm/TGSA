// E21: streaming behaviour. Six modes, each answering a question that a
// bulk-load throughput number cannot.
//
// A store for an event stream is judged on what happens while the stream is
// still arriving, not on how fast it absorbs a file. These modes measure that.
//
//   --mode concurrent   Readers querying a live engine while writers ingest at
//                       full rate. Reports what each side costs the other, and
//                       checks the invariant that makes concurrent reading
//                       meaningful: a query over a time window the stream has
//                       already passed must return the same answer every time,
//                       however much is being appended elsewhere.
//
//   --mode visibility   Wall-clock lag from an edge being inserted to a reader
//                       being able to see it. This is the quantity GraphOne
//                       calls data visibility; for a store that must archive
//                       into an adjacency before vertex-centric reads see an
//                       edge, it is an archiving interval.
//
//   --mode steady       Ingest rate against how much has already been ingested.
//                       A store whose write cost depends on accumulated state
//                       bends downward here; one whose append is O(1) does not.
//
//   --mode batchsize    Ingest rate against arrival batch size, from one edge
//                       at a time up to a million. Collectors deliver small
//                       batches, and a store that only performs well on large
//                       ones is a bulk loader wearing a stream's clothes.
//
//   --mode disorder     Ingest rate and correctness when arrivals are shuffled
//                       within a sliding window, which is what a real collector
//                       delivers. Order-indifference is a design claim; this
//                       measures the rate and proves the final state is
//                       identical to the in-order one.
//
//   --mode retention    Unbounded stream, bounded history: ingest continuously
//                       while releasing everything older than a retention
//                       horizon, and report whether memory reaches a steady
//                       state and what the eviction costs the writer.
//
// Usage:
//   bench_e21_streaming --dataset <bin> --mode concurrent --writers 8 --readers 4
//                       --duration_s 20 --output csv

#include "common.hpp"
#include "barry/engine.hpp"
#include "barry/vertex_index.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <numeric>
#include <random>
#include <string>
#include <thread>
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

bool g_thp = true;          // --thp 0 disables transparent huge pages, for A/B

GraphConfig make_cfg(const GraphMeta& meta, size_t n) {
    GraphConfig cfg;
    cfg.transparent_huge_pages = g_thp;
    cfg.t_min = meta.t_min;
    cfg.t_max = meta.t_max;
    cfg.num_vertices = meta.v_max + 1;
    cfg.build_reverse_index = false;
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, n * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, n * sizeof(EdgeRef) * 8);
    return cfg;
}

// A query the readers can hold to account: a window the stream has already
// passed. Its answer is fixed the moment the last edge inside it is published,
// so any later disagreement is a concurrency bug, not a race with the writer.
struct SettledQuery {
    uint32_t v;
    uint64_t lo, hi;
};

}  // namespace

// -------------------------------------------------------------- concurrent
static int run_concurrent(const Edge* E, size_t n, const GraphMeta& meta,
                          const std::string& ds, int writers, int readers,
                          size_t batch, int samples_in, double window_frac,
                          const std::string& out_csv) {
    const size_t preload = n / 2;            // settled history
    const size_t stream  = n - preload;      // what arrives during the test
    const uint64_t span = (meta.t_max > meta.t_min) ? meta.t_max - meta.t_min : 1;
    const uint64_t window = std::max<uint64_t>(1, static_cast<uint64_t>(span * window_frac));
    int samples = samples_in;

    CsvWriter w(out_csv,
        {"system","dataset","phase","writers","readers","edges_preloaded",
         "edges_streamed","ingest_sec","ingest_Meps","queries","queries_per_sec",
         "q_median_us","q_p95_us","q_p99_us","delivered","settled_mismatches",
         "rss_MB"});

    // Settled queries: windows that close strictly before the earliest edge the
    // streaming phase will deliver, so no arrival can legitimately land inside
    // one. Drawing from the preloaded half is not enough on its own, because a
    // window opened near the split extends past it and then legitimately gains
    // edges; an earlier version of this harness did exactly that and reported
    // 862 "mismatches" that were the query's fault, not the engine's.
    uint64_t t_split = ULLONG_MAX;
    for (size_t i = preload; i < n; ++i)
        if (E[i].timestamp < t_split) t_split = E[i].timestamp;

    std::mt19937_64 rng(0xE21ull);
    std::vector<SettledQuery> qs;
    qs.reserve(samples);
    for (size_t tries = 0; qs.size() < static_cast<size_t>(samples) && tries < 64ull * samples; ++tries) {
        const size_t e = std::uniform_int_distribution<size_t>(0, preload - 1)(rng);
        const uint64_t lo = E[e].timestamp;
        if (lo + window >= t_split) continue;           // window would still be open
        qs.push_back({E[e].src, lo, lo + window});
    }
    if (qs.empty()) {
        std::fprintf(stderr, "[e21-conc] no settled windows: window %llu is wider than "
                     "the preloaded history\n", (unsigned long long)window);
        return 1;
    }
    samples = static_cast<int>(qs.size());

    // Reference answers, computed on a quiescent engine holding the first half.
    std::vector<uint64_t> reference(samples);
    {
        Barry b(make_cfg(meta, n));
        b.insert_batch(E, preload, writers);
        const VertexIndex* vi = b.vertex_index();
        std::vector<TemporalRef> scratch;
        for (int i = 0; i < samples; ++i) {
            scratch.clear();
            vi->neighbors_temporal_into(qs[i].v, qs[i].lo, qs[i].hi, scratch);
            uint64_t h = scratch.size();
            for (const TemporalRef& r : scratch) h += r.ts ^ static_cast<uint64_t>(r.peer);
            reference[i] = h;
        }
    }

    // Phase table: readers alone, writers alone, then both.
    struct Phase { const char* name; bool do_write; bool do_read; };
    const Phase phases[] = {
        {"readers_only", false, true},
        {"writers_only", true,  false},
        {"concurrent",   true,  true},
    };

    for (const Phase& ph : phases) {
        Barry b(make_cfg(meta, n));
        b.insert_batch(E, preload, writers);
        const size_t rss0 = rss_bytes();

        std::atomic<bool> stop{false};
        std::atomic<size_t> edges_done{0};
        std::atomic<size_t> mismatches{0};
        std::atomic<size_t> delivered{0};
        std::vector<std::vector<double>> reader_lat(readers);
        std::vector<size_t> reader_count(readers, 0);

        const double t0 = now_sec();

        std::vector<std::thread> rth;
        if (ph.do_read) {
            for (int r = 0; r < readers; ++r) {
                rth.emplace_back([&, r] {
                    const VertexIndex* vi = b.vertex_index();
                    std::vector<TemporalRef> scratch;
                    std::mt19937_64 lrng(0xBEEFull + r);
                    size_t local_delivered = 0, local_mismatch = 0;
                    reader_lat[r].reserve(1 << 16);
                    while (!stop.load(std::memory_order_relaxed)) {
                        const int i = std::uniform_int_distribution<int>(0, samples - 1)(lrng);
                        const double q0 = now_sec();
                        scratch.clear();
                        vi->neighbors_temporal_into(qs[i].v, qs[i].lo, qs[i].hi, scratch);
                        uint64_t h = scratch.size();
                        for (const TemporalRef& x : scratch)
                            h += x.ts ^ static_cast<uint64_t>(x.peer);
                        reader_lat[r].push_back((now_sec() - q0) * 1e6);
                        local_delivered += scratch.size();
                        if (h != reference[i]) ++local_mismatch;
                    }
                    reader_count[r] = reader_lat[r].size();
                    delivered.fetch_add(local_delivered, std::memory_order_relaxed);
                    mismatches.fetch_add(local_mismatch, std::memory_order_relaxed);
                });
            }
        }

        double ingest_sec = 0;
        if (ph.do_write) {
            const double w0 = now_sec();
            for (size_t off = preload; off < n; off += batch) {
                const size_t m = std::min(batch, n - off);
                b.insert_batch(E + off, m, writers);
                edges_done.fetch_add(m, std::memory_order_relaxed);
            }
            ingest_sec = now_sec() - w0;
        } else {
            // No writer: give the readers the same wall clock the writers took,
            // so the two reader numbers are measured over comparable durations.
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        }

        stop.store(true, std::memory_order_relaxed);
        for (auto& t : rth) t.join();
        const double wall = now_sec() - t0;
        const size_t rss1 = rss_bytes();

        std::vector<double> all_lat;
        for (auto& v : reader_lat) all_lat.insert(all_lat.end(), v.begin(), v.end());
        const size_t nq = all_lat.size();

        w.row(std::string("gnomon"), ds, std::string(ph.name),
              ph.do_write ? writers : 0, ph.do_read ? readers : 0,
              (unsigned long long)preload, (unsigned long long)(ph.do_write ? stream : 0),
              ingest_sec, ph.do_write ? stream / ingest_sec / 1e6 : 0.0,
              (unsigned long long)nq, nq / wall,
              nq ? median(all_lat) : 0.0,
              nq ? percentile(all_lat, 0.95) : 0.0,
              nq ? percentile(all_lat, 0.99) : 0.0,
              (unsigned long long)delivered.load(),
              (unsigned long long)mismatches.load(),
              (rss1 - rss0) / 1e6);

        std::fprintf(stderr,
            "[e21-conc] %-13s ingest=%7.2f Meps  queries=%8llu (%9.0f q/s)  "
            "median=%7.1f us p95=%8.1f us p99=%9.1f us  settled mismatches=%llu\n",
            ph.name, ph.do_write ? stream / ingest_sec / 1e6 : 0.0,
            (unsigned long long)nq, nq / wall,
            nq ? median(all_lat) : 0.0, nq ? percentile(all_lat, 0.95) : 0.0,
            nq ? percentile(all_lat, 0.99) : 0.0,
            (unsigned long long)mismatches.load());
    }
    return 0;
}

// -------------------------------------------------------------- visibility
static int run_visibility(const Edge* E, size_t n, const GraphMeta& meta,
                          const std::string& ds, int writers, int probes,
                          size_t batch, const std::string& out_csv) {
    const size_t preload = std::min<size_t>(n / 2, 20000000);

    Barry b(make_cfg(meta, n));
    b.insert_batch(E, preload, writers);

    CsvWriter w(out_csv,
        {"system","dataset","probes","batch","writers","metric",
         "median_us","p95_us","p99_us","max_us","misses"});

    // Two quantities, separated, because adding them together says nothing
    // about either. `write` is how long the store takes to accept the batch.
    // `publish` is how long after that call returns before a reader can see the
    // batch's last edge, which is the property the publication protocol is
    // responsible for and the one an archiving store pays an interval on.
    // `end_to_end` is their sum, which is what a collector experiences.
    std::vector<double> write_us, publish_us, total_us;
    write_us.reserve(probes); publish_us.reserve(probes); total_us.reserve(probes);
    size_t misses = 0;
    const VertexIndex* vi = b.vertex_index();
    std::vector<TemporalRef> scratch;

    size_t off = preload;
    for (int p = 0; p < probes && off + batch <= n; ++p, off += batch) {
        const Edge& last = E[off + batch - 1];
        const double t0 = now_sec();
        if (batch == 1) b.insert(E[off]);
        else            b.insert_batch(E + off, batch, writers);
        const double t1 = now_sec();

        bool seen = false;
        const double deadline = t1 + 1.0;
        while (now_sec() < deadline) {
            scratch.clear();
            vi->neighbors_temporal_into(last.src, last.timestamp, last.timestamp, scratch);
            for (const TemporalRef& r : scratch) {
                if (r.peer == last.dst && r.ts == last.timestamp) { seen = true; break; }
            }
            if (seen) break;
        }
        if (!seen) { ++misses; continue; }
        const double t2 = now_sec();
        write_us.push_back((t1 - t0) * 1e6);
        publish_us.push_back((t2 - t1) * 1e6);
        total_us.push_back((t2 - t0) * 1e6);
    }

    struct Metric { const char* name; std::vector<double>* v; };
    const Metric metrics[] = {{"write", &write_us}, {"publish", &publish_us},
                              {"end_to_end", &total_us}};
    for (const Metric& m : metrics) {
        std::vector<double>& v = *m.v;
        w.row(std::string("gnomon"), ds, (unsigned long long)v.size(),
              (unsigned long long)batch, writers, std::string(m.name),
              v.empty() ? 0.0 : median(v),
              v.empty() ? 0.0 : percentile(v, 0.95),
              v.empty() ? 0.0 : percentile(v, 0.99),
              v.empty() ? 0.0 : *std::max_element(v.begin(), v.end()),
              (unsigned long long)misses);
        std::fprintf(stderr,
            "[e21-vis] batch=%-8llu %-11s median=%9.2f us  p95=%10.2f us  "
            "p99=%10.2f us  max=%11.2f us\n",
            (unsigned long long)batch, m.name,
            v.empty() ? 0.0 : median(v),
            v.empty() ? 0.0 : percentile(v, 0.95),
            v.empty() ? 0.0 : percentile(v, 0.99),
            v.empty() ? 0.0 : *std::max_element(v.begin(), v.end()));
    }
    if (misses)
        std::fprintf(stderr, "[e21-vis] never visible within 1 s: %llu\n",
                     (unsigned long long)misses);
    return 0;
}

// ------------------------------------------------------------------ steady
static int run_steady(const Edge* E, size_t n, const GraphMeta& meta,
                      const std::string& ds, int threads, size_t batch,
                      int chunks, const std::string& out_csv) {
    const size_t chunk = std::max<size_t>(batch, n / std::max(1, chunks));

    CsvWriter w(out_csv,
        {"system","dataset","threads","batch","edges_before","edges_in_chunk",
         "chunk_sec","chunk_Meps","cumulative_Meps","rss_MB","thp",
         "edges_dropped","blocks_returned"});

    Barry b(make_cfg(meta, n));
    const size_t rss0 = rss_bytes();
    double total = 0;

    for (size_t off = 0; off < n; off += chunk) {
        const size_t m = std::min(chunk, n - off);
        const double c0 = now_sec();
        for (size_t o = off; o < off + m; o += batch)
            b.insert_batch(E + o, std::min(batch, off + m - o), threads);
        const double csec = now_sec() - c0;
        total += csec;

        const Barry::Stats st = b.stats();
        w.row(std::string("gnomon"), ds, threads, (unsigned long long)batch,
              (unsigned long long)off, (unsigned long long)m,
              csec, m / csec / 1e6, (off + m) / total / 1e6,
              (rss_bytes() - rss0) / 1e6, (int)g_thp,
              (unsigned long long)st.edges_dropped,
              (unsigned long long)st.blocks_returned);

        std::fprintf(stderr,
            "[e21-steady] after %11llu edges: chunk %6.2f Meps  cumulative %6.2f Meps  "
            "rss=%7.0f MB  dropped=%llu  returned=%llu\n",
            (unsigned long long)off, m / csec / 1e6, (off + m) / total / 1e6,
            (rss_bytes() - rss0) / 1e6,
            (unsigned long long)st.edges_dropped,
            (unsigned long long)st.blocks_returned);
    }
    return 0;
}

// --------------------------------------------------------------- batchsize
static int run_batchsize(const Edge* E, size_t n, const GraphMeta& meta,
                         const std::string& ds, int threads,
                         const std::string& out_csv) {
    CsvWriter w(out_csv,
        {"system","dataset","threads","batch","edges","ingest_sec","ingest_Meps"});

    // A grid, not a line. Sweeping batch size while letting the thread count
    // follow it confounds the two: a small batch is also a serial batch, and
    // the result reads as "small batches are fast" when it means "one thread is
    // fast on this corpus". Thread count is held fixed down each column.
    const size_t batches[] = {1, 16, 256, 4096, 65536, 1048576};
    const int thread_counts[] = {1, 2, 4, 8, 16};
    for (int t : thread_counts) {
        if (t > threads && t != 1) continue;
        for (size_t bs : batches) {
            Barry b(make_cfg(meta, n));
            const double t0 = now_sec();
            if (bs == 1 && t == 1) {
                for (size_t i = 0; i < n; ++i) b.insert(E[i]);
            } else {
                for (size_t off = 0; off < n; off += bs)
                    b.insert_batch(E + off, std::min(bs, n - off), t);
            }
            const double sec = now_sec() - t0;
            w.row(std::string("gnomon"), ds, t, (unsigned long long)bs,
                  (unsigned long long)n, sec, n / sec / 1e6);
            std::fprintf(stderr,
                "[e21-batch] threads=%2d batch=%9llu  %7.2fs  %6.2f Meps\n",
                t, (unsigned long long)bs, sec, n / sec / 1e6);
        }
    }
    return 0;
}

// ---------------------------------------------------------------- disorder
static int run_disorder(const Edge* E, size_t n, const GraphMeta& meta,
                        const std::string& ds, int threads,
                        size_t batch, const std::string& out_csv) {

    CsvWriter w(out_csv,
        {"system","dataset","threads","shuffle_window","edges","max_lateness",
         "mean_abs_displacement","ingest_sec","ingest_Meps","state_identical"});

    // Reference: the in-order state, as a sorted edge multiset.
    std::vector<std::array<uint64_t, 3>> reference;
    {
        Barry b(make_cfg(meta, n));
        b.insert_batch(E, n, threads);
        std::vector<Edge> all = b.query_range(meta.t_min, meta.t_max);
        reference.reserve(all.size());
        for (const Edge& e : all) reference.push_back({e.timestamp, e.src, e.dst});
        std::sort(reference.begin(), reference.end());
    }

    // Arrival order is the true order shuffled inside a sliding window: an
    // event can overtake any of the `sw` events around it, which is what a
    // multi-source collector with per-source buffering actually produces.
    const size_t windows[] = {1, 64, 4096, 262144, 4194304};
    std::vector<Edge> arrival(n);

    for (size_t sw : windows) {
        std::mt19937_64 rng(0xD150ull ^ sw);
        std::vector<size_t> perm(n);
        std::iota(perm.begin(), perm.end(), 0);
        if (sw > 1) {
            for (size_t off = 0; off < n; off += sw)
                std::shuffle(perm.begin() + off,
                             perm.begin() + std::min(off + sw, n), rng);
        }
        size_t max_late = 0;
        double sum_disp = 0;
        for (size_t i = 0; i < n; ++i) {
            arrival[i] = E[perm[i]];
            const size_t d = (perm[i] > i) ? perm[i] - i : i - perm[i];
            if (perm[i] < i && i - perm[i] > max_late) max_late = i - perm[i];
            sum_disp += static_cast<double>(d);
        }

        Barry b(make_cfg(meta, n));
        const double t0 = now_sec();
        for (size_t off = 0; off < n; off += batch)
            b.insert_batch(arrival.data() + off, std::min(batch, n - off), threads);
        const double sec = now_sec() - t0;

        std::vector<Edge> all = b.query_range(meta.t_min, meta.t_max);
        std::vector<std::array<uint64_t, 3>> got;
        got.reserve(all.size());
        for (const Edge& e : all) got.push_back({e.timestamp, e.src, e.dst});
        std::sort(got.begin(), got.end());
        const bool identical = (got == reference);

        w.row(std::string("gnomon"), ds, threads, (unsigned long long)sw,
              (unsigned long long)n, (unsigned long long)max_late,
              sum_disp / n, sec, n / sec / 1e6, (int)identical);

        std::fprintf(stderr,
            "[e21-disorder] window=%9llu  max lateness=%9llu events  "
            "mean displacement=%10.1f  %7.2fs  %6.2f Meps  state identical=%d\n",
            (unsigned long long)sw, (unsigned long long)max_late,
            sum_disp / n, sec, n / sec / 1e6, (int)identical);
    }
    return 0;
}

// --------------------------------------------------------------- retention
static int run_retention(const Edge* E, size_t n, const GraphMeta& meta,
                         const std::string& ds, int threads, size_t batch,
                         double horizon_frac, int chunks,
                         const std::string& out_csv) {
    const uint64_t span = (meta.t_max > meta.t_min) ? meta.t_max - meta.t_min : 1;
    const uint64_t horizon = std::max<uint64_t>(1, static_cast<uint64_t>(span * horizon_frac));
    const size_t chunk = std::max<size_t>(batch, n / std::max(1, chunks));

    CsvWriter w(out_csv,
        {"system","dataset","threads","horizon_frac","edges_ingested",
         "chunk_Meps","evict_sec","bytes_released_MB","rss_MB","live_edges"});

    Barry b(make_cfg(meta, n));
    const size_t rss0 = rss_bytes();

    for (size_t off = 0; off < n; off += chunk) {
        const size_t m = std::min(chunk, n - off);
        const double c0 = now_sec();
        for (size_t o = off; o < off + m; o += batch)
            b.insert_batch(E + o, std::min(batch, off + m - o), threads);
        const double csec = now_sec() - c0;

        // Release everything older than the horizon behind the newest arrival.
        const uint64_t newest = E[off + m - 1].timestamp;
        double evict_sec = 0;
        double released_mb = 0;
        if (newest > meta.t_min + horizon) {
            const uint64_t cut = newest - horizon;
            const double e0 = now_sec();
            Barry::EvictStats es = b.evict_before(cut);
            evict_sec = now_sec() - e0;
            released_mb = es.bytes_released / 1e6;
        }

        const size_t live = b.query_range(b.min_valid_time(), meta.t_max).size();
        w.row(std::string("gnomon"), ds, threads, horizon_frac,
              (unsigned long long)(off + m), m / csec / 1e6,
              evict_sec, released_mb, (rss_bytes() - rss0) / 1e6,
              (unsigned long long)live);

        std::fprintf(stderr,
            "[e21-ret] after %11llu edges: %6.2f Meps  evict %7.4fs released %8.1f MB  "
            "rss=%8.0f MB  live=%11llu edges\n",
            (unsigned long long)(off + m), m / csec / 1e6, evict_sec, released_mb,
            (rss_bytes() - rss0) / 1e6, (unsigned long long)live);
    }
    return 0;
}

int main(int argc, char** argv) {
    Args args(argc, argv);
    const std::string in_path = args.get("dataset");
    const std::string mode    = args.get("mode", "concurrent");
    const std::string out_csv = args.get("output", "");
    const int    writers  = static_cast<int>(args.getl("writers", 8));
    const int    readers  = static_cast<int>(args.getl("readers", 4));
    const int    threads  = static_cast<int>(args.getl("threads", 8));
    const size_t batch    = static_cast<size_t>(args.getl("batch", 65536));
    const size_t limit    = static_cast<size_t>(args.getl("limit", 0));
    const int    samples  = static_cast<int>(args.getl("samples", 4096));
    const int    probes   = static_cast<int>(args.getl("probes", 2000));
    const int    chunks   = static_cast<int>(args.getl("chunks", 20));
    const double wfrac    = args.getd("window_frac", 0.01);
    const double horizon  = args.getd("horizon_frac", 0.05);
    g_thp = args.getl("thp", 1) != 0;

    if (in_path.empty() || out_csv.empty()) {
        std::fprintf(stderr,
            "usage: %s --dataset <bin> --mode "
            "<concurrent|visibility|steady|batchsize|disorder|retention> "
            "[--writers N] [--readers N] [--threads N] [--batch N] [--limit N] "
            "[--samples N] [--probes N] [--chunks N] [--window_frac F] "
            "[--horizon_frac F] --output csv\n", args.prog().c_str());
        return 1;
    }

    EdgeFile ef;
    if (!ef.load(in_path)) return 1;
    const size_t n = std::min(limit ? limit : ef.count, ef.count);
    const GraphMeta meta = scan_meta(ef.edges, n);
    const std::string ds = basename_noext(in_path);

    // Every mode configures from the prefix it uses, never from the whole file:
    // a prefix occupies a shorter time range, and a mapper sized for the full
    // span would crowd it into a handful of windows.
    if (mode == "concurrent") return run_concurrent(ef.edges, n, meta, ds, writers, readers, batch, samples, wfrac, out_csv);
    if (mode == "visibility") return run_visibility(ef.edges, n, meta, ds, writers, probes, batch, out_csv);
    if (mode == "steady")     return run_steady(ef.edges, n, meta, ds, threads, batch, chunks, out_csv);
    if (mode == "batchsize")  return run_batchsize(ef.edges, n, meta, ds, threads, out_csv);
    if (mode == "disorder")   return run_disorder(ef.edges, n, meta, ds, threads, batch, out_csv);
    if (mode == "retention")  return run_retention(ef.edges, n, meta, ds, threads, batch, horizon, chunks, out_csv);
    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 1;
}
