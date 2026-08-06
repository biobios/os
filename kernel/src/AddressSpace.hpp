#pragma once

#include <cstddef>
#include <cstdint>
#include "Address.hpp"
#include "IFrameManager.hpp"
#include "PageTable.hpp"
#include "PageTableManager.hpp"
#include "x86_64.hpp"

namespace oz {

class AddressSpace {
private:
    PageTable* pml4_virt;
    PhysicalAddress<PageTable> pml4_phys;
    IFrameManager* frame_manager;
    PageTableManager pt_manager;

public:
    AddressSpace(PageTable* pml4_v, PhysicalAddress<PageTable> pml4_p, IFrameManager* fm = nullptr)
        : pml4_virt(pml4_v), pml4_phys(pml4_p), frame_manager(fm), pt_manager(pml4_v, fm) {}

    static AddressSpace* createProcessSpace(IFrameManager* fm, const AddressSpace& kernel_space);

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
    PageTableManager& getPageTableManager() { return pt_manager; }
    void setFrameManager(IFrameManager* fm) {
        frame_manager = fm;
        pt_manager.setFrameManager(fm);
    }
};

} // namespace oz
