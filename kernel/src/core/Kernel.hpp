#pragma once

#include <cstdint>

#include "memory/AddressSpace.hpp"
#include "graphics/Graphics.hpp"
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
#include "drivers/usb/class/HIDKeyboardDriver.hpp"
#include "core/Mutex.hpp"
#include "core/IScheduler.hpp"
#include "drivers/storage/ahci/AHCI.hpp"
#include "fs/FAT32.hpp"
#include "fs/VFS.hpp"
#include "utils/new_delete.hpp"
#include <memory>

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
        requires(kernel_memory_allocator<typename KernelSettings::template KernelMemoryAllocatorFunctor<KernelAccessor1>>)
    struct KernelAccessor2<Args...> : public KernelAccessor1 {
        struct Settings : public KernelAccessor1::Settings {
            using KernelMemoryAllocator = typename KernelSettings::template KernelMemoryAllocatorFunctor<KernelAccessor1>;
        };
        static consteval auto getKernelMemoryAllocator() -> Settings::KernelMemoryAllocator& {
            return kernel_storage.kernel.k_malloc;
        }
    };

    template <typename...>
    struct KernelAccessor3;

    template <typename... Args>
        requires(scheduler<typename KernelSettings::Scheduler>)
    struct KernelAccessor3<Args...> : public KernelAccessor2<> {
        using Settings = KernelAccessor2<>::Settings;
        static consteval auto getScheduler() -> Settings::Scheduler& {
            return kernel_storage.kernel.scheduler;
        }
    };

    template <typename... Args>
        requires(scheduler<typename KernelSettings::template SchedulerFunctor<KernelAccessor2<>>>)
    struct KernelAccessor3<Args...> : public KernelAccessor2<> {
        struct Settings : public KernelAccessor2<>::Settings {
            using Scheduler = typename KernelSettings::template SchedulerFunctor<KernelAccessor2<>>;
        };
        static consteval auto getScheduler() -> Settings::Scheduler& {
            return kernel_storage.kernel.scheduler;
        }
    };

    using KernelAccessor = KernelAccessor3<>;

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
        Settings::Scheduler scheduler;
        Mutex<KernelAccessor> graphics_mutex;
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
    oz::setKernelMemoryAllocator(KernelAccessor{});
    g.clearScreen();
    sh.printString("Finish init\n\rStart Kernel in Higher-Half!\n\r");
    sh.repaint();

    std::unique_ptr<xHCIUtils::Controller> xhci;
    std::unique_ptr<USBClassDriver::HIDKeyboardDriver> kbd_driver;
    std::unique_ptr<AHCI::Controller> ahci_ctrl;
    std::unique_ptr<FAT32FileSystem> fat32_fs;
    VFS vfs;
    
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
                
                xhci = std::make_unique<xHCIUtils::Controller>(xhci_func);
                kbd_driver = std::make_unique<USBClassDriver::HIDKeyboardDriver>();
                xhci->registerClassDriver(kbd_driver.get());
                sh.printString("Initializing xHCI...\n\r");
                
                if (xhci->initialize(fm)) {
                    sh.printString("xHCI Initialized Successfully!\n\r");
                    xhci->scanInitialPorts();
                    xhci->processEvents();
                } else {
                    sh.printString("xHCI Initialization Failed!\n\r");
                }
                sh.repaint();
            }
            
            // --- AHCI Initialization ---
            PCIUtils::PCIFunction ahci_func = mcfgWrapper.findFunction(0x01, 0x06, 0x01);
            if (ahci_func) {
                sh.printString("Found AHCI Controller! Initializing...\n\r");
                sh.repaint();
                
                ahci_ctrl = std::make_unique<AHCI::Controller>(ahci_func);
                if (ahci_ctrl->initialize(fm)) {
                    sh.printString("AHCI Initialized Successfully!\n\r");
                    AHCI::AHCIPortDevice* sata_dev = ahci_ctrl->getFirstSataDevice();
                    if (sata_dev) {
                        sh.printString("SATA Device Found! Mounting FAT32...\n\r");
                        fat32_fs = std::make_unique<FAT32FileSystem>(sata_dev);
                        if (fat32_fs->init()) {
                            sh.printString("FAT32 Mounted Successfully!\n\r");
                            vfs.mountRoot(fat32_fs.get());
                            
                            // Test reading kernel.bin
                            auto file = vfs.open("os/kernel.bin");
                            if (file) {
                                sh.printString("os/kernel.bin opened successfully!\n\r");
                            } else {
                                sh.printString("os/kernel.bin not found.\n\r");
                            }
                            
                            // Test reading and appending to test.txt
                            auto textFile = vfs.open("test.txt");
                            if (textFile) {
                                sh.printString("test.txt opened. Reading content...\n\r");
                                char buf[128] = {0};
                                std::size_t size = textFile->getSize();
                                std::size_t readSize = size < sizeof(buf) - 1 ? size : sizeof(buf) - 1;
                                textFile->read(buf, readSize);
                                
                                sh.printString("Content: ");
                                sh.printString(buf);
                                sh.printString("\n\r");
                                
                                const char* appendStr = "\nAppended data!";
                                textFile->seek(textFile->getSize());
                                std::size_t written = textFile->write(appendStr, sizeof("\nAppended data!") - 1);
                                
                                if (written > 0) {
                                    sh.printString("Appended successfully.\n\r");
                                } else {
                                    sh.printString("Append failed.\n\r");
                                }
                                
                                // Explicitly close to flush metadata (will also be called on destruction, but explicit is good)
                                textFile->close();
                                // To avoid double free since unique_ptr will try to delete it too, release it.
                                textFile.release();
                            } else {
                                sh.printString("test.txt not found.\n\r");
                            }

                        } else {
                            sh.printString("FAT32 Mount Failed.\n\r");
                        }
                    }
                } else {
                    sh.printString("AHCI Initialization Failed!\n\r");
                }
                sh.repaint();
            }
        }
    }
    
    // --- Multithreading Demo ---
    // 1. Create Idle / Reaper thread (level 0 = 4KB stack)
    Thread* idle_t = scheduler.createThread(0, Settings::Scheduler::idle_reaper_task, &scheduler);
    scheduler.queueThread(idle_t);
    
    // 2. Create Animation thread (level 1 = 8KB stack)
    Thread* anim_t = scheduler.createThread(1, [](void* arg) {
        Graphics* g = static_cast<Graphics*>(arg);
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        int dx = 5;
        int dy = 5;
        Pixel color = {255, 0, 0, 0}; // Blue
        Pixel bg = {40, 40, 40, 0};   // Background (Gray)
        
        // Let's get our kernel's graphics mutex
        auto& mutex = KernelStorage<KernelSettings>::kernel_storage.kernel.graphics_mutex;
        
        while(1) {
            mutex.lock();
            
            // Erase old square
            g->setColor(bg);
            g->fillRect(x, y, 20, 20);
            
            // Update position
            if (x + dx >= g->getWidth() - 20 || x + dx <= 0) dx = -dx;
            if (y + dy >= g->getHeight() - 20 || y + dy <= 0) dy = -dy;
            x += dx;
            y += dy;
            
            // Draw new square
            g->setColor(color);
            g->fillRect(x, y, 20, 20);
            
            mutex.unlock();
            
            // Delay to make the animation visible
            kernel_storage.kernel.scheduler.sleep(10);
        }
    }, &g);
    
    scheduler.queueThread(anim_t);
    
    // 3. Start APIC Timer for preemption
    scheduler.initMainThread();
    oz::x86_64::initAPICTimer(32);
    sh.printString("Started multithreading demo with Reaper!\n\r");
    sh.repaint();
    // ---------------------------
    
    while (1) {
        __asm__ volatile("cli");

        bool has_event = false;
        HID::KeyEvent event;
        if (kbd_driver && kbd_driver->getKeyboard().pop(event)) {
            has_event = true;
        }

        if (has_event) {
            __asm__ volatile("sti");
            if (event.state == HID::KeyState::Pressed && event.ascii != 0) {
                char str[2] = {event.ascii, '\0'};
                graphics_mutex.lock();
                g.setColor({255, 255, 255, 0}); // White
                dprint(str);
                graphics_mutex.unlock();
            }
        } else {
            __asm__ volatile("sti; hlt");
        }
    }
}

}  // namespace oz