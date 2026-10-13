#pragma once
#include <cstdint>
#include <cstddef>

namespace oz {

class BlockDevice {
public:
    virtual ~BlockDevice() = default;

    // Read specified number of sectors starting from Logical Block Address (LBA)
    virtual bool readSectors(std::uint64_t lba, std::uint32_t count, void* buffer) = 0;

    // Write specified number of sectors starting from Logical Block Address (LBA)
    virtual bool writeSectors(std::uint64_t lba, std::uint32_t count, const void* buffer) = 0;

    // Get the size of a single sector in bytes (usually 512)
    virtual std::uint32_t getSectorSize() const = 0;
};

} // namespace oz
