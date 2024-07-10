#pragma once

#include <cstdint>

#include "FrameManager.hpp"
#include "Graphics.hpp"
#include "KernelMemoryAllocator.hpp"
#include "PageManager.hpp"
#include "Shell.hpp"
#include "bootStructures.hpp"

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