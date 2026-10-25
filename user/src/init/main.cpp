#include "syscall.hpp"
#include "loader.hpp"
#include "../common/utils/BootInfo.hpp"
#include "../common/ipc/Protocol.hpp"
#include "../common/ipc/StartupInfo.hpp"

// Cap slot assignments for init:
//   0: CAP_SELF_VSPACE
//   1: write to console / COM1 (sys_write / sys_debug_print)
//   240: CAP_UNTYPED
//   241: CAP_AHCI_MMIO
//   242: CAP_XHCI_MMIO
//   243: CAP_BOOTINFO
//   244: CAP_INITRD
//   245: CAP_SELF_CSPACE
//   246: CAP_FRAMEBUFFER
//   247: CAP_ROOT_IOPORT
//   248: CAP_ROOT_BATON

static constexpr std::uint64_t BOOTINFO_VADDR = 0x600000000000ULL;
static constexpr std::uint64_t INITRD_VADDR   = 0x500000000000ULL;

extern "C" void _start() {
    oz::sys_debug_print("[Init] OZ OS Bootstrap Root Task started.\n\r");

    // 1. Map BootInfo Frame (slot 243)
    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, oz::CAP_BOOTINFO, BOOTINFO_VADDR, 0x00, 4096) != 0) {
        oz::sys_debug_print("[Init] ERROR: Failed to map BootInfo!\n\r");
        oz::sys_exit(1);
    }
    const volatile auto* boot_info = reinterpret_cast<const volatile oz::BootInfo*>(BOOTINFO_VADDR);
    std::uint64_t initrd_size = boot_info->initrd_size;
    std::uint64_t ahci_abar_phys = boot_info->ahci_abar_phys;
    std::uint64_t xhci_bar_phys = boot_info->xhci_bar_phys;

    // 2. Map InitRD Frame (slot 244)
    if (initrd_size > 0) {
        if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, oz::CAP_INITRD, INITRD_VADDR, 0x00, initrd_size) != 0) {
            oz::sys_debug_print("[Init] ERROR: Failed to map InitRD!\n\r");
            oz::sys_exit(1);
        }
    }
    const std::uint8_t* initrd_ptr = reinterpret_cast<const std::uint8_t*>(INITRD_VADDR + boot_info->initrd_offset);
    oz::sys_debug_print("[Init] BootInfo and InitRD mapped successfully.\n\r");

    // 3. UntypedMemory Initial Slicing:
    // Retype 4MB for Sched (Core 5 baton pool and notifications)
    std::uint32_t sched_untyped = oz::sys_untyped_retype(
        oz::CAP_UNTYPED,
        static_cast<std::uint32_t>(oz::ObjectType::UntypedMemory),
        4 * 1024 * 1024
    );
    if (sched_untyped == oz::INVALID_CAP_ID) {
        oz::sys_debug_print("[Init] ERROR: Failed to slice Sched UntypedMemory!\n\r");
        oz::sys_exit(1);
    }

    // Retype 2MB for DrivMan (IRQ handlers and device objects)
    std::uint32_t drivman_untyped = oz::sys_untyped_retype(
        oz::CAP_UNTYPED,
        static_cast<std::uint32_t>(oz::ObjectType::UntypedMemory),
        2 * 1024 * 1024
    );
    if (drivman_untyped == oz::INVALID_CAP_ID) {
        oz::sys_debug_print("[Init] ERROR: Failed to slice DrivMan UntypedMemory!\n\r");
        oz::sys_exit(1);
    }

    // =========================================================================
    // Core 5 Component 1: MemMan (Memory Manager)
    // =========================================================================
    oz::loader::TarFile mem_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "memman");
    if (!mem_file.data) {
        oz::sys_debug_print("[Init] ERROR: memman not found in initrd!\n\r");
        oz::sys_exit(1);
    }
    auto mem_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, mem_file.data, mem_file.size);
    if (!mem_proc.success) {
        oz::sys_debug_print("[Init] ERROR: Failed to spawn memman!\n\r");
        oz::sys_exit(1);
    }

    std::uint32_t memman_req_send = 0, memman_req_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &memman_req_send, &memman_req_recv);
    std::uint32_t memman_reply_send = 0, memman_reply_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &memman_reply_send, &memman_reply_recv);

    oz::sys_cspace_cap_insert(mem_proc.cspace_cap, 2, memman_req_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(mem_proc.cspace_cap, 3, memman_reply_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    // Delegate primary UntypedMemory (Slot 240) to MemMan
    oz::sys_cspace_cap_insert(mem_proc.cspace_cap, oz::CAP_UNTYPED, oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::CapRights::ALL));
    // Delegate all remaining physical memory blocks (slots 200..) to MemMan
    for (std::uint32_t i = 0; i < boot_info->untyped_cap_count && (200 + i) < 240; ++i) {
        oz::sys_cspace_cap_insert(mem_proc.cspace_cap, 200 + i, 200 + i, static_cast<std::uint32_t>(oz::CapRights::ALL));
    }
    oz::sys_debug_print("[Init] MemMan configured.\n\r");

    // =========================================================================
    // Core 5 Component 2: VFS (Virtual File System)
    // =========================================================================
    oz::loader::TarFile vfs_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "vfs");
    if (!vfs_file.data) {
        oz::sys_debug_print("[Init] ERROR: vfs not found in initrd!\n\r");
        oz::sys_exit(1);
    }
    auto vfs_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, vfs_file.data, vfs_file.size);
    if (!vfs_proc.success) {
        oz::sys_debug_print("[Init] ERROR: Failed to spawn vfs!\n\r");
        oz::sys_exit(1);
    }

    std::uint32_t vfs_req_send = 0, vfs_req_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &vfs_req_send, &vfs_req_recv);
    std::uint32_t vfs_procman_reply_send = 0, procman_vfs_reply_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &vfs_procman_reply_send, &procman_vfs_reply_recv);

    // Shared buffers for Shell (Client 1) and ProcMan (Client 2)
    std::uint32_t shell_client_frame = oz::sys_frame_alloc(oz::CAP_UNTYPED, 16);
    std::uint32_t procman_client_frame = oz::sys_frame_alloc(oz::CAP_UNTYPED, 16);

    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 4, vfs_req_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 7, vfs_procman_reply_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 231, shell_client_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, oz::CAP_UNTYPED, oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, oz::CAP_INITRD, oz::CAP_INITRD, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 245, procman_client_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_debug_print("[Init] VFS configured.\n\r");

    // =========================================================================
    // Core 5 Component 3: DrivMan (Device Manager Broker)
    // =========================================================================
    oz::loader::TarFile drivman_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "drivman");
    if (!drivman_file.data) {
        oz::sys_debug_print("[Init] ERROR: drivman not found in initrd!\n\r");
        oz::sys_exit(1);
    }
    auto drivman_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, drivman_file.data, drivman_file.size);
    if (!drivman_proc.success) {
        oz::sys_debug_print("[Init] ERROR: Failed to spawn drivman!\n\r");
        oz::sys_exit(1);
    }

    std::uint32_t drivman_send = 0, drivman_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &drivman_send, &drivman_recv);

    oz::sys_cspace_cap_insert(drivman_proc.cspace_cap, 2, drivman_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(drivman_proc.cspace_cap, oz::CAP_UNTYPED, drivman_untyped, static_cast<std::uint32_t>(oz::CapRights::ALL));
    if (ahci_abar_phys != 0) {
        oz::sys_cspace_cap_insert(drivman_proc.cspace_cap, 241, oz::CAP_AHCI_MMIO, static_cast<std::uint32_t>(oz::CapRights::ALL));
    }
    if (xhci_bar_phys != 0) {
        oz::sys_cspace_cap_insert(drivman_proc.cspace_cap, 242, oz::CAP_XHCI_MMIO, static_cast<std::uint32_t>(oz::CapRights::ALL));
    }
    oz::sys_cspace_cap_insert(drivman_proc.cspace_cap, oz::CAP_BOOTINFO, oz::CAP_BOOTINFO, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(drivman_proc.cspace_cap, oz::CAP_FRAMEBUFFER, oz::CAP_FRAMEBUFFER, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(drivman_proc.cspace_cap, oz::CAP_ROOT_IOPORT, oz::CAP_ROOT_IOPORT, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_debug_print("[Init] DrivMan configured.\n\r");

    // =========================================================================
    // Core 5 Component 4: Sched (Scheduler IPC & Notification Endpoints)
    // =========================================================================
    std::uint32_t sched_send = 0, sched_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &sched_send, &sched_recv);
    std::uint32_t sched_notif = oz::sys_notification_create(oz::CAP_UNTYPED);
    static constexpr std::uint64_t BADGE_SCHED_IPC = 1ULL << 63;
    oz::sys_channel_bind_notification(sched_recv, sched_notif, BADGE_SCHED_IPC);

    // =========================================================================
    // Core 5 Component 5: ProcMan (Process Manager & DAG Orchestrator)
    // =========================================================================
    oz::loader::TarFile proc_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "procman");
    if (!proc_file.data) {
        oz::sys_debug_print("[Init] ERROR: procman not found in initrd!\n\r");
        oz::sys_exit(1);
    }

    oz::ProcessStartupInfo procman_startup_info{};
    procman_startup_info.add_cap(6, oz::CapRole::ROOT_DIR, "/", oz::CapType::DIR);
    procman_startup_info.add_cap(11, oz::CapRole::DRIVMAN, "drivman", oz::CapType::STREAM);
    procman_startup_info.add_cap(12, oz::CapRole::CUSTOM, "sched", oz::CapType::STREAM);

    auto procman_proc = oz::loader::spawn_process(
        oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE,
        proc_file.data, proc_file.size, &procman_startup_info
    );
    if (!procman_proc.success) {
        oz::sys_debug_print("[Init] ERROR: Failed to spawn procman!\n\r");
        oz::sys_exit(1);
    }

    std::uint32_t procman_req_send = 0, procman_req_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &procman_req_send, &procman_req_recv);
    std::uint32_t procman_reply_send = 0, procman_reply_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &procman_reply_send, &procman_reply_recv);

    oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 2, procman_req_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 3, procman_reply_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 4, memman_req_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 5, memman_reply_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 6, vfs_req_send, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 7, procman_vfs_reply_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 8, procman_client_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 11, drivman_send, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 12, sched_send, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 13, shell_client_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_debug_print("[Init] ProcMan configured.\n\r");

    // =========================================================================
    // Core 5 Component 4 (Launch): Sched Process
    // =========================================================================
    oz::loader::TarFile sched_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "sched");
    if (!sched_file.data) {
        oz::sys_debug_print("[Init] ERROR: sched not found in initrd!\n\r");
        oz::sys_exit(1);
    }

    oz::ProcessStartupInfo sched_startup_info{};
    sched_startup_info.add_cap(10, oz::CapRole::CUSTOM, "tcb:memman");
    sched_startup_info.add_cap(11, oz::CapRole::CUSTOM, "tcb:vfs");
    sched_startup_info.add_cap(12, oz::CapRole::CUSTOM, "tcb:drivman");
    sched_startup_info.add_cap(13, oz::CapRole::CUSTOM, "tcb:procman");

    auto sched_proc = oz::loader::spawn_process(
        oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE,
        sched_file.data, sched_file.size, &sched_startup_info
    );
    if (!sched_proc.success) {
        oz::sys_debug_print("[Init] ERROR: Failed to spawn sched!\n\r");
        oz::sys_exit(1);
    }

    oz::sys_cspace_cap_insert(sched_proc.cspace_cap, 2, sched_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(sched_proc.cspace_cap, 3, sched_notif, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(sched_proc.cspace_cap, 10, mem_proc.tcb_cap, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(sched_proc.cspace_cap, 11, vfs_proc.tcb_cap, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(sched_proc.cspace_cap, 12, drivman_proc.tcb_cap, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(sched_proc.cspace_cap, 13, procman_proc.tcb_cap, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(sched_proc.cspace_cap, oz::CAP_UNTYPED, sched_untyped, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_debug_print("[Init] Sched configured with pre-wired Core 5 TCBs.\n\r");

    // =========================================================================
    // Handover & Early Retirement: Rebind-and-Switch to Sched
    // =========================================================================
    oz::sys_debug_print("[Init] Core 5 bootstrap complete. Delegating Root Baton to Sched via Rebind-and-Switch...\n\r");
    oz::sys_tcb_resume(sched_proc.tcb_cap);
    oz::sys_tcb_bind_baton(sched_proc.tcb_cap, oz::CAP_ROOT_BATON);

    // Init sleeps indefinitely as its baton was transferred
    for (;;) {
        oz::sys_yield();
    }
}
