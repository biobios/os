#pragma once
#include "drivers/storage/BlockDevice.hpp"
#include "utils/utils.hpp"

namespace oz {

class RamDisk : public BlockDevice {
private:
    std::uint8_t* data;
    std::size_t size;
    std::uint32_t sector_size;
    bool owns_data;

public:
    RamDisk(std::size_t size_in_bytes, std::uint32_t sector_size = 512) 
        : size(size_in_bytes), sector_size(sector_size), owns_data(true) {
        data = new std::uint8_t[size_in_bytes];
        oz::utils::memset(data, 0, size_in_bytes);
    }

    // Convenience constructor for passing pre-loaded data without copying
    RamDisk(void* initial_data, std::size_t size_in_bytes, std::uint32_t sector_size = 512)
        : data(static_cast<std::uint8_t*>(initial_data)), size(size_in_bytes), 
          sector_size(sector_size), owns_data(false) {}

    ~RamDisk() override {
        if (owns_data) {
            delete[] data;
        }
    }

    bool readSectors(std::uint64_t lba, std::uint32_t count, void* buffer) override {
        std::size_t offset = lba * sector_size;
        std::size_t bytes_to_read = count * sector_size;
        
        if (offset + bytes_to_read > size) {
            return false;
        }

        oz::utils::memcpy(buffer, data + offset, bytes_to_read);
        return true;
    }

    bool writeSectors(std::uint64_t lba, std::uint32_t count, const void* buffer) override {
        std::size_t offset = lba * sector_size;
        std::size_t bytes_to_write = count * sector_size;
        
        if (offset + bytes_to_write > size) {
            return false;
        }

        oz::utils::memcpy(data + offset, buffer, bytes_to_write);
        return true;
    }

    std::uint32_t getSectorSize() const override {
        return sector_size;
    }
    
    std::uint8_t* getRawData() {
        return data;
    }
    
    std::size_t getSize() const {
        return size;
    }
};

} // namespace oz
