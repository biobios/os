#pragma once
#include <cstddef>

namespace oz {
struct FrameInfo {
    std::uint64_t flags;
    // 0bit : isUsed;
    void* physicalAddress;
};
class IFrameManager {
   public:
    const std::size_t FRAME_SIZE;
    IFrameManager(std::size_t frame_size) : FRAME_SIZE{frame_size} {};
    virtual FrameInfo* allocatePages(std::size_t frame_length) = 0;
    virtual void freePages(FrameInfo* returnedFrame, std::size_t frame_length) = 0;
};
}  // namespace oz