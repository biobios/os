#pragma once

#include <cstdint>

#include "Graphics.hpp"
#include "Shell.hpp"
#include "PageManager.hpp"
#include "bootStructures.hpp"
#include "FrameManager.hpp"
#include "KernelMemoryAllocator.hpp"

namespace oz {
class Kernel {
    public:
    Graphics g;
    Shell sh;
    private:
    PageManager pm;
    x86_64::FrameManager fm;
    TLSFMemoryAllocator tlsf_malloc;
   public:
    Kernel(oz_boot::PlatformInfo* platformInfo);
    void run();
};
}  // namespace oz