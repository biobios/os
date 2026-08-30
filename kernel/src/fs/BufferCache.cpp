#include "fs/BufferCache.hpp"
#include "utils/utils.hpp"
#include <new>

namespace oz {

BufferCacheManager& getBufferCache() {
    static BufferCacheManager instance;
    return instance;
}

BufferCacheManager::BufferCacheManager() 
    : lru_head(nullptr), lru_tail(nullptr), free_list(nullptr),
      cache_memory(nullptr), buffer_heads(nullptr) 
{
    oz::utils::memset(hash_table, 0, sizeof(hash_table));
}

BufferCacheManager::~BufferCacheManager() {
    sync(); // Flush all dirty blocks
    delete[] cache_memory;
    delete[] buffer_heads;
}

void BufferCacheManager::init() {
    // Already initialized check
    if (cache_memory != nullptr) return;

    cache_memory = new std::uint8_t[MAX_CACHE_BLOCKS * CACHE_BLOCK_SIZE];
    buffer_heads = new BufferHead[MAX_CACHE_BLOCKS];
    
    oz::utils::memset(hash_table, 0, sizeof(hash_table));
    lru_head = nullptr;
    lru_tail = nullptr;
    
    // Initialize free list
    free_list = &buffer_heads[0];
    for (std::size_t i = 0; i < MAX_CACHE_BLOCKS; ++i) {
        buffer_heads[i].data = cache_memory + (i * CACHE_BLOCK_SIZE);
        buffer_heads[i].device = nullptr;
        buffer_heads[i].dirty = false;
        buffer_heads[i].ref_count = 0;
        buffer_heads[i].hash_next = nullptr;
        buffer_heads[i].lru_prev = nullptr;
        
        if (i < MAX_CACHE_BLOCKS - 1) {
            buffer_heads[i].lru_next = &buffer_heads[i + 1];
        } else {
            buffer_heads[i].lru_next = nullptr;
        }
    }
}

std::size_t BufferCacheManager::hash(BlockDevice* device, std::uint64_t block_index) {
    std::uintptr_t dev_val = reinterpret_cast<std::uintptr_t>(device);
    return ((dev_val >> 4) ^ block_index ^ (block_index >> 8)) % HASH_TABLE_SIZE;
}

BufferHead* BufferCacheManager::find(BlockDevice* device, std::uint64_t block_index) {
    std::size_t h = hash(device, block_index);
    BufferHead* bh = hash_table[h];
    while (bh) {
        if (bh->device == device && bh->block_index == block_index) {
            return bh;
        }
        bh = bh->hash_next;
    }
    return nullptr;
}

void BufferCacheManager::insertHash(BufferHead* bh) {
    std::size_t h = hash(bh->device, bh->block_index);
    bh->hash_next = hash_table[h];
    hash_table[h] = bh;
}

void BufferCacheManager::removeHash(BufferHead* bh) {
    std::size_t h = hash(bh->device, bh->block_index);
    BufferHead* curr = hash_table[h];
    BufferHead* prev = nullptr;
    
    while (curr) {
        if (curr == bh) {
            if (prev) {
                prev->hash_next = curr->hash_next;
            } else {
                hash_table[h] = curr->hash_next;
            }
            bh->hash_next = nullptr;
            return;
        }
        prev = curr;
        curr = curr->hash_next;
    }
}

void BufferCacheManager::removeLRU(BufferHead* bh) {
    if (bh->lru_prev) {
        bh->lru_prev->lru_next = bh->lru_next;
    } else {
        lru_head = bh->lru_next;
    }
    
    if (bh->lru_next) {
        bh->lru_next->lru_prev = bh->lru_prev;
    } else {
        lru_tail = bh->lru_prev;
    }
    
    bh->lru_prev = nullptr;
    bh->lru_next = nullptr;
}

void BufferCacheManager::insertLRU(BufferHead* bh) { // Insert at head (MRU)
    bh->lru_next = lru_head;
    bh->lru_prev = nullptr;
    
    if (lru_head) {
        lru_head->lru_prev = bh;
    }
    lru_head = bh;
    
    if (!lru_tail) {
        lru_tail = bh;
    }
}

void BufferCacheManager::moveToMRU(BufferHead* bh) {
    removeLRU(bh);
    insertLRU(bh);
}

void BufferCacheManager::evictOne() {
    // Find the oldest block from tail
    BufferHead* bh = lru_tail;
    while (bh != nullptr) {
        if (bh->ref_count == 0) {
            break;
        }
        bh = bh->lru_prev;
    }
    
    if (!bh) {
        // All blocks are in use! This is a critical situation.
        // For a simple implementation, we might panic or just return.
        // Assuming we always have enough blocks for now.
        return;
    }
    
    if (bh->dirty) {
        writeBack(bh);
    }
    
    removeHash(bh);
    removeLRU(bh);
    
    bh->device = nullptr;
    bh->dirty = false;
    
    // Put back to free list
    bh->lru_next = free_list;
    free_list = bh;
}

BufferHead* BufferCacheManager::getFreeBlock() {
    if (!free_list) {
        evictOne();
    }
    
    if (free_list) {
        BufferHead* bh = free_list;
        free_list = free_list->lru_next;
        bh->lru_next = nullptr;
        return bh;
    }
    
    return nullptr;
}

bool BufferCacheManager::writeBack(BufferHead* bh) {
    if (!bh->dirty || !bh->device) return true;
    
    std::uint32_t sector_size = bh->device->getSectorSize();
    std::uint32_t sectors_per_block = CACHE_BLOCK_SIZE / sector_size;
    std::uint64_t start_lba = bh->block_index * sectors_per_block;
    
    bool result = bh->device->writeSectors(start_lba, sectors_per_block, bh->data);
    if (result) {
        bh->dirty = false;
    }
    return result;
}

bool BufferCacheManager::readFromDevice(BufferHead* bh) {
    if (!bh->device) return false;
    
    std::uint32_t sector_size = bh->device->getSectorSize();
    std::uint32_t sectors_per_block = CACHE_BLOCK_SIZE / sector_size;
    std::uint64_t start_lba = bh->block_index * sectors_per_block;
    
    bool result = bh->device->readSectors(start_lba, sectors_per_block, bh->data);
    return result;
}

bool BufferCacheManager::readBlock(BlockDevice* device, std::uint64_t lba, std::uint32_t count, void* buffer) {
    if (!device || count == 0) return false;
    
    std::uint32_t sector_size = device->getSectorSize();
    if (sector_size == 0) return false;
    
    std::uint32_t sectors_per_block = CACHE_BLOCK_SIZE / sector_size;
    std::uint64_t current_lba = lba;
    std::uint64_t end_lba = lba + count;
    std::uint8_t* out_buf = static_cast<std::uint8_t*>(buffer);
    
    while (current_lba < end_lba) {
        std::uint64_t block_index = current_lba / sectors_per_block;
        std::uint32_t offset_in_block = current_lba % sectors_per_block;
        std::uint32_t sectors_to_process = end_lba - current_lba;
        if (sectors_to_process > sectors_per_block - offset_in_block) {
            sectors_to_process = sectors_per_block - offset_in_block;
        }
        
        BufferHead* bh = find(device, block_index);
        if (!bh) {
            bh = getFreeBlock();
            if (!bh) return false;
            
            bh->device = device;
            bh->block_index = block_index;
            bh->dirty = false;
            bh->ref_count = 1; // Pin
            insertHash(bh);
            insertLRU(bh);
            
            if (!readFromDevice(bh)) {
                bh->ref_count = 0;
                return false;
            }
        } else {
            bh->ref_count++; // Pin
            moveToMRU(bh);
        }
        
        oz::utils::memcpy(out_buf, bh->data + (offset_in_block * sector_size), sectors_to_process * sector_size);
        
        bh->ref_count--; // Unpin
        
        current_lba += sectors_to_process;
        out_buf += sectors_to_process * sector_size;
    }
    
    return true;
}

bool BufferCacheManager::writeBlock(BlockDevice* device, std::uint64_t lba, std::uint32_t count, const void* buffer) {
    if (!device || count == 0) return false;
    
    std::uint32_t sector_size = device->getSectorSize();
    if (sector_size == 0) return false;
    
    std::uint32_t sectors_per_block = CACHE_BLOCK_SIZE / sector_size;
    std::uint64_t current_lba = lba;
    std::uint64_t end_lba = lba + count;
    const std::uint8_t* in_buf = static_cast<const std::uint8_t*>(buffer);
    
    while (current_lba < end_lba) {
        std::uint64_t block_index = current_lba / sectors_per_block;
        std::uint32_t offset_in_block = current_lba % sectors_per_block;
        std::uint32_t sectors_to_process = end_lba - current_lba;
        if (sectors_to_process > sectors_per_block - offset_in_block) {
            sectors_to_process = sectors_per_block - offset_in_block;
        }
        
        BufferHead* bh = find(device, block_index);
        if (!bh) {
            bh = getFreeBlock();
            if (!bh) return false;
            
            bh->device = device;
            bh->block_index = block_index;
            bh->dirty = false;
            bh->ref_count = 1; // Pin
            insertHash(bh);
            insertLRU(bh);
            
            // If we are not overwriting the whole block, we must read the old data first (Read-Modify-Write)
            if (sectors_to_process < sectors_per_block) {
                if (!readFromDevice(bh)) {
                    bh->ref_count = 0;
                    return false;
                }
            }
        } else {
            bh->ref_count++; // Pin
            moveToMRU(bh);
        }
        
        oz::utils::memcpy(bh->data + (offset_in_block * sector_size), in_buf, sectors_to_process * sector_size);
        bh->dirty = true;
        
        bh->ref_count--; // Unpin
        
        current_lba += sectors_to_process;
        in_buf += sectors_to_process * sector_size;
    }
    
    return true;
}

void BufferCacheManager::sync(BlockDevice* device) {
    BufferHead* bh = lru_head;
    while (bh) {
        if (bh->dirty && (device == nullptr || bh->device == device)) {
            writeBack(bh);
        }
        bh = bh->lru_next;
    }
}

} // namespace oz
