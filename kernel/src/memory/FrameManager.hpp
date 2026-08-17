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
    PageFrameDescriptor* pfd_array;
    std::size_t total_page_count;
    PageFrameDescriptor* free_lists[MAX_LEVEL + 1];

    void pushFreeBlock(std::uint8_t level, PageFrameDescriptor* pfd);
    void removeFreeBlock(std::uint8_t level, PageFrameDescriptor* pfd);
    PageFrameDescriptor* popFreeBlock(std::uint8_t level);
    void freeRange(std::size_t start_page_index, std::size_t end_page_index);

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

    std::size_t getTotalPageCount() const { return total_page_count; }
    PageFrameDescriptor* getDescriptorArray() const { return pfd_array; }
};

static_assert(frame_manager<FrameManager>);

}  // namespace x86_64
}  // namespace oz