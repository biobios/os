#pragma once

#include <cstdint>
#include "utils/FlagClass.hpp"

namespace oz {
class MemoryFlags : public utils::FlagClass<std::uint64_t, MemoryFlags> {
    constexpr MemoryFlags(std::uint64_t flags) : FlagClass(flags) {}
public:
    static const MemoryFlags None;
    static const MemoryFlags Present;
    static const MemoryFlags Writable;
    static const MemoryFlags Executable;
    static const MemoryFlags User;
    static const MemoryFlags WriteThrough;
    static const MemoryFlags CacheDisable;
};

inline constexpr MemoryFlags MemoryFlags::None         = MemoryFlags(0);
inline constexpr MemoryFlags MemoryFlags::Present      = MemoryFlags(1ULL << 0);
inline constexpr MemoryFlags MemoryFlags::Writable     = MemoryFlags(1ULL << 1);
inline constexpr MemoryFlags MemoryFlags::Executable   = MemoryFlags(1ULL << 2);
inline constexpr MemoryFlags MemoryFlags::User         = MemoryFlags(1ULL << 3);
inline constexpr MemoryFlags MemoryFlags::WriteThrough = MemoryFlags(1ULL << 4);
inline constexpr MemoryFlags MemoryFlags::CacheDisable = MemoryFlags(1ULL << 5);

} // namespace oz
