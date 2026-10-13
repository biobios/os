#pragma once
#include "core/Capability.hpp"
#include <cstdint>

namespace oz {

// Forward declaration - MemoryFlags is defined in memory/MemoryFlags.hpp
class MemoryFlags;

class VSpace : public KernelObject {
private:
    void* address_space_;  // ProcessAddressSpace<KernelAccessor>*
    bool (*map_fn_)(void* as, std::uintptr_t virt, std::uintptr_t phys, std::uint64_t flags);
    bool (*unmap_fn_)(void* as, std::uintptr_t virt);

public:
    VSpace(void* as,
           bool (*map_fn)(void*, std::uintptr_t, std::uintptr_t, std::uint64_t),
           bool (*unmap_fn)(void*, std::uintptr_t))
        : KernelObject(ObjectType::VSpace),
          address_space_(as), map_fn_(map_fn), unmap_fn_(unmap_fn) {}

    bool mapUser(std::uintptr_t virt, std::uintptr_t phys, std::uint64_t flags) {
        if (!map_fn_) return false;
        return map_fn_(address_space_, virt, phys, flags);
    }

    bool unmapUser(std::uintptr_t virt) {
        if (!unmap_fn_) return false;
        return unmap_fn_(address_space_, virt);
    }
};

} // namespace oz
