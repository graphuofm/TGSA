// Run a labelled provenance query against a Barry graph built from a .bin plus
// the .nodes.tsv dictionary emitted by barry_tc_cdm_to_bin.
//
// This is the case-study driver: it prints WHICH entities were reached, not just
// how many, so an attack chain can be read off directly.
//
// Usage:
//   barry_provenance_query --graph <prefix> --query <forward|backward>
//                          --seed <id|substring> [--at <ns|min|max>]
//                          [--window <ns>] [--limit N]

#include "barry/engine.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace barry;

struct NodeInfo { std::string kind, label; };

static bool load_nodes(const std::string& path, std::vector<NodeInfo>* out) {
    std::ifstream f(path);
    if (!f) { std::perror(path.c_str()); return false; }
    std::string line;
    std::getline(f, line);                       // header
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string id, kind, label;
        std::getline(ss, id, '\t');
        std::getline(ss, kind, '\t');
        std::getline(ss, label);
        size_t idx = std::strtoul(id.c_str(), nullptr, 10);
        if (out->size() <= idx) out->resize(idx + 1);
        (*out)[idx] = NodeInfo{kind, label};
    }
    return true;
}

int main(int argc, char** argv) {
    std::string prefix, query = "forward", seed_arg, at_arg = "min";
    uint64_t window = 0;
    size_t limit = 40;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--graph"  && i + 1 < argc) prefix   = argv[++i];
        else if (a == "--query"  && i + 1 < argc) query    = argv[++i];
        else if (a == "--seed"   && i + 1 < argc) seed_arg = argv[++i];
        else if (a == "--at"     && i + 1 < argc) at_arg   = argv[++i];
        else if (a == "--window" && i + 1 < argc) window   = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--limit"  && i + 1 < argc) limit    = std::strtoul(argv[++i], nullptr, 10);
    }
    if (prefix.empty() || seed_arg.empty()) {
        std::fprintf(stderr, "usage: %s --graph <prefix> --query <forward|backward> "
                     "--seed <id|substring> [--at <ns|min|max>] [--window <ns>] [--limit N]\n", argv[0]);
        return 1;
    }

    // ---- load edges --------------------------------------------------------
    std::string bin = prefix + ".bin";
    int fd = ::open(bin.c_str(), O_RDONLY);
    if (fd < 0) { std::perror(bin.c_str()); return 1; }
    struct stat st{};
    if (::fstat(fd, &st) != 0) { std::perror("fstat"); return 1; }
    size_t bytes = static_cast<size_t>(st.st_size);
    void* addr = ::mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED) { std::perror("mmap"); return 1; }
    const Edge* edges = reinterpret_cast<const Edge*>(addr);
    size_t n = bytes / sizeof(Edge);

    std::vector<NodeInfo> nodes;
    load_nodes(prefix + ".nodes.tsv", &nodes);

    uint64_t t_min = ULLONG_MAX, t_max = 0;
    uint32_t v_max = 0;
    for (size_t i = 0; i < n; ++i) {
        t_min = std::min(t_min, edges[i].timestamp);
        t_max = std::max(t_max, edges[i].timestamp);
        v_max = std::max({v_max, edges[i].src, edges[i].dst});
    }

    GraphConfig cfg;
    cfg.t_min = t_min; cfg.t_max = t_max;
    cfg.num_vertices = v_max + 1;
    cfg.build_reverse_index = true;
    cfg.arena_reserve_bytes        = std::max<size_t>(1ULL << 28, n * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28, n * sizeof(EdgeRef) * 8);
    Barry b(cfg);
    b.insert_batch(edges, n, 0);

    // ---- resolve the seed --------------------------------------------------
    uint32_t seed = UINT32_MAX;
    bool numeric = !seed_arg.empty() &&
                   seed_arg.find_first_not_of("0123456789") == std::string::npos;
    if (numeric) {
        seed = static_cast<uint32_t>(std::strtoul(seed_arg.c_str(), nullptr, 10));
    } else {
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (!nodes[i].label.empty() && nodes[i].label.find(seed_arg) != std::string::npos) {
                seed = static_cast<uint32_t>(i);
                break;
            }
        }
        if (seed == UINT32_MAX) {
            std::fprintf(stderr, "no node label contains \"%s\"\n", seed_arg.c_str());
            return 2;
        }
    }

    uint64_t at = (at_arg == "min") ? t_min : (at_arg == "max") ? t_max
                                            : std::strtoull(at_arg.c_str(), nullptr, 10);
    if (window == 0) window = t_max - t_min;

    auto describe = [&](uint32_t v) {
        if (v < nodes.size() && !nodes[v].label.empty())
            return nodes[v].kind + "  " + nodes[v].label;
        if (v < nodes.size() && !nodes[v].kind.empty())
            return nodes[v].kind + "  <unnamed>";
        return std::string("unknown  <unnamed>");
    };

    std::vector<Barry::TaintNode> res;
    const char* dir;
    if (query == "backward") {
        uint64_t lo = (at > t_min + window) ? at - window : t_min;
        res = b.backward_trace(seed, at, lo);
        dir = "backward causality trace (ancestors)";
    } else {
        uint64_t hi = std::min(t_max, at + window);
        res = b.forward_taint(seed, at, hi);
        dir = "forward taint (impact set)";
    }

    std::printf("graph      : %s  (%zu edges, %u vertices)\n", prefix.c_str(), n, cfg.num_vertices);
    std::printf("query      : %s\n", dir);
    std::printf("seed       : [%u] %s\n", seed, describe(seed).c_str());
    std::printf("anchor time: %llu\n", (unsigned long long)at);
    std::printf("result     : %zu entities (%.4f%% of graph)\n\n",
                res.size(), 100.0 * res.size() / cfg.num_vertices);

    std::sort(res.begin(), res.end(),
              [&](const Barry::TaintNode& a, const Barry::TaintNode& c) {
                  return query == "backward" ? a.time > c.time : a.time < c.time;
              });
    size_t shown = std::min(limit, res.size());
    for (size_t i = 0; i < shown; ++i) {
        std::printf("  %+14lld ns   [%7u]  %s\n",
                    (long long)(res[i].time - t_min), res[i].vertex,
                    describe(res[i].vertex).c_str());
    }
    if (res.size() > shown) std::printf("  ... %zu more\n", res.size() - shown);

    ::munmap(addr, bytes);
    ::close(fd);
    return 0;
}
