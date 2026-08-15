#pragma once

#include <cstdint>
#include <utility>
#include "Address.hpp"
#include "IFrameManager.hpp"
#include "MemoryFlags.hpp"
#include "x86_64AddressSpaceContext.hpp"
#include "IKernelMemoryAllocator.hpp"

namespace oz {

template <frame_manager_accessor Accessor, typename ArchContext = x86_64AddressSpaceContext<Accessor>>
class KernelAddressSpace {
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

template <frame_manager_accessor Accessor, typename ArchContext = x86_64AddressSpaceContext<Accessor>>
class ProcessAddressSpace {
private:
    ArchContext arch_ctx;

public:
    explicit ProcessAddressSpace(ArchContext ctx) : arch_ctx(std::move(ctx)) {}

    static kmalloc_unique_ptr<ProcessAddressSpace, Accessor> create(const KernelAddressSpace<Accessor, ArchContext>& kernel_space) {
        ArchContext new_ctx = ArchContext::cloneProcessSpace(kernel_space.getArchContext());
        if (!new_ctx.isValid()) {
            return nullptr;
        }

        auto new_as = make_kmalloc_unique<ProcessAddressSpace, Accessor>(std::move(new_ctx));
        if (!new_as) {
            new_ctx.destroyProcessSpace();
        }
        return new_as;
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
