#pragma once

#include "hardware/ACPI.hpp"
#include "memory/Address.hpp"

namespace ACPIUtils {
class ExtendedSystemDescriptionTableWrapper : public oz::PhysicalAddressProvider {
    ACPI::ExtendedSystemDescriptionTable* table;
   public:
    ExtendedSystemDescriptionTableWrapper(ACPI::ExtendedSystemDescriptionTable* table) : table(table) {}
    template <typename T>
    T* getTable() {
        std::size_t size = table->size();
        T* ret = nullptr;
        for (std::size_t i = 0; i < size; i++) {
            auto entryPhysPtr = createPhysicalAddress<ACPI::SystemDescriptionTableHeader>(table->getEntry(i));
            ACPI::SystemDescriptionTableHeader* entry = oz::phys_to_virt(entryPhysPtr); 
            if (entry->sameSignature(T::SIGNATURE)) {
                ret = reinterpret_cast<T*>(entry);
                break;
            }
        }
        return ret;
    }
};
}  // namespace ACPIUtils