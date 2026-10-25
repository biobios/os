#include "core/Kernel.hpp"

#include <cstdint>
#include <memory>

#include "../../user/src/common/utils/BootInfo.hpp"
#include "core/Channel.hpp"
#include "core/Frame.hpp"
#include "core/IOPort.hpp"
#include "core/IRQDispatch.hpp"
#include "core/IRQHandler.hpp"
#include "core/Syscall.hpp"
#include "core/TCB.hpp"
#include "core/UntypedMemory.hpp"
#include "core/VSpace.hpp"
#include "drivers/pci/PCIUtils.hpp"
#include "hardware/ACPIUtils.hpp"
#include "hardware/x86_64.hpp"
#include "memory/paging.hpp"
#include "utils/ELF.hpp"
#include "utils/new_delete.hpp"
#include "utils/utils.hpp"
#include "core/SavedInterruptFrame.hpp"
#include "core/Baton.hpp"

namespace oz {

SavedInterruptFrame g_saved_interrupt_frame;
Scheduler* g_scheduler_ptr = nullptr;
static Kernel* g_kernel_instance = nullptr;

Kernel& Kernel::instance() {
    return *g_kernel_instance;
}

Kernel::Kernel(oz_boot::PlatformInfo* platformInfo)
    : fm(&platformInfo->memory_map, oz::paging::x86_64::page_sizes[0])
    , pt_manager(getMasterPML4())
    , kernel_space(getMasterPML4(), createPhysicalAddress<PageTable>(reinterpret_cast<std::uintptr_t>(getMasterPML4()) - oz::KERNEL_VIRT_OFFSET))
    , scheduler()
    , platform_info_(platformInfo)
{
    g_kernel_instance = this;
    g_scheduler_ptr = &scheduler;
}

void Kernel::initSyscallAndStubs() {
    oz::x86_64::initIRQStubs();
    
    oz::g_exit_thread_func = []() {
        Kernel::instance().scheduler.exitThread();
    };
    oz::g_get_current_cspace_func = []() -> CSpace* {
        auto* ctx = Kernel::instance().scheduler.getCurrentContext();
        return ctx ? ctx->cspace : nullptr;
    };
    oz::g_get_current_thread_func = []() -> Thread* {
        return Kernel::instance().scheduler.getCurrentThread();
    };
    oz::g_get_current_context_func = []() -> ExecutionContext* {
        return Kernel::instance().scheduler.getCurrentContext();
    };
    oz::g_block_thread_func = []() {
        Kernel::instance().scheduler.schedule();
    };
    oz::g_unblock_thread_func = [](Thread* t) {
        if (!t) return;
        t->is_blocked = false;
        t->state = ThreadState::Ready;
        if (t->baton) {
            t->baton->notify_if_runnable();
        }
        if (t->sched_notif && t->sched_badge != 0) {
            t->sched_notif->signal(t->sched_badge);
        }
    };
    oz::g_queue_thread_func = [](Thread* t) {
        if (!t) return;
        t->is_blocked = false;
        t->state = ThreadState::Ready;
        if (t->baton) {
            t->baton->notify_if_runnable();
        }
        if (t->sched_notif && t->sched_badge != 0) {
            t->sched_notif->signal(t->sched_badge);
        }
    };
    oz::g_register_irq_handler_fn = [](std::uint8_t vector, IRQHandler* handler) {
        oz::irq::registerHandler(vector, handler);
    };
    oz::g_unregister_irq_handler_fn = [](std::uint8_t vector, IRQHandler* handler) {
        oz::irq::unregisterHandler(vector, handler);
    };
    oz::irq::setUnblockFn([](Thread* t) {
        if (!t) return;
        t->is_blocked = false;
        t->state = ThreadState::Ready;
        if (t->baton) {
            t->baton->notify_if_runnable();
        }
        if (t->sched_notif && t->sched_badge != 0) {
            t->sched_notif->signal(t->sched_badge);
        }
    });
    oz::irq::setTimerTickFn([]() {
        Kernel::instance().scheduler.tick();
    });
    oz::g_eoi_fn = []() {
        oz::x86_64::notifyEndOfInterrupt();
    };
    oz::g_read_io8_fn = oz::x86_64::readIO8;
    oz::g_read_io16_fn = oz::x86_64::readIO16;
    oz::g_read_io32_fn = oz::x86_64::readIO32;
    oz::g_write_io8_fn = oz::x86_64::writeIO8;
    oz::g_write_io16_fn = oz::x86_64::writeIO16;
    oz::g_write_io32_fn = oz::x86_64::writeIO32;

    dprint("[Kernel] Finish init\n\r[Kernel] Start Kernel in Higher-Half!\n\r");
}

Kernel::BootDevices Kernel::probeBootDevices() {
    BootDevices devices;
    if (!platform_info_->RSDP) return devices;

    auto* rsdp = reinterpret_cast<ACPI::RootSystemDescriptionPointer*>(platform_info_->RSDP);
    auto* xsdt = oz::phys_to_virt(createPhysicalAddress<ACPI::ExtendedSystemDescriptionTable>(rsdp->XsdtAddress));
    ACPIUtils::ExtendedSystemDescriptionTableWrapper xsdtWrapper(xsdt);
    
    auto* mcfg = xsdtWrapper.getTable<PCIe::MemorymappedConfigurationSpaceDescriptionTable>();
    if (!mcfg) return devices;

    PCIUtils::MemorymappedConfigurationSpaceWrapper mcfgWrapper(mcfg);

    // 1. Probe AHCI Controller
    PCIUtils::PCIFunction ahci_func = mcfgWrapper.findFunction(0x01, 0x06, 0x01);
    if (ahci_func) {
        auto* ahci_header = reinterpret_cast<volatile PCI::PCIConfigurationHeaderType0*>(ahci_func.header);
        devices.ahci_abar_phys = ahci_header->BaseAddressRegister[5] & 0xFFFFFFFFFFFFFFF0;
        
        char hex_buf[19] = "0x";
        const char hex_chars[] = "0123456789ABCDEF";
        for (int h = 0; h < 16; h++) {
            hex_buf[2 + h] = hex_chars[(devices.ahci_abar_phys >> (60 - h * 4)) & 0xF];
        }
        hex_buf[18] = '\0';
        dprint("[Kernel] AHCI ABAR phys: ");
        dprint(hex_buf);
        dprint("\n\r");

        // Enable PCI bus master and memory space access
        ahci_func.header->Command |= 0b110;
    }
    
    // 2. Probe xHCI Controller
    PCIUtils::PCIFunction xhci_func = mcfgWrapper.findFunction(0x0C, 0x03, 0x30);
    if (xhci_func) {
        dprint("[Kernel] Found xHCI Controller! Setting up for user space...\n\r");
        std::uint32_t bar0 = xhci_func.header->BaseAddressRegister[0];
        std::uint32_t bar1 = xhci_func.header->BaseAddressRegister[1];
        devices.xhci_bar_phys = bar0 & ~0xFULL;
        if ((bar0 & 0b110) == 0b100) {
            devices.xhci_bar_phys |= static_cast<std::uint64_t>(bar1) << 32;
        }
        
        char hex_buf[19] = "0x";
        const char hex_chars[] = "0123456789ABCDEF";
        for (int h = 0; h < 16; h++) {
            hex_buf[2 + h] = hex_chars[(devices.xhci_bar_phys >> (60 - h * 4)) & 0xF];
        }
        hex_buf[18] = '\0';
        dprint("[Kernel] xHCI BAR phys: ");
        dprint(hex_buf);
        dprint("\n\r");
        
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

    return devices;
}

const oz_boot::BootModuleInfo* Kernel::findBootModule(const char* name) {
    for (std::uint64_t i = 0; i < platform_info_->num_modules; ++i) {
        bool match = true;
        for (int j = 0; name[j] != '\0'; ++j) {
            if (platform_info_->modules[i].name[j] != name[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            return &platform_info_->modules[i];
        }
    }
    return nullptr;
}

Thread* Kernel::loadRootServerFromInitrd(UntypedMemory& boot_untyped, const oz_boot::BootModuleInfo* initrd_file) {
    if (!initrd_file || !initrd_file->base_address) return nullptr;

    const auto* tar_ptr = reinterpret_cast<const std::uint8_t*>(initrd_file->base_address);
    std::uint64_t tar_size = initrd_file->size;
    std::uint64_t offset = 0;

    while (offset + 512 <= tar_size) {
        const char* name = reinterpret_cast<const char*>(tar_ptr + offset);
        if (name[0] == '\0') break; // end of tar archive
        
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
        for (int j = 0; j < 4; ++j) {
            if (name[j] != init_name[j]) is_init = false;
        }
        
        if (is_init) {
            return loadProcessFromElfBuffer(boot_untyped, file_data, file_size);
        }
        
        offset += 512 + ((file_size + 511) & ~511ULL);
    }

    return nullptr;
}

void Kernel::setupRootServerCaps(Thread* init_thread, UntypedMemory& boot_untyped, const BootDevices& devices, const oz_boot::BootModuleInfo* initrd_file) {
    CSpace* init_cspace = init_thread->cspace;
    
    auto free_fn = [](void* cleanup_data) {
        auto& fm = Kernel::instance().fm;
        fm.freeBlock(static_cast<PageFrameDescriptor*>(cleanup_data));
    };

    // 2. Setup 64MB UntypedMemory for Init (Slot 240, for backwards compatibility)
    auto ut_block = fm.allocateBlock(14); // 16384 pages = 64MB
    std::uintptr_t ut_phys = fm.getPhysicalAddress(ut_block).get();
    auto* untyped = boot_untyped.allocateObject<UntypedMemory>(
        ut_phys, 16384 * 4096, nullptr, nullptr, false, true);
    Capability untyped_cap(untyped, CapRights::ALL);
    init_cspace->insert(240, untyped_cap);
    untyped->release();

    // 2.1 Allocate BootInfo frame BEFORE harvesting remaining physical memory
    std::uint64_t initrd_offset = 0;
    if (initrd_file && initrd_file->base_address) {
        std::uintptr_t initrd_raw_phys = initrd_file->base_address - oz::DIRECT_MAP_OFFSET;
        initrd_offset = initrd_raw_phys & 0xFFFULL;
    }

    auto* pfd = fm.allocatePages(1);
    std::uint64_t bootinfo_phys = pfd ? fm.getPhysicalAddress(pfd).get() : 0;
    oz::BootInfo* bootinfo_virt = nullptr;
    if (bootinfo_phys) {
        bootinfo_virt = reinterpret_cast<oz::BootInfo*>(phys_to_virt(createPhysicalAddress<void>(bootinfo_phys)));
        bootinfo_virt->ahci_abar_phys = devices.ahci_abar_phys;
        bootinfo_virt->xhci_bar_phys = devices.xhci_bar_phys;
        bootinfo_virt->initrd_size = initrd_file ? initrd_file->size : 0;
        bootinfo_virt->initrd_offset = initrd_offset;
        bootinfo_virt->fb_phys_addr = reinterpret_cast<std::uintptr_t>(platform_info_->frame_buffer_base) - oz::DIRECT_MAP_OFFSET;
        bootinfo_virt->fb_size = platform_info_->frame_buffer_size;
        bootinfo_virt->fb_width = platform_info_->frame_buffer_horizontal;
        bootinfo_virt->fb_height = platform_info_->frame_buffer_vertical;
    }

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
            init_cspace->insert(untyped_start_slot + untyped_count, ut_c);
            ut_obj->release();
            untyped_count++;
        }
    });
    dprint("[Kernel] Transferred system physical memory to Init as UntypedMemory caps.\n\r");

    if (bootinfo_virt) {
        bootinfo_virt->untyped_cap_start = untyped_start_slot;
        bootinfo_virt->untyped_cap_count = untyped_count;

        auto* bootinfo_frame = boot_untyped.allocateObject<Frame>(bootinfo_phys, 4096, false, pfd, free_fn);
        Capability bootinfo_cap(bootinfo_frame, CapRights::ALL);
        init_cspace->insert(243, bootinfo_cap);
        bootinfo_frame->release();
        dprint("[Kernel] BootInfo capability created.\n\r");
    }

    // 3. AHCI MMIO Frame (Slot 241)
    if (devices.ahci_abar_phys != 0) {
        auto* abar_frame = boot_untyped.allocateObject<Frame>(devices.ahci_abar_phys, 4096, true);
        Capability abar_cap(abar_frame, CapRights::ALL);
        init_cspace->insert(241, abar_cap);
        abar_frame->release();
    }

    // 4. xHCI MMIO Frame (Slot 242)
    if (devices.xhci_bar_phys != 0) {
        auto* xhci_mmio_frame = boot_untyped.allocateObject<Frame>(devices.xhci_bar_phys, 16 * 4096, true);
        Capability mmio_cap(xhci_mmio_frame, CapRights::ALL);
        init_cspace->insert(242, mmio_cap);
        xhci_mmio_frame->release();
    }


    // 6. Setup InitRD Frame capability (Slot 244)
    if (initrd_file && initrd_file->base_address) {
        std::uintptr_t initrd_raw_phys = initrd_file->base_address - oz::DIRECT_MAP_OFFSET;
        std::uintptr_t initrd_aligned_phys = initrd_raw_phys & ~0xFFFULL;
        std::size_t initrd_pages = (initrd_file->size + initrd_offset + 4095) / 4096;
        auto* initrd_frame = boot_untyped.allocateObject<Frame>(initrd_aligned_phys, initrd_pages * 4096, true);
        Capability initrd_cap(initrd_frame, CapRights::ALL);
        init_cspace->insert(244, initrd_cap);
        initrd_frame->release();
        dprint("[Kernel] InitRD Frame capability created.\n\r");
    }

    // 7. Setup Self CSpace capability (Slot 245)
    auto* cs_cap_obj = boot_untyped.allocateObject<CSpaceCap>(init_cspace, false);
    Capability cs_cap(cs_cap_obj, CapRights::ALL);
    init_cspace->insert(245, cs_cap);
    cs_cap_obj->release();

    // 8. Setup FrameBuffer capability (Slot 246)
    std::uintptr_t fb_phys = reinterpret_cast<std::uintptr_t>(platform_info_->frame_buffer_base) - oz::DIRECT_MAP_OFFSET;
    std::uint64_t fb_size = platform_info_->frame_buffer_size;
    if (fb_phys != 0 && fb_size != 0) {
        auto* fb_frame = boot_untyped.allocateObject<Frame>(fb_phys, fb_size, true);
        Capability fb_cap(fb_frame, CapRights::ALL);
        init_cspace->insert(246, fb_cap);
        fb_frame->release();
        dprint("[Kernel] FrameBuffer capability created.\n\r");
    }

    // 9. Setup Root IOPort capability (Slot 247)
    auto* root_ioport = boot_untyped.allocateObject<IOPort>(
        0x0000, 0x10000,
        g_read_io8_fn, g_read_io16_fn, g_read_io32_fn,
        g_write_io8_fn, g_write_io16_fn, g_write_io32_fn
    );
    if (root_ioport) {
        Capability ioport_cap(root_ioport, CapRights::ALL);
        init_cspace->insert(247, ioport_cap);
        root_ioport->release();
        dprint("[Kernel] Root IOPort capability created (Slot 247).\n\r");
    }

    // 10. Setup Root Baton capability (Slot 248)
    auto* root_baton = boot_untyped.allocateObject<Baton>(true);
    if (root_baton) {
        root_baton->is_infinite = true;
        root_baton->budget = ~0ULL;
        root_baton->thread = init_thread;
        root_baton->is_active = true;
        init_thread->baton = root_baton;
        Capability baton_cap(root_baton, CapRights::ALL);
        init_cspace->insert(248, baton_cap);
        root_baton->release();
        dprint("[Kernel] Root Baton capability created (Slot 248).\n\r");
    }
}

void Kernel::run() {
    initSyscallAndStubs();

    BootDevices devices = probeBootDevices();

    // Bootstrap UntypedMemory (4MB) for creating root server kernel objects
    dprint("[Kernel] Loading init process from initrd...\n\r");
    PageBlock boot_ut_block = fm.allocateBlock(10); // 1024 pages = 4MB
    std::uintptr_t boot_ut_phys = fm.getPhysicalAddress(boot_ut_block).get();
    UntypedMemory boot_untyped(boot_ut_phys, 1024 * 4096, nullptr, nullptr, false, true);

    const oz_boot::BootModuleInfo* initrd_file = findBootModule("initrd.tar");
    Thread* init_thread = loadRootServerFromInitrd(boot_untyped, initrd_file);

    if (init_thread) {
        setupRootServerCaps(init_thread, boot_untyped, devices, initrd_file);
        dprint("[Kernel] Init process loaded successfully (Root Scheduler).\n\r");
    } else {
        dprint("[Kernel] Failed to load init process!\n\r");
    }

    // Start APIC Timer for preemption
    scheduler.initMainThread();
    oz::x86_64::initAPICTimer(oz::x86_64::APIC_TIMER_VECTOR);

    // Initial dispatch to Root Scheduler (Init)
    scheduler.directSwitchTo(init_thread);

    while (1) {
        __asm__ volatile("sti; hlt");
    }
}

Thread* Kernel::loadProcessFromElfBuffer(UntypedMemory& boot_untyped, const std::uint8_t* file_buf, std::size_t file_size) {
    (void)file_size;

    std::uintptr_t init_pml4_phys = 0;
    void* init_pml4_mem = boot_untyped.allocateBytes(4096, 4096, init_pml4_phys);
    if (!init_pml4_mem) return nullptr;

    auto user_as = ProcessAddressSpace::createFromPhysical(
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
    
    auto* as_obj = boot_untyped.allocateObject<ProcessAddressSpace>(std::move(user_as));
    if (!as_obj) return nullptr;

    auto* init_cspace = boot_untyped.allocateObject<CSpace>();
    if (!init_cspace) return nullptr;

    // Slot 0: VSpace
    auto* vspace = boot_untyped.allocateObject<VSpace>(
        as_obj,
        as_obj->getArchContext().getPhysicalPML4().get()
    );
    Capability vspace_cap(vspace, CapRights::ALL);
    init_cspace->insert(0, vspace_cap);
    vspace->release();

    TCB* user_tcb = boot_untyped.retypeTCB();
    if (!user_tcb) return nullptr;
    Thread* user_thread = user_tcb->getThread();

    user_thread->pml4_phys = as_obj->getArchContext().getPhysicalPML4();
    user_thread->cspace = init_cspace;
    user_thread->user_rip = entry_point;
    user_thread->user_rsp = 0x700000000000 - 8;
    user_thread->cleanup = nullptr;

    scheduler.setupThreadContext(user_thread, user_thread_entry, user_thread);

    return user_thread;
}

}  // namespace oz
