#pragma once

#include <cstdint>
#include "ACPI.hpp"

namespace oz_boot{
    struct BootMemoryMap
    {
        std::uint64_t buffer_size;
        void* buffer;
        std::uint64_t map_size;
        std::uint64_t map_key;
        std::uint64_t descriptor_size;
        std::uint32_t descriptor_version;
    };

    enum class EFI_MEMORY_TYPE{
        EfiReservedMemoryType,
        EfiLoaderCode,
        EfiLoaderData,
        EfiBootServicesCode,
        EfiBootServicesData,
        EfiRuntimeServicesCode,
        EfiRuntimeServicesData,
        EfiConventionalMemory,
        EfiUnusableMemory,
        EfiACPIReclaimMemory,
        EfiACPIMemoryNVS,
        EfiMemoryMappedIO,
        EfiMemoryMappedIOPortSpace,
        EfiPalCode,
        EfiPersistentMemory,
        EfiUnacceptedMemoryType,
        EfiMaxMemoryType
    };

    struct EFI_MEMORY_DESCRIPTOR
    {
        std::uint32_t Type;
        std::uint64_t PhysicalStart;
        std::uint64_t VirtualStart;
        std::uint64_t NumberOfPages;
        std::uint64_t Attribute;
    };

    struct PlatformInfo {
        void* frame_buffer_base;
        std::uint64_t frame_buffer_size;
        std::uint32_t frame_buffer_horizontal;
        std::uint32_t frame_buffer_vertical;
        ACPI::RootSystemDescriptionPointer* RSDP;
        BootMemoryMap memory_map;
    };

    inline bool isAvailable(std::uint32_t type){
        return  type == static_cast<std::uint32_t>(EFI_MEMORY_TYPE::EfiBootServicesCode) ||
                type == static_cast<std::uint32_t>(EFI_MEMORY_TYPE::EfiBootServicesData) ||
                type == static_cast<std::uint32_t>(EFI_MEMORY_TYPE::EfiConventionalMemory);
    }
}