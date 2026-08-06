#include "AddressSpace.hpp"
#include <new>

oz::AddressSpace* oz::AddressSpace::createProcessSpace(IFrameManager* fm, const AddressSpace& kernel_space) {
    if (!fm) return nullptr;

    FrameInfo* pml4_frame = fm->allocatePages(1);
    if (!pml4_frame) return nullptr;

    PhysicalAddress<PageTable> new_pml4_phys = physical_address_cast<PageTable>(pml4_frame->physicalAddress);
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
    FrameInfo* as_frame = fm->allocatePages(1);
    if (!as_frame) {
        fm->freePages(pml4_frame, 1);
        return nullptr;
    }

    AddressSpace* new_as = reinterpret_cast<AddressSpace*>(phys_to_virt(as_frame->physicalAddress));
    new (static_cast<void*>(new_as)) AddressSpace(new_pml4_virt, new_pml4_phys, fm);
    return new_as;
}
