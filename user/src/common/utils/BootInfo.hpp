#pragma once
#include <cstdint>

namespace oz {

struct BootInfo {
    std::uint64_t ahci_abar_phys;
};

} // namespace oz
