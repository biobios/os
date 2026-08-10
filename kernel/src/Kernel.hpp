#pragma once

#include <cstdint>

#include "AddressSpace.hpp"
#include "Graphics.hpp"
#include "KernelMemoryAllocator.hpp"
#include "oznew.hpp"
#include "PageTable.hpp"
#include "PageTableManager.hpp"
#include "paging.hpp"
#include "Shell.hpp"
#include "bootStructures.hpp"

namespace oz {
PageTable* getMasterPML4();

template <typename KernelSettings>
struct KernelStorage {

    struct KernelAccessor {
        using Settings = KernelSettings;
        static consteval Settings::FrameManager& getFrameManager();
    };

    class Kernel : public PhysicalAddressProvider {
    public:
        Graphics g;
        Shell sh;

    private:
        KernelSettings::FrameManager fm;
        TLSFMemoryAllocator<KernelAccessor> tlsf_malloc;
        PageTableManager<KernelAccessor> pt_manager;
        AddressSpace<KernelAccessor> kernel_space;

    public:
        Kernel(oz_boot::PlatformInfo* platformInfo);
        void run();

        AddressSpace<KernelAccessor>& getKernelSpace() { return kernel_space; }
        PageTableManager<KernelAccessor>& getPageTableManager() { return pt_manager; }
        KernelSettings::FrameManager& getFrameManager() { return fm; }
        
        friend class KernelAccessor;
    };

    union Storage {
        Kernel kernel;
        int dummy;
        constexpr Storage() : dummy(0) {}
    };

    static inline Storage kernel_storage = {};
};

template <typename KernelSettings>
consteval KernelStorage<KernelSettings>::KernelAccessor::Settings::FrameManager& KernelStorage<KernelSettings>::KernelAccessor::getFrameManager() {
    return KernelStorage::kernel_storage.kernel.fm;
}

template <typename KernelSettings>
KernelStorage<KernelSettings>::Kernel::Kernel(oz_boot::PlatformInfo* platformInfo)
    : g(static_cast<Pixel*>(platformInfo->frame_buffer_base),
        platformInfo->frame_buffer_size, platformInfo->frame_buffer_horizontal,
        platformInfo->frame_buffer_vertical)
    , sh(&g)
    , fm(&platformInfo->memory_map, oz::paging::x86_64::page_sizes[0])
    , tlsf_malloc()
    , pt_manager(getMasterPML4())
    , kernel_space(getMasterPML4(), createPhysicalAddress<PageTable>(reinterpret_cast<std::uintptr_t>(getMasterPML4()) - oz::KERNEL_VIRT_OFFSET))
{
    setMemoryAllocator(&tlsf_malloc);
}

template <typename KernelSettings>
void KernelStorage<KernelSettings>::Kernel::run() {
    g.clearScreen();
    sh.printString("Finish init\n\rStart Kernel in Higher-Half!\n\r");
    sh.repaint();
    while (1) {
        __asm__("hlt");
    }
}

}  // namespace oz