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
    TLSFMemoryAllocator<x86_64::FrameManager> tlsf_malloc;
    PageTableManager<x86_64::FrameManager> pt_manager;
    AddressSpace<x86_64::FrameManager> kernel_space;

   public:
    Kernel(oz_boot::PlatformInfo* platformInfo);
    void run();

    AddressSpace<x86_64::FrameManager>& getKernelSpace() { return kernel_space; }
    PageTableManager<x86_64::FrameManager>& getPageTableManager() { return pt_manager; }
    x86_64::FrameManager& getFrameManager() { return fm; }
};
}  // namespace oz