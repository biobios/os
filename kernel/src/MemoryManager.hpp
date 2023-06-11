#pragma once

#include <cstddef>

#include "bootStructures.hpp"
#include "utils.hpp"

namespace oz {

namespace paging {
using namespace binary_units;
constexpr std::size_t _4Ki = 4_Ki;
constexpr std::size_t _2Mi = 2_Mi;
constexpr std::size_t _1Gi = 1_Gi;
constexpr std::size_t uefi_page_size = 4_Ki;
}  // namespace available_page_size

template <std::size_t max_memory_size,
          std::size_t page_size = paging::_4Ki>
class MemoryManager {
    static constexpr std::size_t bit_per_byte = 8;
    static_assert(page_size == paging::_1Gi ||
                      page_size == paging::_2Mi ||
                      page_size == paging::_4Ki,
                  "page_size is not 1GiB, 2MiB, or 4KiB.");
    static_assert(max_memory_size % page_size == 0,
                  "max_memory_size is not a multiple of page_size");
    static constexpr std::size_t page_count = max_memory_size / page_size;

   private:
    alignas(16) std::uint8_t
        bit_map_allocated[(page_count + bit_per_byte - 1) / bit_per_byte];
    std::uint64_t range_end;
    void set(std::uint64_t frame) {
        bit_map_allocated[frame / bit_per_byte] |= 0b00000001
                                                   << (frame % bit_per_byte);
    }

    void clear(std::uint64_t frame) {
        bit_map_allocated[frame / bit_per_byte] &=
            ~(0b00000001 << (frame % bit_per_byte));
    }

    bool get(std::uint64_t frame) {
        return (bit_map_allocated[frame / bit_per_byte] &
                (0b00000001 << (frame % bit_per_byte))) != 0;
    }

   public:
    MemoryManager(oz_boot::BootMemoryMap* memmap) {
        setAll();
        for (std::uint8_t* itr =
                 reinterpret_cast<std::uint8_t*>(memmap->buffer);
             itr < (reinterpret_cast<std::uint8_t*>(memmap->buffer) +
                    memmap->map_size);
             itr += memmap->descriptor_size) {
            oz_boot::EFI_MEMORY_DESCRIPTOR* desc =
                reinterpret_cast<oz_boot::EFI_MEMORY_DESCRIPTOR*>(itr);
            if (oz_boot::isAvailable(desc->Type)) {
                std::uint64_t start_page = (desc->PhysicalStart + page_size - 1) / page_size;//start_page_num
                std::uint64_t end_page = (desc->PhysicalStart + desc->NumberOfPages * paging::uefi_page_size) / page_size;//end_page (not included)
                free(start_page, end_page - start_page);
            }
        }
    }

    bool findAvailable(std::size_t need_length,
                       std::uint64_t* out_start_frame) {
        std::uint64_t free_length = 0;
        for (std::size_t i = 0; i < range_end; i++) {
            if (free_length == need_length) {
                *out_start_frame = i - free_length;
                return true;
            }

            if (get(i)) {
                free_length = 0;
            } else {
                free_length++;
            }
        }

        return false;
    }

    void allocate(std::uint64_t start_frame, std::size_t num_frames) {
        if (start_frame >= range_end) return;

        if (start_frame + num_frames > range_end) {
            num_frames = page_count - start_frame;
        }

        for (std::size_t i = 0; i < num_frames; i++) {
            set(start_frame + i);
        }
    }

    void free(std::uint64_t start_frame, std::size_t num_frames) {
        if (start_frame >= range_end) return;

        if (start_frame + num_frames > range_end) {
            num_frames = page_count - start_frame;
        }

        for (std::size_t i = 0; i < num_frames; i++) {
            clear(start_frame + i);
        }
    }

    void setAll() {
        for (std::size_t i = 0; i < sizeof(bit_map_allocated); i++) {
             bit_map_allocated[i] = ~(0b00000000);
        }
    }
    void clearAll() {
        for (std::size_t i = 0; i < sizeof(bit_map_allocated); i++) {
            bit_map_allocated[i] = 0;
        }
    }
};

}  // namespace oz

   /*
   
    size_t free_length = 0;
    for(size_t i = range_begin; i < range_end; i++){
        if(GetBit(i)){
            free_length = 0;
        }else{
            free_length++;
        }
   
        if(free_length == num_frames){
            MarkAllocated( i + 1 - num_frames,num_frames);
            return {i + 1 - num_frames, MAKE_ERROR(Error::kSuccess)};
        }
    }
   
    return {kNullFrame, MAKE_ERROR(Error::kNoEnoughMemory)};
   
   メモリディスクリプタを最初から最後まで見る
   もし現在の利用可能領域より後ろから始まるディスクリプタならば歯抜けの領域を確保する
   もし利用可能なタイプなら利用可能領域の最後を更新する
   そうじゃないならその領域を確保する
   
   すべての領域を確保する
   メモリディスクリプタを最初から最後までみる
   もし利用可能なタイプならその領域を開放する*/