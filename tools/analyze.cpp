// barry_analyze: print statistics for a packed Edge binary, and recommend a
// shift parameter for the temporal mapper.
//
// Usage: barry_analyze <input.bin>

#include "barry/edge.hpp"
#include "barry/time_mapper.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>

using namespace barry;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <input.bin>\n", argv[0]);
        return 1;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { std::perror("open"); return 1; }
    struct stat st;
    if (fstat(fd, &st) != 0) { std::perror("fstat"); return 1; }
    size_t bytes = static_cast<size_t>(st.st_size);
    if (bytes % sizeof(Edge) != 0) {
        std::fprintf(stderr, "bad file size: %zu (not multiple of %zu)\n",
                     bytes, sizeof(Edge));
        return 1;
    }

    void* mp = mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mp == MAP_FAILED) { std::perror("mmap"); return 1; }
    posix_madvise(mp, bytes, POSIX_MADV_SEQUENTIAL);

    const Edge* edges = reinterpret_cast<const Edge*>(mp);
    size_t n = bytes / sizeof(Edge);
    if (n == 0) {
        std::printf("empty file\n");
        return 0;
    }

    uint64_t t_min = edges[0].timestamp, t_max = edges[0].timestamp;
    bool sorted = true;
    std::unordered_set<uint32_t> verts;
    verts.reserve(n / 4);
    for (size_t i = 0; i < n; ++i) {
        if (edges[i].timestamp < t_min) t_min = edges[i].timestamp;
        if (edges[i].timestamp > t_max) t_max = edges[i].timestamp;
        if (i && edges[i].timestamp < edges[i-1].timestamp) sorted = false;
        verts.insert(edges[i].src);
        verts.insert(edges[i].dst);
    }

    uint32_t shift = TimeMapper::auto_select_shift_from_stats(n, t_min, t_max);

    std::printf("file              : %s\n", argv[1]);
    std::printf("num_edges         : %zu\n", n);
    std::printf("num_vertices(=set): %zu\n", verts.size());
    std::printf("t_min             : %lu\n", (unsigned long)t_min);
    std::printf("t_max             : %lu\n", (unsigned long)t_max);
    std::printf("t_span            : %lu\n", (unsigned long)(t_max - t_min));
    std::printf("sorted_by_ts      : %s\n", sorted ? "yes" : "no");
    std::printf("recommended_shift : %u (window=%lu)\n",
                shift, (unsigned long)(1ULL << shift));
    std::printf("file_size_MB      : %.2f\n", bytes / 1e6);

    munmap(mp, bytes);
    close(fd);
    return 0;
}
