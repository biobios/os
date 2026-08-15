#include "FrameManager.hpp"
#include "Address.hpp"

void oz::x86_64::FrameManager::pushFreeBlock(std::uint8_t level, PageFrameDescriptor* pfd) {
    pfd->level = level;
    pfd->flags = DescriptorFlags::FREE | DescriptorFlags::HEAD;
    pfd->ownerType = PageOwnerType::FRAME_MANAGER;

    BuddyFreeListEntry* entry = reinterpret_cast<BuddyFreeListEntry*>(pfd->ownerData);
    entry->prev = nullptr;
    entry->next = free_lists[level];

    if (free_lists[level] != nullptr) {
        BuddyFreeListEntry* head_entry = reinterpret_cast<BuddyFreeListEntry*>(free_lists[level]->ownerData);
        head_entry->prev = pfd;
    }
    free_lists[level] = pfd;
}

void oz::x86_64::FrameManager::removeFreeBlock(std::uint8_t level, PageFrameDescriptor* pfd) {
    BuddyFreeListEntry* entry = reinterpret_cast<BuddyFreeListEntry*>(pfd->ownerData);
    if (entry->prev != nullptr) {
        BuddyFreeListEntry* prev_entry = reinterpret_cast<BuddyFreeListEntry*>(entry->prev->ownerData);
        prev_entry->next = entry->next;
    } else {
        free_lists[level] = entry->next;
    }

    if (entry->next != nullptr) {
        BuddyFreeListEntry* next_entry = reinterpret_cast<BuddyFreeListEntry*>(entry->next->ownerData);
        next_entry->prev = entry->prev;
    }
    entry->next = nullptr;
    entry->prev = nullptr;
    pfd->flags &= ~DescriptorFlags::FREE;
}

oz::PageFrameDescriptor* oz::x86_64::FrameManager::popFreeBlock(std::uint8_t level) {
    PageFrameDescriptor* head = free_lists[level];
    if (!head) return nullptr;
    removeFreeBlock(level, head);
    return head;
}

void oz::x86_64::FrameManager::freeRange(std::size_t start_page_index, std::size_t end_page_index) {
    std::size_t cur = start_page_index;
    while (cur < end_page_index) {
        std::uint8_t level = 0;
        while (level < MAX_LEVEL && (cur % (1ULL << (level + 1)) == 0) && (cur + (1ULL << (level + 1)) <= end_page_index)) {
            level++;
        }
        pfd_array[cur].level = level;
        pfd_array[cur].flags = DescriptorFlags::HEAD;
        pfd_array[cur].ownerType = PageOwnerType::FRAME_MANAGER;
        freeBlock(&pfd_array[cur]);
        cur += (1ULL << level);
    }
}

oz::x86_64::FrameManager::FrameManager(oz_boot::BootMemoryMap* memmap, std::size_t frame_size)
    : FRAME_SIZE{frame_size}
    , pfd_array{nullptr}
    , total_page_count{0}
{
    for (std::size_t l = 0; l <= MAX_LEVEL; l++) {
        free_lists[l] = nullptr;
    }

    // 1. Calculate total pages from BootMemoryMap
    std::uint8_t* itr = reinterpret_cast<std::uint8_t*>(memmap->buffer);
    std::uint8_t* itr_end = itr + memmap->map_size;

    for (; itr < itr_end; itr += memmap->descriptor_size) {
        auto* desc = reinterpret_cast<oz_boot::EFI_MEMORY_DESCRIPTOR*>(itr);
        if (!oz_boot::isAvailable(desc->Type)) {
            continue;
        }

        std::uint64_t end_page =
            (desc->PhysicalStart + desc->NumberOfPages * paging::uefi_page_size) / frame_size;
        if (total_page_count < end_page) {
            total_page_count = end_page;
        }
    }

    // 2. Allocate memory for PageFrameDescriptor array from available UEFI memory
    std::size_t required_bytes = total_page_count * sizeof(PageFrameDescriptor);
    std::size_t required_uefi_pages =
        (required_bytes + paging::uefi_page_size - 1) / paging::uefi_page_size;

    itr = reinterpret_cast<std::uint8_t*>(memmap->buffer);
    for (; itr < itr_end; itr += memmap->descriptor_size) {
        auto* desc = reinterpret_cast<oz_boot::EFI_MEMORY_DESCRIPTOR*>(itr);
        if (oz_boot::isAvailable(desc->Type) && desc->NumberOfPages >= required_uefi_pages) {
            pfd_array = reinterpret_cast<PageFrameDescriptor*>(
                oz::phys_to_virt(createPhysicalAddress<PageFrameDescriptor>(desc->PhysicalStart)));
            desc->NumberOfPages -= required_uefi_pages;
            desc->PhysicalStart += paging::uefi_page_size * required_uefi_pages;
            break;
        }
    }

    if (pfd_array == nullptr) {
        __asm__ volatile("hlt");
    }

    // 3. Initialize all descriptors to RESERVED
    for (std::size_t i = 0; i < total_page_count; i++) {
        pfd_array[i].level = 0;
        pfd_array[i].ownerType = PageOwnerType::RESERVED;
        pfd_array[i].flags = DescriptorFlags::RESERVED;
        for (std::size_t b = 0; b < sizeof(pfd_array[i].ownerData); b++) {
            pfd_array[i].ownerData[b] = 0;
        }
        for (std::size_t r = 0; r < sizeof(pfd_array[i].reserved); r++) {
            pfd_array[i].reserved[r] = 0;
        }
    }

    // 4. Register available memory regions into buddy system
    itr = reinterpret_cast<std::uint8_t*>(memmap->buffer);
    for (; itr < itr_end; itr += memmap->descriptor_size) {
        auto* desc = reinterpret_cast<oz_boot::EFI_MEMORY_DESCRIPTOR*>(itr);
        if (oz_boot::isAvailable(desc->Type)) {
            std::uint64_t start_page_index =
                (desc->PhysicalStart + frame_size - 1) / frame_size;
            std::uint64_t end_page_index =
                (desc->PhysicalStart + desc->NumberOfPages * paging::uefi_page_size) / frame_size;

            if (start_page_index == 0) {
                start_page_index = 1; // Reserve page 0 (0x0000 - 0x0FFF)
            }
            if (start_page_index < end_page_index) {
                freeRange(start_page_index, end_page_index);
            }
        }
    }
}

oz::PageBlock<> oz::x86_64::FrameManager::allocateBlock(std::uint8_t level) {
    if (level > MAX_LEVEL) {
        return PageBlock<>{nullptr};
    }

    std::uint8_t current_level = level;
    while (current_level <= MAX_LEVEL && free_lists[current_level] == nullptr) {
        current_level++;
    }

    if (current_level > MAX_LEVEL) {
        return PageBlock<>{nullptr};
    }

    PageFrameDescriptor* block = popFreeBlock(current_level);

    while (current_level > level) {
        current_level--;
        std::size_t block_idx = block - pfd_array;
        std::size_t buddy_idx = block_idx + (1ULL << current_level);

        PageFrameDescriptor* buddy = &pfd_array[buddy_idx];
        pushFreeBlock(current_level, buddy);
    }

    block->level = level;
    block->flags = DescriptorFlags::HEAD;
    block->ownerType = PageOwnerType::OTHER;

    return PageBlock<>{block};
}

void oz::x86_64::FrameManager::freeBlock(PageFrameDescriptor* pfd) {
    if (pfd == nullptr) return;

    std::size_t idx = pfd - pfd_array;
    if (idx >= total_page_count) return;

    std::uint8_t level = pfd->level;
    if (level > MAX_LEVEL) level = MAX_LEVEL;

    while (level < MAX_LEVEL) {
        std::size_t buddy_idx = idx ^ (1ULL << level);
        if (buddy_idx + (1ULL << level) > total_page_count) {
            break;
        }

        PageFrameDescriptor* buddy = &pfd_array[buddy_idx];

        if ((buddy->flags & (DescriptorFlags::FREE | DescriptorFlags::HEAD)) !=
            (DescriptorFlags::FREE | DescriptorFlags::HEAD)) {
            break;
        }
        if (buddy->level != level) {
            break;
        }
        if (buddy->ownerType != PageOwnerType::FRAME_MANAGER) {
            break;
        }

        removeFreeBlock(level, buddy);

        idx = idx & ~(1ULL << level);
        level++;
    }

    PageFrameDescriptor* merged = &pfd_array[idx];
    pushFreeBlock(level, merged);
}

void oz::x86_64::FrameManager::freeBlock(PageBlock<> block) {
    freeBlock(block.getDescriptor());
}

oz::PhysicalAddress<void> oz::x86_64::FrameManager::getPhysicalAddress(PageBlock<> block) const {
    return getPhysicalAddress(block.getDescriptor());
}

oz::PhysicalAddress<void> oz::x86_64::FrameManager::getPhysicalAddress(const PageFrameDescriptor* pfd) const {
    if (pfd == nullptr) return nullPhysicalAddress<void>();
    std::size_t page_index = pfd - pfd_array;
    return createPhysicalAddress<void>(page_index * FRAME_SIZE);
}

oz::PageFrameDescriptor* oz::x86_64::FrameManager::getDescriptor(PhysicalAddress<void> phys) const {
    std::size_t page_index = phys.get() / FRAME_SIZE;
    if (page_index >= total_page_count) return nullptr;
    return &pfd_array[page_index];
}

oz::PageFrameDescriptor* oz::x86_64::FrameManager::allocatePages(std::size_t frame_length) {
    if (frame_length == 0) return nullptr;
    std::uint8_t level = 0;
    while ((1ULL << level) < frame_length) {
        level++;
    }
    PageBlock<> block = allocateBlock(level);
    return block.getDescriptor();
}

void oz::x86_64::FrameManager::freePages(PageFrameDescriptor* returnedFrame, std::size_t frame_length) {
    (void)frame_length;
    freeBlock(returnedFrame);
}