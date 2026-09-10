// E11: Sustained triage throughput -- can you afford to investigate EVERYTHING?
//
// A speedup on one query is not a pain point. The pain point is what the speedup
// makes affordable. When causality tracing costs seconds, only alerts a human
// picks out get traced, and the rest are closed unexamined. When it costs
// microseconds, tracing every alert becomes a background job.
//
// That argument sharpens in an agent deployment, where the caller is not a human
// triaging tens of alerts a day but a process issuing probes continuously -- most
// speculative, most discarded. What matters there is sustained queries/second and
// the tail, not a single median.
//
// This benchmark runs backward causality tracing over an entire alert set (no
// sampling) and reports sustained throughput plus the full latency distribution.
//
// Usage:
//   bench_e11_sweep --graph <prefix> [--alerts <redteam.tsv>] [--num_alerts N]
//                   [--window_frac F] --threads T --trial i --output csv
//                   [--detail csv]

#include "common.hpp"
#include "barry/engine.hpp"

#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>

using namespace barry;
using namespace barry::bench;

struct Alert { uint64_t t; uint32_t v; };

// Reads the .redteam.tsv emitted by barry_lanl_to_bin: the destination host of a
// confirmed compromise is what an EDR would raise an alert on.
static size_t load_alerts(const std::string& path, std::vector<Alert>* out) {
    std::ifstream f(path);
    if (!f) return 0;
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string t, user, sid, did, sn, dn, resolved;
        std::getline(ss, t, '\t');   std::getline(ss, user, '\t');
        std::getline(ss, sid, '\t'); std::getline(ss, did, '\t');
        std::getline(ss, sn, '\t');  std::getline(ss, dn, '\t');
        std::getline(ss, resolved);
        if (resolved.rfind("yes", 0) != 0) continue;
        out->push_back(Alert{std::strtoull(t.c_str(), nullptr, 10),
                             static_cast<uint32_t>(std::strtoul(did.c_str(), nullptr, 10))});
    }
    return out->size();
}

int main(int argc, char** argv) {
    Args args(argc, argv);
    std::string prefix   = args.get("graph");
    std::string alerts_p = args.get("alerts");
    int    num_alerts    = static_cast<int>(args.getl("num_alerts", 0));
    double window_frac   = args.getd("window_frac", 0.1);
    int    threads       = static_cast<int>(args.getl("threads", 0));
    int    trial         = static_cast<int>(args.getl("trial", 0));
    std::string out_csv  = args.get("output", "");
    std::string detail_p = args.get("detail", "");
    if (prefix.empty() || out_csv.empty()) {
        std::fprintf(stderr, "usage: %s --graph <prefix> [--alerts tsv] [--num_alerts N] "
                     "--window_frac F --threads T --trial i --output csv [--detail csv]\n",
                     args.prog().c_str());
        return 1;
    }

    EdgeFile ef;
    if (!ef.load(prefix + ".bin")) return 1;
    GraphMeta meta = scan_meta(ef.edges, ef.count);
    const uint64_t span   = (meta.t_max > meta.t_min) ? meta.t_max - meta.t_min : 1;
    const uint64_t window = std::max<uint64_t>(1, static_cast<uint64_t>(span * window_frac));

    GraphConfig cfg;
    cfg.t_min = meta.t_min; cfg.t_max = meta.t_max;
    cfg.num_vertices = meta.v_max + 1;
    cfg.build_reverse_index = true;               // backward tracing needs in-edges
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, ef.count * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, ef.count * sizeof(EdgeRef) * 8);

    Barry b(cfg);
    double l0 = now_sec();
    b.insert_batch(ef.edges, ef.count, threads);
    b.finalize();   // bulk load then query: compact once, explicitly
    double load_sec = now_sec() - l0;
    Barry::Stats st = b.stats();

    // ---- assemble the alert set -------------------------------------------
    std::vector<Alert> alerts;
    std::string alert_source = "redteam";
    if (!alerts_p.empty()) load_alerts(alerts_p, &alerts);
    if (alerts.empty()) {
        alert_source = "synthetic";
        if (num_alerts <= 0) num_alerts = 1000;
        std::mt19937_64 rng(0xE11 ^ trial);
        const VertexIndex* vi = b.vertex_index();
        alerts.reserve(num_alerts);
        while (static_cast<int>(alerts.size()) < num_alerts) {
            uint32_t v = std::uniform_int_distribution<uint32_t>(0, meta.v_max)(rng);
            if (vi->in_degree(v) == 0) continue;
            uint64_t t = meta.t_min +
                std::uniform_int_distribution<uint64_t>(0, span - 1)(rng);
            alerts.push_back(Alert{t, v});
        }
    }
    std::fprintf(stderr, "[e11] %zu alerts (%s), sweeping ALL of them\n",
                 alerts.size(), alert_source.c_str());

    std::unique_ptr<CsvWriter> detail;
    if (!detail_p.empty()) {
        detail.reset(new CsvWriter(detail_p,
            {"graph","trial","alert_idx","vertex","time","ancestors","latency_us"}));
    }

    // ---- sweep every alert, no sampling -----------------------------------
    std::vector<double> lat;
    lat.reserve(alerts.size());
    double total_ancestors = 0, max_ancestors = 0;
    double sweep0 = now_sec();
    for (size_t i = 0; i < alerts.size(); ++i) {
        uint64_t lo = (alerts[i].t > meta.t_min + window) ? alerts[i].t - window : meta.t_min;
        double a = now_sec();
        auto res = b.backward_trace(alerts[i].v, alerts[i].t, lo);
        double us = (now_sec() - a) * 1e6;
        lat.push_back(us);
        total_ancestors += res.size();
        max_ancestors = std::max(max_ancestors, static_cast<double>(res.size()));
        if (detail) {
            detail->row(basename_noext(prefix), trial, (unsigned long long)i,
                        alerts[i].v, (unsigned long long)alerts[i].t,
                        (unsigned long long)res.size(), us);
        }
    }
    double sweep_sec = now_sec() - sweep0;

    const double qps = alerts.size() / sweep_sec;
    // What the throughput buys: alerts one core can fully trace in a day.
    const double per_day = qps * 86400.0;

    CsvWriter w(out_csv,
        {"graph","trial","alert_source","alerts","window_frac",
         "sweep_sec","qps","traceable_per_day",
         "median_us","p95_us","p99_us","max_us","mean_us",
         "avg_ancestors","max_ancestors","total_edges","num_vertices","rss_MB","load_sec"});
    w.row(basename_noext(prefix), trial, alert_source,
          (unsigned long long)alerts.size(), window_frac,
          sweep_sec, qps, per_day,
          median(lat), percentile(lat, 0.95), percentile(lat, 0.99),
          percentile(lat, 1.0), mean(lat),
          total_ancestors / alerts.size(), max_ancestors,
          (unsigned long long)ef.count, (unsigned long long)cfg.num_vertices,
          st.bytes_rss / 1e6, load_sec);

    std::fprintf(stderr,
        "[e11] %-20s  %zu alerts swept in %.3fs  =>  %.0f queries/s  "
        "(%.1fM alerts/day/core)\n"
        "      latency: median=%.1fus p95=%.1fus p99=%.1fus max=%.1fus   "
        "ancestors: avg=%.1f max=%.0f\n",
        basename_noext(prefix).c_str(), alerts.size(), sweep_sec, qps, per_day / 1e6,
        median(lat), percentile(lat, 0.95), percentile(lat, 0.99), percentile(lat, 1.0),
        total_ancestors / alerts.size(), max_ancestors);
    return 0;
}
