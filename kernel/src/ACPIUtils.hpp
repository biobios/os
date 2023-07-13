#pragma once

#include "ACPI.hpp"
#include "PCIe.hpp"

namespace ACPIUtils {
struct ExtendedSystemDescriptionTableWrapper
    : public ACPI::ExtendedSystemDescriptionTable {
   public:
    template <typename T>
    T* getTable();

    template <>
    PCIe::MemorymappedConfigurationSpaceDescriptionTable*
    getTable<PCIe::MemorymappedConfigurationSpaceDescriptionTable>() {
        std::size_t size = this->size();
        PCIe::MemorymappedConfigurationSpaceDescriptionTable* ret = nullptr;
        for (std::size_t i = 0; i < size; i++) {
            ACPI::SystemDescriptionTableHeader* entry = this->getEntry(i);
            if (entry->sameSignature(
                    reinterpret_cast<const std::uint8_t*>("MCFG"))) {
                ret = reinterpret_cast<
                    PCIe::MemorymappedConfigurationSpaceDescriptionTable*>(
                    entry);
                break;
            }
        }
        return ret;
    }

    static ExtendedSystemDescriptionTableWrapper* wrap(
        ACPI::ExtendedSystemDescriptionTable* src);
};
}  // namespace ACPIUtils