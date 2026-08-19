#pragma once

#include <cstdint>

#include "memory/AddressSpace.hpp"
#include "graphics/Graphics.hpp"
#include "memory/KernelMemoryAllocator.hpp"
#include "memory/IKernelMemoryAllocator.hpp"
#include "memory/PageTable.hpp"
#include "memory/PageTableManager.hpp"
#include "memory/paging.hpp"
#include "shell/Shell.hpp"
#include "core/bootStructures.hpp"
#include "drivers/usb/xhci/xHCIUtils.hpp"
#include "drivers/pci/PCIUtils.hpp"
#include "hardware/ACPIUtils.hpp"
#include "utils/utils.hpp"
#include <new>
#include "drivers/usb/class/HIDKeyboardDriver.hpp"

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
        oz_boot::PlatformInfo* platform_info_;

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
    , platform_info_(platformInfo)
{
    setShellPtr(&sh);
}

template <typename KernelSettings>
void KernelStorage<KernelSettings>::Kernel::run() {
    g.clearScreen();
    sh.printString("Finish init\n\rStart Kernel in Higher-Half!\n\r");
    sh.repaint();

    kmalloc_unique_ptr<xHCIUtils::Controller, KernelAccessor> xhci;
    kmalloc_unique_ptr<USBClassDriver::HIDKeyboardDriver, KernelAccessor> kbd_driver;
    
    if (platform_info_->RSDP) {
        ACPI::RootSystemDescriptionPointer* rsdp = reinterpret_cast<ACPI::RootSystemDescriptionPointer*>(platform_info_->RSDP);
        ACPI::ExtendedSystemDescriptionTable* xsdt = oz::phys_to_virt(createPhysicalAddress<ACPI::ExtendedSystemDescriptionTable>(rsdp->XsdtAddress));
        ACPIUtils::ExtendedSystemDescriptionTableWrapper xsdtWrapper(xsdt);
        
        PCIe::MemorymappedConfigurationSpaceDescriptionTable* mcfg = xsdtWrapper.getTable<PCIe::MemorymappedConfigurationSpaceDescriptionTable>();
        if (mcfg) {
            PCIUtils::MemorymappedConfigurationSpaceWrapper mcfgWrapper(mcfg);
            
            PCIUtils::PCIFunction xhci_func = mcfgWrapper.findFunction(0x0C, 0x03, 0x30);
            if (xhci_func) {
                sh.printString("Found xHCI Controller! Initializing...\n\r");
                sh.repaint();
                
                xhci = make_kmalloc_unique<xHCIUtils::Controller, KernelAccessor>(xhci_func);
                kbd_driver = make_kmalloc_unique<USBClassDriver::HIDKeyboardDriver, KernelAccessor>();
                xhci->registerClassDriver(kbd_driver.get());
                sh.printString("Initializing xHCI...\n\r");
                
                if (xhci->initialize(fm)) {
                    sh.printString("xHCI Initialized Successfully!\n\r");
                } else {
                    sh.printString("xHCI Initialization Failed!\n\r");
                }
                sh.repaint();
            }
        }
    }
    
    while (1) {
        if (xhci) {
            xhci->pollPorts();
            xhci->processEvents();
        }
        
        // __asm__("hlt"); // HLTを呼ぶと割り込みが来るまで停止してしまうので、ポーリングの場合はコメントアウトするか、タイマー割り込み等を設定する
    }
}

}  // namespace oz