// Convert LDBC SNB (BI composite-merged-fk layout) dynamic relationship
// tables into a Gnomon/TGSR .bin temporal edge stream.
//
// The social network's activity IS a temporal event graph: every dynamic
// relationship row carries the creationDate at which it happened and never
// changes afterwards. We take the four pure relationship tables
//
//   Person_knows_Person       creationDate | Person1Id | Person2Id
//   Person_likes_Comment      creationDate | PersonId  | CommentId
//   Person_likes_Post         creationDate | PersonId  | PostId
//   Forum_hasMember_Person    creationDate | ForumId   | PersonId
//
// from BOTH the initial snapshot and the insert stream (together they cover
// the full simulated timeline), remap the per-type entity id spaces into one
// dense uint32 vertex space (Person/Comment/Post/Forum ids are separate
// LDBC namespaces, so ids are tagged with their type before remapping),
// sort by creationDate (LongDateFormatter epoch milliseconds), and emit
// Edge{src, dst, ts_ms, w=1}. Deletes are ignored: our model treats removal
// as retention policy, and the paper says so where the corpus is described.
//
// Usage: ldbc_to_bin --root <extracted bi-sfN dir> --out <corpus.bin>

#include "barry/edge.hpp"

#include <zlib.h>
#include <time.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <unordered_map>
#include <vector>

using barry::Edge;

namespace {

struct Table { const char* name; int type_src, type_dst; };
// entity type tags: 0=Person 1=Comment 2=Post 3=Forum
const Table kTables[] = {
    {"Person_knows_Person",    0, 0},
    {"Person_likes_Comment",   0, 1},
    {"Person_likes_Post",      0, 2},
    {"Forum_hasMember_Person", 3, 0},
};

std::vector<std::string> find_csvs(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        std::string p = dir + "/" + e->d_name;
        if (e->d_type == DT_DIR) {
            auto sub = find_csvs(p);
            out.insert(out.end(), sub.begin(), sub.end());
        } else if (p.size() > 7 && p.compare(p.size() - 7, 7, ".csv.gz") == 0) {
            out.push_back(p);
        }
    }
    closedir(d);
    return out;
}

uint32_t remap(std::unordered_map<uint64_t, uint32_t>& ids, int type,
               uint64_t raw) {
    // Tag the type into the top bits so the per-type namespaces stay apart.
    const uint64_t key = (uint64_t(type) << 60) | raw;
    auto [it, fresh] = ids.emplace(key, (uint32_t)ids.size());
    (void)fresh;
    return it->second;
}

}  // namespace

int main(int argc, char** argv) {
    std::string root, out_path;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--root")) root = argv[i + 1];
        else if (!std::strcmp(argv[i], "--out")) out_path = argv[i + 1];
    }
    if (root.empty() || out_path.empty()) {
        std::fprintf(stderr, "usage: %s --root <bi-sfN dir> --out <bin>\n", argv[0]);
        return 1;
    }

    std::unordered_map<uint64_t, uint32_t> ids;
    std::vector<Edge> edges;
    edges.reserve(1u << 26);
    std::vector<char> line(1 << 16);

    const char* phases[] = {"initial_snapshot", "inserts"};
    for (const Table& t : kTables) {
        size_t before = edges.size();
        for (const char* ph : phases) {
            const std::string dir = root + "/graphs/csv/bi/composite-merged-fk/" +
                                    ph + "/dynamic/" + t.name;
            for (const std::string& f : find_csvs(dir)) {
                gzFile g = gzopen(f.c_str(), "rb");
                if (!g) { std::fprintf(stderr, "open failed: %s\n", f.c_str()); return 1; }
                bool header = true;
                while (gzgets(g, line.data(), (int)line.size())) {
                    if (header) { header = false; continue; }
                    // creationDate|srcId|dstId   (LongDateFormatter: epoch ms)
                    char* p = line.data();
                    char* q1 = std::strchr(p, '|');
                    if (!q1) continue;
                    char* q2 = std::strchr(q1 + 1, '|');
                    if (!q2) continue;
                    // creationDate is either epoch millis (interactive
                    // LongDateFormatter) or an ISO datetime string (BI
                    // layout, "2010-01-03T05:12:16.318+00:00").
                    uint64_t ts;
                    if (q1 - p > 4 && p[4] == '-') {
                        struct tm tmv = {};
                        int Y, M, D, h, m, sec, ms = 0;
                        if (std::sscanf(p, "%d-%d-%dT%d:%d:%d.%d",
                                        &Y, &M, &D, &h, &m, &sec, &ms) < 6)
                            continue;
                        tmv.tm_year = Y - 1900; tmv.tm_mon = M - 1;
                        tmv.tm_mday = D; tmv.tm_hour = h; tmv.tm_min = m;
                        tmv.tm_sec = sec;
                        ts = (uint64_t)timegm(&tmv) * 1000ull + (uint64_t)ms;
                    } else {
                        ts = std::strtoull(p, nullptr, 10);
                    }
                    const uint64_t a  = std::strtoull(q1 + 1, nullptr, 10);
                    const uint64_t b  = std::strtoull(q2 + 1, nullptr, 10);
                    Edge e;
                    e.src = remap(ids, t.type_src, a);
                    e.dst = remap(ids, t.type_dst, b);
                    e.timestamp = ts ? ts : 1;
                    e.weight = 1.0f;
                    edges.push_back(e);
                }
                gzclose(g);
            }
        }
        std::fprintf(stderr, "[ldbc] %-24s %zu edges\n", t.name,
                     edges.size() - before);
    }

    std::fprintf(stderr, "[ldbc] total %zu edges, %zu vertices; sorting by time\n",
                 edges.size(), ids.size());
    std::stable_sort(edges.begin(), edges.end(),
                     [](const Edge& a, const Edge& b) {
                         return a.timestamp < b.timestamp;
                     });

    FILE* f = std::fopen(out_path.c_str(), "wb");
    if (!f) { std::perror(out_path.c_str()); return 1; }
    if (std::fwrite(edges.data(), sizeof(Edge), edges.size(), f) != edges.size()) {
        std::perror("fwrite"); return 1;
    }
    std::fclose(f);
    std::fprintf(stderr, "[ldbc] wrote %s (%zu edges, t=[%llu,%llu])\n",
                 out_path.c_str(), edges.size(),
                 (unsigned long long)edges.front().timestamp,
                 (unsigned long long)edges.back().timestamp);
    return 0;
}
