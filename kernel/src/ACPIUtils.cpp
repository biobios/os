#include "ACPIUtils.hpp"

ACPIUtils::ExtendedSystemDescriptionTableWrapper*
ACPIUtils::ExtendedSystemDescriptionTableWrapper::wrap(
    ACPI::ExtendedSystemDescriptionTable* src) {
    return reinterpret_cast<ExtendedSystemDescriptionTableWrapper*>(src);
}
