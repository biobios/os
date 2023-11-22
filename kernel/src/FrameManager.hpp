#pragma once
#include <cstddef>

#include "IFrameManager.hpp"
#include "bootStructures.hpp"
#include "paging.hpp"

namespace oz {

namespace x86_64 {

class FrameManager : public IFrameManager {
   private:
    FrameInfo* fi_array;
    std::size_t fi_array_size;
    void setAllocateFlag(std::size_t index, std::size_t length);

    void clearAllocateFlag(std::size_t index, std::size_t length);

   public:
    FrameManager(oz_boot::BootMemoryMap* memmap, std::size_t frame_size);

    FrameInfo* allocatePages(std::size_t frame_length) override;

    void freePages(FrameInfo* returnedFrame, std::size_t frame_length) override;
};
}  // namespace x86_64
}  // namespace oz