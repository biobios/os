#pragma once
#include <cstddef>
#include <memory>
#include <utility>
#include <concepts>

namespace oz {
template <typename T>
concept kernel_memory_allocator = requires(T allocator) {
    { allocator.malloc(std::declval<std::size_t>()) } -> std::same_as<void*>;
    { allocator.free(std::declval<void*>()) } -> std::same_as<void>;
};

template <typename Accessor>
concept kernel_memory_allocator_accessor = requires() {
    typename Accessor::Settings::KernelMemoryAllocator;
    {Accessor::getKernelMemoryAllocator()} -> std::same_as<typename Accessor::Settings::KernelMemoryAllocator&>;
} && kernel_memory_allocator<typename Accessor::Settings::KernelMemoryAllocator>;

template <kernel_memory_allocator_accessor Accessor>
struct KMallocDeleter {
    using Allocator = typename Accessor::Settings::KernelMemoryAllocator;

    static void operator()(void* ptr) {
        Allocator& allocator = Accessor::getKernelMemoryAllocator();
        allocator.free(ptr);
    }
};

template <typename T, kernel_memory_allocator_accessor Accessor>
using kmalloc_unique_ptr = std::unique_ptr<T, KMallocDeleter<Accessor>>;

template <typename T, kernel_memory_allocator_accessor Accessor, typename... Args>
    requires std::constructible_from<T, Args...>
kmalloc_unique_ptr<T, Accessor> make_kmalloc_unique(Args&&... args) {
    using Allocator = typename Accessor::Settings::KernelMemoryAllocator;
    Allocator& allocator = Accessor::getKernelMemoryAllocator();
    void* raw_ptr = allocator.malloc(sizeof(T));
    if (!raw_ptr) {
        return kmalloc_unique_ptr<T, Accessor>(nullptr);
    }
    T* obj_ptr = new (raw_ptr) T(std::forward<Args>(args)...);
    return kmalloc_unique_ptr<T, Accessor>(obj_ptr);
}

}  // namespace oz