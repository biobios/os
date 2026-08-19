#pragma once

#include <cstddef>
#include <cstdint>
#include "memory/Address.hpp"
#include "memory/IFrameManager.hpp"
#include "memory/PageTable.hpp"
#include "memory/PageTableManager.hpp"
#include "memory/MemoryFlags.hpp"
#include "hardware/x86_64.hpp"

namespace oz {

template <frame_manager_accessor Accessor>
class x86_64AddressSpaceContext : public PhysicalAddressProvider {
private:
    PageTable* pml4_virt;
    PhysicalAddress<PageTable> pml4_phys;
    PageTableManager<Accessor> pt_manager;

public:
    x86_64AddressSpaceContext(PageTable* pml4_v, PhysicalAddress<PageTable> pml4_p)
        : pml4_virt(pml4_v), pml4_phys(pml4_p), pt_manager(pml4_v) {}

    static x86_64AddressSpaceContext cloneProcessSpace(const x86_64AddressSpaceContext& kernel_space) {
        constexpr auto& fm = Accessor::getFrameManager();

        PageBlock pml4_block = fm.allocateBlock(0);
        if (!pml4_block) {
            return x86_64AddressSpaceContext(nullptr, PhysicalAddressProvider::nullPhysicalAddress<PageTable>());
        }
        pml4_block.setOwner(PageOwnerType::PAGE_TABLE);

        PhysicalAddress<PageTable> new_pml4_phys = physical_address_cast<PageTable>(fm.getPhysicalAddress(pml4_block));
        PageTable* new_pml4_virt = phys_to_virt(new_pml4_phys);
        const PageTable* k_pml4_virt = kernel_space.getVirtualPML4();

        // 0..255: Lower half (Process-private) -> clear
        for (std::size_t i = 0; i < 256; ++i) {
            (*new_pml4_virt)[i].clear();
        }

        // 256..511: Higher half (Kernel shared) -> copy from kernel master PML4
        for (std::size_t i = 256; i < 512; ++i) {
            (*new_pml4_virt)[i] = (*k_pml4_virt)[i];
        }

        return x86_64AddressSpaceContext(new_pml4_virt, new_pml4_phys);
    }

    bool isValid() const {
        return pml4_virt != nullptr;
    }

    void destroyProcessSpace() {
        // To be implemented when page table freeing is added
    }

    static PageFlags translateFlags(MemoryFlags flags) {
        PageFlags pflags = PageFlags::None;
        if ((flags & MemoryFlags::Present) != MemoryFlags::None) pflags |= PageFlags::Present;
        if ((flags & MemoryFlags::Writable) != MemoryFlags::None) pflags |= PageFlags::Writable;
        if ((flags & MemoryFlags::User) != MemoryFlags::None) pflags |= PageFlags::User;
        if ((flags & MemoryFlags::WriteThrough) != MemoryFlags::None) pflags |= PageFlags::WriteThrough;
        if ((flags & MemoryFlags::CacheDisable) != MemoryFlags::None) pflags |= PageFlags::CacheDisable;
        if ((flags & MemoryFlags::Executable) == MemoryFlags::None) pflags |= PageFlags::NoExecute;
        return pflags;
    }

    bool mapUser(std::uintptr_t virt_addr, PhysicalAddress<void> phys_addr, MemoryFlags flags) {
        if (virt_addr >= 0x0000800000000000ULL) return false;
        return pt_manager.mapPage(virt_addr, phys_addr, translateFlags(flags) | PageFlags::User);
    }

    bool mapUser(std::uintptr_t virt_addr, std::uintptr_t phys_addr, MemoryFlags flags) {
        if (virt_addr >= 0x0000800000000000ULL) return false;
        return pt_manager.mapPage(virt_addr, phys_addr, translateFlags(flags) | PageFlags::User);
    }

    bool unmapUser(std::uintptr_t virt_addr) {
        if (virt_addr >= 0x0000800000000000ULL) return false;
        return pt_manager.unmapPage(virt_addr);
    }

    void activate() const {
        x86_64::setPageMap(reinterpret_cast<const void*>(pml4_phys.get()));
    }

    PhysicalAddress<PageTable> getPhysicalPML4() const { return pml4_phys; }
    PageTable* getVirtualPML4() const { return pml4_virt; }
    PageTableManager<Accessor>& getPageTableManager() { return pt_manager; }
};

} // namespace oz
