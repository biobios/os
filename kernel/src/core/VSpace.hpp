#pragma once

#include <cstdint>
#include "core/Capability.hpp"
#include "memory/AddressSpace.hpp"

namespace oz {

class VSpace : public KernelObject, public PhysicalAddressProvider {
private:
    ProcessAddressSpace* address_space_{nullptr};
    std::uintptr_t pml4_phys_{0};

public:
    using PhysicalAddressProvider::createPhysicalAddress;

    VSpace(ProcessAddressSpace* as, std::uintptr_t pml4_phys)
        : KernelObject(ObjectType::VSpace),
          address_space_(as), pml4_phys_(pml4_phys) {}

    std::uintptr_t getPhysicalPML4() const { return pml4_phys_; }
    ProcessAddressSpace* getAddressSpace() const { return address_space_; }

    bool mapUser(std::uintptr_t virt, std::uintptr_t phys, std::uint64_t flags) {
        if (!address_space_) return false;
        MemoryFlags mem_flags = MemoryFlags::Present | MemoryFlags::User;
        if (flags & 0x01) mem_flags = mem_flags | MemoryFlags::Writable;
        if (flags & 0x02) mem_flags = mem_flags | MemoryFlags::Executable;
        if (flags & 0x10) mem_flags = mem_flags | MemoryFlags::CacheDisable;
        return address_space_->mapUser(virt, phys, mem_flags);
    }

    bool unmapUser(std::uintptr_t virt) {
        if (!address_space_) return false;
        return address_space_->unmapUser(virt);
    }

    bool installPageTable(std::uintptr_t virt, std::uintptr_t pt_phys, std::uint8_t level) {
        if (!address_space_) return false;
        return address_space_->installPageTable(virt, createPhysicalAddress<PageTable>(pt_phys), level);
    }

    std::uint8_t getMissingTableLevel(std::uintptr_t virt) const {
        if (!address_space_) return 0;
        return address_space_->getMissingTableLevel(virt);
    }
};

} // namespace oz
