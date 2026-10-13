#pragma once
#include "core/Capability.hpp"
#include "core/Frame.hpp"
#include <cstdint>

namespace oz {

class UntypedMemory : public KernelObject {
public:
    // Function pointer types for allocating/freeing physical pages
    // These are set up by Kernel.hpp where FrameManager template params are known
    using AllocFn = std::uintptr_t (*)(std::size_t num_pages, void** cleanup_data);
    using FreeFn = void (*)(void* cleanup_data);

private:
    std::size_t remaining_pages_;
    AllocFn alloc_fn_;
    FreeFn free_fn_;

public:
    UntypedMemory(std::size_t num_pages, AllocFn alloc_fn, FreeFn free_fn)
        : KernelObject(ObjectType::UntypedMemory),
          remaining_pages_(num_pages), alloc_fn_(alloc_fn), free_fn_(free_fn) {}

    std::size_t remainingPages() const { return remaining_pages_; }

    // Retype into a Frame (allocate physical pages)
    Frame* retypeFrame(std::size_t num_pages) {
        if (num_pages == 0 || num_pages > remaining_pages_) return nullptr;
        if (!alloc_fn_) return nullptr;

        void* cleanup_data = nullptr;
        std::uintptr_t phys = alloc_fn_(num_pages, &cleanup_data);
        if (phys == 0) return nullptr;

        remaining_pages_ -= num_pages;
        return new Frame(phys, num_pages * 4096, false, cleanup_data, free_fn_);
    }
};

} // namespace oz
