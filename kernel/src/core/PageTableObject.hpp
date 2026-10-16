#pragma once
#include "core/Capability.hpp"
#include "memory/Address.hpp"
#include "utils/utils.hpp"
#include <cstdint>

namespace oz {

class PageTableObject : public KernelObject {
private:
    std::uintptr_t phys_{0};
    bool is_mapped_{false};
    std::uint8_t level_{0}; // 1: PT, 2: PD, 3: PDPT

public:
    explicit PageTableObject(std::uintptr_t phys, bool untyped = true)
        : KernelObject(ObjectType::PageTable, untyped), phys_(phys) {
        if (phys_ != 0) {
            void* virt = reinterpret_cast<void*>(phys_ + DIRECT_MAP_OFFSET);
            utils::memset(virt, 0, 4096);
        }
    }

    std::uintptr_t physicalAddress() const { return phys_; }
    bool isMapped() const { return is_mapped_; }
    void setMapped(bool m, std::uint8_t level) {
        is_mapped_ = m;
        level_ = level;
    }
    std::uint8_t getLevel() const { return level_; }
};

} // namespace oz
