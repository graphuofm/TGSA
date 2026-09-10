// barry_demo: load a packed Edge binary, ingest into Barry, time it, and run a
// few sanity queries. Used for quick end-to-end smoke tests on real data.
//
// Usage: barry_demo <input.bin> [num_threads]

#include "barry/edge.hpp"
#include "barry/engine.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace barry;

static double now_sec() {
    using clk = std::chrono::high_resolution_clock;
    return std::chrono::duration<double>(clk::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: %s <input.bin> [num_threads]\n", argv[0]);
        return 1;
    }
    int num_threads = (argc == 3) ? std::atoi(argv[2]) : 0;

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { std::perror("open"); return 1; }
    struct stat st;
    if (fstat(fd, &st) != 0) { std::perror("fstat"); return 1; }
    size_t bytes = static_cast<size_t>(st.st_size);
    if (bytes % sizeof(Edge) != 0) {
        std::fprintf(stderr, "bad file size\n"); return 1;
    }
    void* mp = mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mp == MAP_FAILED) { std::perror("mmap"); return 1; }
    posix_madvise(mp, bytes, POSIX_MADV_SEQUENTIAL);

    const Edge* edges = reinterpret_cast<const Edge*>(mp);
    size_t n = bytes / sizeof(Edge);

    // Scan once for t range and vertex count.
    uint64_t t_min = ULLONG_MAX, t_max = 0;
    uint32_t v_max = 0;
    for (size_t i = 0; i < n; ++i) {
        if (edges[i].timestamp < t_min) t_min = edges[i].timestamp;
        if (edges[i].timestamp > t_max) t_max = edges[i].timestamp;
        if (edges[i].src > v_max) v_max = edges[i].src;
        if (edges[i].dst > v_max) v_max = edges[i].dst;
    }

    GraphConfig cfg;
    cfg.t_min = t_min;
    cfg.t_max = t_max;
    cfg.shift = 0;  // auto
    cfg.timeline_slots = 0;  // auto
    cfg.num_vertices = v_max + 1;
    cfg.arena_reserve_bytes = std::max<size_t>(1ULL << 30,
                                               n * sizeof(Edge) * 4);
    cfg.vertex_arena_reserve_bytes = std::max<size_t>(1ULL << 28,
                                                      n * sizeof(EdgeRef) * 4);
    cfg.default_block_capacity = 16;

    std::fprintf(stderr,
        "[demo] %zu edges, V=%u, t in [%lu, %lu]\n",
        n, cfg.num_vertices, (unsigned long)t_min, (unsigned long)t_max);

    Barry b(cfg);

    double t0 = now_sec();
    b.insert_batch(edges, n, num_threads);
    double t1 = now_sec();

    auto s = b.stats();
    double secs = t1 - t0;
    double Mes  = (n / 1e6) / secs;
    std::fprintf(stderr,
        "[demo] ingested %zu edges in %.3fs => %.2f M edges/s (threads=%d)\n",
        n, secs, Mes, num_threads);
    std::fprintf(stderr,
        "[demo] blocks=%zu  arena_used=%.2f MB  vertex_idx=%.2f MB  RSS=%.2f MB\n",
        s.num_blocks,
        s.bytes_arena_used     / 1e6,
        s.bytes_vertex_index   / 1e6,
        s.bytes_rss            / 1e6);
    std::fprintf(stderr,
        "[demo] timeline virtual=%.2f GB (slots=%zu)\n",
        s.bytes_timeline_virtual / 1e9,
        s.bytes_timeline_virtual / sizeof(void*));

    // A couple of probe queries against the middle of the time range.
    uint64_t mid = t_min + (t_max - t_min) / 2;
    uint64_t win = std::max<uint64_t>(1, (t_max - t_min) / 100);

    double q0 = now_sec();
    auto rng = b.query_range(mid, mid + win);
    double q1 = now_sec();
    std::fprintf(stderr,
        "[demo] query_range(mid, mid+1%%): %zu edges in %.3f ms\n",
        rng.size(), (q1 - q0) * 1e3);

    if (b.vertex_index() && cfg.num_vertices >= 2) {
        // Pick the highest-degree vertex among the first 4096 by quick scan.
        uint32_t best = 0, best_d = 0;
        for (uint32_t v = 0; v < std::min<uint32_t>(4096, cfg.num_vertices); ++v) {
            uint32_t d = b.vertex_index()->out_degree(v);
            if (d > best_d) { best_d = d; best = v; }
        }
        double b0 = now_sec();
        auto level = b.bfs(best);
        double b1 = now_sec();
        size_t reached = 0;
        for (auto l : level) if (l >= 0) ++reached;
        std::fprintf(stderr,
            "[demo] bfs(v=%u, deg=%u): %zu reached in %.3f ms\n",
            best, best_d, reached, (b1 - b0) * 1e3);
    }

    munmap(mp, bytes);
    close(fd);
    return 0;
}
