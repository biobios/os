#pragma once
#include "core/Capability.hpp"
#include "core/Thread.hpp"
#include "core/TCB.hpp"
#include "memory/Address.hpp"
#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <new>

namespace oz {

namespace detail {

inline void* untyped_bump_allocate(std::uintptr_t phys_base, std::size_t size_bytes,
                                   std::size_t& watermark, std::size_t bytes,
                                   std::size_t alignment, std::uintptr_t& out_phys) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return nullptr;
    if (watermark > size_bytes) return nullptr;
    std::size_t mask = alignment - 1;
    if (watermark > (~static_cast<std::size_t>(0) - mask)) return nullptr;
    std::size_t aligned_watermark = (watermark + mask) & ~mask;
    if (aligned_watermark > size_bytes || bytes > (size_bytes - aligned_watermark)) {
        return nullptr;
    }
    out_phys = phys_base + aligned_watermark;
    watermark = aligned_watermark + bytes;
    return reinterpret_cast<void*>(out_phys + DIRECT_MAP_OFFSET);
}

inline TCB* construct_tcb_in_block(void* block_mem, void (*queue_thread_fn)(Thread*)) {
    if (!block_mem) return nullptr;
    std::uint8_t* base = reinterpret_cast<std::uint8_t*>(block_mem);
    void* tcb_mem = base;
    void* thread_mem = base + 64;
    void* stack_base = base + 512;
    std::size_t stack_size = 8192 - 512;

    Thread* thread = new (thread_mem) Thread(
        0, stack_base, stack_size, PageBlock<>{nullptr}, 0
    );
    thread->is_untyped_backed = true;

    return new (tcb_mem) TCB(thread, true, queue_thread_fn, true);
}

} // namespace detail

class UntypedMemory {
private:
    std::uintptr_t phys_base_{0};
    std::size_t size_{0};
    std::size_t watermark_{0};
    bool is_device_{false};

public:
    UntypedMemory() = default;
    UntypedMemory(std::uintptr_t phys_base, std::size_t size,
                  void* cleanup_data = nullptr, void (*free_fn)(void*) = nullptr,
                  bool is_device = false, bool untyped = false)
        : phys_base_(phys_base), size_(size), watermark_(0), is_device_(is_device) {
        (void)cleanup_data; (void)free_fn; (void)untyped;
    }

    explicit UntypedMemory(Capability* slot) {
        if (slot && slot->type() == CapType::UntypedMemory) {
            phys_base_ = slot->untyped_phys_base();
            size_      = slot->untyped_size();
            watermark_ = slot->untyped_watermark();
            is_device_ = slot->untyped_is_device();
        }
    }

    void syncToSlot(Capability* slot) const {
        if (slot && slot->type() == CapType::UntypedMemory) {
            slot->data[0] = phys_base_;
            slot->data[1] = size_;
            slot->set_untyped_watermark(watermark_);
            slot->data[3] = is_device_ ? 1 : 0;
        }
    }

    std::uintptr_t physicalBase() const { return phys_base_; }
    std::size_t totalBytes() const { return size_; }
    std::size_t watermark() const { return watermark_; }
    std::size_t remainingBytes() const {
        return (watermark_ < size_) ? (size_ - watermark_) : 0;
    }
    std::size_t remainingPages() const { return remainingBytes() / 4096; }
    bool isDevice() const { return is_device_; }

    void release() {} // 互換用 no-op

    void* allocateBytes(std::size_t bytes, std::size_t alignment, std::uintptr_t& out_phys) {
        return detail::untyped_bump_allocate(phys_base_, size_, watermark_, bytes, alignment, out_phys);
    }

    template <typename T, typename... Args>
    T* allocateObject(Args&&... args) {
        std::uintptr_t obj_phys = 0;
        void* obj_mem = allocateBytes(sizeof(T), alignof(T), obj_phys);
        if (!obj_mem) return nullptr;
        return new (obj_mem) T(static_cast<Args&&>(args)...);
    }

    TCB* retypeTCB(void (*queue_thread_fn)(Thread*) = nullptr) {
        std::uintptr_t block_phys = 0;
        void* block_mem = allocateBytes(8192, 4096, block_phys);
        return detail::construct_tcb_in_block(block_mem, queue_thread_fn);
    }
};

inline void* untyped_allocate_bytes(Capability* slot, std::size_t bytes, std::size_t alignment, std::uintptr_t& out_phys) {
    if (!slot || slot->type() != CapType::UntypedMemory) return nullptr;
    std::size_t watermark = slot->untyped_watermark();
    void* mem = detail::untyped_bump_allocate(
        slot->untyped_phys_base(), slot->untyped_size(), watermark, bytes, alignment, out_phys);
    if (mem) {
        slot->set_untyped_watermark(watermark);
    }
    return mem;
}

template <typename T, typename... Args>
inline T* untyped_allocate_object(Capability* slot, Args&&... args) {
    std::uintptr_t obj_phys = 0;
    void* obj_mem = untyped_allocate_bytes(slot, sizeof(T), alignof(T), obj_phys);
    if (!obj_mem) return nullptr;
    return new (obj_mem) T(static_cast<Args&&>(args)...);
}

inline TCB* untyped_retype_tcb(Capability* slot, void (*queue_thread_fn)(Thread*) = nullptr) {
    if (!slot || slot->type() != CapType::UntypedMemory) return nullptr;
    std::uintptr_t block_phys = 0;
    void* block_mem = untyped_allocate_bytes(slot, 8192, 4096, block_phys);
    return detail::construct_tcb_in_block(block_mem, queue_thread_fn);
}

} // namespace oz
