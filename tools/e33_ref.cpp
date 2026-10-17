// E33 reference: the query list and the ground truth for q1..q4, computed
// straight from the input arrays with no engine in the loop (docs/E33_SPEC.md).
//
// Writes <outdir>/<basename>.queries (N records of three little-endian u64
// {v, lo, hi}, the same bytes bench_e33_matrix's tgsr run writes) and
// <outdir>/<basename>.ref (one line per phase: `phase count checksum`).
// An existing file is never overwritten: it is read back and compared, so the
// tool doubles as a cross-check of a .queries/.ref another harness produced.
//
// Build (header-only, no engine link):
//   g++ -O2 -std=c++17 -I include -I benchmark tools/e33_ref.cpp -o build/e33_ref
// Usage:
//   build/e33_ref --dataset /home/jding/dataset/bin/<name>.bin --outdir results/e33_matrix
//                 [--samples 1000] [--phases q1,q2,q3,q4]

#include "common.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using barry::Edge;
using barry::bench::Args;
using barry::bench::EdgeFile;

namespace {

struct Query { uint64_t v, lo, hi; };
struct Result { uint64_t count = 0, chk = 0; };

// Out-adjacency in input order (stable counting sort by src), so every
// per-vertex phase walks exactly the event multiset the engines hold.
struct Csr {
    std::vector<uint64_t> off;       // V+1
    std::vector<uint32_t> dst;
    std::vector<uint64_t> ts;
    void build(const Edge* e, size_t n, uint32_t v_max) {
        const size_t V = (size_t)v_max + 1;
        off.assign(V + 1, 0);
        for (size_t i = 0; i < n; ++i) ++off[e[i].src + 1];
        for (size_t v = 0; v < V; ++v) off[v + 1] += off[v];
        dst.resize(n); ts.resize(n);
        std::vector<uint64_t> pos(off.begin(), off.end() - 1);
        for (size_t i = 0; i < n; ++i) {
            const uint64_t p = pos[e[i].src]++;
            dst[p] = e[i].dst; ts[p] = e[i].timestamp;
        }
    }
};

Result q1_scan(const Edge* e, size_t n, uint64_t lo, uint64_t hi) {
    Result r;
    for (size_t i = 0; i < n; ++i) {
        const uint64_t t = e[i].timestamp;
        if (t >= lo && t <= hi) {
            r.chk += t ^ ((uint64_t)e[i].dst << 32) ^ (uint64_t)e[i].src;
            ++r.count;
        }
    }
    return r;
}

Result q2_window(const Csr& g, const std::vector<Query>& qs) {
    Result r;
    for (const Query& q : qs) {
        for (uint64_t p = g.off[q.v]; p < g.off[q.v + 1]; ++p) {
            if (g.ts[p] >= q.lo && g.ts[p] <= q.hi) {
                r.chk += g.ts[p] ^ (uint64_t)g.dst[p];
                ++r.count;
            }
        }
    }
    return r;
}

// Plain label-correcting search with a FIFO worklist, as the spec words it.
// GraphOne's job 35 pops in increasing-label order instead; the fixpoint is
// the same, so agreement between the two is also a check of that claim.
Result q3_reach(const Csr& g, const std::vector<Query>& qs) {
    Result r;
    std::unordered_map<uint64_t, uint64_t> a;
    std::deque<uint64_t> work;
    for (const Query& q : qs) {
        a.clear();
        a.emplace(q.v, q.lo);
        work.clear();
        work.push_back(q.v);
        while (!work.empty()) {
            const uint64_t u = work.front(); work.pop_front();
            const uint64_t au = a[u];
            for (uint64_t p = g.off[u]; p < g.off[u + 1]; ++p) {
                const uint64_t t = g.ts[p];
                if (t < au || t > q.hi) continue;
                const uint64_t d = g.dst[p];
                auto it = a.find(d);
                if (it == a.end()) { a.emplace(d, t); work.push_back(d); }
                else if (t < it->second) { it->second = t; work.push_back(d); }
            }
        }
        for (const auto& kv : a) {
            if (kv.first == q.v) continue;
            ++r.count;
            r.chk += kv.first ^ kv.second;
        }
    }
    return r;
}

Result q4_asof(const Csr& g, const std::vector<Query>& qs) {
    Result r;
    std::unordered_map<uint64_t, uint64_t> latest;
    for (const Query& q : qs) {
        latest.clear();
        for (uint64_t p = g.off[q.v]; p < g.off[q.v + 1]; ++p) {
            if (g.ts[p] > q.hi) continue;
            auto it = latest.find(g.dst[p]);
            if (it == latest.end()) latest.emplace(g.dst[p], g.ts[p]);
            else if (g.ts[p] > it->second) it->second = g.ts[p];
        }
        r.count += latest.size();
        for (const auto& kv : latest) r.chk += (kv.first << 20) ^ kv.second ^ q.v;
    }
    return r;
}

std::string basename_no_ext(const std::string& path) {
    std::string b = path;
    auto slash = b.find_last_of('/');
    if (slash != std::string::npos) b = b.substr(slash + 1);
    if (b.size() > 4 && b.compare(b.size() - 4, 4, ".bin") == 0) b.resize(b.size() - 4);
    return b;
}

bool file_exists(const std::string& p) { FILE* f = std::fopen(p.c_str(), "rb"); if (f) std::fclose(f); return f != nullptr; }

}  // namespace

int main(int argc, char** argv) {
    Args args(argc, argv);
    const std::string dataset = args.get("dataset");
    const std::string outdir  = args.get("outdir", "results/e33_matrix");
    const size_t samples      = (size_t)args.getl("samples", 1000);
    const std::string phases  = args.get("phases", "q1,q2,q3,q4");
    if (dataset.empty()) {
        std::fprintf(stderr, "usage: %s --dataset <bin> [--outdir dir] [--samples N] [--phases q1,q2,q3,q4]\n", argv[0]);
        return 1;
    }
    auto want = [&](const char* p) { return phases.find(p) != std::string::npos; };

    EdgeFile ef;
    if (!ef.load(dataset)) return 1;
    const Edge* e = ef.edges;
    const size_t n = ef.count;
    if (n == 0) { std::fprintf(stderr, "empty dataset\n"); return 1; }
    const auto meta = barry::bench::scan_meta(e, n);
    uint64_t w = (meta.t_max - meta.t_min) / 100;
    if (w < 1) w = 1;
    std::fprintf(stderr, "[e33_ref] %s: %zu events, V=%u, t_min=%llu t_max=%llu w=%llu\n",
                 dataset.c_str(), n, meta.v_max + 1,
                 (unsigned long long)meta.t_min, (unsigned long long)meta.t_max,
                 (unsigned long long)w);

    // -------------------------------------------------------------- queries
    std::vector<Query> qs(samples);
    {
        std::mt19937_64 rng(0xE33ull);
        for (size_t i = 0; i < samples; ++i) {
            const size_t idx = std::uniform_int_distribution<size_t>(0, n - 1)(rng);
            qs[i].v = e[idx].src;
            qs[i].lo = e[idx].timestamp;
            qs[i].hi = qs[i].lo + w;
        }
    }
    const std::string base = basename_no_ext(dataset);
    const std::string qpath = outdir + "/" + base + ".queries";
    const std::string rpath = outdir + "/" + base + ".ref";
    int rc = 0;
    if (file_exists(qpath)) {
        std::vector<Query> have;
        FILE* f = std::fopen(qpath.c_str(), "rb");
        Query q;
        while (std::fread(&q, sizeof(q), 1, f) == 1) have.push_back(q);
        std::fclose(f);
        size_t diff = 0;
        for (size_t i = 0; i < std::max(have.size(), qs.size()); ++i) {
            if (i >= have.size() || i >= qs.size() ||
                have[i].v != qs[i].v || have[i].lo != qs[i].lo || have[i].hi != qs[i].hi) ++diff;
        }
        if (diff == 0) {
            std::fprintf(stderr, "[e33_ref] %s exists and matches (%zu queries)\n", qpath.c_str(), have.size());
        } else {
            std::fprintf(stderr, "[e33_ref] MISMATCH: %s exists with %zu queries, %zu differ from the spec RNG; "
                                 "ground truth below is computed for the EXISTING file\n",
                         qpath.c_str(), have.size(), diff);
            qs = have;
            rc = 2;
        }
    } else {
        FILE* f = std::fopen(qpath.c_str(), "wb");
        if (!f) { std::perror(qpath.c_str()); return 1; }
        for (const Query& q : qs) {
            const uint64_t rec[3] = {q.v, q.lo, q.hi};
            if (std::fwrite(rec, sizeof(rec), 1, f) != 1) { std::perror("fwrite"); return 1; }
        }
        std::fclose(f);
        std::fprintf(stderr, "[e33_ref] wrote %zu queries to %s\n", qs.size(), qpath.c_str());
    }

    // --------------------------------------------------------- ground truth
    Csr g;
    g.build(e, n, meta.v_max);
    std::vector<std::pair<std::string, Result>> rows;
    if (want("q1")) rows.emplace_back("q1", q1_scan(e, n, meta.t_max - w, meta.t_max));
    if (want("q2")) rows.emplace_back("q2", q2_window(g, qs));
    if (want("q3")) rows.emplace_back("q3", q3_reach(g, qs));
    if (want("q4")) rows.emplace_back("q4", q4_asof(g, qs));
    for (const auto& r : rows)
        std::printf("%s %llu %llu\n", r.first.c_str(),
                    (unsigned long long)r.second.count, (unsigned long long)r.second.chk);

    if (file_exists(rpath)) {
        std::unordered_map<std::string, Result> have;
        FILE* f = std::fopen(rpath.c_str(), "r");
        char ph[64]; unsigned long long c, k;
        while (std::fscanf(f, "%63s %llu %llu", ph, &c, &k) == 3) { Result r; r.count = c; r.chk = k; have[ph] = r; }
        std::fclose(f);
        for (const auto& r : rows) {
            auto it = have.find(r.first);
            if (it == have.end()) { std::fprintf(stderr, "[e33_ref] %s: no %s line in %s\n", base.c_str(), r.first.c_str(), rpath.c_str()); continue; }
            const bool ok = it->second.count == r.second.count && it->second.chk == r.second.chk;
            std::fprintf(stderr, "[e33_ref] %s %s: %s (ref %llu %llu)\n", base.c_str(), r.first.c_str(),
                         ok ? "MATCH" : "MISMATCH",
                         (unsigned long long)it->second.count, (unsigned long long)it->second.chk);
            if (!ok) rc = 2;
        }
    } else {
        FILE* f = std::fopen(rpath.c_str(), "w");
        if (!f) { std::perror(rpath.c_str()); return 1; }
        for (const auto& r : rows)
            std::fprintf(f, "%s %llu %llu\n", r.first.c_str(),
                         (unsigned long long)r.second.count, (unsigned long long)r.second.chk);
        std::fclose(f);
        std::fprintf(stderr, "[e33_ref] wrote %s\n", rpath.c_str());
    }
    return rc;
}
