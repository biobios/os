#pragma once

#include <cstdint>

#include "Graphics.hpp"
#include "Shell.hpp"

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
    void* RSDP;
    BootMemoryMap memory_map;
};

namespace oz {
class Kernel {
    Graphics g;
    Shell sh;

   public:
    Kernel(PlatformInfo* platformInfo);
    void run();
};
}  // namespace oz