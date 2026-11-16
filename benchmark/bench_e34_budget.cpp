// E34: Retention under a fixed storage budget, framed the way an investigator
// meets it.
//
// A log store has a budget B, a fraction of what the lossless corpus would
// occupy. There are two ways to live inside it. LOSSLESS-SHORT keeps every
// event and shortens the horizon: at investigation time T_inv it holds the
// events in [T_inv - H, T_inv] for the largest H that fits. REDUCED-LONG keeps
// the whole history up to T_inv but passes it through CPR (causality-
// preserving reduction, one event per (src,dst) pair per window) with the
// smallest window that fits; when even CPR's ceiling (one event per pair,
// ever) does not fit, it keeps the ceiling-reduced stream for the largest
// horizon that fits and the row says so.
//
// The investigation starts at T_inv = the last ground-truth event plus a
// delay. Both policies are pure stream filters over the raw corpus (the same
// style as the E8 reduction loop); the survivors are loaded into the engine
// and the attack is measured there: attack-edge survival (LANL red team) or
// chain recall (CADETS, the E14 logic), the survived fraction of ground-truth
// events, the bytes the engine actually holds, and forward-taint latency from
// the first ground-truth event.
//
// One load of the raw .bin serves every setting: the filters run over the
// mapped file, the engine is instantiated per setting and torn down.
//
//   bench_e34_budget --graph <prefix> --corpus lanl|cadets [--classes 1]
//                    [--attack N] --budgets 1,0.5,0.25,... --delays 0,1,7,max
//                    [--verify] --threads 16 --output e34.csv
//                    [--max_setting_sec 1200]

#include "common.hpp"
#include "barry/engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace barry;
using namespace barry::bench;

namespace {

// Same key as bench_e8_attack: the (src, dst, t) triple the red team generated.
inline uint64_t edge_key(uint32_t s, uint32_t d, uint64_t t) {
    return (static_cast<uint64_t>(s) * 1000003ULL ^
            static_cast<uint64_t>(d) * 31ULL ^ (t << 20));
}
inline uint64_t pair_key(const Edge& e) {
    return (static_cast<uint64_t>(e.src) << 32) | e.dst;
}
// CPR state is per (src,dst) pair, so a pass can be split across threads by
// pair without changing a single keep/drop decision.
inline unsigned pair_part(uint64_t key, unsigned T) {
    return static_cast<unsigned>(((key * 0x9E3779B97F4A7C15ULL) >> 33) % T);
}

struct Scope {
    uint64_t cls_mask = 0;                       // 0 = every edge is in scope
    bool operator()(const Edge& e) const {
        if (!cls_mask) return true;
        int c = static_cast<int>(e.weight);
        return c >= 0 && c < 64 && ((cls_mask >> c) & 1);
    }
};

// Time-ordered view of the in-scope corpus. Points at the mapped file when the
// file is already sorted (LANL), at a sorted copy otherwise (CADETS has 179
// out-of-order records).
struct Stream {
    const Edge* e = nullptr;
    size_t n = 0;
    uint64_t t_min = 0, t_max = 0;
    uint32_t v_max = 0;
    size_t n_scope = 0;                          // in-scope events, whole corpus
    size_t upper_idx(uint64_t t) const {         // first index with timestamp > t
        return std::upper_bound(e, e + n, t,
            [](uint64_t v, const Edge& x) { return v < x.timestamp; }) - e;
    }
    size_t lower_idx(uint64_t t) const {         // first index with timestamp >= t
        return std::lower_bound(e, e + n, t,
            [](const Edge& x, uint64_t v) { return x.timestamp < v; }) - e;
    }
};

struct RedTeamEvent { uint64_t t; uint32_t src, dst; };

bool load_redteam(const std::string& path, std::vector<RedTeamEvent>* out) {
    std::ifstream f(path);
    if (!f) { std::perror(path.c_str()); return false; }
    std::string line;
    std::getline(f, line);
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

struct Truth {
    bool lanl = true;
    uint32_t seed = 0;
    uint64_t t_seed = 0, t_last = 0;
    uint32_t v_need = 0;                         // max ground-truth id + 1
    // LANL: the red team's (src,dst,t) triples and the hosts they touched.
    std::unordered_set<uint64_t> edge_keys;      // E8's hash, as a fast pre-filter
    std::set<std::tuple<uint32_t, uint32_t, uint64_t>> triples;   // exact
    std::set<uint32_t> hosts;
    // CADETS: the attack's netflow entities (its C2 channel) and chain ids.
    std::unordered_set<uint32_t> nf_nodes;
    std::vector<uint32_t> nf_seeds;
    std::unordered_set<uint32_t> gt_ids;

    bool is_gt(const Edge& e) const {
        if (!lanl) return nf_nodes.count(e.src) > 0 || nf_nodes.count(e.dst) > 0;
        return edge_keys.count(edge_key(e.src, e.dst, e.timestamp)) > 0 &&
               triples.count({e.src, e.dst, e.timestamp}) > 0;
    }
};

// Identity for counting ground-truth events: the distinct (src, dst, t) triple,
// on both corpora. E8 counted LANL this way; on CADETS the trace stamps many
// syscalls on one flow with one timestamp (70% of records repeat a triple), so
// counting records would score the collapse of those repeats as lost evidence.
using GtId = std::pair<uint64_t, uint64_t>;
inline GtId gt_id(const Edge& e) { return {pair_key(e), e.timestamp}; }

// Anonymous resident bytes of this process. The engine's own allocators report
// bump high-water marks over MAP_NORESERVE reservations, which include buckets
// abandoned by chain growth and CAS losers; RssAnon is what the machine holds.
// File-backed pages of the mapped corpus are excluded by construction.
size_t rss_anon_bytes() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("RssAnon:", 0) == 0) return std::strtoull(line.c_str() + 8, nullptr, 10) * 1024;
    return 0;
}

template <class F> void run_threads(int T, F f) {
    std::vector<std::thread> th;
    for (int t = 0; t < T; ++t) th.emplace_back(f, t);
    for (auto& x : th) x.join();
}

// Count, for every CPR window in W and every investigation time (hi[d] = index
// of the first event after T_inv_d, ascending), how many events survive CPR
// over the history up to T_inv_d. W[0] == 0 means no reduction. One pass over
// the stream, threads split by pair. Windows are bucketed from t_min so that
// W = span + 1 is exactly "one event per pair, ever": the CPR ceiling.
std::vector<uint64_t> cpr_counts(const Stream& S, Scope sc, const std::vector<uint64_t>& W,
                                 const std::vector<size_t>& hi, int T) {
    const size_t K = W.size(), D = hi.size(), n_max = hi.back();
    std::vector<std::vector<uint64_t>> per(T, std::vector<uint64_t>(K * D, 0));
    run_threads(T, [&](int t) {
        std::unordered_map<uint64_t, uint32_t> pid;
        std::vector<uint64_t> last;
        std::vector<uint64_t> cur(K, 0);
        size_t d = 0;
        auto snap = [&]() { for (size_t k = 0; k < K; ++k) per[t][k * D + d] = cur[k]; ++d; };
        for (size_t i = 0; i < n_max; ++i) {
            while (d < D && hi[d] == i) snap();
            const Edge& e = S.e[i];
            if (!sc(e)) continue;
            uint64_t key = pair_key(e);
            if (pair_part(key, T) != static_cast<unsigned>(t)) continue;
            auto [it, ins] = pid.try_emplace(key, static_cast<uint32_t>(pid.size()));
            if (ins) last.resize(last.size() + K, UINT64_MAX);
            const size_t base = static_cast<size_t>(it->second) * K;
            const uint64_t rel = e.timestamp - S.t_min;
            ++cur[0];
            for (size_t k = 1; k < K; ++k) {
                uint64_t b = rel / W[k];
                if (last[base + k] != b) { last[base + k] = b; ++cur[k]; }
            }
        }
        while (d < D) snap();
    });
    std::vector<uint64_t> cnt(K * D, 0);
    for (int t = 0; t < T; ++t)
        for (size_t i = 0; i < K * D; ++i) cnt[i] += per[t][i];
    return cnt;
}

// Mark the events that CPR with window w keeps over [0, hi). flags[i] is
// written only by the thread that owns pair(i), so no two threads touch the
// same byte.
void cpr_flags(const Stream& S, Scope sc, uint64_t w, size_t hi, int T,
               std::vector<uint8_t>& flags) {
    std::fill_n(flags.data(), hi, static_cast<uint8_t>(0));
    run_threads(T, [&](int t) {
        std::unordered_map<uint64_t, uint64_t> last;
        for (size_t i = 0; i < hi; ++i) {
            const Edge& e = S.e[i];
            if (!sc(e)) continue;
            uint64_t key = pair_key(e);
            if (pair_part(key, T) != static_cast<unsigned>(t)) continue;
            uint64_t b = (e.timestamp - S.t_min) / w;
            auto [it, ins] = last.try_emplace(key, b);
            if (ins || it->second != b) { it->second = b; flags[i] = 1; }
        }
    });
}

// Largest horizon whose eligible events in [t_cut, T_inv] number at most
// `budget`. Eligible = in scope and, when flags is given, flagged. Returns the
// first stream index to keep and sets t_cut. Ties at the boundary second are
// excluded whole, so the kept count never exceeds the budget.
size_t fit_horizon(const Stream& S, Scope sc, const std::vector<uint8_t>* flags,
                   size_t hi, size_t budget, uint64_t* t_cut) {
    size_t cnt = 0, i = hi;
    while (i > 0) {
        const Edge& e = S.e[i - 1];
        bool ok = sc(e) && (!flags || (*flags)[i - 1]);
        if (ok) { if (cnt == budget) break; ++cnt; }
        --i;
    }
    if (i == 0) { *t_cut = S.t_min; return 0; }
    *t_cut = S.e[i - 1].timestamp + 1;          // this event must go, and its second with it
    return S.lower_idx(*t_cut);
}

std::vector<Edge> gather(const Stream& S, Scope sc, size_t lo, size_t hi,
                         const std::vector<uint8_t>* flags, size_t reserve) {
    std::vector<Edge> out;
    out.reserve(reserve);
    for (size_t i = lo; i < hi; ++i) {
        const Edge& e = S.e[i];
        if (!sc(e)) continue;
        if (flags && !(*flags)[i]) continue;
        out.push_back(e);
    }
    return out;
}

struct Measure {
    size_t bytes = 0;
    double load_sec = 0;
    double recall = 0;                           // LANL: compromised-host recall; CADETS: chain recall
    double latency_us = 0;                       // forward taint from the first ground-truth event
    double trace_us = 0;                         // forward + backward (the E14 primitive; CADETS)
    size_t reached = 0;
};

Measure run_engine(const std::vector<Edge>& kept, const Truth& tr, const Stream& S,
                   uint64_t T_inv, int threads) {
    Measure m;
    GraphMeta meta = scan_meta(kept.data(), kept.size());
    GraphConfig cfg;
    cfg.t_min = meta.t_min; cfg.t_max = meta.t_max;
    cfg.num_vertices = std::max<uint32_t>(S.v_max + 1, tr.v_need);
    cfg.build_reverse_index = true;
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 30, kept.size() * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, kept.size() * sizeof(EdgeRef) * 8);
    const size_t anon0 = rss_anon_bytes();
    Barry b(cfg);
    double l0 = now_sec();
    b.insert_batch(kept.data(), kept.size(), threads);
    b.finalize();
    m.load_sec = now_sec() - l0;
    const size_t anon1 = rss_anon_bytes();
    Barry::Stats st = b.stats();
    if (st.edges_dropped) {
        std::fprintf(stderr, "[e34] FATAL: arena refused %zu edges\n", st.edges_dropped);
        std::exit(2);
    }
    m.bytes = anon1 > anon0 ? anon1 - anon0 : 0;

    std::unordered_set<uint32_t> reached;
    double q0 = now_sec();
    auto fwd = b.forward_taint(tr.seed, tr.t_seed, T_inv);
    m.latency_us = (now_sec() - q0) * 1e6;
    for (const auto& n : fwd) reached.insert(n.vertex);
    if (!tr.lanl) {
        // The investigation primitive is bidirectional (E14): forward from the
        // intrusion point, backward from every confirmed C2 flow.
        for (uint32_t nf : tr.nf_seeds) {
            auto back = b.backward_trace(nf, T_inv, meta.t_min);
            for (const auto& n : back) reached.insert(n.vertex);
        }
    }
    m.trace_us = (now_sec() - q0) * 1e6;
    m.reached = reached.size();
    size_t hit = 0;
    if (tr.lanl) {
        for (uint32_t v : tr.hosts) if (reached.count(v)) ++hit;
        m.recall = tr.hosts.empty() ? 0.0 : static_cast<double>(hit) / tr.hosts.size();
    } else {
        for (uint32_t v : tr.gt_ids) if (reached.count(v)) ++hit;
        m.recall = tr.gt_ids.empty() ? 0.0 : static_cast<double>(hit) / tr.gt_ids.size();
    }
    return m;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out; std::istringstream ss(s); std::string tok;
    while (std::getline(ss, tok, sep)) if (!tok.empty()) out.push_back(tok);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    Args args(argc, argv);
    std::string prefix   = args.get("graph");
    std::string corpus   = args.get("corpus", "lanl");
    std::string classes  = args.get("classes", corpus == "lanl" ? "1" : "");
    std::string attack   = args.get("attack", "1");
    std::string budgets  = args.get("budgets", "1,0.5,0.25,0.125,0.0625,0.03125,0.015625");
    std::string delays   = args.get("delays", "0,1,7,30");
    bool        verify   = args.has("verify");
    int         threads  = static_cast<int>(args.getl("threads", 16));
    double      max_set  = args.getd("max_setting_sec", 1200.0);
    std::string out_csv  = args.get("output", "");
    const bool lanl = (corpus == "lanl");
    const uint64_t tick = lanl ? 1ULL : 1000000000ULL;          // ticks per second
    const uint64_t day  = 86400ULL * tick;
    if (prefix.empty() || out_csv.empty()) {
        std::fprintf(stderr, "usage: %s --graph <prefix> --corpus lanl|cadets [--classes 1] "
                     "[--attack N] --budgets 1,0.5,... --delays 0,1,7,max [--verify] "
                     "--threads T --output csv\n", args.prog().c_str());
        return 1;
    }

    Scope sc;
    for (const std::string& c : split(classes, ',')) sc.cls_mask |= 1ULL << std::stoi(c);

    EdgeFile ef;
    if (!ef.load(prefix + ".bin")) return 1;

    // ---- ground truth -------------------------------------------------------
    Truth tr; tr.lanl = lanl;
    std::string label = basename_noext(prefix);
    if (lanl) {
        std::vector<RedTeamEvent> rt;
        if (!load_redteam(prefix + ".redteam.tsv", &rt)) {
            std::fprintf(stderr, "no resolved red team events\n"); return 1;
        }
        tr.seed = rt.front().src; tr.t_seed = rt.front().t; tr.t_last = rt.back().t;
        for (const RedTeamEvent& e : rt) {
            tr.edge_keys.insert(edge_key(e.src, e.dst, e.t));
            tr.triples.insert({e.src, e.dst, e.t});
            tr.hosts.insert(e.src); tr.hosts.insert(e.dst);
            tr.v_need = std::max<uint32_t>(tr.v_need, std::max(e.src, e.dst) + 1);
        }
    } else {
        std::ifstream f(prefix + ".seeds.tsv");
        std::string line; std::getline(f, line);
        while (std::getline(f, line)) {
            std::istringstream ss(line);
            std::string a, kind, match, nid, nkind, lab;
            std::getline(ss, a, '\t');    std::getline(ss, kind, '\t');
            std::getline(ss, match, '\t');std::getline(ss, nid, '\t');
            std::getline(ss, nkind, '\t');std::getline(ss, lab);
            if (a != attack) continue;
            uint32_t id = static_cast<uint32_t>(std::strtoul(nid.c_str(), nullptr, 10));
            tr.gt_ids.insert(id);
            tr.v_need = std::max<uint32_t>(tr.v_need, id + 1);
            if (nkind == "netflow" && tr.nf_nodes.insert(id).second) tr.nf_seeds.push_back(id);
        }
        if (tr.gt_ids.empty() || tr.nf_seeds.empty()) {
            std::fprintf(stderr, "no seeds for attack %s\n", attack.c_str()); return 1;
        }
        label += ".attack" + attack;
    }

    // ---- stream: in-scope, time-ordered -------------------------------------
    Stream S;
    S.e = ef.edges; S.n = ef.count;
    std::vector<Edge> sorted_copy;
    {
        size_t ooo = 0;
        for (size_t i = 1; i < ef.count; ++i)
            if (ef.edges[i].timestamp < ef.edges[i - 1].timestamp) ++ooo;
        if (ooo) {
            sorted_copy.reserve(ef.count);
            for (size_t i = 0; i < ef.count; ++i) if (sc(ef.edges[i])) sorted_copy.push_back(ef.edges[i]);
            std::stable_sort(sorted_copy.begin(), sorted_copy.end(),
                [](const Edge& a, const Edge& b) { return a.timestamp < b.timestamp; });
            S.e = sorted_copy.data(); S.n = sorted_copy.size();
            std::fprintf(stderr, "[e34] %zu out-of-order records: using a sorted copy\n", ooo);
        }
    }

    // One parallel pass: scope count, vertex range, time range, ground-truth
    // event indices.
    std::vector<size_t> gt_idx;
    {
        std::vector<std::vector<size_t>> gt_per(threads);
        std::vector<size_t> n_per(threads, 0);
        std::vector<uint32_t> v_per(threads, 0);
        run_threads(threads, [&](int t) {
            size_t lo = S.n * t / threads, hi = S.n * (t + 1) / threads;
            for (size_t i = lo; i < hi; ++i) {
                const Edge& e = S.e[i];
                if (!sc(e)) continue;
                ++n_per[t];
                v_per[t] = std::max(v_per[t], std::max(e.src, e.dst));
                if (tr.is_gt(e)) gt_per[t].push_back(i);
            }
        });
        for (int t = 0; t < threads; ++t) {
            S.n_scope += n_per[t]; S.v_max = std::max(S.v_max, v_per[t]);
            gt_idx.insert(gt_idx.end(), gt_per[t].begin(), gt_per[t].end());
        }
        S.t_min = S.e[0].timestamp; S.t_max = S.e[S.n - 1].timestamp;
    }
    if (!lanl) {
        // Seed = earliest event on any of the attack's netflow entities (E14);
        // the last ground-truth event is the last event on any of them.
        if (gt_idx.empty()) { std::fprintf(stderr, "no events on the attack's netflow nodes\n"); return 1; }
        const Edge& first = S.e[gt_idx.front()];
        tr.t_seed = first.timestamp;
        tr.seed = tr.nf_nodes.count(first.src) ? first.src : first.dst;
        tr.t_last = S.e[gt_idx.back()].timestamp;
    }
    const uint64_t span = S.t_max - S.t_min;
    std::fprintf(stderr,
        "[e34] %s: %zu events in scope of %zu, span %.2f days, %zu ground-truth events, "
        "seed %u @ %llu, last ground-truth event @ %llu (day %.2f)\n",
        label.c_str(), S.n_scope, S.n, static_cast<double>(span) / day, gt_idx.size(),
        tr.seed, (unsigned long long)tr.t_seed, (unsigned long long)tr.t_last,
        static_cast<double>(tr.t_last - S.t_min) / day);

    // ---- investigation times ------------------------------------------------
    struct Inv { double days; uint64_t T; size_t hi; std::string note; };
    std::vector<Inv> invs;
    for (const std::string& tok : split(delays, ',')) {
        double d;
        if (tok == "max") d = std::floor(static_cast<double>(S.t_max - tr.t_last) / day);
        else d = std::stod(tok);
        uint64_t T = tr.t_last + static_cast<uint64_t>(d * day);
        if (T > S.t_max) {
            std::fprintf(stderr, "[e34] delay %g days: T_inv is %.1f days past the end of the "
                         "corpus, skipped\n", d, static_cast<double>(T - S.t_max) / day);
            continue;
        }
        bool dup = false;
        for (const Inv& x : invs) if (x.T == T) dup = true;
        if (dup) continue;
        invs.push_back(Inv{d, T, S.upper_idx(T), ""});
    }
    std::sort(invs.begin(), invs.end(), [](const Inv& a, const Inv& b) { return a.T < b.T; });
    if (invs.empty()) { std::fprintf(stderr, "no investigation time fits the corpus\n"); return 1; }

    // ---- CPR window grid and the survivor counts for every (window, T_inv) --
    // Half-octave grid from one tick (1 s on LANL, 1 ns on CADETS) to the span.
    std::vector<uint64_t> W{0};
    const int k0 = static_cast<int>(std::ceil(-2.0 * std::log2(static_cast<double>(tick))));
    for (int k = k0;; ++k) {
        uint64_t w = static_cast<uint64_t>(std::llround(std::pow(2.0, k / 2.0) * tick));
        if (w < 1) continue;
        if (w > span) break;
        if (w != W.back()) W.push_back(w);
    }
    W.push_back(span + 1);                       // the ceiling: one event per pair, ever
    std::vector<size_t> hi_all;
    for (const Inv& x : invs) hi_all.push_back(x.hi);
    double c0 = now_sec();
    std::vector<uint64_t> cnt = cpr_counts(S, sc, W, hi_all, threads);
    std::fprintf(stderr, "[e34] CPR survivor counts for %zu windows x %zu investigation times: %.0f s\n",
                 W.size(), invs.size(), now_sec() - c0);
    const size_t D = invs.size();

    std::vector<double> budget_list;
    for (const std::string& b : split(budgets, ',')) budget_list.push_back(std::stod(b));

    CsvWriter w(out_csv,
        {"corpus","policy","budget_frac","delay_days","horizon_sec","cpr_window_sec",
         "events_kept","bytes_resident","gt_present","gt_kept","gt_survival","chain_recall",
         "latency_us","trace_latency_us","reached","budget_bytes","fit_loads","note","wall_sec"});

    std::vector<uint8_t> flags(S.n, 0);
    size_t flags_k = SIZE_MAX, flags_hi = SIZE_MAX;      // what `flags` currently holds
    auto ensure_flags = [&](size_t k, size_t hi) {
        if (flags_k == k && flags_hi == hi) return;
        cpr_flags(S, sc, W[k], hi, threads, flags);
        flags_k = k; flags_hi = hi;
    };
    double sec_per_event = 0;                    // worst load rate seen so far

    auto gt_count = [&](size_t lo, size_t hi, const std::vector<uint8_t>* fl, size_t* present) {
        std::set<GtId> p, k;
        for (size_t i : gt_idx) {
            if (i >= hi) break;
            const Edge& e = S.e[i];
            p.insert(gt_id(e));
            if (i >= lo && (!fl || (*fl)[i])) k.insert(gt_id(e));
        }
        *present = p.size();
        return k.size();
    };

    // A retained set: stream indices [lo, hi) of in-scope events, CPR-filtered
    // with window W[k] when k > 0.
    struct Sel { size_t lo = 0; uint64_t t_cut = 0; size_t k = 0; size_t events = 0; std::string note; };

    auto materialize = [&](const Sel& sel, const Inv& inv) {
        if (sel.k == 0) return gather(S, sc, sel.lo, inv.hi, nullptr, sel.events + 1);
        ensure_flags(sel.k, inv.hi);
        return gather(S, sc, sel.lo, inv.hi, &flags, sel.events + 1);
    };

    auto row = [&](const char* policy, double B, const Inv& inv, const Sel& sel, const Measure& m,
                   double budget_bytes, int loads, std::string note, double t_start, bool measured) {
        size_t present = 0;
        size_t gk = gt_count(sel.lo, inv.hi, sel.k ? (ensure_flags(sel.k, inv.hi), &flags) : nullptr,
                             &present);
        if (!sel.note.empty()) note = note.empty() ? sel.note : sel.note + ";" + note;
        const double horizon = static_cast<double>(inv.T - sel.t_cut) / tick;
        const double window  = sel.k ? static_cast<double>(W[sel.k]) / tick : 0.0;
        double wall = now_sec() - t_start;
        w.row(label, policy, B, inv.days, horizon, window, (unsigned long long)sel.events,
              (unsigned long long)(measured ? m.bytes : 0), (unsigned long long)present,
              (unsigned long long)gk, present ? static_cast<double>(gk) / present : 0.0,
              measured ? m.recall : 0.0, measured ? m.latency_us : 0.0,
              measured ? m.trace_us : 0.0, (unsigned long long)(measured ? m.reached : 0),
              budget_bytes, loads, note, wall);
        std::fprintf(stderr,
            "[e34] %-14s B=%-9g delay=%-5g  H=%7.2f d  w=%9.3g s  kept=%11zu  %6.3f/%6.3f GB  "
            "gt %zu/%zu (%5.1f%%)  recall %.3f  |T|=%zu  fwd %.0f us  loads %d  wall %.0f s%s%s\n",
            policy, B, inv.days, horizon * tick / day, window, sel.events,
            measured ? m.bytes / 1e9 : 0.0, budget_bytes / 1e9, gk, present,
            present ? 100.0 * gk / present : 0.0, measured ? m.recall : 0.0,
            measured ? m.reached : 0, measured ? m.latency_us : 0.0, loads, wall,
            note.empty() ? "" : "  ", note.c_str());
    };

    // ---- the lossless full corpus: the budget's unit, and the verification ---
    // Budget 1 is the whole corpus held losslessly; every budget is a fraction
    // of the resident bytes this load occupies. The same load must reproduce
    // E8 (LANL) and E14 (CADETS).
    double full_bytes = 0;
    {
        double t0 = now_sec();
        Inv full{static_cast<double>(S.t_max - tr.t_last) / day, S.t_max, S.n, "verify"};
        Sel sel; sel.t_cut = S.t_min; sel.events = S.n_scope;
        std::vector<Edge> kept = materialize(sel, full);
        Measure m = run_engine(kept, tr, S, full.T, threads);
        sec_per_event = m.load_sec / std::max<size_t>(1, kept.size());
        kept.clear(); kept.shrink_to_fit();
        full_bytes = static_cast<double>(m.bytes);
        row("lossless-short", 1.0, full, sel, m, full_bytes, 1, "verify", t0, true);
        row("reduced-long",   1.0, full, sel, m, full_bytes, 1, "verify", t0, true);
    }

    // ---- the sweep ----------------------------------------------------------
    // Each policy is fitted to the byte budget, not to an event count: a
    // CPR-reduced history spreads its events over many more (src,dst) pairs and
    // vertices than a short lossless window, so the two pay different bytes per
    // event. Start from the event-proportional guess, load, measure the
    // resident bytes, rescale, and keep the largest set that fits; at most
    // max_loads loads per setting.
    const int max_loads = static_cast<int>(args.getl("max_loads", 5));
    for (size_t d = 0; d < D; ++d) {
        const Inv& inv = invs[d];
        const size_t all_up_to = cnt[0 * D + d];   // in-scope events in [t_min, T_inv]

        auto sel_lossless = [&](size_t n) {
            Sel s;
            s.lo = fit_horizon(S, sc, nullptr, inv.hi, n, &s.t_cut);
            s.events = std::min(n, all_up_to);
            if (s.lo == 0) { s.t_cut = S.t_min; s.events = all_up_to; }
            else {
                size_t c = 0;
                for (size_t i = s.lo; i < inv.hi; ++i) c += sc(S.e[i]);
                s.events = c;
            }
            return s;
        };
        auto sel_reduced = [&](size_t n) {
            Sel s; s.t_cut = S.t_min;
            for (size_t j = 0; j < W.size(); ++j)
                if (cnt[j * D + d] <= n) {
                    s.k = j; s.events = cnt[j * D + d];
                    if (j + 1 == W.size()) s.note = "cpr-ceiling";
                    return s;
                }
            // Even one event per pair does not fit: ceiling CPR, then the
            // largest horizon over the reduced stream.
            s.k = W.size() - 1;
            ensure_flags(s.k, inv.hi);
            s.lo = fit_horizon(S, sc, &flags, inv.hi, n, &s.t_cut);
            size_t c = 0;
            for (size_t i = s.lo; i < inv.hi; ++i) c += sc(S.e[i]) && flags[i];
            s.events = c;
            s.note = "cpr-ceiling+horizon";
            return s;
        };

        for (double B : budget_list) {
            if (B >= 1.0 && d != 0) continue;    // budget 1 holds everything; once is enough
            const double budget_bytes = B * full_bytes;
            for (int pol = 0; pol < 2; ++pol) {
                const char* name = pol == 0 ? "lossless-short" : "reduced-long";
                double t0 = now_sec();
                size_t n = static_cast<size_t>(std::floor(B * S.n_scope));
                std::set<std::pair<size_t, size_t>> tried;
                bool have_best = false, have_any = false;
                Sel best, last; Measure bm, lm;
                int loads = 0;
                std::string skip;
                for (int it = 0; it < max_loads; ++it) {
                    Sel sel = pol == 0 ? sel_lossless(n) : sel_reduced(n);
                    if (!tried.insert({sel.lo, sel.k}).second) break;
                    double est = sec_per_event * sel.events;
                    if (est > max_set) {
                        skip = "skipped load: projected " + std::to_string(static_cast<int>(est)) +
                               " s exceeds the per-setting limit";
                        if (!have_any) { last = sel; }
                        break;
                    }
                    std::vector<Edge> kept = materialize(sel, inv);
                    Measure m = run_engine(kept, tr, S, inv.T, threads);
                    ++loads;
                    sec_per_event = std::max(sec_per_event, m.load_sec / std::max<size_t>(1, kept.size()));
                    kept.clear(); kept.shrink_to_fit();
                    const bool fits = m.bytes <= budget_bytes;
                    const bool whole = (sel.lo == 0 && sel.k == 0);
                    if (fits && (!have_best || sel.events > best.events)) { best = sel; bm = m; have_best = true; }
                    if (!have_best) { last = sel; lm = m; have_any = true; }
                    if (fits && (whole || m.bytes >= 0.95 * budget_bytes)) break;
                    const double r = budget_bytes / std::max<double>(1.0, static_cast<double>(m.bytes));
                    const size_t n2 = static_cast<size_t>(sel.events * r * (fits ? 0.99 : 0.98));
                    if (fits && n2 <= sel.events) break;
                    if (fits && have_best && n2 <= best.events) break;
                    n = n2;
                    if (n == 0) break;
                }
                if (have_best)
                    row(name, B, inv, best, bm, budget_bytes, loads, skip, t0, true);
                else if (have_any)
                    row(name, B, inv, last, lm, budget_bytes, loads,
                        "over budget after " + std::to_string(loads) + " loads" +
                        (skip.empty() ? "" : ";" + skip), t0, true);
                else
                    row(name, B, inv, last, lm, budget_bytes, loads, skip, t0, false);
            }
        }
    }
    (void)verify;
    return 0;
}
