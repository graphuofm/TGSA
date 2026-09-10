// E12: Rebuild versus stream -- the cost a static index pays to stay current.
//
// A time-sorted CSR beats Barry on traversal latency (E9), so the honest question
// is not which is faster to query but which can be queried at all against a live
// stream. A sorted structure has one update path: rebuild. While that rebuild
// runs, arriving events are not in the index, and a query issued in that window
// does not return a stale answer -- it returns a WRONG one, silently missing
// everything that arrived since the last rebuild.
//
// Protocol: split the trace at `split_frac`. The sorted CSR is built on the prefix
// only, standing in for "the last rebuild finished this long ago". Barry ingests
// the whole thing incrementally. Both then answer identical queries, and we count
// what the stale index missed.
//
// Usage:
//   bench_e12_staleness --dataset <bin> [--redteam <tsv>] --split_frac 0.99
//                       --num_queries N --window_frac F --batch M
//                       --threads T --trial i --output csv

#include "common.hpp"
#include "temporal_csr.hpp"
#include "barry/engine.hpp"

#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>

using namespace barry;
using namespace barry::bench;

int main(int argc, char** argv) {
    Args args(argc, argv);
    std::string in_path  = args.get("dataset");
    std::string rt_path  = args.get("redteam");
    double split_frac    = args.getd("split_frac", 0.99);
    int    num_queries   = static_cast<int>(args.getl("num_queries", 200));
    double window_frac   = args.getd("window_frac", 0.05);
    size_t batch         = static_cast<size_t>(args.getl("batch", 100000));
    int    threads       = static_cast<int>(args.getl("threads", 0));
    int    trial         = static_cast<int>(args.getl("trial", 0));
    std::string out_csv  = args.get("output", "");
    if (in_path.empty() || out_csv.empty()) {
        std::fprintf(stderr, "usage: %s --dataset <bin> [--redteam tsv] --split_frac F "
                     "--num_queries N --window_frac W --batch M --threads T "
                     "--trial i --output csv\n", args.prog().c_str());
        return 1;
    }

    EdgeFile ef;
    if (!ef.load(in_path)) return 1;
    GraphMeta meta = scan_meta(ef.edges, ef.count);
    const uint64_t span = (meta.t_max > meta.t_min) ? meta.t_max - meta.t_min : 1;
    const uint64_t window = std::max<uint64_t>(1, static_cast<uint64_t>(span * window_frac));

    const size_t split = static_cast<size_t>(ef.count * split_frac);
    const size_t n_suffix = ef.count - split;
    const uint64_t t_split = ef.edges[split].timestamp;

    // The trace carries its own arrival rate: events per second of trace time.
    const double arrival_rate = static_cast<double>(ef.count) / static_cast<double>(span);

    std::fprintf(stderr,
        "[e12] %s: %zu edges over %.1f days => %.0f events/s\n"
        "      split at %.4f -> prefix %zu, suffix %zu\n",
        basename_noext(in_path).c_str(), ef.count, span / 86400.0, arrival_rate,
        split_frac, split, n_suffix);

    // ---- the static structure's only update path: rebuild from scratch -----
    TemporalCsr csr;
    double r0 = now_sec();
    csr.build(ef.edges, split, meta.v_max + 1);
    const double rebuild_sec = now_sec() - r0;

    // ---- Barry: prefix, then the suffix arriving incrementally -------------
    GraphConfig cfg;
    cfg.t_min = meta.t_min; cfg.t_max = meta.t_max;
    cfg.num_vertices = meta.v_max + 1;
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, ef.count * sizeof(Edge) * 3);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, ef.count * sizeof(EdgeRef) * 3);
    Barry b(cfg);
    b.insert_batch(ef.edges, split, threads);
    // Compact the prefix once -- the streaming counterpart of the CSR's rebuild.
    // The incremental appends below deliberately do NOT recompact: that is the
    // whole point, and folding compaction into every batch would make Barry cost
    // exactly what the static structure costs.
    double comp0 = now_sec();
    b.finalize();
    const double compact_sec = now_sec() - comp0;

    std::vector<double> append_us;
    double inc0 = now_sec();
    for (size_t off = split; off < ef.count; off += batch) {
        size_t m = std::min(batch, ef.count - off);
        double a = now_sec();
        b.insert_batch(ef.edges + off, m, threads);
        append_us.push_back((now_sec() - a) * 1e6);
    }
    const double incremental_sec = now_sec() - inc0;

    // ---- what the stale index cannot see -----------------------------------
    // During one rebuild, this many events arrive at the trace's own rate.
    const double events_per_rebuild = arrival_rate * rebuild_sec;

    size_t rt_total = 0, rt_in_suffix = 0;
    if (!rt_path.empty()) {
        std::ifstream f(rt_path);
        std::string line;
        if (f) {
            std::getline(f, line);
            while (std::getline(f, line)) {
                std::istringstream ss(line);
                std::string t, user, sid, did, sn, dn, ok;
                std::getline(ss, t, '\t');   std::getline(ss, user, '\t');
                std::getline(ss, sid, '\t'); std::getline(ss, did, '\t');
                std::getline(ss, sn, '\t');  std::getline(ss, dn, '\t');
                std::getline(ss, ok);
                if (ok.rfind("yes", 0) != 0) continue;
                ++rt_total;
                if (std::strtoull(t.c_str(), nullptr, 10) >= t_split) ++rt_in_suffix;
            }
        }
    }

    // ---- identical queries against current Barry and stale CSR -------------
    std::mt19937_64 rng(0xE12 ^ trial);
    const VertexIndex* vi = b.vertex_index();
    size_t q_run = 0, q_with_loss = 0;
    double total_true = 0, total_missed = 0, worst_loss = 0;

    for (int i = 0; i < num_queries; ++i) {
        uint32_t seed = 0;
        for (int k = 0; k < 64; ++k) {
            uint32_t v = std::uniform_int_distribution<uint32_t>(0, meta.v_max)(rng);
            if (vi->out_degree(v) > 0) { seed = v; break; }
        }
        // Ask about the live window -- the period a stale index cannot cover.
        uint64_t lo = (t_split > meta.t_min + window) ? t_split - window : meta.t_min;
        uint64_t hi = meta.t_max;

        auto cur = b.forward_taint(seed, lo, hi);
        auto old = csr.forward_taint(seed, lo, hi);
        if (cur.empty()) continue;
        ++q_run;

        std::unordered_set<uint32_t> seen;
        seen.reserve(old.size() * 2);
        for (const auto& p : old) seen.insert(p.first);
        size_t missed = 0;
        for (const auto& n : cur) if (!seen.count(n.vertex)) ++missed;

        total_true   += cur.size();
        total_missed += missed;
        double loss = static_cast<double>(missed) / cur.size();
        worst_loss = std::max(worst_loss, loss);
        if (missed) ++q_with_loss;
    }

    const double avg_loss = total_true > 0 ? total_missed / total_true : 0.0;

    CsvWriter w(out_csv,
        {"dataset","trial","split_frac","total_edges","prefix_edges","suffix_edges",
         "span_days","arrival_rate_eps","csr_rebuild_sec","events_per_rebuild",
         "barry_compact_sec","barry_incremental_sec","barry_batch_median_us","barry_eps",
         "queries","queries_with_loss","avg_answer_loss","worst_answer_loss",
         "redteam_total","redteam_invisible"});
    w.row(basename_noext(in_path), trial, split_frac,
          (unsigned long long)ef.count, (unsigned long long)split,
          (unsigned long long)n_suffix, span / 86400.0, arrival_rate,
          rebuild_sec, events_per_rebuild,
          compact_sec, incremental_sec, median(append_us),
          incremental_sec > 0 ? n_suffix / incremental_sec : 0.0,
          (unsigned long long)q_run, (unsigned long long)q_with_loss,
          avg_loss, worst_loss,
          (unsigned long long)rt_total, (unsigned long long)rt_in_suffix);

    std::fprintf(stderr,
        "[e12] CSR rebuild: %.2fs (Barry compaction: %.2fs)  =>  %.0f events unindexed\n"
        "      Barry ingests the same %zu events incrementally in %.2fs (%.2f Meps)\n"
        "      of %zu queries, %zu returned an incomplete answer; "
        "avg loss %.1f%%, worst %.1f%%\n",
        rebuild_sec, compact_sec, events_per_rebuild, n_suffix, incremental_sec,
        incremental_sec > 0 ? n_suffix / incremental_sec / 1e6 : 0.0,
        q_run, q_with_loss, 100 * avg_loss, 100 * worst_loss);
    if (rt_total)
        std::fprintf(stderr, "      red team: %zu/%zu events fall in the unindexed window\n",
                     rt_in_suffix, rt_total);
    return 0;
}
