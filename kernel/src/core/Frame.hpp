#pragma once
#include "core/Capability.hpp"
#include <cstdint>

namespace oz {

class Frame : public KernelObject {
private:
    std::uintptr_t phys_addr_;
    std::size_t size_;  // in bytes
    bool is_device_;    // true for MMIO

    // For cleanup when Frame is destroyed (free physical pages)
    void* cleanup_data_;
    void (*cleanup_fn_)(void*);

public:
    Frame(std::uintptr_t phys, std::size_t size, bool device,
          void* cleanup_data = nullptr, void (*cleanup_fn)(void*) = nullptr,
          bool untyped = false)
        : KernelObject(ObjectType::Frame, untyped),
          phys_addr_(phys), size_(size), is_device_(device),
          cleanup_data_(cleanup_data), cleanup_fn_(cleanup_fn) {}
    
    ~Frame() override {
        if (cleanup_fn_) cleanup_fn_(cleanup_data_);
    }

    std::uintptr_t physicalAddress() const { return phys_addr_; }
    std::size_t size() const { return size_; }
    bool isDevice() const { return is_device_; }
};

} // namespace oz
