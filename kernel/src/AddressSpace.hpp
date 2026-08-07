#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include "Address.hpp"
#include "IFrameManager.hpp"
#include "PageTable.hpp"
#include "PageTableManager.hpp"
#include "x86_64.hpp"

namespace oz {

template <frame_manager FrameManager>
class AddressSpace {
private:
    PageTable* pml4_virt;
    PhysicalAddress<PageTable> pml4_phys;
    FrameManager* frame_manager;
    PageTableManager<FrameManager> pt_manager;

public:
    AddressSpace(PageTable* pml4_v, PhysicalAddress<PageTable> pml4_p, FrameManager* fm = nullptr)
        : pml4_virt(pml4_v), pml4_phys(pml4_p), frame_manager(fm), pt_manager(pml4_v, fm) {}

    static AddressSpace* createProcessSpace(FrameManager* fm, const AddressSpace& kernel_space) {
        if (!fm) return nullptr;

        PageBlock pml4_block = fm->allocateBlock(0);
        if (!pml4_block) return nullptr;
        pml4_block.setOwner(PageOwnerType::PAGE_TABLE);

        PhysicalAddress<PageTable> new_pml4_phys = physical_address_cast<PageTable>(fm->getPhysicalAddress(pml4_block));
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

        // Allocate AddressSpace object structure
        PageBlock as_block = fm->allocateBlock(0);
        if (!as_block) {
            fm->freeBlock(pml4_block);
            return nullptr;
        }
        as_block.setOwner(PageOwnerType::OTHER);

        AddressSpace* new_as = reinterpret_cast<AddressSpace*>(phys_to_virt(fm->getPhysicalAddress(as_block)));
        new (static_cast<void*>(new_as)) AddressSpace(new_pml4_virt, new_pml4_phys, fm);
        return new_as;
    }

    bool mapUser(std::uintptr_t virt_addr, PhysicalAddress<void> phys_addr, PageFlags flags) {
        // User addresses must be within lower half (0x0000000000000000 ~ 0x00007FFFFFFFFFFF)
        if (virt_addr >= 0x0000800000000000ULL) return false;
        return pt_manager.mapPage(virt_addr, phys_addr, flags | PageFlags::User);
    }

    bool mapUser(std::uintptr_t virt_addr, std::uintptr_t phys_addr, PageFlags flags) {
        if (virt_addr >= 0x0000800000000000ULL) return false;
        return pt_manager.mapPage(virt_addr, phys_addr, flags | PageFlags::User);
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
    PageTableManager<FrameManager>& getPageTableManager() { return pt_manager; }
    void setFrameManager(FrameManager* fm) {
        frame_manager = fm;
        pt_manager.setFrameManager(fm);
    }
};

} // namespace oz
