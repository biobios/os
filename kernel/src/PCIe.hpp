#pragma once

#include "ACPI.hpp"

namespace PCIe {

struct MemorymappedConfigurationSpaceDescription {
    ACPI::Byte8AlignedBy4 BaseAddress;
    std::uint16_t PCISegmentGroupNumber;
    std::uint8_t StartPCIBusNumber;
    std::uint8_t EndPCIBusNumber;
    std::uint8_t Reserved[4];
};

struct MemorymappedConfigurationSpaceDescriptionTable {
    ACPI::SystemDescriptionTableHeader SDTH;
    std::uint8_t Reserved[8];
    std::size_t size() {
        std::size_t length =
            SDTH.Length - sizeof(ACPI::SystemDescriptionTableHeader);
        return length / sizeof(MemorymappedConfigurationSpaceDescription);
    }
    MemorymappedConfigurationSpaceDescription* getDescription(
        std::size_t index) {
        MemorymappedConfigurationSpaceDescription* desc =
            reinterpret_cast<MemorymappedConfigurationSpaceDescription*>(
                reinterpret_cast<std::uint8_t*>(this) +
                sizeof(MemorymappedConfigurationSpaceDescriptionTable));
        desc += index;
        return desc;
    }
};
}  // namespace PCIe