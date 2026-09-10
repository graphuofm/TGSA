// barry_preprocess: convert a whitespace-separated "src dst ts [w]" text graph
// into a packed binary of barry::Edge records, sorted by timestamp.
//
// Usage:
//   barry_preprocess <input.txt> <output.bin>
//
// Outputs:
//   - <output.bin>            packed Edge[]
//   - <output.bin>.meta.json  basic stats
//
// Notes:
//   - Vertex IDs are remapped to a contiguous [0, V) range using insertion order.
//   - Lines starting with '#' or '%' are ignored.
//   - Timestamps may be float; they are floored to int64.

#include "barry/edge.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

using namespace barry;

namespace {

struct ParsedEdge {
    uint32_t src_orig;
    uint32_t dst_orig;
    uint64_t ts;
    float    weight;
};

// Skip whitespace & comments. Returns false at EOF.
inline bool advance_to_next_line(const char*& p, const char* end) {
    while (p < end && *p != '\n') ++p;
    if (p < end) ++p;
    return p < end;
}

inline const char* skip_ws(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    return p;
}

inline bool parse_uint64(const char*& p, const char* end, uint64_t& out) {
    p = skip_ws(p, end);
    if (p >= end) return false;
    bool neg = false;
    if (*p == '-') { neg = true; ++p; }
    if (p >= end || *p < '0' || *p > '9') return false;
    uint64_t v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10 + uint64_t(*p - '0');
        ++p;
    }
    // tolerate float timestamps: skip ".\d+"
    if (p < end && *p == '.') {
        ++p;
        while (p < end && *p >= '0' && *p <= '9') ++p;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        ++p;
        if (p < end && (*p == '+' || *p == '-')) ++p;
        while (p < end && *p >= '0' && *p <= '9') ++p;
    }
    out = neg ? uint64_t(0) : v;
    return true;
}

inline bool parse_uint32(const char*& p, const char* end, uint32_t& out) {
    uint64_t v;
    if (!parse_uint64(p, end, v)) return false;
    out = static_cast<uint32_t>(v);
    return true;
}

inline bool parse_float(const char*& p, const char* end, float& out) {
    p = skip_ws(p, end);
    if (p >= end || *p == '\n' || *p == '\r') { out = 1.0f; return true; }
    // Use strtof — tail-friendly enough.
    char* eptr = nullptr;
    out = std::strtof(p, &eptr);
    if (eptr == p) { out = 1.0f; return true; }
    p = eptr;
    return true;
}

double now_sec() {
    using clk = std::chrono::high_resolution_clock;
    return std::chrono::duration<double>(clk::now().time_since_epoch()).count();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <input.txt> <output.bin>\n", argv[0]);
        return 1;
    }
    const char* in_path  = argv[1];
    const char* out_path = argv[2];

    int fd = open(in_path, O_RDONLY);
    if (fd < 0) { std::perror("open input"); return 1; }
    struct stat st;
    if (fstat(fd, &st) != 0) { std::perror("fstat"); return 1; }
    size_t bytes = static_cast<size_t>(st.st_size);
    void* mp = mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mp == MAP_FAILED) { std::perror("mmap"); return 1; }
    posix_madvise(mp, bytes, POSIX_MADV_SEQUENTIAL);

    const char* p   = static_cast<const char*>(mp);
    const char* end = p + bytes;

    std::unordered_map<uint32_t, uint32_t> id_map;
    id_map.reserve(1u << 20);
    std::vector<ParsedEdge> edges;
    edges.reserve(1u << 20);

    double t0 = now_sec();
    size_t line_no = 0, skipped = 0;

    while (p < end) {
        // Comments / blanks
        if (*p == '#' || *p == '%' || *p == '\n' || *p == '\r') {
            advance_to_next_line(p, end);
            ++line_no;
            continue;
        }

        uint32_t src_orig, dst_orig;
        uint64_t ts;
        float    w;
        if (!parse_uint32(p, end, src_orig) ||
            !parse_uint32(p, end, dst_orig) ||
            !parse_uint64(p, end, ts)) {
            ++skipped;
            advance_to_next_line(p, end);
            ++line_no;
            continue;
        }
        parse_float(p, end, w);

        edges.push_back({src_orig, dst_orig, ts, w});
        advance_to_next_line(p, end);
        ++line_no;

        if ((edges.size() & ((1u << 20) - 1)) == 0) {
            std::fprintf(stderr, "  parsed %zu lines, %zu edges...\r",
                         line_no, edges.size());
            std::fflush(stderr);
        }
    }
    std::fprintf(stderr, "\n[parse] %zu edges from %zu lines (%zu skipped) in %.2fs\n",
                 edges.size(), line_no, skipped, now_sec() - t0);

    munmap(mp, bytes);
    close(fd);

    // ID remap (insertion order preserves something close to original locality).
    double t1 = now_sec();
    auto remap = [&](uint32_t orig) {
        auto it = id_map.find(orig);
        if (it != id_map.end()) return it->second;
        uint32_t nid = static_cast<uint32_t>(id_map.size());
        id_map.emplace(orig, nid);
        return nid;
    };

    std::vector<Edge> out;
    out.reserve(edges.size());
    uint64_t t_min = UINT64_MAX, t_max = 0;
    for (const auto& pe : edges) {
        Edge e;
        e.src       = remap(pe.src_orig);
        e.dst       = remap(pe.dst_orig);
        e.timestamp = pe.ts;
        e.weight    = pe.weight;
        out.push_back(e);
        t_min = std::min(t_min, pe.ts);
        t_max = std::max(t_max, pe.ts);
    }
    std::fprintf(stderr, "[remap] V=%zu, t in [%lu, %lu] in %.2fs\n",
                 id_map.size(),
                 (unsigned long)(out.empty() ? 0 : t_min),
                 (unsigned long)t_max, now_sec() - t1);

    // Sort by timestamp. (Stable to keep insertion order within ties.)
    double t2 = now_sec();
    std::stable_sort(out.begin(), out.end(),
        [](const Edge& a, const Edge& b) { return a.timestamp < b.timestamp; });
    std::fprintf(stderr, "[sort]  done in %.2fs\n", now_sec() - t2);

    // Write packed binary.
    double t3 = now_sec();
    FILE* of = std::fopen(out_path, "wb");
    if (!of) { std::perror("fopen output"); return 1; }
    if (!out.empty()) {
        size_t want = out.size() * sizeof(Edge);
        size_t got  = std::fwrite(out.data(), 1, want, of);
        if (got != want) {
            std::fprintf(stderr, "[write] short write %zu/%zu\n", got, want);
            std::fclose(of);
            return 1;
        }
    }
    std::fclose(of);
    std::fprintf(stderr, "[write] %zu edges (%.2f MB) in %.2fs\n",
                 out.size(), (out.size() * sizeof(Edge)) / 1e6, now_sec() - t3);

    // Sidecar JSON.
    char meta_path[4096];
    std::snprintf(meta_path, sizeof meta_path, "%s.meta.json", out_path);
    FILE* mf = std::fopen(meta_path, "w");
    if (mf) {
        std::fprintf(mf,
            "{\n"
            "  \"num_edges\": %zu,\n"
            "  \"num_vertices\": %zu,\n"
            "  \"t_min\": %lu,\n"
            "  \"t_max\": %lu,\n"
            "  \"edge_size_bytes\": %zu\n"
            "}\n",
            out.size(),
            id_map.size(),
            (unsigned long)(out.empty() ? 0 : t_min),
            (unsigned long)t_max,
            sizeof(Edge));
        std::fclose(mf);
        std::fprintf(stderr, "[meta]  %s\n", meta_path);
    }

    std::fprintf(stderr, "[total] %.2fs\n", now_sec() - t0);
    return 0;
}
