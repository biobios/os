#pragma once
#include <cstddef>

namespace oz {
class IKernelMemoryAllocator {
   public:
    virtual void* malloc(std::size_t size) = 0;
    virtual void free(void* returnedPtr) = 0;
};
}  // namespace oz