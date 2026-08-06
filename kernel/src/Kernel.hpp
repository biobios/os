#pragma once

#include <cstdint>

#include "AddressSpace.hpp"
#include "FrameManager.hpp"
#include "Graphics.hpp"
#include "KernelMemoryAllocator.hpp"
#include "PageTable.hpp"
#include "PageTableManager.hpp"
#include "Shell.hpp"
#include "bootStructures.hpp"

namespace oz {
PageTable* getMasterPML4();

class Kernel : public PhysicalAddressProvider {
   public:
    Graphics g;
    Shell sh;

   private:
    x86_64::FrameManager fm;
    TLSFMemoryAllocator tlsf_malloc;
    PageTableManager pt_manager;
    AddressSpace kernel_space;

   public:
    Kernel(oz_boot::PlatformInfo* platformInfo);
    void run();

    AddressSpace& getKernelSpace() { return kernel_space; }
    PageTableManager& getPageTableManager() { return pt_manager; }
    x86_64::FrameManager& getFrameManager() { return fm; }
};
}  // namespace oz