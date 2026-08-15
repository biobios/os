#pragma once

#include <cstdint>

#include "AddressSpace.hpp"
#include "Graphics.hpp"
#include "KernelMemoryAllocator.hpp"
#include "IKernelMemoryAllocator.hpp"
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

    struct KernelAccessor1 {
        using Settings = KernelSettings;
        static consteval auto getFrameManager() -> Settings::FrameManager& {
            return kernel_storage.kernel.fm;
        }
    };

    template <typename...>
    struct KernelAccessor2;

    template <typename... Args>
        requires(kernel_memory_allocator<typename KernelSettings::KernelMemoryAllocator>)
    struct KernelAccessor2<Args...> : public KernelAccessor1 {
        using Settings = KernelAccessor1::Settings;
        static consteval auto getKernelMemoryAllocator() -> Settings::KernelMemoryAllocator& {
            return kernel_storage.kernel.k_malloc;
        }
    };

    template <typename... Args>
        requires(kernel_memory_allocator<typename KernelSettings::template KernelMemoryAllocatorFunctor<typename KernelStorage<KernelSettings>::KernelAccessor1>>)
    struct KernelAccessor2<Args...> : public KernelAccessor1 {
        struct Settings : public KernelAccessor1::Settings {
            using KernelMemoryAllocator = typename KernelSettings::template KernelMemoryAllocatorFunctor<KernelAccessor1>;
        };
        static consteval auto getKernelMemoryAllocator() -> Settings::KernelMemoryAllocator& {
            return kernel_storage.kernel.k_malloc;
        }   
    };

    using KernelAccessor = KernelAccessor2<>;

    class Kernel : public PhysicalAddressProvider {
        using Settings = KernelAccessor::Settings;
    public:
        Graphics g;
        Shell sh;

    public:
        Settings::FrameManager fm;
        Settings::KernelMemoryAllocator k_malloc;
        PageTableManager<KernelAccessor> pt_manager;
        KernelAddressSpace<KernelAccessor> kernel_space;

        Kernel(oz_boot::PlatformInfo* platformInfo);
        void run();

        KernelAddressSpace<KernelAccessor>& getKernelSpace() { return kernel_space; }
        PageTableManager<KernelAccessor>& getPageTableManager() { return pt_manager; }
        Settings::FrameManager& getFrameManager() { return fm; }
    };

    union Storage {
        Kernel kernel;
        int dummy;
        constexpr Storage() : dummy(0) {}
    };

    static inline Storage kernel_storage = {};
};

template <typename KernelSettings>
KernelStorage<KernelSettings>::Kernel::Kernel(oz_boot::PlatformInfo* platformInfo)
    : g(static_cast<Pixel*>(platformInfo->frame_buffer_base),
        platformInfo->frame_buffer_size, platformInfo->frame_buffer_horizontal,
        platformInfo->frame_buffer_vertical)
    , sh(&g)
    , fm(&platformInfo->memory_map, oz::paging::x86_64::page_sizes[0])
    , k_malloc()
    , pt_manager(getMasterPML4())
    , kernel_space(getMasterPML4(), createPhysicalAddress<PageTable>(reinterpret_cast<std::uintptr_t>(getMasterPML4()) - oz::KERNEL_VIRT_OFFSET))
{
    setMemoryAllocator(&k_malloc);
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