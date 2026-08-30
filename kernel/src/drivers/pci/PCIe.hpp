#pragma once

#include "hardware/ACPI.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wpadded"


namespace PCIe {

struct MemorymappedConfigurationSpaceDescription {
    std::uint32_t BaseAddressLo;
    std::uint32_t BaseAddressHi;
    std::uint16_t PCISegmentGroupNumber;
    std::uint8_t StartPCIBusNumber;
    std::uint8_t EndPCIBusNumber;
    std::uint8_t Reserved[4];

    std::uint64_t getBaseAddress() const {
        return static_cast<std::uint64_t>(BaseAddressLo) | (static_cast<std::uint64_t>(BaseAddressHi) << 32);
    }
};

struct MemorymappedConfigurationSpaceDescriptionTable {
    ACPI::SystemDescriptionTableHeader SDTH;
    std::uint8_t Reserved[8];
    
    static constexpr std::uint8_t SIGNATURE[4] = {'M', 'C', 'F', 'G'};

    MemorymappedConfigurationSpaceDescription* getDescription(std::size_t index) {
        return &reinterpret_cast<MemorymappedConfigurationSpaceDescription*>(this + 1)[index];
    }
    std::size_t size() {
        std::size_t length =
            SDTH.Length - sizeof(ACPI::SystemDescriptionTableHeader);
        return length / sizeof(MemorymappedConfigurationSpaceDescription);
    }
};
}  // namespace PCIe
#pragma GCC diagnostic pop
