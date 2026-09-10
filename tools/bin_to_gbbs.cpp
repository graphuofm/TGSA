// barry_bin_to_gbbs: convert our packed Edge .bin file to GBBS
// AdjacencyGraph text format expected by BYO/DCSR.
//
// GBBS unweighted adjacency-graph text format:
//   AdjacencyGraph
//   <n>            # number of vertices
//   <m>            # number of (directed or symmetrized) edges
//   <offset_0>
//   ...
//   <offset_{n-1}>
//   <dst_0>
//   ...
//   <dst_{m-1}>
//
// BYO/DCSR require **symmetric** input. We symmetrize by emitting both
// (u,v) and (v,u) for each input edge, deduping at the per-vertex level.
// Self-loops are dropped to match BYO conventions.
//
// Usage: barry_bin_to_gbbs <input.bin> <output.gbbs>

#include "barry/edge.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace barry;

static double now_sec() {
    using clk = std::chrono::high_resolution_clock;
    return std::chrono::duration<double>(clk::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <input.bin> <output.gbbs>\n", argv[0]);
        return 1;
    }
    const char* in_path  = argv[1];
    const char* out_path = argv[2];

    int fd = ::open(in_path, O_RDONLY);
    if (fd < 0) { std::perror("open input"); return 1; }
    struct stat st;
    if (::fstat(fd, &st) != 0) { std::perror("fstat"); return 1; }
    size_t bytes = static_cast<size_t>(st.st_size);
    if (bytes % sizeof(Edge) != 0) {
        std::fprintf(stderr, "bad size %zu (not multiple of %zu)\n",
                     bytes, sizeof(Edge));
        return 1;
    }
    void* mp = ::mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mp == MAP_FAILED) { std::perror("mmap"); return 1; }
    ::posix_madvise(mp, bytes, POSIX_MADV_SEQUENTIAL);
    const Edge* in = reinterpret_cast<const Edge*>(mp);
    size_t nE = bytes / sizeof(Edge);

    double t0 = now_sec();

    // Pass 1: find max vertex id and emit symmetrized (u,v) pairs (drop self-loops).
    uint32_t v_max = 0;
    std::vector<std::pair<uint32_t,uint32_t>> sym;
    sym.reserve(nE * 2);
    for (size_t i = 0; i < nE; ++i) {
        uint32_t u = in[i].src, v = in[i].dst;
        if (u > v_max) v_max = u;
        if (v > v_max) v_max = v;
        if (u == v) continue;          // drop self-loops per BYO convention
        sym.emplace_back(u, v);
        sym.emplace_back(v, u);
    }
    uint32_t N = v_max + 1;
    std::fprintf(stderr,
        "[gbbs] read %zu edges, V=%u, symmetrized=%zu in %.2fs\n",
        nE, N, sym.size(), now_sec() - t0);

    // Pass 2: sort symmetrized edges by (src, dst) and dedupe within (src, dst).
    double t1 = now_sec();
    std::sort(sym.begin(), sym.end());
    sym.erase(std::unique(sym.begin(), sym.end()), sym.end());
    size_t M = sym.size();
    std::fprintf(stderr, "[gbbs] sort+dedupe -> M=%zu in %.2fs\n",
                 M, now_sec() - t1);

    // Pass 3: build offsets array.
    double t2 = now_sec();
    std::vector<uint64_t> offsets(N + 1, 0);
    for (auto& p : sym) ++offsets[p.first + 1];
    for (uint32_t v = 0; v < N; ++v) offsets[v + 1] += offsets[v];
    // offsets[N] == M now.
    std::fprintf(stderr, "[gbbs] offsets built in %.2fs\n", now_sec() - t2);

    // Pass 4: write GBBS text.
    double t3 = now_sec();
    FILE* of = std::fopen(out_path, "w");
    if (!of) { std::perror("fopen output"); return 1; }
    // Bigger stdio buffer for faster writes.
    static char wbuf[1 << 20];
    std::setvbuf(of, wbuf, _IOFBF, sizeof wbuf);

    std::fprintf(of, "AdjacencyGraph\n%u\n%zu\n", N, M);
    for (uint32_t v = 0; v < N; ++v) std::fprintf(of, "%lu\n", (unsigned long)offsets[v]);
    for (size_t i = 0; i < M; ++i)   std::fprintf(of, "%u\n",  sym[i].second);

    std::fclose(of);
    std::fprintf(stderr, "[gbbs] wrote %s in %.2fs\n",
                 out_path, now_sec() - t3);

    ::munmap(mp, bytes);
    ::close(fd);

    std::fprintf(stderr, "[gbbs] total %.2fs (V=%u M=%zu)\n",
                 now_sec() - t0, N, M);
    return 0;
}
