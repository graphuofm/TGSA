#pragma once
#include <cstdint>

namespace barry {

struct Edge {
    uint32_t src;
    uint32_t dst;
    uint64_t timestamp;
    float    weight;
} __attribute__((packed));
static_assert(sizeof(Edge) == 20, "Edge must be packed to 20 bytes");

}  // namespace barry
