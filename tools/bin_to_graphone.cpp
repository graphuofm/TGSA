// Convert a Gnomon .bin edge stream into the binary edge-list directory GraphOne
// reads (-s 1). GraphOne slurps every file in the input directory into one
// buffer before its timer starts, so the chunking is only about file size.
//
// Two layouts, matching two GraphOne builds:
//
//   plain32   graphone32 -j 0/1/2, edgeT_t<dst_id_t> = {u32 src, u32 dst}.
//             Eight bytes, no room for time. This is GraphOne's headline
//             configuration and it cannot represent a timestamp at all.
//
//   weight64  graphone64 -j 20/21/22, edgeT_t<weight_sid_t>
//             = {u64 src, u64 dst, u64 weight}. Twenty-four bytes, with the
//             timestamp travelling in the weight slot. This is the fair
//             configuration for a temporal comparison: time as edge payload,
//             which is where an entity-keyed store has to put it.
//
// Usage:
//   barry_bin_to_graphone --dataset <bin> --outdir <dir>
//                         [--format plain32|weight64] [--limit N]
//                         [--chunk_edges N]

#include "barry/edge.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

using barry::Edge;

namespace {

struct Plain32 { uint32_t src, dst; };
struct Weight64 { uint64_t src, dst, weight; };
static_assert(sizeof(Plain32) == 8, "GraphOne B32 plain edge is 8 bytes");
static_assert(sizeof(Weight64) == 24, "GraphOne B64 weighted edge is 24 bytes");

std::string arg(int argc, char** argv, const char* key, const char* def = "") {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
    return def;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string in   = arg(argc, argv, "--dataset");
    const std::string out  = arg(argc, argv, "--outdir");
    const std::string fmt  = arg(argc, argv, "--format", "weight64");
    const size_t limit     = std::strtoull(arg(argc, argv, "--limit", "0").c_str(), nullptr, 10);
    const size_t chunk     = std::strtoull(arg(argc, argv, "--chunk_edges", "8000000").c_str(), nullptr, 10);

    if (in.empty() || out.empty()) {
        std::fprintf(stderr,
            "usage: %s --dataset <bin> --outdir <dir> "
            "[--format plain32|weight64] [--limit N] [--chunk_edges N]\n", argv[0]);
        return 1;
    }
    if (fmt != "plain32" && fmt != "weight64") {
        std::fprintf(stderr, "unknown format %s\n", fmt.c_str());
        return 1;
    }

    int fd = ::open(in.c_str(), O_RDONLY);
    if (fd < 0) { std::perror(in.c_str()); return 1; }
    struct stat st{};
    if (::fstat(fd, &st) != 0) { std::perror("fstat"); return 1; }
    const size_t total = static_cast<size_t>(st.st_size) / sizeof(Edge);
    void* addr = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED) { std::perror("mmap"); return 1; }
    ::posix_madvise(addr, static_cast<size_t>(st.st_size), POSIX_MADV_SEQUENTIAL);
    const Edge* edges = static_cast<const Edge*>(addr);
    const size_t n = limit ? std::min(limit, total) : total;

    if (::mkdir(out.c_str(), 0755) != 0 && errno != EEXIST) { std::perror(out.c_str()); return 1; }

    uint64_t v_max = 0;
    size_t written = 0, files = 0;
    std::vector<Plain32>  b32;
    std::vector<Weight64> b64;

    for (size_t off = 0; off < n; off += chunk, ++files) {
        const size_t m = std::min(chunk, n - off);
        char path[4096];
        std::snprintf(path, sizeof(path), "%s/part%05zu.dat", out.c_str(), files);
        FILE* f = std::fopen(path, "wb");
        if (!f) { std::perror(path); return 1; }

        if (fmt == "plain32") {
            b32.resize(m);
            for (size_t i = 0; i < m; ++i) {
                b32[i].src = edges[off + i].src;
                b32[i].dst = edges[off + i].dst;
                if (b32[i].src > v_max) v_max = b32[i].src;
                if (b32[i].dst > v_max) v_max = b32[i].dst;
            }
            if (std::fwrite(b32.data(), sizeof(Plain32), m, f) != m) { std::perror("fwrite"); return 1; }
        } else {
            b64.resize(m);
            for (size_t i = 0; i < m; ++i) {
                b64[i].src    = edges[off + i].src;
                b64[i].dst    = edges[off + i].dst;
                b64[i].weight = edges[off + i].timestamp;
                if (b64[i].src > v_max) v_max = b64[i].src;
                if (b64[i].dst > v_max) v_max = b64[i].dst;
            }
            if (std::fwrite(b64.data(), sizeof(Weight64), m, f) != m) { std::perror("fwrite"); return 1; }
        }
        std::fclose(f);
        written += m;
    }

    ::munmap(addr, static_cast<size_t>(st.st_size));
    ::close(fd);

    // GraphOne wants the vertex count on the command line (-v). Print it in a
    // form the runner script can read back rather than recomputing.
    std::fprintf(stdout, "edges=%zu files=%zu vcount=%llu format=%s\n",
                 written, files, (unsigned long long)(v_max + 1), fmt.c_str());
    std::fprintf(stderr, "[bin_to_graphone] %s -> %s: %zu edges, %zu files, vcount=%llu (%s)\n",
                 in.c_str(), out.c_str(), written, files,
                 (unsigned long long)(v_max + 1), fmt.c_str());
    return 0;
}
