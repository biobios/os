#include "syscall.hpp"
#include "loader.hpp"
#include "../common/utils/BootInfo.hpp"
#include "../common/ipc/Protocol.hpp"

#ifdef ENABLE_BOOT_TESTS
#include "tests.hpp"
#endif

// Cap slot assignments for init:
//   0: CAP_SELF_VSPACE
//   1: write to console (sys_write uses this)
//   2: send to AHCI (control)
//   3: send to VFS (file operation requests)
//   4: recv from VFS (file operation responses)
//   240: CAP_UNTYPED
//   241: CAP_AHCI_MMIO
//   242: CAP_XHCI_MMIO
//   243: CAP_BOOTINFO
//   244: CAP_INITRD
//   245: CAP_SELF_CSPACE
//   246: CAP_FRAMEBUFFER
//   247: CAP_ROOT_IOPORT

static constexpr std::uint32_t CAP_AHCI_SEND = 2;
static constexpr std::uint32_t CAP_VFS_SEND = 3;
static constexpr std::uint32_t CAP_VFS_RECV = 4;
static constexpr std::uint32_t CAP_VFS_SHARED_BUF = 5;
static constexpr std::uint32_t CAP_DISPLAY_SEND = 6;
static constexpr std::uint32_t CAP_WM_SEND      = 7;
static constexpr std::uint32_t CAP_PROCMAN_SEND = 8;
static constexpr std::uint32_t CAP_PROCMAN_RECV = 9;

static constexpr std::uint64_t BOOTINFO_VADDR = 0x600000000000ULL;
static constexpr std::uint64_t INITRD_VADDR   = 0x500000000000ULL;
static constexpr std::uint64_t VFS_SHARED_BUF_VIRT = 0x300000000000ULL;
static char* g_vfs_shared_buf = nullptr;

// Helper: send a VFS file open request
static int vfs_open(const char* path) {
    oz::IPCMessage req{};
    req.data[0] = oz::ipc::vfs_make_command(oz::ipc::VfsCmd::OPEN, 0);

    // Pack path into data[1..3] (up to 24 chars)
    char* dst = reinterpret_cast<char*>(&req.data[1]);
    int i = 0;
    for (; i < 23 && path[i] != '\0'; i++) {
        dst[i] = path[i];
    }
    dst[i] = '\0';

    oz::IPCMessage resp{};
    if (oz::sys_call(CAP_VFS_SEND, &req, &resp)) {
        if (resp.data[0] == 0) {
            return static_cast<int>(resp.data[1]); // file handle
        }
    }
    return -1;
}

// Helper: read from a VFS file using shared buffer (up to 64KB per chunk)
static std::size_t vfs_read(int handle, std::size_t offset, char* buf, std::size_t size) {
    if (!g_vfs_shared_buf) {
        oz::IPCMessage req{};
        req.data[0] = oz::ipc::vfs_make_command(oz::ipc::VfsCmd::READ, 0);
        req.data[1] = static_cast<std::uint64_t>(handle);
        req.data[2] = static_cast<std::uint64_t>(offset);
        req.data[3] = static_cast<std::uint64_t>(size);

        oz::IPCMessage resp{};
        if (!oz::sys_call(CAP_VFS_SEND, &req, &resp) || resp.data[0] != 0) return 0;

        std::size_t bytes_read = static_cast<std::size_t>(resp.data[1]);
        const char* src = reinterpret_cast<const char*>(&resp.data[2]);
        std::size_t to_copy = bytes_read > 16 ? 16 : bytes_read;
        if (to_copy > size) to_copy = size;
        for (std::size_t i = 0; i < to_copy; i++) {
            buf[i] = src[i];
        }
        return to_copy;
    }

    std::size_t total_read = 0;
    while (total_read < size) {
        std::size_t chunk = size - total_read;
        if (chunk > 64 * 1024) chunk = 64 * 1024;

        oz::IPCMessage req{};
        req.data[0] = oz::ipc::vfs_make_command(oz::ipc::VfsCmd::READ, 0);
        req.data[1] = static_cast<std::uint64_t>(handle);
        req.data[2] = static_cast<std::uint64_t>(offset + total_read);
        req.data[3] = static_cast<std::uint64_t>(chunk);

        oz::IPCMessage resp{};
        if (!oz::sys_call(CAP_VFS_SEND, &req, &resp) || resp.data[0] != 0) break;
        std::size_t bytes_read = static_cast<std::size_t>(resp.data[1]);
        if (bytes_read == 0) break;

        for (std::size_t i = 0; i < bytes_read; i++) {
            buf[total_read + i] = g_vfs_shared_buf[i];
        }
        total_read += bytes_read;
        if (bytes_read < chunk) break;
    }
    return total_read;
}

// Helper: write to a VFS file using shared buffer (up to 64KB per chunk)
static std::size_t vfs_write(int handle, std::size_t offset, const char* buf, std::size_t size) {
    if (!g_vfs_shared_buf || size == 0) return 0;

    std::size_t total_written = 0;
    while (total_written < size) {
        std::size_t chunk = size - total_written;
        if (chunk > 64 * 1024) chunk = 64 * 1024;

        for (std::size_t i = 0; i < chunk; i++) {
            g_vfs_shared_buf[i] = buf[total_written + i];
        }

        oz::IPCMessage req{};
        req.data[0] = oz::ipc::vfs_make_command(oz::ipc::VfsCmd::WRITE, 0);
        req.data[1] = static_cast<std::uint64_t>(handle);
        req.data[2] = static_cast<std::uint64_t>(offset + total_written);
        req.data[3] = static_cast<std::uint64_t>(chunk);

        oz::IPCMessage resp{};
        if (!oz::sys_call(CAP_VFS_SEND, &req, &resp) || resp.data[0] != 0) break;
        std::size_t bytes_written = static_cast<std::size_t>(resp.data[1]);
        if (bytes_written == 0) break;

        total_written += bytes_written;
        if (bytes_written < chunk) break;
    }
    return total_written;
}

// Helper: get file size
static std::size_t vfs_get_size(int handle) {
    oz::IPCMessage req{};
    req.data[0] = oz::ipc::vfs_make_command(oz::ipc::VfsCmd::GET_SIZE, 0);
    req.data[1] = static_cast<std::uint64_t>(handle);

    oz::IPCMessage resp{};
    if (oz::sys_call(CAP_VFS_SEND, &req, &resp)) {
        if (resp.data[0] == 0) return static_cast<std::size_t>(resp.data[1]);
    }
    return 0;
}

// Helper: close a VFS file
static void vfs_close(int handle) {
    oz::IPCMessage req{};
    req.data[0] = oz::ipc::vfs_make_command(oz::ipc::VfsCmd::CLOSE, 0);
    req.data[1] = static_cast<std::uint64_t>(handle);

    oz::IPCMessage resp{};
    oz::sys_call(CAP_VFS_SEND, &req, &resp);
}

extern "C" void _start() {
    oz::sys_debug_print("[Init] OZ OS Root Server started (Ring 3)!\n\r");

    // 1. Map BootInfo Frame (slot 243) into our VSpace
    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, oz::CAP_BOOTINFO, BOOTINFO_VADDR, 0x00, 4096) != 0) {
        oz::sys_debug_print("[Init] ERROR: Failed to map BootInfo!\n\r");
        oz::sys_exit(1);
    }
    const volatile auto* boot_info = reinterpret_cast<const volatile oz::BootInfo*>(BOOTINFO_VADDR);
    std::uint64_t initrd_size = boot_info->initrd_size;
    std::uint64_t ahci_abar_phys = boot_info->ahci_abar_phys;
    std::uint64_t xhci_bar_phys = boot_info->xhci_bar_phys;

    oz::sys_debug_print("[Init] BootInfo mapped! Untyped caps received: ");
    char num_buf[4] = {static_cast<char>('0' + (boot_info->untyped_cap_count / 10)),
                       static_cast<char>('0' + (boot_info->untyped_cap_count % 10)),
                       '\n', '\r'};
    oz::sys_debug_print(num_buf);

    // 2. Map InitRD Frame (slot 244) into our VSpace
    if (initrd_size > 0) {
        if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, oz::CAP_INITRD, INITRD_VADDR, 0x00, initrd_size) != 0) {
            oz::sys_debug_print("[Init] ERROR: Failed to map InitRD!\n\r");
            oz::sys_exit(1);
        }
    }
    oz::sys_debug_print("[Init] InitRD mapped successfully.\n\r");
    const std::uint8_t* initrd_ptr = reinterpret_cast<const std::uint8_t*>(INITRD_VADDR + boot_info->initrd_offset);
    
    std::uint32_t cons_tcb_cap = oz::INVALID_CAP_ID;
    std::uint32_t disp_tcb_cap = oz::INVALID_CAP_ID;
    std::uint32_t ahci_tcb_cap = oz::INVALID_CAP_ID;
    std::uint32_t vfs_tcb_cap = oz::INVALID_CAP_ID;
    std::uint32_t mem_tcb_cap = oz::INVALID_CAP_ID;
    std::uint32_t wm_tcb_cap = oz::INVALID_CAP_ID;
    std::uint32_t procman_tcb_cap = oz::INVALID_CAP_ID;
    std::uint32_t xhci_tcb_cap = oz::INVALID_CAP_ID;
    std::uint32_t shell_tcb_cap = oz::INVALID_CAP_ID;
    
    std::uint32_t init_baton = oz::sys_untyped_retype(oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::ObjectType::Baton), 0);
    constexpr std::uint32_t INIT_BATON_SLOT = 30;
    oz::sys_cap_move(INIT_BATON_SLOT, init_baton);
    init_baton = INIT_BATON_SLOT;

    auto init_server_coroutine = [&](std::uint32_t tcb_cap, const char* name) {
        if (tcb_cap == oz::INVALID_CAP_ID) return;
        oz::sys_tcb_bind_baton(tcb_cap, init_baton);
        oz::sys_baton_inject(init_baton, 1000000 /* 1000ms = 1s */);
        oz::SchedResult res = oz::sys_baton_switch(init_baton);
        oz::sys_debug_print("[Init] Server ");
        oz::sys_debug_print(name);
        oz::sys_debug_print(" init coroutine returned reason=");
        char r_str[4] = {static_cast<char>('0' + static_cast<int>(res.reason)), '\n', '\r', '\0'};
        oz::sys_debug_print(r_str);
    };
    
    // 3. Spawn Console Server
    oz::loader::TarFile cons_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "console");
    if (!cons_file.data) {
    oz::sys_debug_print("[Init] ERROR: console not found in initrd!\n\r");
    oz::sys_exit(1);
    }
    oz::sys_debug_print("[Init] Spawning console...\n\r");
    auto cons_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, cons_file.data, cons_file.size);
    if (!cons_proc.success) {
    oz::sys_debug_print("[Init] ERROR: Failed to spawn console process!\n\r");
    oz::sys_exit(1);
    }
    constexpr std::uint32_t INIT_CONS_TCB_SLOT = 100;
    oz::sys_cap_copy(INIT_CONS_TCB_SLOT, cons_proc.tcb_cap, static_cast<std::uint32_t>(oz::CapRights::ALL));
    cons_tcb_cap = INIT_CONS_TCB_SLOT;
    oz::sys_debug_print("[Init] Console process spawned, configuring channels...\n\r");
    
    std::uint32_t cons_send = 0;
    std::uint32_t cons_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &cons_send, &cons_recv);
    
    int cs_res = oz::sys_cspace_cap_insert(cons_proc.cspace_cap, 1, cons_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(cons_proc.cspace_cap, oz::CAP_UNTYPED, oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_debug_print("[Init] Resuming console...\n\r");
    int resume_res = oz::sys_tcb_resume(cons_proc.tcb_cap);
    oz::sys_debug_print("[Init] Console resumed.\n\r");
    init_server_coroutine(cons_tcb_cap, "Console");
    
    int self_res = oz::sys_cspace_cap_insert(oz::CAP_SELF_CSPACE, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::ALL));
    (void)cs_res; (void)resume_res; (void)self_res;
    
    oz::sys_debug_print("[Init] Writing hello to console via sys_write...\n\r");
    oz::sys_write("Hello from init process (Ring 3)!\n\r");
    oz::sys_write("[Init] Console server spawned successfully.\n\r");
    
    // 3.5 Spawn Display Server (GUI mode)
    std::uint32_t init_disp_send = 0;
    std::uint32_t disp_cmd_send = 0;
    oz::loader::TarFile disp_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "display");
    if (disp_file.data) {
    auto disp_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, disp_file.data, disp_file.size);
    if (disp_proc.success) {
    disp_tcb_cap = disp_proc.tcb_cap;
    std::uint32_t disp_init_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &init_disp_send, &disp_init_recv);
    
    std::uint32_t disp_cmd_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &disp_cmd_send, &disp_cmd_recv);
    
    // Slot 1: console send (sys_write)
    oz::sys_cspace_cap_insert(disp_proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    // Slot 2: display recv (Console text stream)
    oz::sys_cspace_cap_insert(disp_proc.cspace_cap, 2, disp_init_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    // Slot 3: command recv (DisplayCmd IPC from WM / Admin)
    oz::sys_cspace_cap_insert(disp_proc.cspace_cap, 3, disp_cmd_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    // Slot 240: Untyped
    oz::sys_cspace_cap_insert(disp_proc.cspace_cap, oz::CAP_UNTYPED, oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::CapRights::ALL));
    // Slot 246: FrameBuffer
    oz::sys_cspace_cap_insert(disp_proc.cspace_cap, oz::CAP_FRAMEBUFFER, oz::CAP_FRAMEBUFFER, static_cast<std::uint32_t>(oz::CapRights::ALL));
    
    oz::sys_tcb_resume(disp_proc.tcb_cap);
    init_server_coroutine(disp_tcb_cap, "Display");
    oz::sys_write("[Init] Display server spawned successfully.\n\r");
    
    // Send framebuffer info to Display Server via Console Channel (initial bootstrap)
    oz::IPCMessage config_msg{};
    config_msg.data[0] = boot_info->fb_width;
    config_msg.data[1] = boot_info->fb_height;
    config_msg.data[2] = boot_info->fb_size;
    config_msg.data[3] = boot_info->fb_phys_addr;
    oz::IPCMessage config_resp{};
    oz::sys_call(init_disp_send, &config_msg, &config_resp);
    oz::sys_write("[Init] Sent framebuffer config to Display server.\n\r");
    
    // Forward Display Server input channel to Console Server (Slot 2)
    oz::sys_cspace_cap_insert(cons_proc.cspace_cap, 2, init_disp_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_write("[Init] Connected Console server to Display server.\n\r");
    
    // Keep dedicated Display Command channel in Init for Surface/Admin operations
    oz::sys_cspace_cap_insert(oz::CAP_SELF_CSPACE, CAP_DISPLAY_SEND, disp_cmd_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    
    } else {
    oz::sys_write("[Init] ERROR: Failed to spawn display server!\n\r");
    }
    } else {
    oz::sys_write("[Init] WARNING: display not found in initrd!\n\r");
    }
    
    // 4. Spawn AHCI Driver
    oz::loader::TarFile ahci_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "ahci");
    if (!ahci_file.data) {
    oz::sys_write("[Init] ERROR: ahci not found in initrd!\n\r");
    oz::sys_exit(1);
    }
    auto ahci_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, ahci_file.data, ahci_file.size);
    if (!ahci_proc.success) {
    oz::sys_write("[Init] ERROR: Failed to spawn ahci process!\n\r");
    oz::sys_exit(1);
    }
    ahci_tcb_cap = ahci_proc.tcb_cap;
    
    std::uint32_t init_ahci_send = 0;
    std::uint32_t ahci_init_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &init_ahci_send, &ahci_init_recv);
    oz::sys_cspace_cap_insert(oz::CAP_SELF_CSPACE, CAP_AHCI_SEND, init_ahci_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    
    std::uint32_t vfs_ahci_send = 0;
    std::uint32_t ahci_vfs_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &vfs_ahci_send, &ahci_vfs_recv);
    
    std::uint32_t ahci_vfs_send = 0;
    std::uint32_t vfs_ahci_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &ahci_vfs_send, &vfs_ahci_recv);
    
    // Shared buffer (16 pages = 64KB) for block I/O
    std::uint32_t shared_buf_frame = oz::sys_frame_alloc(oz::CAP_UNTYPED, 16);
    
    // Shared Ring Buffer (1 page = 4KB) for Async Block I/O
    std::uint32_t ring_buf_frame = oz::sys_frame_alloc(oz::CAP_UNTYPED, 1);
    
    // Notifications for AHCI and VFS async coordination
    std::uint32_t notif_ahci = oz::sys_notification_create(oz::CAP_UNTYPED);
    std::uint32_t notif_vfs = oz::sys_notification_create(oz::CAP_UNTYPED);
    
    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, 2, ahci_init_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, 3, ahci_vfs_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, 4, ahci_vfs_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, 5, notif_ahci, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, 6, notif_vfs, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, oz::CAP_UNTYPED, oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::CapRights::ALL));
    if (ahci_abar_phys != 0) {
    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, 241, oz::CAP_AHCI_MMIO, static_cast<std::uint32_t>(oz::CapRights::ALL));
    }
    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, 242, shared_buf_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));
    // Slot 244 (ring_buf_frame) omitted: synchronous Baton transfer model
    
    oz::sys_tcb_resume(ahci_proc.tcb_cap);
    init_server_coroutine(ahci_tcb_cap, "AHCI");
    oz::sys_write("[Init] AHCI server spawned successfully.\n\r");
    
    // Send AHCI MMIO address to AHCI driver immediately
    oz::IPCMessage ahci_mmio_msg{};
    ahci_mmio_msg.data[0] = ahci_abar_phys;
    oz::IPCMessage ahci_resp{};
    if (oz::sys_call(CAP_AHCI_SEND, &ahci_mmio_msg, &ahci_resp)) {
    oz::sys_write("[Init] Sent AHCI MMIO address to ahci server.\n\r");
    } else {
    oz::sys_write("[Init] Failed to send AHCI MMIO address!\n\r");
    }
    
    // 5. Spawn VFS Server
    oz::loader::TarFile vfs_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "vfs");
    if (!vfs_file.data) {
    oz::sys_write("[Init] ERROR: vfs not found in initrd!\n\r");
    oz::sys_exit(1);
    }
    auto vfs_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, vfs_file.data, vfs_file.size);
    if (!vfs_proc.success) {
    oz::sys_write("[Init] ERROR: Failed to spawn vfs process!\n\r");
    oz::sys_exit(1);
    }
    vfs_tcb_cap = vfs_proc.tcb_cap;
    
    // Shared VFS request channel (all clients send requests to vfs_req_send)
    std::uint32_t vfs_req_send = 0;
    std::uint32_t vfs_req_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &vfs_req_send, &vfs_req_recv);
    
    // Dedicated reply channel for Client 0 (init)
    std::uint32_t vfs_init_reply_send = 0;
    std::uint32_t init_reply_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &vfs_init_reply_send, &init_reply_recv);
    
    // Dedicated reply channel for Client 1 (shell)
    std::uint32_t vfs_shell_reply_send = 0;
    std::uint32_t shell_reply_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &vfs_shell_reply_send, &shell_reply_recv);
    
    // Wire up Init's CSpace for VFS
    oz::sys_cspace_cap_insert(oz::CAP_SELF_CSPACE, CAP_VFS_SEND, vfs_req_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(oz::CAP_SELF_CSPACE, CAP_VFS_RECV, init_reply_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    
    // Wire up VFS server CSpace
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 2, vfs_ahci_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 3, vfs_ahci_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 4, vfs_req_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 5, vfs_init_reply_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 6, vfs_shell_reply_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 8, notif_ahci, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 9, notif_vfs, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, oz::CAP_UNTYPED, oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 242, shared_buf_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));
    // Slot 250 (ring_buf_frame) omitted: synchronous Baton transfer model
    
    // Dedicated shared buffer for Client 0 (init) (16 pages = 64KB)
    std::uint32_t init_client_frame = oz::sys_frame_alloc(oz::CAP_UNTYPED, 16);
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 243, init_client_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(oz::CAP_SELF_CSPACE, CAP_VFS_SHARED_BUF, init_client_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));
    
    // Dedicated shared buffer for Client 1 (shell) (16 pages = 64KB)
    std::uint32_t shell_client_frame = oz::sys_frame_alloc(oz::CAP_UNTYPED, 16);
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 244, shell_client_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));
    
    // Dedicated shared buffer for Client 2 (procman) (16 pages = 64KB)
    std::uint32_t procman_client_frame = oz::sys_frame_alloc(oz::CAP_UNTYPED, 16);
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 245, procman_client_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));
    
    // Dedicated reply channel for Client 2 (procman)
    std::uint32_t vfs_procman_reply_send = 0;
    std::uint32_t procman_vfs_reply_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &vfs_procman_reply_send, &procman_vfs_reply_recv);
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 7, vfs_procman_reply_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    
    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, CAP_VFS_SHARED_BUF, VFS_SHARED_BUF_VIRT, 0x01, 16 * 4096) == 0) {
    g_vfs_shared_buf = reinterpret_cast<char*>(VFS_SHARED_BUF_VIRT);
    oz::sys_write("[Init] Mapped VFS client shared buffer (64KB).\n\r");
    }
    
    oz::sys_tcb_resume(vfs_proc.tcb_cap);
    init_server_coroutine(vfs_tcb_cap, "VFS");
    oz::sys_write("[Init] VFS server spawned successfully.\n\r");
    
    // 5.5 Spawn Memory Manager (MemMan)
    std::uint32_t memman_req_send = 0;
    std::uint32_t memman_req_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &memman_req_send, &memman_req_recv);
    
    std::uint32_t memman_reply_send = 0;
    std::uint32_t memman_reply_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &memman_reply_send, &memman_reply_recv);
    
    oz::loader::TarFile mem_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "memman");
    if (mem_file.data) {
    auto mem_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, mem_file.data, mem_file.size);
    if (mem_proc.success) {
    mem_tcb_cap = mem_proc.tcb_cap;
    oz::sys_cspace_cap_insert(mem_proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(mem_proc.cspace_cap, 2, memman_req_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(mem_proc.cspace_cap, 3, memman_reply_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(mem_proc.cspace_cap, oz::CAP_UNTYPED, oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::CapRights::ALL));
    
    oz::sys_tcb_resume(mem_proc.tcb_cap);
    init_server_coroutine(mem_tcb_cap, "MemMan");
    oz::sys_write("[Init] Memory Manager (MemMan) spawned successfully.\n\r");
    } else {
    oz::sys_write("[Init] ERROR: Failed to spawn memman!\n\r");
    }
    } else {
    oz::sys_write("[Init] ERROR: memman not found in initrd!\n\r");
    }
    
    // 5.6 Spawn Process Manager (ProcMan)
    std::uint32_t procman_req_send = 0;
    std::uint32_t procman_req_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &procman_req_send, &procman_req_recv);
    
    std::uint32_t procman_reply_send = 0;
    std::uint32_t procman_reply_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &procman_reply_send, &procman_reply_recv);
    
    oz::loader::TarFile proc_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "procman");
    oz::loader::SpawnedProcess procman_proc{};
    if (proc_file.data) {
    procman_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, proc_file.data, proc_file.size);
    if (procman_proc.success) {
    procman_tcb_cap = procman_proc.tcb_cap;
    // Slot 1: Console Send
    oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT | oz::CapRights::DUPLICATE));
    // Slot 2: ProcMan Req Recv
    oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 2, procman_req_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    // Slot 3: ProcMan Reply Send
    oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 3, procman_reply_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    // Slot 4: MemMan Send
    oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 4, memman_req_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    // Slot 5: MemMan Recv
    oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 5, memman_reply_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    // Slot 6: VFS Send
    oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 6, vfs_req_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT | oz::CapRights::DUPLICATE));
    // Slot 7: VFS Recv
    oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 7, procman_vfs_reply_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    // Slot 8: VFS Shared Buffer
    oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 8, procman_client_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));
    
    // Register ProcMan channels in Init CSpace
    oz::sys_cspace_cap_insert(oz::CAP_SELF_CSPACE, CAP_PROCMAN_SEND, procman_req_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(oz::CAP_SELF_CSPACE, CAP_PROCMAN_RECV, procman_reply_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    } else {
    oz::sys_write("[Init] ERROR: Failed to spawn procman!\n\r");
    }
    } else {
    oz::sys_write("[Init] ERROR: procman not found in initrd!\n\r");
    }
    
    std::uint32_t xhci_key_send = cons_send;
    
    // 6.5 Spawn Window Manager (WM)
    std::uint32_t wm_req_send = 0;
    std::uint32_t wm_req_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &wm_req_send, &wm_req_recv);
    
    std::uint32_t wm_disp_reply_send = 0;
    std::uint32_t wm_disp_reply_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &wm_disp_reply_send, &wm_disp_reply_recv);
    
    // Shared Ring Buffer Frame for Keyboard Input (xHCI -> WM)
    std::uint32_t kbd_ring_frame = oz::sys_frame_alloc(oz::CAP_UNTYPED, 1);
    std::uint32_t notif_wm_kbd = oz::sys_notification_create(oz::CAP_UNTYPED);
    
    oz::loader::TarFile wm_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "wm");
    if (wm_file.data) {
    auto wm_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, wm_file.data, wm_file.size);
    if (wm_proc.success) {
    wm_tcb_cap = wm_proc.tcb_cap;
    // Slot 0: Self VSpace
    oz::sys_cspace_cap_insert(wm_proc.cspace_cap, 0, wm_proc.vspace_cap, static_cast<std::uint32_t>(oz::CapRights::ALL));
    // Slot 1: Console Send
    oz::sys_cspace_cap_insert(wm_proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    // Slot 2: Display Send (Command Channel)
    oz::sys_cspace_cap_insert(wm_proc.cspace_cap, 2, disp_cmd_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    // Slot 3: Display Reply Recv
    oz::sys_cspace_cap_insert(wm_proc.cspace_cap, 3, wm_disp_reply_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    // Slot 4: WM Request / Input Recv
    oz::sys_cspace_cap_insert(wm_proc.cspace_cap, 4, wm_req_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    // Slot 5: Shared Keyboard Ring Buffer Frame
    oz::sys_cspace_cap_insert(wm_proc.cspace_cap, 5, kbd_ring_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));
    // Slot 6: WM Notification (receives from xHCI and bound channels)
    oz::sys_cspace_cap_insert(wm_proc.cspace_cap, 6, notif_wm_kbd, static_cast<std::uint32_t>(oz::CapRights::ALL));
    // Slot 240: Untyped
    int ut_res = oz::sys_cspace_cap_insert(wm_proc.cspace_cap, oz::CAP_UNTYPED, oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::CapRights::ALL));
    if (ut_res != 0) {
    oz::sys_write("[Init] ERROR: Failed to insert CAP_UNTYPED into WM CSpace!\n\r");
    } else {
    oz::sys_write("[Init] Successfully inserted CAP_UNTYPED into WM CSpace.\n\r");
    }
    
    oz::sys_tcb_resume(wm_proc.tcb_cap);
    init_server_coroutine(wm_tcb_cap, "WM");
    oz::sys_write("[Init] Window Manager (WM) spawned successfully.\n\r");
    
    // Direct xHCI keyboard input to WM instead of shell/console directly
    xhci_key_send = wm_req_send;
    
    // Also keep WM channel in Init
    oz::sys_cspace_cap_insert(oz::CAP_SELF_CSPACE, CAP_WM_SEND, wm_req_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    
    // Grant WM and Display to ProcMan for delegation to children
    if (procman_proc.success) {
    oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 9, wm_req_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT | oz::CapRights::DUPLICATE));
    oz::sys_cspace_cap_insert(procman_proc.cspace_cap, 10, disp_cmd_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT | oz::CapRights::DUPLICATE));
    // Resume ProcMan after all slots (1..10) are fully populated
    oz::sys_tcb_resume(procman_proc.tcb_cap);
    init_server_coroutine(procman_tcb_cap, "ProcMan");
    oz::sys_write("[Init] Process Manager (ProcMan) spawned and resumed successfully.\n\r");
    }
    } else {
    oz::sys_write("[Init] ERROR: Failed to spawn WM!\n\r");
    }
    } else {
    oz::sys_write("[Init] WARNING: wm not found in initrd!\n\r");
    }
    
    // 7. Spawn xHCI Server (as InterruptThread)
    oz::loader::TarFile xhci_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "xhci");
    if (xhci_file.data) {
    auto xhci_proc = oz::loader::spawn_interrupt_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, xhci_file.data, xhci_file.size);
    if (xhci_proc.success) {
    xhci_tcb_cap = xhci_proc.tcb_cap;
    std::uint32_t irq_send = 0;
    std::uint32_t irq_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &irq_send, &irq_recv);
    
    std::uint32_t irq_handler = oz::sys_irq_handler_create(oz::CAP_UNTYPED, 0x40, irq_send);
    
    oz::sys_cspace_cap_insert(xhci_proc.cspace_cap, 1, xhci_key_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(xhci_proc.cspace_cap, 2, irq_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_move(xhci_proc.cspace_cap, 3, irq_handler);
    oz::sys_cspace_cap_insert(xhci_proc.cspace_cap, 4, kbd_ring_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(xhci_proc.cspace_cap, 5, notif_wm_kbd, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(xhci_proc.cspace_cap, oz::CAP_UNTYPED, oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::CapRights::ALL));
    if (xhci_bar_phys != 0) {
    oz::sys_cspace_cap_insert(xhci_proc.cspace_cap, 241, oz::CAP_XHCI_MMIO, static_cast<std::uint32_t>(oz::CapRights::ALL));
    }
    
    oz::sys_tcb_resume(xhci_proc.tcb_cap);
    init_server_coroutine(xhci_tcb_cap, "xHCI");
    oz::sys_write("[Init] xHCI driver spawned successfully.\n\r");
    }
    }
    
    // 8. Attempt file operations via VFS
    oz::sys_write("Attempting to open test.txt via VFS...\n\r");
    int handle = vfs_open("test.txt");
    if (handle >= 0) {
    oz::sys_write("File opened! handle=");
    char num[4] = {static_cast<char>('0' + handle), '\n', '\r', '\0'};
    oz::sys_write(num);
    
    std::size_t size = vfs_get_size(handle);
    oz::sys_write("File size: ");
    char size_str[16];
    int pos = 0;
    if (size == 0) {
    size_str[pos++] = '0';
    } else {
    char tmp[16];
    int tmp_pos = 0;
    std::size_t s = size;
    while (s > 0) {
    tmp[tmp_pos++] = '0' + static_cast<char>(s % 10);
    s /= 10;
    }
    for (int i = tmp_pos - 1; i >= 0; i--) {
    size_str[pos++] = tmp[i];
    }
    }
    size_str[pos++] = '\n';
    size_str[pos++] = '\r';
    size_str[pos] = '\0';
    oz::sys_write(size_str);
    
    char buf[17] = {};
    std::size_t bytes = vfs_read(handle, 0, buf, 16);
    buf[bytes] = '\0';
    oz::sys_write("Content: ");
    oz::sys_write(buf);
    oz::sys_write("\n\r");
    
    vfs_close(handle);
    oz::sys_write("File closed.\n\r");
    } else {
    oz::sys_write("test.txt not found (this is OK if disk has no test.txt).\n\r");
    }
    
    #ifdef ENABLE_BOOT_TESTS
    oz::init_tests::run_vfs_tests(CAP_VFS_SEND, CAP_VFS_RECV);
    oz::init_tests::run_display_tests(CAP_DISPLAY_SEND);
    oz::init_tests::run_wm_tests(CAP_WM_SEND);
    oz::init_tests::run_manifest_tests(initrd_ptr, initrd_size, cons_send);
    oz::init_tests::run_cat_tests(initrd_ptr, initrd_size, cons_send, CAP_VFS_SEND, CAP_VFS_RECV);
    #endif
    
    // ========================================================================
    // Step 5: Launch Interactive Shell via Process Manager (ProcMan)
    // ========================================================================
    oz::sys_debug_print("[Init] Launching interactive shell via ProcMan...\n\r");
    oz::IPCMessage s_init{};
    s_init.data[0] = static_cast<std::uint64_t>(oz::ipc::ProcCmd::SPAWN_INIT);
    char sh_name[24] = "shell";
    for (int k = 0; k < 3; ++k) {
    s_init.data[k + 1] = *reinterpret_cast<std::uint64_t*>(&sh_name[k * 8]);
    }
    s_init.has_cap = false;
    
    oz::IPCMessage s_resp{};
    if (oz::sys_call(CAP_PROCMAN_SEND, &s_init, &s_resp) && s_resp.data[0] == 0) {
    std::uint32_t sess_id = static_cast<std::uint32_t>(s_resp.data[1]);
    oz::IPCMessage s_commit{};
    s_commit.data[0] = static_cast<std::uint64_t>(oz::ipc::ProcCmd::SPAWN_COMMIT);
    s_commit.data[1] = sess_id;
    oz::IPCMessage s_cresp{};
    if (oz::sys_call(CAP_PROCMAN_SEND, &s_commit, &s_cresp) && s_cresp.data[0] == 0) {
    std::uint32_t shell_pid = static_cast<std::uint32_t>(s_cresp.data[1]);
    if (s_cresp.has_cap) {
    shell_tcb_cap = s_cresp.transferred_cap;
    }
    oz::sys_debug_print("[Init] Shell process spawned via ProcMan! PID=");
    char pid_str[4] = {static_cast<char>('0' + (shell_pid % 10)), '\n', '\r', '\0'};
    oz::sys_debug_print(pid_str);
    } else {
    oz::sys_debug_print("[Init] ERROR: ProcMan SPAWN_COMMIT failed for shell, code=");
    char c_buf[4] = {static_cast<char>('0' + s_cresp.data[0]), '\n', '\r', '\0'};
    oz::sys_debug_print(c_buf);
    }
    } else {
    oz::sys_debug_print("[Init] ERROR: ProcMan SPAWN_INIT failed for shell, code=");
    char c_buf[4] = {static_cast<char>('0' + s_resp.data[0]), '\n', '\r', '\0'};
    oz::sys_debug_print(c_buf);
    }
    
    // ========================================================================
    // Step 6: Spawn User-space Scheduler Daemon (sched) & Transfer Root Role
    // ========================================================================
    oz::loader::TarFile sched_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "sched");
    if (sched_file.data) {
    auto sched_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, sched_file.data, sched_file.size);
    if (sched_proc.success) {
    std::uint32_t sched_send = 0;
    std::uint32_t sched_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &sched_send, &sched_recv);
    
    std::uint32_t sched_notif = oz::sys_notification_create(oz::CAP_UNTYPED);
    
    // Bind IPC channel to sched notification so incoming registration IPC wakes sched up
    static constexpr std::uint64_t BADGE_SCHED_IPC = 1ULL << 63;
    oz::sys_channel_bind_notification(sched_recv, sched_notif, BADGE_SCHED_IPC);
    
    // Slot 1: console send (sys_write)
    oz::sys_cspace_cap_insert(sched_proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    // Slot 2: sched IPC recv
    oz::sys_cspace_cap_insert(sched_proc.cspace_cap, 2, sched_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    // Slot 3: sched self notification
    oz::sys_cspace_cap_insert(sched_proc.cspace_cap, 3, sched_notif, static_cast<std::uint32_t>(oz::CapRights::ALL));
    // Slot 240: Untyped
    oz::sys_cspace_cap_insert(sched_proc.cspace_cap, oz::CAP_UNTYPED, oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::CapRights::ALL));
    
    oz::sys_tcb_resume(sched_proc.tcb_cap);
    init_server_coroutine(sched_proc.tcb_cap, "Sched");
    oz::sys_write("[Init] Sched process spawned and resumed.\n\r");
    
    // Register threads with sched daemon
    auto register_thread = [&](std::uint32_t tcb_cap, const char* name, std::uint32_t priority = 1) {
    if (tcb_cap == oz::INVALID_CAP_ID) return;
    constexpr std::uint32_t COPY_SLOT = 500;
    if (oz::sys_cap_copy(COPY_SLOT, tcb_cap, static_cast<std::uint32_t>(oz::CapRights::ALL)) != 0) {
    oz::sys_write("[Init] Failed to copy TCB cap for ");
    oz::sys_write(name);
    oz::sys_write("\n\r");
    return;
    }
    oz::IPCMessage reg_msg{};
    reg_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::SchedCmd::REGISTER_THREAD);
    reg_msg.data[1] = priority;
    reg_msg.transferred_cap = COPY_SLOT;
    reg_msg.has_cap = true;
    oz::IPCMessage reg_resp{};
    if (oz::sys_call(sched_send, &reg_msg, &reg_resp) && reg_resp.data[0] == 0) {
    oz::sys_write("[Init] Registered thread with sched: ");
    oz::sys_write(name);
    oz::sys_write("\n\r");
    } else {
    oz::sys_write("[Init] Failed to register thread with sched: ");
    oz::sys_write(name);
    oz::sys_write("\n\r");
    }
    };
    
    auto register_irq_thread = [&](std::uint32_t tcb_cap, const char* name, std::uint64_t budget_us = 10000) {
    if (tcb_cap == oz::INVALID_CAP_ID) return;
    constexpr std::uint32_t COPY_SLOT = 500;
    if (oz::sys_cap_copy(COPY_SLOT, tcb_cap, static_cast<std::uint32_t>(oz::CapRights::ALL)) != 0) {
    oz::sys_write("[Init] Failed to copy TCB cap for IRQ handler ");
    oz::sys_write(name);
    oz::sys_write("\n\r");
    return;
    }
    oz::IPCMessage reg_msg{};
    reg_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::SchedCmd::REGISTER_IRQ_HANDLER);
    reg_msg.data[1] = 0;
    reg_msg.data[2] = budget_us;
    reg_msg.transferred_cap = COPY_SLOT;
    reg_msg.has_cap = true;
    oz::IPCMessage reg_resp{};
    if (oz::sys_call(sched_send, &reg_msg, &reg_resp) && reg_resp.data[0] == 0) {
    oz::sys_write("[Init] Registered dedicated IRQ handler with sched: ");
    oz::sys_write(name);
    oz::sys_write("\n\r");
    } else {
    oz::sys_write("[Init] Failed to register dedicated IRQ handler with sched: ");
    oz::sys_write(name);
    oz::sys_write("\n\r");
    }
    };
    
    register_thread(cons_tcb_cap, "Console", 1);
    register_thread(disp_tcb_cap, "Display", 1);
    register_thread(ahci_tcb_cap, "AHCI", 1);
    register_thread(vfs_tcb_cap, "VFS", 1);
    register_thread(mem_tcb_cap, "MemMan", 1);
    register_thread(wm_tcb_cap, "WM", 1);
    register_thread(procman_tcb_cap, "ProcMan", 1);
    register_irq_thread(xhci_tcb_cap, "xHCI", 10000);
    register_thread(shell_tcb_cap, "Shell", 1);
    
    // Delegate Root Baton to sched daemon via Rebind-and-Switch (Section 1.4 A)
    oz::sys_write("[Init] Delegating Root Baton to sched daemon via sys_tcb_bind_baton (Rebind-and-Switch)...\n\r");
    oz::sys_tcb_bind_baton(sched_proc.tcb_cap, oz::CAP_ROOT_BATON);
    } else {
    oz::sys_write("[Init] ERROR: Failed to spawn sched process!\n\r");
    }
    } else {
    oz::sys_write("[Init] WARNING: sched not found in initrd!\n\r");
    }
    
    oz::sys_write("Init process completed. Exiting Init thread...\n\r");
    oz::sys_exit(0);
    }
    
