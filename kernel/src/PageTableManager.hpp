#pragma once

#include <cstddef>
#include <cstdint>
#include "Address.hpp"
#include "IFrameManager.hpp"
#include "PageTable.hpp"

namespace oz {

class PageTableManager : public PhysicalAddressProvider {
public:
    struct TranslateResult {
        PhysicalAddress<void> address;
        bool success;
        bool padding[7];
    };

private:
    PageTable* pml4_table; // Direct-mapped virtual pointer to PML4
    IFrameManager* frame_manager;

public:
    PageTableManager(PageTable* pml4_virt, IFrameManager* fm = nullptr);

    bool mapPage(std::uintptr_t virt_addr, PhysicalAddress<void> phys_addr, PageFlags flags);
    bool mapPage(std::uintptr_t virt_addr, std::uintptr_t phys_addr, PageFlags flags) {
        return mapPage(virt_addr, createPhysicalAddress<void>(phys_addr), flags);
    }

    bool unmapPage(std::uintptr_t virt_addr);

    TranslateResult translate(std::uintptr_t virt_addr);
    bool translate(std::uintptr_t virt_addr, std::uintptr_t* out_phys) {
        if (!out_phys) return false;
        auto res = translate(virt_addr);
        if (!res.success) return false;
        *out_phys = res.address.get();
        return true;
    }

    PageTable* getPML4() const { return pml4_table; }
    void setFrameManager(IFrameManager* fm) { frame_manager = fm; }
};

} // namespace oz
