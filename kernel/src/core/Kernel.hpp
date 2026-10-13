#pragma once

#include <cstdint>
#include "../../user/src/common/utils/BootInfo.hpp"

#include "memory/AddressSpace.hpp"
#include "graphics/Graphics.hpp"
#include "memory/IKernelMemoryAllocator.hpp"
#include "memory/PageTable.hpp"
#include "memory/PageTableManager.hpp"
#include "memory/paging.hpp"
#include "shell/Shell.hpp"
#include "core/bootStructures.hpp"
#include "drivers/pci/PCIUtils.hpp"
#include "hardware/ACPIUtils.hpp"
#include "utils/utils.hpp"
#include "core/Channel.hpp"
#include "core/Mutex.hpp"
#include "core/IScheduler.hpp"

#include "fs/BufferCache.hpp"
#include "utils/new_delete.hpp"
#include <memory>
#include "core/Process.hpp"
#include "core/Syscall.hpp"
#include "utils/ELF.hpp"
#include "core/Frame.hpp"
#include "core/VSpace.hpp"
#include "core/UntypedMemory.hpp"
#include "core/IRQHandler.hpp"
#include "core/IOPort.hpp"
#include "core/IRQDispatch.hpp"
#include "hardware/x86_64.hpp"

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
        BufferCacheManager buffer_cache;
        Mutex<KernelAccessor> graphics_mutex;
        oz_boot::PlatformInfo* platform_info_;

        Kernel(oz_boot::PlatformInfo* platformInfo);
        void run();

    private:
        Thread* loadProcessFromElfBuffer(const std::uint8_t* file_buf, std::size_t file_size);

    public:

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
    oz::x86_64::initIRQStubs();
    oz::setKernelMemoryAllocator(KernelAccessor{});
    kernel_storage.kernel.buffer_cache.init();
    
    oz::g_exit_thread_func = []() {
        kernel_storage.kernel.scheduler.exitThread();
    };
    oz::g_get_current_cspace_func = []() -> CSpace* {
        auto* thread = kernel_storage.kernel.scheduler.getCurrentThread();
        if (!thread || !thread->user_data) return nullptr;
        auto* proc = static_cast<Process<KernelAccessor>*>(thread->user_data);
        return proc->cspace.get();
    };
    oz::g_get_current_thread_func = []() -> Thread* {
        return kernel_storage.kernel.scheduler.getCurrentThread();
    };
    oz::g_block_thread_func = []() {
        // The current thread's state has already been set to BlockedOnRecv
        // by the syscall handler. Just call schedule() to switch away.
        kernel_storage.kernel.scheduler.schedule();
    };
    oz::g_unblock_thread_func = [](Thread* t) {
        kernel_storage.kernel.scheduler.queueThread(t);
    };
    oz::g_create_thread_func = [](void (*entry)(void*), void* arg) -> Thread* {
        return kernel_storage.kernel.scheduler.createThread(1, entry, arg);
    };
    oz::g_queue_thread_func = [](Thread* t) {
        kernel_storage.kernel.scheduler.queueThread(t);
    };
    oz::g_create_vspace_func = []() -> VSpaceCreateResult {
        auto user_as = ProcessAddressSpace<KernelAccessor>::create(kernel_storage.kernel.kernel_space);
        if (!user_as) return {nullptr, nullptr};
        auto pml4 = user_as->getArchContext().getPhysicalPML4();
        // Release ownership - caller is responsible for managing this
        auto* raw_ptr = user_as.release();
        return {raw_ptr, reinterpret_cast<void*>(pml4.get())};
    };
    oz::g_vspace_map_fn = [](void* as, std::uintptr_t virt, std::uintptr_t phys, std::uint64_t flags) -> bool {
        MemoryFlags mem_flags = MemoryFlags::Present | MemoryFlags::User;
        if (flags & 0x01) mem_flags = mem_flags | MemoryFlags::Writable;
        if (flags & 0x02) mem_flags = mem_flags | MemoryFlags::Executable;
        if (flags & 0x10) mem_flags = mem_flags | MemoryFlags::CacheDisable;
        return static_cast<Process<KernelAccessor>::AddressSpaceType*>(as)->mapUser(virt, phys, mem_flags);
    };
    oz::g_vspace_unmap_fn = [](void* as, std::uintptr_t virt) -> bool {
        return static_cast<Process<KernelAccessor>::AddressSpaceType*>(as)->unmapUser(virt);
    };
    // Phase 4: IRQ handler dispatch table (maps vector -> IRQHandler*)
    oz::g_register_irq_handler_fn = [](std::uint8_t vector, IRQHandler* handler) {
        oz::irq::registerHandler(vector, handler);
    };
    oz::irq::setUnblockFn([](Thread* t) {
        kernel_storage.kernel.scheduler.queueThread(t);
    });
    oz::g_eoi_fn = []() {
        oz::x86_64::notifyEndOfInterrupt();
    };
    // Phase 4: I/O port function pointers
    oz::g_read_io8_fn = oz::x86_64::readIO8;
    oz::g_read_io16_fn = oz::x86_64::readIO16;
    oz::g_read_io32_fn = oz::x86_64::readIO32;
    oz::g_write_io8_fn = oz::x86_64::writeIO8;
    oz::g_write_io16_fn = oz::x86_64::writeIO16;
    oz::g_write_io32_fn = oz::x86_64::writeIO32;
    g.clearScreen();
    sh.printString("Finish init\n\rStart Kernel in Higher-Half!\n\r");
    sh.repaint();



    
    std::uint64_t ahci_abar_phys = 0;
    std::uint64_t xhci_bar_phys = 0;
    if (platform_info_->RSDP) {
        ACPI::RootSystemDescriptionPointer* rsdp = reinterpret_cast<ACPI::RootSystemDescriptionPointer*>(platform_info_->RSDP);
        ACPI::ExtendedSystemDescriptionTable* xsdt = oz::phys_to_virt(createPhysicalAddress<ACPI::ExtendedSystemDescriptionTable>(rsdp->XsdtAddress));
        ACPIUtils::ExtendedSystemDescriptionTableWrapper xsdtWrapper(xsdt);
        
        PCIe::MemorymappedConfigurationSpaceDescriptionTable* mcfg = xsdtWrapper.getTable<PCIe::MemorymappedConfigurationSpaceDescriptionTable>();
        if (mcfg) {
            PCIUtils::MemorymappedConfigurationSpaceWrapper mcfgWrapper(mcfg);
            PCIUtils::PCIFunction ahci_func = mcfgWrapper.findFunction(0x01, 0x06, 0x01);
            if (ahci_func) {
                auto* ahci_header = reinterpret_cast<volatile PCI::PCIConfigurationHeaderType0*>(ahci_func.header);
                ahci_abar_phys = ahci_header->BaseAddressRegister[5] & 0xFFFFFFFFFFFFFFF0;
                {
                    char hex_buf[19] = "0x";
                    const char hex_chars[] = "0123456789ABCDEF";
                    for (int h = 0; h < 16; h++) {
                        hex_buf[2 + h] = hex_chars[(ahci_abar_phys >> (60 - h * 4)) & 0xF];
                    }
                    hex_buf[18] = '\0';
                    dprint("[Kernel] AHCI ABAR phys: ");
                    dprint(hex_buf);
                    dprint("\n\r");
                }
            }
            
            PCIUtils::PCIFunction xhci_func = mcfgWrapper.findFunction(0x0C, 0x03, 0x30);
            if (xhci_func) {
                sh.printString("Found xHCI Controller! Setting up for user space...\n\r");
                sh.repaint();
                std::uint32_t bar0 = xhci_func.header->BaseAddressRegister[0];
                std::uint32_t bar1 = xhci_func.header->BaseAddressRegister[1];
                xhci_bar_phys = bar0 & ~0xFULL;
                if ((bar0 & 0b110) == 0b100) {
                    xhci_bar_phys |= static_cast<std::uint64_t>(bar1) << 32;
                }
                {
                    char hex_buf[19] = "0x";
                    const char hex_chars[] = "0123456789ABCDEF";
                    for (int h = 0; h < 16; h++) {
                        hex_buf[2 + h] = hex_chars[(xhci_bar_phys >> (60 - h * 4)) & 0xF];
                    }
                    hex_buf[18] = '\0';
                    dprint("[Kernel] xHCI BAR phys: ");
                    dprint(hex_buf);
                    dprint("\n\r");
                }
                
                // Enable PCI bus master and memory space access, disable INTx
                xhci_func.header->Command |= (1 << 10) | 0b110;
                
                std::uint8_t msi_offset;
                if (xhci_func.findCapability(PCI::CapabilityID_MSI, &msi_offset)) {
                    PCIUtils::MSICapabilityWrapper msi(xhci_func, msi_offset);
                    msi.setMessageAddress(PCI::x86_64::makeMSIMessageAddress(oz::x86_64::getLocalAPICID()));
                    msi.setMessageData(PCI::x86_64::makeMSIMessageData(0x40));
                    msi.enable();
                } else if (xhci_func.findCapability(PCI::CapabilityID_MSIX, &msi_offset)) {
                    PCIUtils::MSIXCapabilityWrapper msix(xhci_func, msi_offset);
                    msix.setEntry(0, 0, PCI::x86_64::makeMSIMessageData(0x40), PCI::x86_64::makeMSIMessageAddress(oz::x86_64::getLocalAPICID()));
                    msix.enable();
                }
            } else {
                dprint("[Kernel] xHCI Controller NOT FOUND!\n\r");
            }
            

        }
    }
    

    // --- User Process Test ---
    sh.printString("Loading servers from initrd...\n\r");
    sh.repaint();

    Thread* console_thread = nullptr;
    Thread* init_thread = nullptr;
    Thread* vfs_thread = nullptr;
    Thread* ahci_thread = nullptr;
    Thread* xhci_thread = nullptr;

    const oz_boot::BootModuleInfo* initrd_file = nullptr;
    for (std::uint64_t i = 0; i < platform_info_->num_modules; ++i) {
        bool match = true;
        const char* target = "initrd.tar";
        for (int j = 0; j < 10; ++j) {
            if (platform_info_->modules[i].name[j] != target[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            initrd_file = &platform_info_->modules[i];
            break;
        }
    }

    if (initrd_file && initrd_file->base_address) {
        const std::uint8_t* tar_ptr = reinterpret_cast<const std::uint8_t*>(initrd_file->base_address);
        std::uint64_t tar_size = initrd_file->size;
        
        std::uint64_t offset = 0;
        while (offset + 512 <= tar_size) {
            const char* name = reinterpret_cast<const char*>(tar_ptr + offset);
            if (name[0] == '\0') break; // end of tar
            
            const char* size_str = reinterpret_cast<const char*>(tar_ptr + offset + 124);
            std::uint64_t file_size = 0;
            for (int i = 0; i < 11; ++i) {
                if (size_str[i] >= '0' && size_str[i] <= '7') {
                    file_size = (file_size * 8) + (size_str[i] - '0');
                }
            }
            
            const std::uint8_t* file_data = tar_ptr + offset + 512;
            
            bool is_console = true;
            const char* cons_name = "console";
            for (int j=0; j<7; ++j) if(name[j] != cons_name[j]) is_console = false;
            
            bool is_init = true;
            const char* init_name = "init";
            for (int j=0; j<4; ++j) if(name[j] != init_name[j]) is_init = false;
            
            bool is_vfs = true;
            const char* vfs_name = "vfs";
            for (int j=0; j<3; ++j) if(name[j] != vfs_name[j]) is_vfs = false;
            
            bool is_ahci = true;
            const char* ahci_name = "ahci";
            for (int j=0; j<4; ++j) if(name[j] != ahci_name[j]) is_ahci = false;

            bool is_xhci = true;
            const char* xhci_name = "xhci";
            for (int j=0; j<4; ++j) if(name[j] != xhci_name[j]) is_xhci = false;

            if (is_console) console_thread = loadProcessFromElfBuffer(file_data, file_size);
            if (is_init) init_thread = loadProcessFromElfBuffer(file_data, file_size);
            if (is_vfs) vfs_thread = loadProcessFromElfBuffer(file_data, file_size);
            if (is_ahci) ahci_thread = loadProcessFromElfBuffer(file_data, file_size);
            if (is_xhci) xhci_thread = loadProcessFromElfBuffer(file_data, file_size);
            
            offset += 512 + ((file_size + 511) & ~511ULL);
        }
    }

    if (console_thread && init_thread && vfs_thread && ahci_thread && xhci_thread) {
        auto* console_proc = static_cast<Process<KernelAccessor>*>(console_thread->user_data);
        auto* init_proc = static_cast<Process<KernelAccessor>*>(init_thread->user_data);
        auto* vfs_proc = static_cast<Process<KernelAccessor>*>(vfs_thread->user_data);
        auto* ahci_proc = static_cast<Process<KernelAccessor>*>(ahci_thread->user_data);
        auto* xhci_proc = static_cast<Process<KernelAccessor>*>(xhci_thread->user_data);
        
        // Helper lambdas for UntypedMemory
        auto alloc_fn = [](std::size_t num_pages, void** cleanup_data) -> std::uintptr_t {
            auto& fm = KernelAccessor::getFrameManager();
            std::uint8_t level = 0;
            std::size_t pages = 1;
            while (pages < num_pages) { level++; pages <<= 1; }
            PageBlock<> block = fm.allocateBlock(level);
            if (!block) return 0;
            *cleanup_data = block.getDescriptor();
            return fm.getPhysicalAddress(block).get();
        };
        auto free_fn = [](void* cleanup_data) {
            auto& fm = KernelAccessor::getFrameManager();
            fm.freeBlock(static_cast<PageFrameDescriptor*>(cleanup_data));
        };

        auto setup_proc_caps = [&](Process<KernelAccessor>* proc) {
            auto* vspace = new VSpace(proc->address_space.get(), g_vspace_map_fn, g_vspace_unmap_fn);
            Capability vspace_cap(vspace, CapRights::ALL);
            proc->cspace->insert(0, vspace_cap);
            vspace->release(); // CSpace now owns it

            auto* untyped = new UntypedMemory(1024, alloc_fn, free_fn); // 1024 pages = 4MB budget
            Capability untyped_cap(untyped, CapRights::ALL);
            proc->cspace->insert(240, untyped_cap);
            untyped->release();
        };

        setup_proc_caps(console_proc);
        setup_proc_caps(init_proc);
        setup_proc_caps(vfs_proc);
        setup_proc_caps(ahci_proc);
        setup_proc_caps(xhci_proc);

        ChannelEndpoint* end_init = nullptr;
        ChannelEndpoint* end_console = nullptr;
        create_channel_pair(&end_init, &end_console);
        
        Capability write_cap(end_init, CapRights::WRITE | CapRights::GRANT);
        init_proc->cspace->insert(1, write_cap); // Slot 1
        vfs_proc->cspace->insert(1, write_cap); // Slot 1
        ahci_proc->cspace->insert(1, write_cap); // Slot 1
        xhci_proc->cspace->insert(1, write_cap); // Slot 1
        
        Capability console_cap(end_console, CapRights::READ);
        console_proc->cspace->insert(1, console_cap); // Slot 1
        
        ChannelEndpoint* end_init_ahci = nullptr;
        ChannelEndpoint* end_ahci_init = nullptr;
        create_channel_pair(&end_init_ahci, &end_ahci_init);
        
        Capability init_ahci_cap(end_init_ahci, CapRights::WRITE | CapRights::GRANT);
        init_proc->cspace->insert(2, init_ahci_cap); // Slot 2 for init to write to ahci
        
        Capability ahci_init_cap(end_ahci_init, CapRights::READ);
        ahci_proc->cspace->insert(2, ahci_init_cap); // Slot 2 for ahci to read from init
        
        // Channel: VFS → AHCI (block I/O requests)
        ChannelEndpoint* end_vfs_to_ahci = nullptr;
        ChannelEndpoint* end_ahci_from_vfs = nullptr;
        create_channel_pair(&end_vfs_to_ahci, &end_ahci_from_vfs);
        
        Capability vfs_send_cap(end_vfs_to_ahci, CapRights::WRITE | CapRights::GRANT);
        vfs_proc->cspace->insert(2, vfs_send_cap);   // VFS slot 2: send requests to AHCI
        
        Capability ahci_recv_cap(end_ahci_from_vfs, CapRights::READ);
        ahci_proc->cspace->insert(3, ahci_recv_cap);  // AHCI slot 3: recv requests from VFS
        
        // Channel: AHCI → VFS (block I/O responses)
        ChannelEndpoint* end_ahci_to_vfs = nullptr;
        ChannelEndpoint* end_vfs_from_ahci = nullptr;
        create_channel_pair(&end_ahci_to_vfs, &end_vfs_from_ahci);
        
        Capability ahci_send_cap(end_ahci_to_vfs, CapRights::WRITE | CapRights::GRANT);
        ahci_proc->cspace->insert(4, ahci_send_cap);  // AHCI slot 4: send responses to VFS
        
        Capability vfs_recv_cap(end_vfs_from_ahci, CapRights::READ);
        vfs_proc->cspace->insert(3, vfs_recv_cap);    // VFS slot 3: recv responses from AHCI

        // Channel: init → VFS (file operation requests)
        ChannelEndpoint* end_init_to_vfs = nullptr;
        ChannelEndpoint* end_vfs_from_init = nullptr;
        create_channel_pair(&end_init_to_vfs, &end_vfs_from_init);

        Capability init_vfs_send_cap(end_init_to_vfs, CapRights::WRITE | CapRights::GRANT);
        init_proc->cspace->insert(3, init_vfs_send_cap);  // Init slot 3: send to VFS

        Capability vfs_init_recv_cap(end_vfs_from_init, CapRights::READ);
        vfs_proc->cspace->insert(4, vfs_init_recv_cap);   // VFS slot 4: recv from init

        // Channel: VFS → init (file operation responses)
        ChannelEndpoint* end_vfs_to_init = nullptr;
        ChannelEndpoint* end_init_from_vfs = nullptr;
        create_channel_pair(&end_vfs_to_init, &end_init_from_vfs);

        Capability vfs_init_send_cap(end_vfs_to_init, CapRights::WRITE | CapRights::GRANT);
        vfs_proc->cspace->insert(5, vfs_init_send_cap);   // VFS slot 5: send to init

        Capability init_vfs_recv_cap(end_init_from_vfs, CapRights::READ);
        init_proc->cspace->insert(4, init_vfs_recv_cap);  // Init slot 4: recv from VFS

        // Setup BootInfo
        auto* pfd = fm.allocatePages(1);
        std::uint64_t bootinfo_phys = pfd ? fm.getPhysicalAddress(pfd).get() : 0;
        if (bootinfo_phys) {
            auto* bootinfo_virt = reinterpret_cast<std::uint64_t*>(phys_to_virt(createPhysicalAddress<void>(bootinfo_phys)));
            bootinfo_virt[0] = ahci_abar_phys; // Set ahci_abar_phys
            
            // Create Frame cap for BootInfo page
            auto* bootinfo_frame = new Frame(bootinfo_phys, 4096, false,
                pfd, free_fn); // pfd is the allocated page descriptor
            Capability bootinfo_cap(bootinfo_frame, CapRights::READ);
            init_proc->cspace->insert(243, bootinfo_cap);
            bootinfo_frame->release();
            sh.printString("BootInfo capability created.\n\r");
        } else {
            sh.printString("ERROR: Failed to allocate BootInfo page!\n\r");
        }

        if (ahci_abar_phys != 0) {
            auto* abar_frame = new Frame(ahci_abar_phys, 4096, true); // device memory, no cleanup
            Capability abar_cap(abar_frame, CapRights::ALL);
            ahci_proc->cspace->insert(241, abar_cap);
            abar_frame->release();
        }

        // Allocate shared buffer (16 pages = 64KB) for VFS-AHCI data transfer
        auto shared_block = fm.allocateBlock(4); // 2^4 = 16 pages
        if (shared_block) {
            std::uint64_t shared_phys = fm.getPhysicalAddress(shared_block).get();
            auto* shared_frame = new Frame(shared_phys, 16 * 4096, false,
                shared_block.getDescriptor(), free_fn);
            
            Capability shared_cap_vfs(shared_frame, CapRights::ALL);
            vfs_proc->cspace->insert(242, shared_cap_vfs);
            
            Capability shared_cap_ahci(shared_frame, CapRights::ALL);
            ahci_proc->cspace->insert(242, shared_cap_ahci);
            
            shared_frame->release(); // Both CSpaces now own references
        }
        

        // IRQ notification channel for xHCI interrupts
        ChannelEndpoint* end_irq_send = nullptr;
        ChannelEndpoint* end_irq_recv = nullptr;
        create_channel_pair(&end_irq_send, &end_irq_recv);

        Capability irq_recv_cap(end_irq_recv, CapRights::READ);
        xhci_proc->cspace->insert(2, irq_recv_cap); // Slot 2: recv IRQ notifications

        // Create IRQHandler for vector 0x40
        auto* xhci_irq_handler = new IRQHandler(0x40, 
            [](std::uint8_t vector, IRQHandler* handler) {
                oz::irq::registerHandler(vector, handler);
            },
            []() { oz::x86_64::notifyEndOfInterrupt(); });
        xhci_irq_handler->setNotificationEndpoint(end_irq_send);
        xhci_irq_handler->registerHandler();

        Capability irq_handler_cap(xhci_irq_handler, CapRights::ALL);
        xhci_proc->cspace->insert(3, irq_handler_cap); // Slot 3: IRQ handler
        xhci_irq_handler->release();

        // xHCI MMIO BAR Frame capability
        if (xhci_bar_phys != 0) {
            auto* xhci_mmio_frame = new Frame(xhci_bar_phys, 16 * 4096, true); // 64KB device memory, no cleanup
            Capability mmio_cap(xhci_mmio_frame, CapRights::ALL);
            xhci_proc->cspace->insert(241, mmio_cap);
            xhci_mmio_frame->release();
        }

        scheduler.queueThread(console_thread);
        scheduler.queueThread(ahci_thread);
        scheduler.queueThread(vfs_thread);
        scheduler.queueThread(init_thread);
        scheduler.queueThread(xhci_thread);
        sh.printString("All servers loaded successfully.\n\r");
    } else {
        sh.printString("Failed to load servers!\n\r");
    }
    sh.repaint();
    // ---------------------------
    
    // Create Idle / Reaper thread
    Thread* idle_t = scheduler.createThread(0, Settings::Scheduler::idle_reaper_task, &scheduler);
    scheduler.queueThread(idle_t);

    // Start APIC Timer for preemption
    scheduler.initMainThread();

    oz::x86_64::initAPICTimer(32);

    
    while (1) {
        __asm__ volatile("sti; hlt");
    }
}

template <typename KernelSettings>
Thread* KernelStorage<KernelSettings>::Kernel::loadProcessFromElfBuffer(const std::uint8_t* file_buf, std::size_t file_size) {
    auto user_as = ProcessAddressSpace<KernelAccessor>::create(kernel_space);
    if (!user_as) return nullptr;

    auto* elf_header = reinterpret_cast<const elf::Elf64_Ehdr*>(file_buf);
    if (!elf::isValidElf(elf_header)) {
        return nullptr;
    }

    std::uintptr_t entry_point = elf_header->e_entry;
    auto* phdr_table = reinterpret_cast<const elf::Elf64_Phdr*>(file_buf + elf_header->e_phoff);
    
    for (int i = 0; i < elf_header->e_phnum; ++i) {
        if (phdr_table[i].p_type == elf::PT_LOAD) {
            std::uintptr_t vaddr = phdr_table[i].p_vaddr;
            std::uint64_t memsz = phdr_table[i].p_memsz;
            std::uint64_t filesz = phdr_table[i].p_filesz;
            std::uint64_t offset = phdr_table[i].p_offset;
            
            std::uintptr_t vaddr_aligned = vaddr & ~0xFFFULL;
            std::uintptr_t vaddr_offset = vaddr - vaddr_aligned;
            std::uint64_t map_size = (memsz + vaddr_offset + 0xFFF) & ~0xFFFULL;
            
            for (std::uint64_t p = 0; p < map_size; p += 4096) {
                auto block = fm.allocateBlock(0);
                if (block) {
                    std::uint64_t paddr = fm.getPhysicalAddress(block).get();
                    MemoryFlags flags = MemoryFlags::Present | MemoryFlags::User;
                    if (phdr_table[i].p_flags & elf::PF_W) flags = flags | MemoryFlags::Writable;
                    if (phdr_table[i].p_flags & elf::PF_X) flags = flags | MemoryFlags::Executable;
                    
                    user_as->mapUser(vaddr_aligned + p, paddr, flags);
                    
                    void* virt_ptr = phys_to_virt(createPhysicalAddress<void>(paddr));
                    utils::memset(virt_ptr, 0, 4096);
                    
                    std::uint64_t page_vaddr_start = vaddr_aligned + p;
                    std::uint64_t page_vaddr_end = page_vaddr_start + 4096;
                    
                    if (page_vaddr_start < vaddr + filesz && page_vaddr_end > vaddr) {
                        std::uint64_t copy_start = utils::max(page_vaddr_start, static_cast<std::uint64_t>(vaddr));
                        std::uint64_t copy_end = utils::min(page_vaddr_end, static_cast<std::uint64_t>(vaddr + filesz));
                        std::uint64_t copy_size = copy_end - copy_start;
                        std::uint64_t file_offset = offset + (copy_start - vaddr);
                        std::uint64_t page_offset = copy_start - page_vaddr_start;
                        
                        utils::memcpy(reinterpret_cast<std::uint8_t*>(virt_ptr) + page_offset, 
                                        file_buf + file_offset, 
                                        copy_size);
                    }
                }
            }
        }
    }
    
    

    auto stack_block = fm.allocateBlock(1); // 2 pages
    if (!stack_block) return nullptr;

    void* stack_phys = reinterpret_cast<void*>(fm.getPhysicalAddress(stack_block).get());
    std::uintptr_t stack_virt = 0x700000000000 - 8192;
    
    user_as->mapUser(stack_virt, reinterpret_cast<std::uintptr_t>(stack_phys), MemoryFlags::Present | MemoryFlags::Writable | MemoryFlags::User);
    user_as->mapUser(stack_virt + 4096, reinterpret_cast<std::uintptr_t>(stack_phys) + 4096, MemoryFlags::Present | MemoryFlags::Writable | MemoryFlags::User);
    
    auto* proc = new Process<KernelAccessor>(1, std::move(user_as), nullptr, entry_point);

    Thread* user_thread = scheduler.createThread(1, [](void* arg) {
        auto* proc_ptr = static_cast<Process<KernelAccessor>*>(arg);
        
        auto* current_thread = KernelStorage<KernelSettings>::kernel_storage.kernel.scheduler.getCurrentThread();
        std::uint64_t kernel_stack_top = reinterpret_cast<std::uint64_t>(current_thread->stack_base) + current_thread->stack_size;
        
        setSyscallKernelStack(kernel_stack_top);
        x86_64::setKernelStack(kernel_stack_top);
        
        proc_ptr->activate();
        
        std::uintptr_t stack_top = 0x700000000000;
        enter_user_mode(proc_ptr->entry_point, stack_top - 8, 0x202);
    }, proc);
    
    user_thread->pml4_phys = proc->address_space->getArchContext().getPhysicalPML4();
    user_thread->user_data = proc;
    user_thread->cleanup = [](void* arg) {
        auto* p = static_cast<Process<KernelAccessor>*>(arg);
        delete p;
    };

    return user_thread;
}
}  // namespace oz
