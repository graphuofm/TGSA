// E8: Does lossy log reduction destroy attack-relevant reachability?
//
// This is the experiment the provenance-storage argument rests on. The field's
// standard answer to storage pressure is to throw data away -- CPR-style causality
// preserving reduction, redundancy removal, sampling. Barry's claim is that the
// storage cost can be paid by restructuring instead of discarding. That claim is
// only interesting if discarding actually costs something measurable.
//
// Protocol:
//   1. Take the red team ground truth as the set of hosts known to be compromised.
//   2. Seed forward taint at the FIRST red team event and propagate to the end of
//      the trace. Recall = fraction of later-compromised hosts the taint reaches.
//   3. Repeat under each reduction policy. Report reduction ratio against recall.
//
// A reduction policy that shrinks the graph 10x but drops recall from 100% to 60%
// has deleted evidence an investigator needed.
//
// Usage:
//   bench_e8_attack --graph <prefix> --policy <full|cpr|random>
//                   --param <window_sec|drop_pct> --classes <csv of edge classes>
//                   --trial i --output csv

#include "common.hpp"
#include "barry/engine.hpp"

#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace barry;
using namespace barry::bench;

struct RedTeamEvent { uint64_t t; uint32_t src, dst; };

static bool load_redteam(const std::string& path, std::vector<RedTeamEvent>* out) {
    std::ifstream f(path);
    if (!f) { std::perror(path.c_str()); return false; }
    std::string line;
    std::getline(f, line);                       // header
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string t, user, sid, did, sname, dname, resolved;
        std::getline(ss, t, '\t');    std::getline(ss, user, '\t');
        std::getline(ss, sid, '\t');  std::getline(ss, did, '\t');
        std::getline(ss, sname, '\t');std::getline(ss, dname, '\t');
        std::getline(ss, resolved);
        if (resolved.rfind("yes", 0) != 0) continue;
        RedTeamEvent e;
        e.t   = std::strtoull(t.c_str(), nullptr, 10);
        e.src = static_cast<uint32_t>(std::strtoul(sid.c_str(), nullptr, 10));
        e.dst = static_cast<uint32_t>(std::strtoul(did.c_str(), nullptr, 10));
        out->push_back(e);
    }
    std::sort(out->begin(), out->end(),
              [](const RedTeamEvent& a, const RedTeamEvent& b) { return a.t < b.t; });
    return !out->empty();
}

int main(int argc, char** argv) {
    Args args(argc, argv);
    std::string prefix  = args.get("graph");
    std::string policy  = args.get("policy", "full");
    double      param   = args.getd("param", 0.0);
    std::string classes = args.get("classes", "1");       // default: successful auth only
    // An investigation asks "what did this touch in the next few hours", not "in the
    // next two months". An unbounded horizon guarantees the taint covers a large
    // fraction of a dense graph, which is both useless to an analyst and dominated
    // by the sheer number of in-range edges rather than by the index layout.
    std::string windows = args.get("windows", "");        // comma-separated fractions of span
    int         trial   = static_cast<int>(args.getl("trial", 0));
    int         threads = static_cast<int>(args.getl("threads", 0));
    std::string out_csv = args.get("output", "");
    if (prefix.empty() || out_csv.empty()) {
        std::fprintf(stderr, "usage: %s --graph <prefix> --policy <full|cpr|random> "
                     "--param P --classes 1,2,3 --trial i --output csv\n", args.prog().c_str());
        return 1;
    }

    std::unordered_set<int> keep_class;
    { std::istringstream ss(classes); std::string tok;
      while (std::getline(ss, tok, ',')) if (!tok.empty()) keep_class.insert(std::stoi(tok)); }

    EdgeFile ef;
    if (!ef.load(prefix + ".bin")) return 1;

    std::vector<RedTeamEvent> rt;
    if (!load_redteam(prefix + ".redteam.tsv", &rt)) {
        std::fprintf(stderr, "no resolved red team events\n"); return 1;
    }

    // ---- ground truth: every host the red team touched after the first event ----
    const uint64_t t_seed = rt.front().t;
    const uint32_t seed   = rt.front().src;
    std::set<uint32_t> gt;
    for (const RedTeamEvent& e : rt) {
        if (e.t < t_seed) continue;
        gt.insert(e.src);
        gt.insert(e.dst);
    }

    // Ground-truth attack EDGES: the specific (src, dst, t) triples the red team
    // generated. Node-level recall is trivially satisfied once taint over-
    // approximates the graph, so edge survival is the metric that actually shows
    // whether a reduction policy deleted evidence.
    std::unordered_set<uint64_t> gt_edge_key;      // hash of (src,dst,t)
    auto edge_key = [](uint32_t s, uint32_t d, uint64_t t) {
        return (static_cast<uint64_t>(s) * 1000003ULL ^
                static_cast<uint64_t>(d) * 31ULL ^ (t << 20));
    };
    for (const RedTeamEvent& e : rt) gt_edge_key.insert(edge_key(e.src, e.dst, e.t));

    // ---- apply the reduction policy ---------------------------------------
    std::vector<Edge> kept;
    kept.reserve(ef.count / 2);
    std::mt19937_64 rng(0xE8 ^ trial);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    // CPR: within a time window, a repeated (src,dst) pair carries no new causal
    // information, so only the first occurrence is retained.
    std::unordered_map<uint64_t, uint64_t> last_seen;   // (src,dst) -> window index
    size_t dropped_class = 0;
    size_t gt_edges_present = 0, gt_edges_kept = 0;
    std::unordered_set<uint64_t> seen_gt_present, seen_gt_kept;

    for (size_t i = 0; i < ef.count; ++i) {
        const Edge& e = ef.edges[i];
        int cls = static_cast<int>(e.weight);
        uint64_t ek = edge_key(e.src, e.dst, e.timestamp);
        bool is_gt = gt_edge_key.count(ek) > 0;
        if (is_gt && seen_gt_present.insert(ek).second) ++gt_edges_present;

        if (!keep_class.empty() && !keep_class.count(cls)) { ++dropped_class; continue; }

        if (policy == "cpr" && param > 0) {
            uint64_t key = (static_cast<uint64_t>(e.src) << 32) | e.dst;
            uint64_t win = e.timestamp / static_cast<uint64_t>(param);
            auto it = last_seen.find(key);
            if (it != last_seen.end() && it->second == win) continue;   // redundant
            last_seen[key] = win;
        } else if (policy == "random" && param > 0) {
            if (unit(rng) < param / 100.0) continue;
        }
        kept.push_back(e);
        if (is_gt && seen_gt_kept.insert(ek).second) ++gt_edges_kept;
    }

    const size_t in_scope = ef.count - dropped_class;
    if (kept.empty()) { std::fprintf(stderr, "policy kept no edges\n"); return 1; }

    GraphMeta meta = scan_meta(kept.data(), kept.size());
    GraphConfig cfg;
    cfg.t_min = meta.t_min; cfg.t_max = meta.t_max;
    cfg.num_vertices = std::max<uint32_t>(meta.v_max + 1, *gt.rbegin() + 1);
    cfg.build_reverse_index = true;
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, kept.size() * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, kept.size() * sizeof(EdgeRef) * 8);

    Barry b(cfg);
    double load0 = now_sec();
    b.insert_batch(kept.data(), kept.size(), threads);
    b.finalize();   // bulk load then query: compact once, explicitly
    double load_sec = now_sec() - load0;
    Barry::Stats st = b.stats();

    // ---- forward taint from the first compromise --------------------------
    // Loading this graph costs minutes and ~96 GB, so every horizon is measured
    // inside a single load rather than one process per configuration.
    std::vector<double> win_fracs;
    if (!windows.empty()) {
        std::istringstream ws(windows); std::string tok;
        while (std::getline(ws, tok, ',')) if (!tok.empty()) win_fracs.push_back(std::stod(tok));
    }
    const uint64_t full_span = (meta.t_max > meta.t_min) ? meta.t_max - meta.t_min : 1;

    CsvWriter ws_out(out_csv + ".windows",
        {"policy","param","window_frac","window_sec","taint_size","taint_frac",
         "gt_hosts","fwd_recall","latency_us"});
    for (double wf : win_fracs) {
        uint64_t win = std::max<uint64_t>(1, static_cast<uint64_t>(full_span * wf));
        uint64_t hi  = std::min(meta.t_max, t_seed + win);
        double w0 = now_sec();
        auto tw = b.forward_taint(seed, t_seed, hi);
        double w_us = (now_sec() - w0) * 1e6;
        std::unordered_set<uint32_t> rw;
        for (const auto& n : tw) rw.insert(n.vertex);
        size_t h = 0;
        for (uint32_t v : gt) if (rw.count(v)) ++h;
        ws_out.row(policy, param, wf, (unsigned long long)win,
                   (unsigned long long)tw.size(),
                   static_cast<double>(tw.size()) / (meta.v_max + 1),
                   (unsigned long long)gt.size(),
                   gt.empty() ? 0.0 : static_cast<double>(h) / gt.size(), w_us);
        std::fprintf(stderr,
            "[e8w] window=%-7g (%8llu s)  |T|=%6zu (%5.1f%% of V)  recall=%5.1f%%  %10.1f us\n",
            wf, (unsigned long long)win, tw.size(),
            100.0 * tw.size() / (meta.v_max + 1),
            gt.empty() ? 0.0 : 100.0 * h / gt.size(), w_us);
    }

    double q0 = now_sec();
    auto taint = b.forward_taint(seed, t_seed, meta.t_max);
    double q_us = (now_sec() - q0) * 1e6;

    std::unordered_set<uint32_t> reached;
    reached.reserve(taint.size() * 2);
    for (const auto& n : taint) reached.insert(n.vertex);

    size_t hit = 0;
    for (uint32_t v : gt) if (reached.count(v)) ++hit;
    double recall = gt.empty() ? 0.0 : static_cast<double>(hit) / gt.size();

    // ---- backward trace from the last compromise --------------------------
    double q1 = now_sec();
    auto back = b.backward_trace(rt.back().dst, rt.back().t, meta.t_min);
    double b_us = (now_sec() - q1) * 1e6;
    std::unordered_set<uint32_t> back_reached;
    for (const auto& n : back) back_reached.insert(n.vertex);
    size_t b_hit = 0;
    for (uint32_t v : gt) if (back_reached.count(v)) ++b_hit;
    double b_recall = gt.empty() ? 0.0 : static_cast<double>(b_hit) / gt.size();

    CsvWriter w(out_csv,
        {"policy","param","classes","trial","edges_in_scope","edges_kept","reduction_ratio",
         "gt_hosts","gt_edges_present","gt_edges_kept","gt_edge_survival",
         "taint_size","taint_frac","fwd_recall","fwd_latency_us",
         "back_size","back_recall","back_latency_us",
         "num_vertices","rss_MB","load_sec"});
    w.row(policy, param, classes, trial,
          (unsigned long long)in_scope, (unsigned long long)kept.size(),
          static_cast<double>(in_scope) / kept.size(),
          (unsigned long long)gt.size(),
          (unsigned long long)gt_edges_present, (unsigned long long)gt_edges_kept,
          gt_edges_present ? static_cast<double>(gt_edges_kept) / gt_edges_present : 0.0,
          (unsigned long long)taint.size(),
          static_cast<double>(taint.size()) / cfg.num_vertices,
          recall, q_us,
          (unsigned long long)back.size(), b_recall, b_us,
          (unsigned long long)cfg.num_vertices, st.bytes_rss / 1e6, load_sec);

    std::fprintf(stderr,
        "[e8] %-8s p=%-7g  kept=%9zu/%-9zu (%5.2fx)  "
        "ATTACK-EDGE survival=%3zu/%-3zu (%5.1f%%)  "
        "|T|=%zu (%.1f%% of V) recall=%.1f%%  %.0fus\n",
        policy.c_str(), param, kept.size(), in_scope,
        static_cast<double>(in_scope) / kept.size(),
        gt_edges_kept, gt_edges_present,
        gt_edges_present ? 100.0 * gt_edges_kept / gt_edges_present : 0.0,
        taint.size(), 100.0 * taint.size() / cfg.num_vertices,
        100.0 * recall, q_us);
    (void)b_recall; (void)b_us;
    return 0;
}
