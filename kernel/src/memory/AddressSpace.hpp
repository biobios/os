#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include "memory/Address.hpp"
#include "memory/MemoryFlags.hpp"
#include "memory/x86_64AddressSpaceContext.hpp"

namespace oz {

class KernelAddressSpace {
public:
    using ArchContext = x86_64AddressSpaceContext;

private:
    ArchContext arch_ctx;

public:
    template <typename... Args>
    explicit KernelAddressSpace(Args&&... args) : arch_ctx(std::forward<Args>(args)...) {}

    void activate() const {
        arch_ctx.activate();
    }

    ArchContext& getArchContext() { return arch_ctx; }
    const ArchContext& getArchContext() const { return arch_ctx; }
};

class ProcessAddressSpace {
public:
    using ArchContext = x86_64AddressSpaceContext;

private:
    ArchContext arch_ctx;

public:
    explicit ProcessAddressSpace(ArchContext ctx) : arch_ctx(std::move(ctx)) {}

    static ProcessAddressSpace createFromPhysical(PhysicalAddress<PageTable> pml4_phys, const KernelAddressSpace& kernel_space) {
        ArchContext new_ctx = ArchContext::createFromPhysical(pml4_phys, kernel_space.getArchContext());
        return ProcessAddressSpace(std::move(new_ctx));
    }

    bool isValid() const {
        return arch_ctx.isValid();
    }

    bool installPageTable(std::uintptr_t virt_addr, PhysicalAddress<PageTable> pt_phys, std::uint8_t level) {
        return arch_ctx.installPageTable(virt_addr, pt_phys, level);
    }

    std::uint8_t getMissingTableLevel(std::uintptr_t virt_addr) const {
        return arch_ctx.getMissingTableLevel(virt_addr);
    }

    bool mapUser(std::uintptr_t virt_addr, PhysicalAddress<void> phys_addr, MemoryFlags flags) {
        return arch_ctx.mapUser(virt_addr, phys_addr, flags);
    }

    bool mapUser(std::uintptr_t virt_addr, std::uintptr_t phys_addr, MemoryFlags flags) {
        return arch_ctx.mapUser(virt_addr, phys_addr, flags);
    }

    bool unmapUser(std::uintptr_t virt_addr) {
        return arch_ctx.unmapUser(virt_addr);
    }

    void activate() const {
        arch_ctx.activate();
    }

    ArchContext& getArchContext() { return arch_ctx; }
    const ArchContext& getArchContext() const { return arch_ctx; }
};

} // namespace oz
