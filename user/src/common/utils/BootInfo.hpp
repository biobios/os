#pragma once
#include <cstdint>

namespace oz {

struct BootInfo {
    std::uint64_t ahci_abar_phys;
    std::uint64_t xhci_bar_phys;
    std::uint64_t initrd_size;
    std::uint64_t initrd_offset;
    std::uint64_t fb_phys_addr;
    std::uint64_t fb_size;
    std::uint32_t fb_width;
    std::uint32_t fb_height;
};

} // namespace oz
