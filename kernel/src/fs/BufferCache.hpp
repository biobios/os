#pragma once
#include <cstdint>
#include <cstddef>
#include "drivers/storage/BlockDevice.hpp"

namespace oz {

constexpr std::size_t CACHE_BLOCK_SIZE = 4096; // 4KB page size
constexpr std::size_t HASH_TABLE_SIZE = 256;
constexpr std::size_t MAX_CACHE_BLOCKS = 1024; // 4MB cache

struct BufferHead {
    BlockDevice* device;
    std::uint64_t block_index; // LBA aligned to CACHE_BLOCK_SIZE (LBA / sectors_per_block)
    std::uint8_t* data;
    
    bool dirty;
    std::uint32_t ref_count;
    
    BufferHead* hash_next;
    BufferHead* lru_prev;
    BufferHead* lru_next;
};

class BufferCacheManager {
private:
    BufferHead* hash_table[HASH_TABLE_SIZE];
    BufferHead* lru_head;
    BufferHead* lru_tail;
    
    BufferHead* free_list;
    
    std::uint8_t* cache_memory;
    BufferHead* buffer_heads;

    std::size_t hash(BlockDevice* device, std::uint64_t block_index);
    BufferHead* find(BlockDevice* device, std::uint64_t block_index);
    void insertHash(BufferHead* bh);
    void removeHash(BufferHead* bh);
    
    void moveToMRU(BufferHead* bh);
    void removeLRU(BufferHead* bh);
    void insertLRU(BufferHead* bh); // inserts at MRU (head)
    
    BufferHead* getFreeBlock();
    void evictOne();
    
    bool writeBack(BufferHead* bh);
    bool readFromDevice(BufferHead* bh);

public:
    BufferCacheManager();
    ~BufferCacheManager();
    
    void init();
    
    // Equivalent to BlockDevice::readSectors / writeSectors but cached
    bool readBlock(BlockDevice* device, std::uint64_t lba, std::uint32_t count, void* buffer);
    bool writeBlock(BlockDevice* device, std::uint64_t lba, std::uint32_t count, const void* buffer);
    
    void sync(BlockDevice* device = nullptr);
};

// Global instance getter
BufferCacheManager& getBufferCache();

} // namespace oz
