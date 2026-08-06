#include "PageTableManager.hpp"
#include "Address.hpp"

oz::PageTableManager::PageTableManager(PageTable* pml4_virt, IFrameManager* fm)
    : pml4_table(pml4_virt), frame_manager(fm) {}

bool oz::PageTableManager::mapPage(std::uintptr_t virt_addr, PhysicalAddress<void> phys_addr, PageFlags flags) {
    if (!pml4_table) return false;

    std::size_t pml4_idx = paging::pml4Index(virt_addr);
    std::size_t pdpt_idx = paging::pdptIndex(virt_addr);
    std::size_t pd_idx   = paging::pdIndex(virt_addr);
    std::size_t pt_idx   = paging::ptIndex(virt_addr);

    // PML4 entry
    if (!(*pml4_table)[pml4_idx].isPresent()) {
        if (!frame_manager) return false;
        FrameInfo* fi = frame_manager->allocatePages(1);
        if (!fi) return false;

        PhysicalAddress<PageTable> pdpt_phys = physical_address_cast<PageTable>(fi->physicalAddress);
        PageTable* pdpt_virt = phys_to_virt(pdpt_phys);
        pdpt_virt->clear();

        (*pml4_table)[pml4_idx].set(pdpt_phys, PageFlags::Present | PageFlags::Writable | PageFlags::User);
    }

    PageTable* pdpt = phys_to_virt(createPhysicalAddress<PageTable>((*pml4_table)[pml4_idx].getAddress()));

    // PDPT entry
    if (!(*pdpt)[pdpt_idx].isPresent()) {
        if (!frame_manager) return false;
        FrameInfo* fi = frame_manager->allocatePages(1);
        if (!fi) return false;

        PhysicalAddress<PageTable> pd_phys = physical_address_cast<PageTable>(fi->physicalAddress);
        PageTable* pd_virt = phys_to_virt(pd_phys);
        pd_virt->clear();

        (*pdpt)[pdpt_idx].set(pd_phys, PageFlags::Present | PageFlags::Writable | PageFlags::User);
    }

    PageTable* pd = phys_to_virt(createPhysicalAddress<PageTable>((*pdpt)[pdpt_idx].getAddress()));

    // PD entry
    if (!(*pd)[pd_idx].isPresent()) {
        if (!frame_manager) return false;
        FrameInfo* fi = frame_manager->allocatePages(1);
        if (!fi) return false;

        PhysicalAddress<PageTable> pt_phys = physical_address_cast<PageTable>(fi->physicalAddress);
        PageTable* pt_virt = phys_to_virt(pt_phys);
        pt_virt->clear();

        (*pd)[pd_idx].set(pt_phys, PageFlags::Present | PageFlags::Writable | PageFlags::User);
    }

    PageTable* pt = phys_to_virt(createPhysicalAddress<PageTable>((*pd)[pd_idx].getAddress()));

    (*pt)[pt_idx].set(phys_addr, flags | PageFlags::Present);

    __asm__ volatile("invlpg (%0)" :: "r"(virt_addr) : "memory");
    return true;
}

bool oz::PageTableManager::unmapPage(std::uintptr_t virt_addr) {
    if (!pml4_table) return false;

    std::size_t pml4_idx = paging::pml4Index(virt_addr);
    std::size_t pdpt_idx = paging::pdptIndex(virt_addr);
    std::size_t pd_idx   = paging::pdIndex(virt_addr);
    std::size_t pt_idx   = paging::ptIndex(virt_addr);

    if (!(*pml4_table)[pml4_idx].isPresent()) return false;
    PageTable* pdpt = phys_to_virt(createPhysicalAddress<PageTable>((*pml4_table)[pml4_idx].getAddress()));

    if (!(*pdpt)[pdpt_idx].isPresent()) return false;
    PageTable* pd = phys_to_virt(createPhysicalAddress<PageTable>((*pdpt)[pdpt_idx].getAddress()));

    if (!(*pd)[pd_idx].isPresent()) return false;
    PageTable* pt = phys_to_virt(createPhysicalAddress<PageTable>((*pd)[pd_idx].getAddress()));

    (*pt)[pt_idx].clear();
    __asm__ volatile("invlpg (%0)" :: "r"(virt_addr) : "memory");
    return true;
}

oz::PageTableManager::TranslateResult oz::PageTableManager::translate(std::uintptr_t virt_addr) {
    if (!pml4_table) return {nullPhysicalAddress<void>(), false, {}};

    std::size_t pml4_idx = paging::pml4Index(virt_addr);
    std::size_t pdpt_idx = paging::pdptIndex(virt_addr);
    std::size_t pd_idx   = paging::pdIndex(virt_addr);
    std::size_t pt_idx   = paging::ptIndex(virt_addr);

    if (!(*pml4_table)[pml4_idx].isPresent()) return {nullPhysicalAddress<void>(), false, {}};
    PageTable* pdpt = phys_to_virt(createPhysicalAddress<PageTable>((*pml4_table)[pml4_idx].getAddress()));

    if (!(*pdpt)[pdpt_idx].isPresent()) return {nullPhysicalAddress<void>(), false, {}};
    // 1GB huge page check
    if ((*pdpt)[pdpt_idx].isHuge()) {
        return {createPhysicalAddress<void>(((*pdpt)[pdpt_idx].raw() & ~0x3FFFFFFFULL) | (virt_addr & 0x3FFFFFFFULL)), true, {}};
    }

    PageTable* pd = phys_to_virt(createPhysicalAddress<PageTable>((*pdpt)[pdpt_idx].getAddress()));

    if (!(*pd)[pd_idx].isPresent()) return {nullPhysicalAddress<void>(), false, {}};
    // 2MB huge page check
    if ((*pd)[pd_idx].isHuge()) {
        return {createPhysicalAddress<void>(((*pd)[pd_idx].raw() & ~0x1FFFFFULL) | (virt_addr & 0x1FFFFFULL)), true, {}};
    }

    PageTable* pt = phys_to_virt(createPhysicalAddress<PageTable>((*pd)[pd_idx].getAddress()));

    if (!(*pt)[pt_idx].isPresent()) return {nullPhysicalAddress<void>(), false, {}};
    return {createPhysicalAddress<void>((*pt)[pt_idx].getAddress() | (virt_addr & 0xFFFULL)), true, {}};
}
