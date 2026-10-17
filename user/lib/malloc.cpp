#include "syscall.hpp"

namespace {
    std::uint8_t* heap_ptr = nullptr;
    std::uint64_t heap_remaining = 0;
    std::uint64_t heap_virt = 0x100000000000; // Virtual address bump pointer for heap
    
    static constexpr std::uint32_t CAP_SELF_VSPACE = 0;
    static constexpr std::uint32_t CAP_UNTYPED = 240;
}

void* malloc(std::size_t size) {
    if (size == 0) return nullptr;
    
    // align to 16 bytes
    size = (size + 15) & ~15ULL;
    
    if (heap_remaining < size) {
        std::uint64_t pages = (size + 4095) / 4096;
        if (pages < 16) pages = 16; // alloc chunks of 16 pages (64KB)
        
        // Allocate Frame from UntypedMemory capability
        std::uint32_t frame_cap = oz::sys_frame_alloc(CAP_UNTYPED, pages);
        if (frame_cap == 0xFFFFFFFF) return nullptr;
        
        // Map Frame into our VSpace
        if (oz::sys_vspace_map_with_pagetables(CAP_SELF_VSPACE, CAP_UNTYPED, frame_cap, heap_virt, 0x01, pages * 4096) != 0) {
            return nullptr; // 0x01 = writable
        }
        
        heap_ptr = reinterpret_cast<std::uint8_t*>(heap_virt);
        heap_remaining = pages * 4096;
        heap_virt += pages * 4096;
    }
    
    void* ret = heap_ptr;
    heap_ptr += size;
    heap_remaining -= size;
    return ret;
}

void free(void* ptr) {
    // dummy
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
