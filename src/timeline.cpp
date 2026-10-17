#include "barry/timeline.hpp"
#include <sys/mman.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace barry {

static void* try_mmap(size_t bytes, bool huge) {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    if (huge) flags |= MAP_HUGETLB;
    return mmap(nullptr, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
}

Timeline::Timeline(size_t n, bool huge_pages)
    : slots_(nullptr), num_slots_(n), bytes_(0), huge_pages_(huge_pages) {

    bytes_ = n * sizeof(std::atomic<BlockHeader*>);
    // CRITICAL: never MAP_POPULATE — sparsity is the whole point.
    void* p = try_mmap(bytes_, huge_pages_);
    if (p == MAP_FAILED && huge_pages_) {
        std::fprintf(stderr, "[Timeline] huge pages mmap failed (%s), falling back\n",
                     std::strerror(errno));
        huge_pages_ = false;
        p = try_mmap(bytes_, false);
    }
    if (p == MAP_FAILED) {
        std::perror("[Timeline] mmap");
        std::abort();
    }
    slots_ = reinterpret_cast<std::atomic<BlockHeader*>*>(p);
    std::fprintf(stderr, "[Timeline] %zu slots = %.2f GB virtual (huge=%d)\n",
                 n, bytes_ / 1e9, huge_pages_ ? 1 : 0);
}

Timeline::~Timeline() {
    if (slots_) munmap(slots_, bytes_);
}

size_t Timeline::process_rss_bytes() {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    size_t kb = 0;
    while (std::fgets(line, sizeof line, f)) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            std::sscanf(line + 6, "%zu", &kb);
            break;
        }
    }
    std::fclose(f);
    return kb * 1024;
}

}  // namespace barry
