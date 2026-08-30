#pragma once

#include "memory/IKernelMemoryAllocator.hpp"

namespace oz {

void setMallocAndFree(void* (*malloc_func)(std::size_t), void (*free_func)(void*));

void setKernelMemoryAllocator(kernel_memory_allocator_accessor auto accessor) {
    struct MallocFree {
        static void* malloc(std::size_t size) {
            return decltype(accessor){}.getKernelMemoryAllocator().malloc(size);
        }

        static void free(void* ptr) {
            decltype(accessor){}.getKernelMemoryAllocator().free(ptr);
        }
    };
    setMallocAndFree(&MallocFree::malloc, &MallocFree::free);
}

} // namespace oz