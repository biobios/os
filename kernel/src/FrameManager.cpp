#include "FrameManager.hpp"

void oz::x86_64::FrameManager::setAllocateFlag(std::size_t index,
                                               std::size_t length) {
    for (std::size_t i = index; i < index + length; i++) {
        fi_array[i].flags |= 0b1;
    }
}

void oz::x86_64::FrameManager::clearAllocateFlag(std::size_t index,
                                                 std::size_t length) {
    for (std::size_t i = index; i < index + length; i++) {
        fi_array[i].flags &= ~(0b1);
    }
}

oz::x86_64::FrameManager::FrameManager(oz_boot::BootMemoryMap* memmap,
                                       std::size_t frame_size)
    : IFrameManager{frame_size} {
    fi_array_size = 0;

    // pages_countを求める
    std::uint8_t* itr = reinterpret_cast<std::uint8_t*>(memmap->buffer);
    std::uint8_t* itr_end =
        reinterpret_cast<std::uint8_t*>(memmap->buffer) + memmap->map_size;

    for (; itr < itr_end; itr += memmap->descriptor_size) {
        oz_boot::EFI_MEMORY_DESCRIPTOR* desc =
            reinterpret_cast<oz_boot::EFI_MEMORY_DESCRIPTOR*>(itr);

        std::uint64_t end_page =
            (desc->PhysicalStart +
             desc->NumberOfPages * paging::uefi_page_size - 1) /
            frame_size;
        if (fi_array_size < end_page + 1) {
            fi_array_size = end_page + 1;
        }
    }

    // pages_count分のFrameInfoを収容できる領域を確保する
    std::size_t required_size = fi_array_size * sizeof(FrameInfo);
    std::size_t required_page_size =
        (required_size + paging::uefi_page_size - 1) / paging::uefi_page_size;

    itr = reinterpret_cast<std::uint8_t*>(memmap->buffer);

    fi_array = nullptr;
    for (; itr < itr_end; itr += memmap->descriptor_size) {
        oz_boot::EFI_MEMORY_DESCRIPTOR* desc =
            reinterpret_cast<oz_boot::EFI_MEMORY_DESCRIPTOR*>(itr);

        if (oz_boot::isAvailable(desc->Type) &&
            desc->NumberOfPages >= required_page_size) {
            fi_array = reinterpret_cast<FrameInfo*>(desc->PhysicalStart);
            desc->NumberOfPages -= required_page_size;
            desc->PhysicalStart += paging::uefi_page_size * required_page_size;

            break;
        }
    }
    if (fi_array == nullptr) {
        __asm__ volatile("hlt");
    }

    // fi_array初期化
    for (std::size_t i = 0; i < fi_array_size; i++) {
        fi_array[i].flags = 1;  // isUsed;
        fi_array[i].physicalAddress = reinterpret_cast<void*>(frame_size * i);
    }

    // 利用可能なFrameInfoのisUsedを下す
    itr = reinterpret_cast<std::uint8_t*>(memmap->buffer);

    for (; itr < itr_end; itr += memmap->descriptor_size) {
        oz_boot::EFI_MEMORY_DESCRIPTOR* desc =
            reinterpret_cast<oz_boot::EFI_MEMORY_DESCRIPTOR*>(itr);
        if (oz_boot::isAvailable(desc->Type)) {
            std::uint64_t start_page_index =
                (desc->PhysicalStart + frame_size - 1) / frame_size;
            std::uint64_t end_page_index =  // not included
                (desc->PhysicalStart +
                 desc->NumberOfPages * paging::uefi_page_size) /
                frame_size;
            for (std::size_t i = start_page_index; i < end_page_index; i++) {
                fi_array[i].flags &= ~(0b1);
            }
        }
    }
}

oz::FrameInfo* oz::x86_64::FrameManager::allocatePages(
    std::size_t frame_length) {
    FrameInfo* ret = nullptr;
    std::size_t currentStreak = 0;
    for (std::size_t i = 0; i < fi_array_size; i++) {
        if (currentStreak >= frame_length) {
            setAllocateFlag(i - frame_length, frame_length);
            ret = &fi_array[i - frame_length];
            break;
        }
        if ((fi_array[i].flags & 0b1) != 0) {
            currentStreak = 0;
        } else {
            currentStreak++;
        }
    }
    return ret;
}

void oz::x86_64::FrameManager::freePages(FrameInfo* returnedFrame,
                                         std::size_t frame_length) {
    std::size_t index = (reinterpret_cast<std::size_t>(returnedFrame) -
                         reinterpret_cast<std::size_t>(fi_array)) /
                        sizeof(FrameInfo);
    clearAllocateFlag(index, frame_length);
}