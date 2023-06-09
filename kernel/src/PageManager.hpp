#pragma once

#include <cstdint>

namespace oz{

    constexpr std::uint64_t PAGE_DIRECTORY_COUNT = 64;

    class alignas(4096) PageManager{
        alignas(4096) std::uint64_t pml4_table[512];
        alignas(4096) std::uint64_t pdp_table[512];
        alignas(4096) std::uint64_t page_directory[PAGE_DIRECTORY_COUNT][512];
    public:
        PageManager();
    };

}