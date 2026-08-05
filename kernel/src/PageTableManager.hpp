#pragma once

#include <cstddef>
#include <cstdint>
#include "Address.hpp"
#include "IFrameManager.hpp"

namespace oz {

enum class PageFlags : std::uint64_t {
    None         = 0,
    Present      = 1ULL << 0,
    Writable     = 1ULL << 1,
    User         = 1ULL << 2,
    WriteThrough = 1ULL << 3,
    CacheDisable = 1ULL << 4,
    HugePage     = 1ULL << 7,
    Global       = 1ULL << 8,
    NoExecute    = 1ULL << 63
};

inline PageFlags operator|(PageFlags a, PageFlags b) {
    return static_cast<PageFlags>(static_cast<std::uint64_t>(a) | static_cast<std::uint64_t>(b));
}

inline PageFlags operator&(PageFlags a, PageFlags b) {
    return static_cast<PageFlags>(static_cast<std::uint64_t>(a) & static_cast<std::uint64_t>(b));
}

class PageTableManager : public PhysicalAddressProvider {
public:
    struct TranslateResult {
        PhysicalAddress<void> address;
        bool success;
    };

private:
    std::uint64_t* pml4_table; // Direct-mapped virtual pointer to PML4
    IFrameManager* frame_manager;

public:
    PageTableManager(std::uint64_t* pml4_virt, IFrameManager* fm = nullptr);

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

    std::uint64_t* getPML4() const { return pml4_table; }
    void setFrameManager(IFrameManager* fm) { frame_manager = fm; }
};

} // namespace oz
