// E14: Cross-corpus validation of the reduction finding on real whole-system
// provenance (DARPA TC E3 CADETS), with the chain-entity metric.
//
// LANL's node-level recall saturates because host-granularity taint
// over-approximates (29% of the network). CADETS entities are fine-grained
// (2.86M nodes), so "which ground-truth chain entities does forward taint still
// reach after reduction" is a discriminating measurement, not a saturated one.
//
//   bench_e14_tc --graph <prefix> --seeds <seeds.tsv> --attack 1
//                --policy <full|cpr|random> --param P --trial i --output csv
//
// Seed file: attack \t kind \t match \t node_id \t node_kind \t label
// The earliest-timestamped netflow seed of the attack is the taint source;
// recall is measured over ALL that attack's resolved node ids.

#include "common.hpp"
#include "barry/engine.hpp"

#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace barry;
using namespace barry::bench;

int main(int argc, char** argv) {
    Args args(argc, argv);
    std::string prefix = args.get("graph");
    std::string seeds_p = args.get("seeds");
    std::string attack = args.get("attack", "1");
    std::string policy = args.get("policy", "full");
    double param = args.getd("param", 0.0);
    int trial = static_cast<int>(args.getl("trial", 0));
    int threads = static_cast<int>(args.getl("threads", 8));
    std::string out_csv = args.get("output", "");
    if (prefix.empty() || seeds_p.empty() || out_csv.empty()) {
        std::fprintf(stderr, "usage: %s --graph p --seeds s.tsv --attack N "
                     "--policy full|cpr|random --param P --trial i --output csv\n",
                     args.prog().c_str());
        return 1;
    }

    // ---- ground-truth ids for this attack ----
    std::unordered_set<uint32_t> gt_ids;
    std::vector<uint32_t> netflow_seeds;
    {
        std::ifstream f(seeds_p);
        std::string line; std::getline(f, line);
        while (std::getline(f, line)) {
            std::istringstream ss(line);
            std::string a, kind, match, nid, nkind, label;
            std::getline(ss, a, '\t');    std::getline(ss, kind, '\t');
            std::getline(ss, match, '\t');std::getline(ss, nid, '\t');
            std::getline(ss, nkind, '\t');std::getline(ss, label);
            if (a != attack) continue;
            uint32_t id = static_cast<uint32_t>(std::strtoul(nid.c_str(), nullptr, 10));
            gt_ids.insert(id);
            if (nkind == "netflow") netflow_seeds.push_back(id);
        }
    }
    if (gt_ids.empty() || netflow_seeds.empty()) {
        std::fprintf(stderr, "no seeds for attack %s\n", attack.c_str()); return 1;
    }

    EdgeFile ef;
    if (!ef.load(prefix + ".bin")) return 1;

    // seed = earliest event touching any of the attack's netflow nodes
    std::unordered_set<uint32_t> nf(netflow_seeds.begin(), netflow_seeds.end());
    uint32_t seed = 0; uint64_t t_seed = ~0ULL;
    for (size_t i = 0; i < ef.count; ++i) {
        const Edge& e = ef.edges[i];
        if ((nf.count(e.src) || nf.count(e.dst)) && e.timestamp < t_seed) {
            t_seed = e.timestamp;
            seed = nf.count(e.src) ? e.src : e.dst;
        }
    }

    // ---- reduction ----
    std::vector<Edge> kept;
    kept.reserve(ef.count);
    std::mt19937_64 rng(0xE14 ^ trial);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::unordered_map<uint64_t, uint64_t> last_seen;
    for (size_t i = 0; i < ef.count; ++i) {
        const Edge& e = ef.edges[i];
        if (policy == "cpr" && param > 0) {
            uint64_t key = (static_cast<uint64_t>(e.src) << 32) | e.dst;
            uint64_t win = e.timestamp / static_cast<uint64_t>(param);
            auto it = last_seen.find(key);
            if (it != last_seen.end() && it->second == win) continue;
            last_seen[key] = win;
        } else if (policy == "random" && param > 0) {
            if (unit(rng) < param / 100.0) continue;
        }
        kept.push_back(e);
    }

    GraphMeta meta = scan_meta(kept.data(), kept.size());
    GraphConfig cfg;
    cfg.t_min = meta.t_min; cfg.t_max = meta.t_max;
    cfg.num_vertices = meta.v_max + 1;
    cfg.build_reverse_index = true;   // an investigator runs both directions
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, kept.size() * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, kept.size() * sizeof(EdgeRef) * 8);
    Barry b(cfg);
    b.insert_batch(kept.data(), kept.size(), threads);
    b.finalize();   // bulk load then query: compact once, explicitly

    // The investigation primitive is bidirectional: forward impact from the
    // intrusion point PLUS backward causality from every confirmed C2 flow.
    double q0 = now_sec();
    auto taint = b.forward_taint(seed, t_seed, meta.t_max);
    std::unordered_set<uint32_t> reached;
    for (const auto& n : taint) reached.insert(n.vertex);
    for (uint32_t nfid : netflow_seeds) {
        auto back = b.backward_trace(nfid, meta.t_max, meta.t_min);
        for (const auto& n : back) reached.insert(n.vertex);
    }
    double q_us = (now_sec() - q0) * 1e6;
    size_t hit = 0;
    for (uint32_t v : gt_ids) if (reached.count(v)) ++hit;

    CsvWriter w(out_csv,
        {"corpus","attack","policy","param","trial","edges_kept","reduction",
         "gt_entities","chain_recall","union_size","union_frac","latency_us"});
    w.row(basename_noext(prefix), attack, policy, param, trial,
          (unsigned long long)kept.size(),
          static_cast<double>(ef.count) / kept.size(),
          (unsigned long long)gt_ids.size(),
          static_cast<double>(hit) / gt_ids.size(),
          (unsigned long long)reached.size(),
          static_cast<double>(reached.size()) / cfg.num_vertices, q_us);
    std::fprintf(stderr,
        "[e14] attack %s  %-7s p=%-7g  %.2fx reduction  CHAIN recall %zu/%zu (%5.1f%%)  "
        "|fwd u back|=%zu (%.4f%% of V)  %.0fus\n",
        attack.c_str(), policy.c_str(), param,
        static_cast<double>(ef.count) / kept.size(),
        hit, gt_ids.size(), 100.0 * hit / gt_ids.size(),
        reached.size(), 100.0 * reached.size() / cfg.num_vertices, q_us);
    return 0;
}
