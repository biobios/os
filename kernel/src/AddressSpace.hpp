#pragma once

#include <cstdint>
#include "Address.hpp"
#include "IFrameManager.hpp"
#include "MemoryFlags.hpp"
#include "x86_64AddressSpaceContext.hpp"

namespace oz {

template <frame_manager_accessor Accessor, typename ArchContext = x86_64AddressSpaceContext<Accessor>>
class AddressSpace {
private:
    ArchContext arch_ctx;

public:
    template <typename... Args>
    AddressSpace(Args&&... args) : arch_ctx(std::forward<Args>(args)...) {}

    static AddressSpace* createProcessSpace(const AddressSpace& kernel_space) {
        constexpr auto& fm = Accessor::getFrameManager();

        ArchContext new_ctx = ArchContext::cloneProcessSpace(kernel_space.arch_ctx);
        if (!new_ctx.isValid()) {
            return nullptr;
        }

        // Allocate AddressSpace object structure
        PageBlock as_block = fm.allocateBlock(0);
        if (!as_block) {
            new_ctx.destroyProcessSpace();
            return nullptr;
        }
        as_block.setOwner(PageOwnerType::OTHER);

        AddressSpace* new_as = reinterpret_cast<AddressSpace*>(phys_to_virt(fm.getPhysicalAddress(as_block)));
        new (static_cast<void*>(new_as)) AddressSpace(new_ctx);
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
