#include "memory/FrameManager.hpp"
#include "memory/Address.hpp"
#include "utils/utils.hpp"

namespace oz::x86_64 {

bool FrameManager::advanceToNextRegion() {
    std::uint8_t* buf = reinterpret_cast<std::uint8_t*>(memmap_->buffer);
    std::size_t num_desc = memmap_->map_size / memmap_->descriptor_size;

    while (desc_index_ < num_desc) {
        auto* desc = reinterpret_cast<oz_boot::EFI_MEMORY_DESCRIPTOR*>(buf + desc_index_ * memmap_->descriptor_size);
        desc_index_++;
        if (oz_boot::isAvailable(desc->Type)) {
            std::uintptr_t start = desc->PhysicalStart;
            std::uintptr_t end = start + desc->NumberOfPages * paging::uefi_page_size;
            if (start == 0) start = 4096; // Reserve page 0
            if (start < end) {
                current_phys_ = (start + 4095) & ~4095ULL;
                end_phys_ = end;
                return true;
            }
        }
    }
    return false;
}

FrameManager::FrameManager(oz_boot::BootMemoryMap* memmap, std::size_t frame_size)
    : FRAME_SIZE{frame_size}
    , memmap_{memmap}
    , current_phys_{0}
    , end_phys_{0}
    , desc_index_{0}
    , total_page_count_{0}
    , desc_alloc_count_{0}
{
    utils::memset(boot_descriptors_, 0, sizeof(boot_descriptors_));

    // Calculate total pages for information
    std::uint8_t* itr = reinterpret_cast<std::uint8_t*>(memmap->buffer);
    std::uint8_t* itr_end = itr + memmap->map_size;

    for (; itr < itr_end; itr += memmap->descriptor_size) {
        auto* desc = reinterpret_cast<oz_boot::EFI_MEMORY_DESCRIPTOR*>(itr);
        if (!oz_boot::isAvailable(desc->Type)) continue;

        std::uint64_t end_page =
            (desc->PhysicalStart + desc->NumberOfPages * paging::uefi_page_size) / frame_size;
        if (total_page_count_ < end_page) {
            total_page_count_ = end_page;
        }
    }

    advanceToNextRegion();
}

PageFrameDescriptor* FrameManager::allocatePages(std::size_t frame_length) {
    if (frame_length == 0) return nullptr;
    if (desc_alloc_count_ >= MAX_BOOT_DESCRIPTORS) return nullptr;

    std::size_t bytes = frame_length * FRAME_SIZE;
    std::uintptr_t alloc_phys = 0;

    while (true) {
        std::uintptr_t aligned = (current_phys_ + (FRAME_SIZE - 1)) & ~(FRAME_SIZE - 1);
        if (aligned + bytes <= end_phys_) {
            alloc_phys = aligned;
            current_phys_ = aligned + bytes;
            break;
        }
        if (!advanceToNextRegion()) {
            return nullptr;
        }
    }

    PageFrameDescriptor* pfd = &boot_descriptors_[desc_alloc_count_++];
    pfd->level = 0;
    pfd->ownerType = PageOwnerType::FRAME_MANAGER;
    pfd->flags = DescriptorFlags::NONE;
    *reinterpret_cast<std::uintptr_t*>(pfd->ownerData) = alloc_phys;
    return pfd;
}

PageBlock<> FrameManager::allocateBlock(std::uint8_t level) {
    if (level > MAX_LEVEL) return PageBlock<>{nullptr};
    if (desc_alloc_count_ >= MAX_BOOT_DESCRIPTORS) return PageBlock<>{nullptr};

    std::size_t num_pages = 1ULL << level;
    std::size_t alignment = num_pages * FRAME_SIZE;
    std::size_t bytes = num_pages * FRAME_SIZE;
    std::uintptr_t alloc_phys = 0;

    while (true) {
        std::uintptr_t aligned = (current_phys_ + (alignment - 1)) & ~(alignment - 1);
        if (aligned + bytes <= end_phys_) {
            alloc_phys = aligned;
            current_phys_ = aligned + bytes;
            break;
        }
        if (!advanceToNextRegion()) {
            return PageBlock<>{nullptr};
        }
    }

    PageFrameDescriptor* pfd = &boot_descriptors_[desc_alloc_count_++];
    pfd->level = level;
    pfd->ownerType = PageOwnerType::FRAME_MANAGER;
    pfd->flags = DescriptorFlags::HEAD;
    *reinterpret_cast<std::uintptr_t*>(pfd->ownerData) = alloc_phys;
    return PageBlock<>{pfd};
}

void FrameManager::freeBlock(PageBlock<> /*block*/) {
    // Zero-policy: Kernel does not free boot memory; memory reclamation is managed via UntypedMemory/user space.
}

void FrameManager::freeBlock(PageFrameDescriptor* /*pfd*/) {
}

void FrameManager::freePages(PageFrameDescriptor* /*returnedFrame*/, std::size_t /*frame_length*/) {
}

PhysicalAddress<void> FrameManager::getPhysicalAddress(PageBlock<> block) const {
    if (!block) return nullPhysicalAddress<void>();
    return getPhysicalAddress(block.getDescriptor());
}

PhysicalAddress<void> FrameManager::getPhysicalAddress(const PageFrameDescriptor* pfd) const {
    if (!pfd) return nullPhysicalAddress<void>();
    std::uintptr_t phys = *reinterpret_cast<const std::uintptr_t*>(pfd->ownerData);
    return createPhysicalAddress<void>(phys);
}

PageFrameDescriptor* FrameManager::getDescriptor(PhysicalAddress<void> phys) const {
    for (std::size_t i = 0; i < desc_alloc_count_; ++i) {
        if (*reinterpret_cast<const std::uintptr_t*>(boot_descriptors_[i].ownerData) == phys.get()) {
            return const_cast<PageFrameDescriptor*>(&boot_descriptors_[i]);
        }
    }
    return nullptr;
}

} // namespace oz::x86_64