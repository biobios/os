#pragma once
#include <cstddef>
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
}  // namespace oz