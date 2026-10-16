#pragma once
#include <cstddef>
#include <cstdint>

#include "memory/Address.hpp"
#include "memory/IFrameManager.hpp"
#include "core/bootStructures.hpp"
#include "memory/paging.hpp"

namespace oz {

namespace x86_64 {

class FrameManager : public PhysicalAddressProvider {
public:
    static constexpr std::uint8_t MAX_LEVEL = 18; // 2^18 * 4KB = 1GB
    const std::size_t FRAME_SIZE;

private:
    oz_boot::BootMemoryMap* memmap_{nullptr};
    std::uintptr_t current_phys_{0};
    std::uintptr_t end_phys_{0};
    std::size_t desc_index_{0};
    std::size_t total_page_count_{0};

    // Minimal descriptor pool for early boot allocations only (no gigantic array!)
    static constexpr std::size_t MAX_BOOT_DESCRIPTORS = 128;
    PageFrameDescriptor boot_descriptors_[MAX_BOOT_DESCRIPTORS];
    std::size_t desc_alloc_count_{0};

    bool advanceToNextRegion();

public:
    FrameManager(oz_boot::BootMemoryMap* memmap, std::size_t frame_size);

    PageBlock<> allocateBlock(std::uint8_t level);
    void freeBlock(PageBlock<> block);
    void freeBlock(PageFrameDescriptor* pfd);

    PhysicalAddress<void> getPhysicalAddress(PageBlock<> block) const;
    PhysicalAddress<void> getPhysicalAddress(const PageFrameDescriptor* pfd) const;
    PageFrameDescriptor* getDescriptor(PhysicalAddress<void> phys) const;

    PageFrameDescriptor* allocatePages(std::size_t frame_length);
    void freePages(PageFrameDescriptor* returnedFrame, std::size_t frame_length);

    std::size_t getTotalPageCount() const { return total_page_count_; }
    PageFrameDescriptor* getDescriptorArray() const { return const_cast<PageFrameDescriptor*>(boot_descriptors_); }

    template <typename Callback>
    void forEachRemainingRegion(Callback&& cb) {
        if (current_phys_ < end_phys_) {
            std::uintptr_t start = (current_phys_ + 4095) & ~4095ULL;
            if (start < end_phys_) {
                cb(start, end_phys_ - start);
            }
        }
        std::uint8_t* buf = reinterpret_cast<std::uint8_t*>(memmap_->buffer);
        std::size_t num_desc = memmap_->map_size / memmap_->descriptor_size;
        for (std::size_t i = desc_index_; i < num_desc; ++i) {
            auto* desc = reinterpret_cast<oz_boot::EFI_MEMORY_DESCRIPTOR*>(buf + i * memmap_->descriptor_size);
            if (oz_boot::isAvailable(desc->Type)) {
                std::uintptr_t start = desc->PhysicalStart;
                std::uintptr_t end = start + desc->NumberOfPages * paging::uefi_page_size;
                if (start == 0) start = 4096;
                if (start < end) {
                    cb(start, end - start);
                }
            }
        }
    }
};

static_assert(frame_manager<FrameManager>);

}  // namespace x86_64
}  // namespace oz