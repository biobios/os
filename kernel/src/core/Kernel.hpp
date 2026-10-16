#pragma once

#include <cstdint>
#include "../../user/src/common/utils/BootInfo.hpp"

#include "memory/AddressSpace.hpp"
#include "memory/PageTable.hpp"
#include "memory/PageTableManager.hpp"
#include "memory/paging.hpp"
#include "core/bootStructures.hpp"
#include "drivers/pci/PCIUtils.hpp"
#include "hardware/ACPIUtils.hpp"
#include "utils/utils.hpp"
#include "core/Channel.hpp"
#include "core/Mutex.hpp"
#include "core/IScheduler.hpp"

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
        requires(scheduler<typename KernelSettings::Scheduler>)
    struct KernelAccessor2<Args...> : public KernelAccessor1 {
        using Settings = KernelAccessor1::Settings;
        static consteval auto getScheduler() -> Settings::Scheduler& {
            return kernel_storage.kernel.scheduler;
        }
    };

    template <typename... Args>
        requires(scheduler<typename KernelSettings::template SchedulerFunctor<KernelAccessor1>>)
    struct KernelAccessor2<Args...> : public KernelAccessor1 {
        struct Settings : public KernelAccessor1::Settings {
            using Scheduler = typename KernelSettings::template SchedulerFunctor<KernelAccessor1>;
        };
        static consteval auto getScheduler() -> Settings::Scheduler& {
            return kernel_storage.kernel.scheduler;
        }
    };

    using KernelAccessor = KernelAccessor2<>;

    class Kernel : public PhysicalAddressProvider {
        using Settings = KernelAccessor::Settings;
    public:
        Settings::FrameManager fm;
        PageTableManager<KernelAccessor> pt_manager;
        KernelAddressSpace<KernelAccessor> kernel_space;
        Settings::Scheduler scheduler;
        oz_boot::PlatformInfo* platform_info_;

        Kernel(oz_boot::PlatformInfo* platformInfo);
        void run();

    private:
        Thread* loadProcessFromElfBuffer(UntypedMemory& boot_untyped, const std::uint8_t* file_buf, std::size_t file_size);

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
    : fm(&platformInfo->memory_map, oz::paging::x86_64::page_sizes[0])
    , pt_manager(getMasterPML4())
    , kernel_space(getMasterPML4(), createPhysicalAddress<PageTable>(reinterpret_cast<std::uintptr_t>(getMasterPML4()) - oz::KERNEL_VIRT_OFFSET))
    , platform_info_(platformInfo)
{
}

template <typename KernelSettings>
void KernelStorage<KernelSettings>::Kernel::run() {
    oz::x86_64::initIRQStubs();
    
    oz::g_exit_thread_func = []() {
        kernel_storage.kernel.scheduler.exitThread();
    };
    oz::g_get_current_cspace_func = []() -> CSpace* {
        auto* thread = kernel_storage.kernel.scheduler.getCurrentThread();
        if (!thread) return nullptr;
        if (thread->cspace) return thread->cspace;
        if (!thread->user_data) return nullptr;
        auto* proc = static_cast<Process<KernelAccessor>*>(thread->user_data);
        return proc->cspace;
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
    oz::g_create_thread_func = nullptr;
    oz::g_queue_thread_func = [](Thread* t) {
        kernel_storage.kernel.scheduler.queueThread(t);
    };
    oz::g_create_vspace_func = [](UntypedMemory* untyped) -> VSpaceCreateResult {
        if (!untyped) return {nullptr, nullptr};
        std::uintptr_t pml4_phys = 0;
        void* pml4_mem = untyped->allocateBytes(4096, 4096, pml4_phys);
        if (!pml4_mem) return {nullptr, nullptr};

        auto user_as = ProcessAddressSpace<KernelAccessor>::createFromPhysical(
            createPhysicalAddress<PageTable>(pml4_phys),
            kernel_storage.kernel.kernel_space);
        if (!user_as.isValid()) return {nullptr, nullptr};
        auto* as_obj = untyped->allocateObject<ProcessAddressSpace<KernelAccessor>>(std::move(user_as));
        if (!as_obj) return {nullptr, nullptr};
        return {as_obj, reinterpret_cast<void*>(pml4_phys)};
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
    oz::g_vspace_install_pt_fn = [](void* as, std::uintptr_t virt, std::uintptr_t pt_phys, std::uint8_t level) -> bool {
        return static_cast<Process<KernelAccessor>::AddressSpaceType*>(as)->installPageTable(
            virt, createPhysicalAddress<PageTable>(pt_phys), level);
    };
    oz::g_vspace_missing_level_fn = [](void* as, std::uintptr_t virt) -> std::uint8_t {
        return static_cast<Process<KernelAccessor>::AddressSpaceType*>(as)->getMissingTableLevel(virt);
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
    dprint("[Kernel] Finish init\n\r[Kernel] Start Kernel in Higher-Half!\n\r");



    
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
                dprint("[Kernel] Found xHCI Controller! Setting up for user space...\n\r");
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
    

    // --- User Process Startup (Root Server) ---
    dprint("[Kernel] Loading init process from initrd...\n\r");

    PageBlock boot_ut_block = fm.allocateBlock(10); // 1024 pages = 4MB
    std::uintptr_t boot_ut_phys = fm.getPhysicalAddress(boot_ut_block).get();
    UntypedMemory boot_untyped(boot_ut_phys, 1024 * 4096, nullptr, nullptr, false, true);

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

    Thread* init_thread = nullptr;

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
            
            bool is_init = true;
            const char* init_name = "init";
            for (int j = 0; j < 4; ++j) if (name[j] != init_name[j]) is_init = false;
            
            if (is_init) {
                init_thread = loadProcessFromElfBuffer(boot_untyped, file_data, file_size);
                break;
            }
            
            offset += 512 + ((file_size + 511) & ~511ULL);
        }
    }

    if (init_thread) {
        auto* init_proc = static_cast<Process<KernelAccessor>*>(init_thread->user_data);
        
        auto free_fn = [](void* cleanup_data) {
            auto& fm = KernelAccessor::getFrameManager();
            fm.freeBlock(static_cast<PageFrameDescriptor*>(cleanup_data));
        };

        // 1. Setup VSpace (Slot 0)
        auto* vspace = boot_untyped.allocateObject<VSpace>(
            &init_proc->address_space, g_vspace_map_fn, g_vspace_unmap_fn,
            init_proc->address_space.getArchContext().getPhysicalPML4().get(),
            g_vspace_install_pt_fn, g_vspace_missing_level_fn
        );
        Capability vspace_cap(vspace, CapRights::ALL);
        init_proc->cspace->insert(0, vspace_cap);
        vspace->release();

        // 2. Setup 64MB UntypedMemory for Init (Slot 240, for backwards compatibility)
        auto ut_block = fm.allocateBlock(14); // 16384 pages = 64MB
        std::uintptr_t ut_phys = fm.getPhysicalAddress(ut_block).get();
        auto* untyped = boot_untyped.allocateObject<UntypedMemory>(
            ut_phys, 16384 * 4096, nullptr, nullptr, false, true);
        Capability untyped_cap(untyped, CapRights::ALL);
        init_proc->cspace->insert(240, untyped_cap);
        untyped->release();

        // 2.5 Transfer all remaining physical memory to Init as UntypedMemory capabilities (Slots 200..)
        std::uint32_t untyped_start_slot = 200;
        std::uint32_t untyped_count = 0;
        fm.forEachRemainingRegion([&](std::uintptr_t base, std::size_t size) {
            if (size < 64 * 1024) return;
            if (untyped_start_slot + untyped_count >= 240) return;
            auto* ut_obj = boot_untyped.allocateObject<UntypedMemory>(
                base, size, nullptr, nullptr, false, true);
            if (ut_obj) {
                Capability ut_c(ut_obj, CapRights::ALL);
                init_proc->cspace->insert(untyped_start_slot + untyped_count, ut_c);
                ut_obj->release();
                untyped_count++;
            }
        });
        dprint("[Kernel] Transferred system physical memory to Init as UntypedMemory caps.\n\r");

        // 3. AHCI MMIO Frame (Slot 241)
        if (ahci_abar_phys != 0) {
            auto* abar_frame = boot_untyped.allocateObject<Frame>(ahci_abar_phys, 4096, true);
            Capability abar_cap(abar_frame, CapRights::ALL);
            init_proc->cspace->insert(241, abar_cap);
            abar_frame->release();
        }

        // 4. xHCI MMIO Frame (Slot 242)
        if (xhci_bar_phys != 0) {
            auto* xhci_mmio_frame = boot_untyped.allocateObject<Frame>(xhci_bar_phys, 16 * 4096, true);
            Capability mmio_cap(xhci_mmio_frame, CapRights::ALL);
            init_proc->cspace->insert(242, mmio_cap);
            xhci_mmio_frame->release();
        }

        // 5. Setup BootInfo (Slot 243)
        std::uint64_t initrd_offset = 0;
        if (initrd_file && initrd_file->base_address) {
            std::uintptr_t initrd_raw_phys = initrd_file->base_address - oz::DIRECT_MAP_OFFSET;
            initrd_offset = initrd_raw_phys & 0xFFFULL;
        }

        auto* pfd = fm.allocatePages(1);
        std::uint64_t bootinfo_phys = pfd ? fm.getPhysicalAddress(pfd).get() : 0;
        if (bootinfo_phys) {
            auto* bootinfo_virt = reinterpret_cast<oz::BootInfo*>(phys_to_virt(createPhysicalAddress<void>(bootinfo_phys)));
            bootinfo_virt->ahci_abar_phys = ahci_abar_phys;
            bootinfo_virt->xhci_bar_phys = xhci_bar_phys;
            bootinfo_virt->initrd_size = initrd_file ? initrd_file->size : 0;
            bootinfo_virt->initrd_offset = initrd_offset;
            bootinfo_virt->fb_phys_addr = reinterpret_cast<std::uintptr_t>(platform_info_->frame_buffer_base) - oz::DIRECT_MAP_OFFSET;
            bootinfo_virt->fb_size = platform_info_->frame_buffer_size;
            bootinfo_virt->fb_width = platform_info_->frame_buffer_horizontal;
            bootinfo_virt->fb_height = platform_info_->frame_buffer_vertical;
            bootinfo_virt->untyped_cap_start = untyped_start_slot;
            bootinfo_virt->untyped_cap_count = untyped_count;

            auto* bootinfo_frame = boot_untyped.allocateObject<Frame>(bootinfo_phys, 4096, false,
                pfd, free_fn);
            Capability bootinfo_cap(bootinfo_frame, CapRights::READ);
            init_proc->cspace->insert(243, bootinfo_cap);
            bootinfo_frame->release();
            dprint("[Kernel] BootInfo capability created.\n\r");
        }

        // 6. Setup InitRD Frame capability (Slot 244)
        if (initrd_file && initrd_file->base_address) {
            std::uintptr_t initrd_raw_phys = initrd_file->base_address - oz::DIRECT_MAP_OFFSET;
            std::uintptr_t initrd_aligned_phys = initrd_raw_phys & ~0xFFFULL;
            std::size_t initrd_pages = (initrd_file->size + initrd_offset + 4095) / 4096;
            auto* initrd_frame = boot_untyped.allocateObject<Frame>(initrd_aligned_phys, initrd_pages * 4096, true);
            Capability initrd_cap(initrd_frame, CapRights::READ);
            init_proc->cspace->insert(244, initrd_cap);
            initrd_frame->release();
            dprint("[Kernel] InitRD Frame capability created.\n\r");
        }

        // 7. Setup Self CSpace capability (Slot 245)
        auto* cs_cap_obj = boot_untyped.allocateObject<CSpaceCap>(init_proc->cspace, false);
        Capability cs_cap(cs_cap_obj, CapRights::ALL);
        init_proc->cspace->insert(245, cs_cap);
        cs_cap_obj->release();

        // 8. Setup FrameBuffer capability (Slot 246)
        std::uintptr_t fb_phys = reinterpret_cast<std::uintptr_t>(platform_info_->frame_buffer_base) - oz::DIRECT_MAP_OFFSET;
        std::uint64_t fb_size = platform_info_->frame_buffer_size;
        if (fb_phys != 0 && fb_size != 0) {
            auto* fb_frame = boot_untyped.allocateObject<Frame>(fb_phys, fb_size, true);
            Capability fb_cap(fb_frame, CapRights::ALL);
            init_proc->cspace->insert(246, fb_cap);
            fb_frame->release();
            dprint("[Kernel] FrameBuffer capability created.\n\r");
        }

        scheduler.queueThread(init_thread);
        dprint("[Kernel] Init process loaded successfully.\n\r");
    } else {
        dprint("[Kernel] Failed to load init process!\n\r");
    }
    // ---------------------------
    
    // Create Idle / Reaper thread (static allocation)
    alignas(Thread) static std::uint8_t idle_thread_mem[sizeof(Thread)];
    alignas(16) static std::uint8_t idle_stack[4096];
    Thread* idle_t = new (idle_thread_mem) Thread(
        0, idle_stack, sizeof(idle_stack), PageBlock<>{nullptr}, 0
    );
    idle_t->is_untyped_backed = true;
    scheduler.setupThreadContext(idle_t, Settings::Scheduler::idle_reaper_task, &scheduler);
    scheduler.queueThread(idle_t);

    // Start APIC Timer for preemption
    scheduler.initMainThread();

    oz::x86_64::initAPICTimer(32);

    
    while (1) {
        __asm__ volatile("sti; hlt");
    }
}

template <typename KernelSettings>
Thread* KernelStorage<KernelSettings>::Kernel::loadProcessFromElfBuffer(UntypedMemory& boot_untyped, const std::uint8_t* file_buf, std::size_t file_size) {
    (void)file_size;

    std::uintptr_t init_pml4_phys = 0;
    void* init_pml4_mem = boot_untyped.allocateBytes(4096, 4096, init_pml4_phys);
    if (!init_pml4_mem) return nullptr;

    auto user_as = ProcessAddressSpace<KernelAccessor>::createFromPhysical(
        createPhysicalAddress<PageTable>(init_pml4_phys), kernel_space);
    if (!user_as.isValid()) return nullptr;

    auto ensure_pt = [&](std::uintptr_t vaddr) -> bool {
        for (std::uint8_t lvl = 3; lvl >= 1; --lvl) {
            if (user_as.getMissingTableLevel(vaddr) == lvl) {
                std::uintptr_t pt_phys = 0;
                void* pt_mem = boot_untyped.allocateBytes(4096, 4096, pt_phys);
                if (!pt_mem) return false;
                utils::memset(pt_mem, 0, 4096);
                if (!user_as.installPageTable(vaddr, createPhysicalAddress<PageTable>(pt_phys), lvl)) {
                    return false;
                }
            }
        }
        return true;
    };

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
                std::uintptr_t paddr = 0;
                void* virt_ptr = boot_untyped.allocateBytes(4096, 4096, paddr);
                if (virt_ptr) {
                    MemoryFlags flags = MemoryFlags::Present | MemoryFlags::User;
                    if (phdr_table[i].p_flags & elf::PF_W) flags = flags | MemoryFlags::Writable;
                    if (phdr_table[i].p_flags & elf::PF_X) flags = flags | MemoryFlags::Executable;
                    
                    if (!ensure_pt(vaddr_aligned + p)) return nullptr;
                    user_as.mapUser(vaddr_aligned + p, paddr, flags);
                    
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

    std::uintptr_t stack_phys0 = 0;
    void* stack_mem0 = boot_untyped.allocateBytes(4096, 4096, stack_phys0);
    std::uintptr_t stack_phys1 = 0;
    void* stack_mem1 = boot_untyped.allocateBytes(4096, 4096, stack_phys1);
    if (!stack_mem0 || !stack_mem1) return nullptr;

    std::uintptr_t stack_virt = 0x700000000000 - 8192;
    if (!ensure_pt(stack_virt)) return nullptr;
    user_as.mapUser(stack_virt, stack_phys0, MemoryFlags::Present | MemoryFlags::Writable | MemoryFlags::User);
    user_as.mapUser(stack_virt + 4096, stack_phys1, MemoryFlags::Present | MemoryFlags::Writable | MemoryFlags::User);
    
    auto* proc = boot_untyped.allocateObject<Process<KernelAccessor>>(1, std::move(user_as), nullptr, entry_point, boot_untyped.allocateObject<CSpace>());
    if (!proc) return nullptr;

    TCB* user_tcb = boot_untyped.retypeTCB();
    if (!user_tcb) return nullptr;
    Thread* user_thread = user_tcb->getThread();

    scheduler.setupThreadContext(user_thread, [](void* arg) {
        auto* proc_ptr = static_cast<Process<KernelAccessor>*>(arg);
        
        auto* current_thread = KernelStorage<KernelSettings>::kernel_storage.kernel.scheduler.getCurrentThread();
        std::uint64_t kernel_stack_top = reinterpret_cast<std::uint64_t>(current_thread->stack_base) + current_thread->stack_size;
        
        setSyscallKernelStack(kernel_stack_top);
        x86_64::setKernelStack(kernel_stack_top);
        
        proc_ptr->activate();
        
        std::uintptr_t stack_top = 0x700000000000;
        enter_user_mode(proc_ptr->entry_point, stack_top - 8, 0x202);
    }, proc);
    
    user_thread->pml4_phys = proc->address_space.getArchContext().getPhysicalPML4();
    user_thread->user_data = proc;
    user_thread->cspace = proc->cspace;
    user_thread->cleanup = nullptr;

    return user_thread;
}
}  // namespace oz
