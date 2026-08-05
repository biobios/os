#include "PageTableManager.hpp"
#include "Address.hpp"

oz::PageTableManager::PageTableManager(std::uint64_t* pml4_virt, IFrameManager* fm)
    : pml4_table(pml4_virt), frame_manager(fm) {}

bool oz::PageTableManager::mapPage(std::uintptr_t virt_addr, PhysicalAddress<void> phys_addr, PageFlags flags) {
    if (!pml4_table) return false;

    std::uint64_t pml4_idx = (virt_addr >> 39) & 0x1FF;
    std::uint64_t pdpt_idx = (virt_addr >> 30) & 0x1FF;
    std::uint64_t pd_idx   = (virt_addr >> 21) & 0x1FF;
    std::uint64_t pt_idx   = (virt_addr >> 12) & 0x1FF;

    // PML4 entry
    if (!(pml4_table[pml4_idx] & 0x1)) {
        if (!frame_manager) return false;
        FrameInfo* fi = frame_manager->allocatePages(1);
        if (!fi) return false;

        PhysicalAddress<std::uint64_t> pdpt_phys = physical_address_cast<std::uint64_t>(fi->physicalAddress);
        std::uint64_t* pdpt_virt = phys_to_virt(pdpt_phys);
        for (std::size_t i = 0; i < 512; ++i) pdpt_virt[i] = 0;

        pml4_table[pml4_idx] = pdpt_phys.get() | 0x07; // Present | R/W | User
    }

    std::uint64_t* pdpt = phys_to_virt(createPhysicalAddress<std::uint64_t>(pml4_table[pml4_idx] & ~0xFFFULL));

    // PDPT entry
    if (!(pdpt[pdpt_idx] & 0x1)) {
        if (!frame_manager) return false;
        FrameInfo* fi = frame_manager->allocatePages(1);
        if (!fi) return false;

        PhysicalAddress<std::uint64_t> pd_phys = physical_address_cast<std::uint64_t>(fi->physicalAddress);
        std::uint64_t* pd_virt = phys_to_virt(pd_phys);
        for (std::size_t i = 0; i < 512; ++i) pd_virt[i] = 0;

        pdpt[pdpt_idx] = pd_phys.get() | 0x07; // Present | R/W | User
    }

    std::uint64_t* pd = phys_to_virt(createPhysicalAddress<std::uint64_t>(pdpt[pdpt_idx] & ~0xFFFULL));

    // PD entry
    if (!(pd[pd_idx] & 0x1)) {
        if (!frame_manager) return false;
        FrameInfo* fi = frame_manager->allocatePages(1);
        if (!fi) return false;

        PhysicalAddress<std::uint64_t> pt_phys = physical_address_cast<std::uint64_t>(fi->physicalAddress);
        std::uint64_t* pt_virt = phys_to_virt(pt_phys);
        for (std::size_t i = 0; i < 512; ++i) pt_virt[i] = 0;

        pd[pd_idx] = pt_phys.get() | 0x07; // Present | R/W | User
    }

    std::uint64_t* pt = phys_to_virt(createPhysicalAddress<std::uint64_t>(pd[pd_idx] & ~0xFFFULL));

    pt[pt_idx] = (phys_addr.get() & ~0xFFFULL) | static_cast<std::uint64_t>(flags) | 0x01;

    __asm__ volatile("invlpg (%0)" :: "r"(virt_addr) : "memory");
    return true;
}

bool oz::PageTableManager::unmapPage(std::uintptr_t virt_addr) {
    if (!pml4_table) return false;

    std::uint64_t pml4_idx = (virt_addr >> 39) & 0x1FF;
    std::uint64_t pdpt_idx = (virt_addr >> 30) & 0x1FF;
    std::uint64_t pd_idx   = (virt_addr >> 21) & 0x1FF;
    std::uint64_t pt_idx   = (virt_addr >> 12) & 0x1FF;

    if (!(pml4_table[pml4_idx] & 0x1)) return false;
    std::uint64_t* pdpt = phys_to_virt(createPhysicalAddress<std::uint64_t>(pml4_table[pml4_idx] & ~0xFFFULL));

    if (!(pdpt[pdpt_idx] & 0x1)) return false;
    std::uint64_t* pd = phys_to_virt(createPhysicalAddress<std::uint64_t>(pdpt[pdpt_idx] & ~0xFFFULL));

    if (!(pd[pd_idx] & 0x1)) return false;
    std::uint64_t* pt = phys_to_virt(createPhysicalAddress<std::uint64_t>(pd[pd_idx] & ~0xFFFULL));

    pt[pt_idx] = 0;
    __asm__ volatile("invlpg (%0)" :: "r"(virt_addr) : "memory");
    return true;
}

oz::PageTableManager::TranslateResult oz::PageTableManager::translate(std::uintptr_t virt_addr) {
    if (!pml4_table) return {nullPhysicalAddress<void>(), false};

    std::uint64_t pml4_idx = (virt_addr >> 39) & 0x1FF;
    std::uint64_t pdpt_idx = (virt_addr >> 30) & 0x1FF;
    std::uint64_t pd_idx   = (virt_addr >> 21) & 0x1FF;
    std::uint64_t pt_idx   = (virt_addr >> 12) & 0x1FF;

    if (!(pml4_table[pml4_idx] & 0x1)) return {nullPhysicalAddress<void>(), false};
    std::uint64_t* pdpt = phys_to_virt(createPhysicalAddress<std::uint64_t>(pml4_table[pml4_idx] & ~0xFFFULL));

    if (!(pdpt[pdpt_idx] & 0x1)) return {nullPhysicalAddress<void>(), false};
    // 1GB huge page check
    if (pdpt[pdpt_idx] & 0x80) {
        return {createPhysicalAddress<void>((pdpt[pdpt_idx] & ~0x3FFFFFFFULL) | (virt_addr & 0x3FFFFFFFULL)), true};
    }

    std::uint64_t* pd = phys_to_virt(createPhysicalAddress<std::uint64_t>(pdpt[pdpt_idx] & ~0xFFFULL));

    if (!(pd[pd_idx] & 0x1)) return {nullPhysicalAddress<void>(), false};
    // 2MB huge page check
    if (pd[pd_idx] & 0x80) {
        return {createPhysicalAddress<void>((pd[pd_idx] & ~0x1FFFFFULL) | (virt_addr & 0x1FFFFFULL)), true};
    }

    std::uint64_t* pt = phys_to_virt(createPhysicalAddress<std::uint64_t>(pd[pd_idx] & ~0xFFFULL));

    if (!(pt[pt_idx] & 0x1)) return {nullPhysicalAddress<void>(), false};
    return {createPhysicalAddress<void>((pt[pt_idx] & ~0xFFFULL) | (virt_addr & 0xFFFULL)), true};
}
