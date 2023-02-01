#pragma once

#include <new>
#include <cstddef>
#include <cstdint>

namespace ozstd
{
    struct nothrow_t{};
    extern const nothrow_t nothrow;
}

[[nodiscard]] void* operator new(std::size_t size);
[[nodiscard]] void* operator new(std::size_t size, std::align_val_t alignment);
[[nodiscard]] void* operator new(std::size_t size, const ozstd::nothrow_t&)noexcept;
[[nodiscard]] void* operator new(std::size_t size, std::align_val_t alignment, const ozstd::nothrow_t&)noexcept;
[[nodiscard]] void* operator new(std::size_t size, void* ptr)noexcept;

void operator delete(void* ptr) noexcept;//1
void operator delete(void* ptr, std::size_t size)noexcept;//2
void operator delete(void* ptr, std::align_val_t alignment)noexcept;//3
void operator delete(void* ptr, std::size_t size, std::align_val_t alignment)noexcept;//4
void operator delete(void* ptr, const ozstd::nothrow_t&)noexcept;//5
void operator delete(void* ptr, std::align_val_t alignment, const ozstd::nothrow_t&) noexcept;//6
void operator delete(void* ptr, void*)noexcept;//7