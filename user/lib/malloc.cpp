#include "syscall.hpp"

namespace {
    struct alignas(16) BlockHeader {
        std::size_t size;          // Payload size (16-byte aligned)
        bool is_free;              // Whether this block is free
        std::uint8_t padding[7];   // Padding to maintain 16-byte alignment
        BlockHeader* next;         // Next block in memory list
        BlockHeader* prev;         // Previous block in memory list
        std::uint64_t magic;       // Integrity verification magic
    };
    static_assert(sizeof(BlockHeader) == 48 && sizeof(BlockHeader) % 16 == 0, "BlockHeader size must be 48 bytes (16-byte aligned)");

    constexpr std::uint64_t BLOCK_MAGIC = 0x4F5A4D414C4C4F43ULL; // "OZMALLOC"

    BlockHeader* block_list_head = nullptr;
    BlockHeader* block_list_tail = nullptr;

    std::uint64_t heap_virt = 0x008000000000ULL; // Virtual address bump pointer for heap (PML4[1])
    
    static constexpr std::uint32_t CAP_SELF_VSPACE = 0;
    static constexpr std::uint32_t CAP_UNTYPED = 240;

    // Helper to check if block B immediately follows block A in virtual address space
    inline bool are_physically_adjacent(const BlockHeader* a, const BlockHeader* b) {
        if (!a || !b) return false;
        const auto* a_end = reinterpret_cast<const std::uint8_t*>(a) + sizeof(BlockHeader) + a->size;
        return a_end == reinterpret_cast<const std::uint8_t*>(b);
    }
}

void* malloc(std::size_t size) {
    if (size == 0) return nullptr;
    
    // align to 16 bytes
    size = (size + 15) & ~15ULL;

    // 1. First-fit search in existing blocks
    BlockHeader* curr = block_list_head;
    while (curr) {
        if (curr->magic == BLOCK_MAGIC && curr->is_free && curr->size >= size) {
            // Split block if remaining space can hold a new BlockHeader + at least 16 bytes payload
            if (curr->size >= size + sizeof(BlockHeader) + 16) {
                std::size_t remaining_size = curr->size - size - sizeof(BlockHeader);
                auto* new_block = reinterpret_cast<BlockHeader*>(
                    reinterpret_cast<std::uint8_t*>(curr) + sizeof(BlockHeader) + size
                );
                new_block->size = remaining_size;
                new_block->is_free = true;
                new_block->magic = BLOCK_MAGIC;
                new_block->next = curr->next;
                new_block->prev = curr;
                if (curr->next) {
                    curr->next->prev = new_block;
                } else {
                    block_list_tail = new_block;
                }
                curr->next = new_block;
                curr->size = size;
            }
            curr->is_free = false;
            return reinterpret_cast<void*>(reinterpret_cast<std::uint8_t*>(curr) + sizeof(BlockHeader));
        }
        curr = curr->next;
    }

    // 2. Allocate new memory chunk from UntypedMemory
    std::size_t total_needed = size + sizeof(BlockHeader);
    std::uint64_t pages = (total_needed + 4095) / 4096;
    if (pages < 16) pages = 16; // alloc chunks of 16 pages (64KB)
    std::uint64_t alloc_bytes = pages * 4096;

    // Allocate Frame batch from UntypedMemory capability
    std::uint32_t start_slot = oz::sys_frame_alloc(CAP_UNTYPED, 0, pages);
    if (start_slot == 0xFFFFFFFF) return nullptr;

    // Map each 4KB Frame into our VSpace
    for (std::uint64_t i = 0; i < pages; ++i) {
        if (oz::sys_vspace_map_with_pagetables(CAP_SELF_VSPACE, CAP_UNTYPED, start_slot + i, heap_virt + i * 4096, 0x01, 4096) != 0) {
            return nullptr; // 0x01 = writable
        }
    }

    auto* new_chunk = reinterpret_cast<BlockHeader*>(heap_virt);
    heap_virt += alloc_bytes;

    new_chunk->size = alloc_bytes - sizeof(BlockHeader);
    new_chunk->is_free = true;
    new_chunk->magic = BLOCK_MAGIC;
    new_chunk->next = nullptr;
    new_chunk->prev = block_list_tail;

    if (block_list_tail) {
        block_list_tail->next = new_chunk;
    } else {
        block_list_head = new_chunk;
    }
    block_list_tail = new_chunk;

    // Check if new chunk can be coalesced with previous adjacent free block
    if (new_chunk->prev && new_chunk->prev->magic == BLOCK_MAGIC && new_chunk->prev->is_free &&
        are_physically_adjacent(new_chunk->prev, new_chunk)) {
        BlockHeader* prev_block = new_chunk->prev;
        prev_block->size += sizeof(BlockHeader) + new_chunk->size;
        prev_block->next = nullptr;
        block_list_tail = prev_block;
        new_chunk->magic = 0;
        new_chunk = prev_block;
    }

    // Split block if remaining space is sufficient
    if (new_chunk->size >= size + sizeof(BlockHeader) + 16) {
        std::size_t remaining_size = new_chunk->size - size - sizeof(BlockHeader);
        auto* split_block = reinterpret_cast<BlockHeader*>(
            reinterpret_cast<std::uint8_t*>(new_chunk) + sizeof(BlockHeader) + size
        );
        split_block->size = remaining_size;
        split_block->is_free = true;
        split_block->magic = BLOCK_MAGIC;
        split_block->next = nullptr;
        split_block->prev = new_chunk;
        new_chunk->next = split_block;
        block_list_tail = split_block;
        new_chunk->size = size;
    }

    new_chunk->is_free = false;
    return reinterpret_cast<void*>(reinterpret_cast<std::uint8_t*>(new_chunk) + sizeof(BlockHeader));
}

void free(void* ptr) {
    if (!ptr) return;

    auto* bh = reinterpret_cast<BlockHeader*>(static_cast<std::uint8_t*>(ptr) - sizeof(BlockHeader));
    if (bh->magic != BLOCK_MAGIC) {
        return; // Corrupted or invalid pointer
    }

    bh->is_free = true;

    // Coalesce with next block if free and physically adjacent
    if (bh->next && bh->next->magic == BLOCK_MAGIC && bh->next->is_free && are_physically_adjacent(bh, bh->next)) {
        BlockHeader* next_block = bh->next;
        bh->size += sizeof(BlockHeader) + next_block->size;
        bh->next = next_block->next;
        if (next_block->next) {
            next_block->next->prev = bh;
        } else {
            block_list_tail = bh;
        }
        next_block->magic = 0; // Invalidate old header
    }

    // Coalesce with prev block if free and physically adjacent
    if (bh->prev && bh->prev->magic == BLOCK_MAGIC && bh->prev->is_free && are_physically_adjacent(bh->prev, bh)) {
        BlockHeader* prev_block = bh->prev;
        prev_block->size += sizeof(BlockHeader) + bh->size;
        prev_block->next = bh->next;
        if (bh->next) {
            bh->next->prev = prev_block;
        } else {
            block_list_tail = prev_block;
        }
        bh->magic = 0; // Invalidate old header
    }
}

void* operator new(std::size_t size) { return malloc(size); }
void* operator new[](std::size_t size) { return malloc(size); }
void operator delete(void* ptr) noexcept { free(ptr); }
void operator delete[](void* ptr) noexcept { free(ptr); }
void operator delete(void* ptr, std::size_t size) noexcept { free(ptr); }
void operator delete[](void* ptr, std::size_t size) noexcept { free(ptr); }

extern "C" void __cxa_pure_virtual() {
    while (1);
}

extern "C" void* memset(void* dest, int val, std::size_t count) {
    std::uint8_t* d = static_cast<std::uint8_t*>(dest);
    while (count--) *d++ = val;
    return dest;
}

extern "C" void* memcpy(void* dest, const void* src, std::size_t count) {
    char* d = static_cast<char*>(dest);
    const char* s = static_cast<const char*>(src);
    while (count--) *d++ = *s++;
    return dest;
}
