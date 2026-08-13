#pragma once

#include <cstdint>

namespace oz {

enum class MemoryFlags : std::uint64_t {
    None         = 0,
    Present      = 1ULL << 0,
    Writable     = 1ULL << 1,
    Executable   = 1ULL << 2,
    User         = 1ULL << 3,
    WriteThrough = 1ULL << 4,
    CacheDisable = 1ULL << 5,
};

constexpr MemoryFlags operator|(MemoryFlags a, MemoryFlags b) {
    return static_cast<MemoryFlags>(static_cast<std::uint64_t>(a) | static_cast<std::uint64_t>(b));
}

constexpr MemoryFlags operator&(MemoryFlags a, MemoryFlags b) {
    return static_cast<MemoryFlags>(static_cast<std::uint64_t>(a) & static_cast<std::uint64_t>(b));
}

constexpr MemoryFlags operator~(MemoryFlags a) {
    return static_cast<MemoryFlags>(~static_cast<std::uint64_t>(a));
}

constexpr MemoryFlags operator^(MemoryFlags a, MemoryFlags b) {
    return static_cast<MemoryFlags>(static_cast<std::uint64_t>(a) ^ static_cast<std::uint64_t>(b));
}

constexpr MemoryFlags& operator|=(MemoryFlags& a, MemoryFlags b) {
    a = a | b;
    return a;
}

constexpr MemoryFlags& operator&=(MemoryFlags& a, MemoryFlags b) {
    a = a & b;
    return a;
}

constexpr MemoryFlags& operator^=(MemoryFlags& a, MemoryFlags b) {
    a = a ^ b;
    return a;
}

} // namespace oz
