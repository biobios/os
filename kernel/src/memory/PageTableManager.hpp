#pragma once

#include <cstddef>
#include <cstdint>
#include "memory/Address.hpp"
#include "memory/PageTable.hpp"

namespace oz {

// 0xffff800000000000 ~ 0xffff880000000000 : reserved (8TB)
// 0xffff880000000000 ~ 0xffffc80000000000 : direct map領域 (64TB)
// 0xffffc80000000000 ~ 0xffffc90000000000 : reserved (1TB)
// 0xffffc90000000000 ~ 0xffffe90000000000 : 仮想メモリ領域 (32TB)
// 0xffffe90000000000 ~ 0xffffea0000000000 : reserved (1TB)
// 0xffffea0000000000 ~ 0xffffeb0000000000 : EFI runtime services領域 (1TB)
// 0xffffeb0000000000 ~ 0xffffec0000000000 : reserved (1TB)
// 0xffffec0000000000 ~ 0xffffec8000000000 : スタック領域 (512GB)
// 0xffffec8000000000 ~ 0xffffed8000000000 : reserved (1TB)
// 0xffffee0000000000 ~ 0xffffef0000000000 : メモリマップ領域 (1TB)
// 0xffffef0000000000 ~ 0xffffff0000000000 : reserved (32TB)
// 0xffffff0000000000 ~ 0xffffff8000000000 : reserved (512GB)
// 0xffffff8000000000 ~ 0xfffffff000000000 : reserved (64 * 7GB)
// 0xfffffff000000000 ~ 0xffffffff00000000 : reserved (8 * 7GB)
// 0xffffffff00000000 ~ 0xffffffff80000000 : reserved (2GB)
// 0xffffffff80000000 ~ 0xffffffffa0000000 : カーネルコード領域 (512MB)
// 0xffffffffa0000000 ~ 0xffffffffe0000000 : カーネルモジュール領域 (1GB)
// 0xffffffffe0000000 ~ 0xffffffffffffffff : reserved (512MB)

class PageTableManager : public PhysicalAddressProvider {
public:
    struct TranslateResult {
        PhysicalAddress<void> address;
        bool success;
        bool padding[7];
    };

private:
    PageTable* pml4_table; // Direct-mapped virtual pointer to PML4

public:
    PageTableManager(PageTable* pml4_virt)
        : pml4_table(pml4_virt) {}

    std::uint8_t getMissingTableLevel(std::uintptr_t virt_addr) const {
        if (!pml4_table) return 4;
        std::size_t pml4_idx = paging::pml4Index(virt_addr);
        if (!(*pml4_table)[pml4_idx].isPresent()) return 3; // PDPT missing
        PageTable* pdpt = phys_to_virt(createPhysicalAddress<PageTable>((*pml4_table)[pml4_idx].getAddress()));
        std::size_t pdpt_idx = paging::pdptIndex(virt_addr);
        if (!(*pdpt)[pdpt_idx].isPresent()) return 2; // PD missing
        PageTable* pd = phys_to_virt(createPhysicalAddress<PageTable>((*pdpt)[pdpt_idx].getAddress()));
        std::size_t pd_idx   = paging::pdIndex(virt_addr);
        if (!(*pd)[pd_idx].isPresent()) return 1; // PT missing
        return 0; // All present
    }

    bool installPageTable(std::uintptr_t virt_addr, PhysicalAddress<PageTable> pt_phys, std::uint8_t level) {
        if (!pml4_table) return false;

        std::size_t pml4_idx = paging::pml4Index(virt_addr);
        std::size_t pdpt_idx = paging::pdptIndex(virt_addr);
        std::size_t pd_idx   = paging::pdIndex(virt_addr);

        if (level == 3) {
            // Install PDPT into PML4
            if ((*pml4_table)[pml4_idx].isPresent()) return false;
            (*pml4_table)[pml4_idx].set(pt_phys, PageFlags::Present | PageFlags::Writable | PageFlags::User);
            return true;
        } else if (level == 2) {
            // Install PD into PDPT
            if (!(*pml4_table)[pml4_idx].isPresent()) return false;
            PageTable* pdpt = phys_to_virt(createPhysicalAddress<PageTable>((*pml4_table)[pml4_idx].getAddress()));
            if ((*pdpt)[pdpt_idx].isPresent()) return false;
            (*pdpt)[pdpt_idx].set(pt_phys, PageFlags::Present | PageFlags::Writable | PageFlags::User);
            return true;
        } else if (level == 1) {
            // Install PT into PD
            if (!(*pml4_table)[pml4_idx].isPresent()) return false;
            PageTable* pdpt = phys_to_virt(createPhysicalAddress<PageTable>((*pml4_table)[pml4_idx].getAddress()));
            if (!(*pdpt)[pdpt_idx].isPresent()) return false;
            PageTable* pd = phys_to_virt(createPhysicalAddress<PageTable>((*pdpt)[pdpt_idx].getAddress()));
            if ((*pd)[pd_idx].isPresent()) return false;
            (*pd)[pd_idx].set(pt_phys, PageFlags::Present | PageFlags::Writable | PageFlags::User);
            return true;
        }
        return false;
    }

    bool uninstallPageTable(std::uintptr_t virt_addr, std::uintptr_t expected_pt_phys, std::uint8_t level) {
        if (!pml4_table) return false;

        std::size_t pml4_idx = paging::pml4Index(virt_addr);
        std::size_t pdpt_idx = paging::pdptIndex(virt_addr);
        std::size_t pd_idx   = paging::pdIndex(virt_addr);

        bool uninstalled = false;
        if (level == 3) {
            if (!(*pml4_table)[pml4_idx].isPresent()) return false;
            if ((*pml4_table)[pml4_idx].getAddress() == expected_pt_phys) {
                (*pml4_table)[pml4_idx].clear();
                uninstalled = true;
            }
        } else if (level == 2) {
            if (!(*pml4_table)[pml4_idx].isPresent()) return false;
            PageTable* pdpt = phys_to_virt(createPhysicalAddress<PageTable>((*pml4_table)[pml4_idx].getAddress()));
            if (!(*pdpt)[pdpt_idx].isPresent()) return false;
            if ((*pdpt)[pdpt_idx].getAddress() == expected_pt_phys) {
                (*pdpt)[pdpt_idx].clear();
                uninstalled = true;
            }
        } else if (level == 1) {
            if (!(*pml4_table)[pml4_idx].isPresent()) return false;
            PageTable* pdpt = phys_to_virt(createPhysicalAddress<PageTable>((*pml4_table)[pml4_idx].getAddress()));
            if (!(*pdpt)[pdpt_idx].isPresent()) return false;
            PageTable* pd = phys_to_virt(createPhysicalAddress<PageTable>((*pdpt)[pdpt_idx].getAddress()));
            if (!(*pd)[pd_idx].isPresent()) return false;
            if ((*pd)[pd_idx].getAddress() == expected_pt_phys) {
                (*pd)[pd_idx].clear();
                uninstalled = true;
            }
        }
        if (uninstalled) {
            __asm__ volatile("invlpg (%0)" :: "r"(virt_addr) : "memory");
            std::uint64_t cr3 = 0;
            __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
            std::uintptr_t pml4_virt_addr = reinterpret_cast<std::uintptr_t>(pml4_table);
            if (pml4_virt_addr >= DIRECT_MAP_OFFSET) {
                std::uintptr_t pml4_phys = pml4_virt_addr - DIRECT_MAP_OFFSET;
                if ((cr3 & ~0xFFFULL) == (pml4_phys & ~0xFFFULL)) {
                    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
                }
            }
            return true;
        }
        return false;
    }

    bool mapPage(std::uintptr_t virt_addr, PhysicalAddress<void> phys_addr, PageFlags flags) {
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

        (*pt)[pt_idx].set(phys_addr, flags | PageFlags::Present);

        __asm__ volatile("invlpg (%0)" :: "r"(virt_addr) : "memory");
        return true;
    }

    bool mapPage(std::uintptr_t virt_addr, std::uintptr_t phys_addr, PageFlags flags) {
        return mapPage(virt_addr, createPhysicalAddress<void>(phys_addr), flags);
    }

    bool unmapPage(std::uintptr_t virt_addr) {
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

        if (!(*pt)[pt_idx].isPresent()) return false;
        (*pt)[pt_idx].clear();
        __asm__ volatile("invlpg (%0)" :: "r"(virt_addr) : "memory");
        return true;
    }

    TranslateResult translate(std::uintptr_t virt_addr) {
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

    bool translate(std::uintptr_t virt_addr, std::uintptr_t* out_phys) {
        if (!out_phys) return false;
        auto res = translate(virt_addr);
        if (!res.success) return false;
        *out_phys = res.address.get();
        return true;
    }

    PageTable* getPML4() const { return pml4_table; }
};

} // namespace oz
