#include "syscall.hpp"
#include "loader.hpp"
#include "../common/utils/BootInfo.hpp"
#include "../common/ipc/Protocol.hpp"

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

static constexpr std::uint32_t CAP_AHCI_SEND = 2;
static constexpr std::uint32_t CAP_VFS_SEND = 3;
static constexpr std::uint32_t CAP_VFS_RECV = 4;
static constexpr std::uint32_t CAP_VFS_SHARED_BUF = 5;

static constexpr std::uint64_t BOOTINFO_VADDR = 0x600000000000ULL;
static constexpr std::uint64_t INITRD_VADDR   = 0x500000000000ULL;
static constexpr std::uint64_t VFS_SHARED_BUF_VIRT = 0x300000000000ULL;
static char* g_vfs_shared_buf = nullptr;

// Helper: send a VFS file open request
static int vfs_open(const char* path) {
    oz::IPCMessage req{};
    req.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsCmd::OPEN);
    
    // Pack path into data[1..3] (up to 24 chars)
    char* dst = reinterpret_cast<char*>(&req.data[1]);
    int i = 0;
    for (; i < 23 && path[i] != '\0'; i++) {
        dst[i] = path[i];
    }
    dst[i] = '\0';
    
    oz::sys_channel_send(CAP_VFS_SEND, &req);
    
    oz::IPCMessage resp{};
    oz::sys_channel_recv(CAP_VFS_RECV, &resp);
    
    if (resp.data[0] == 0) {
        return static_cast<int>(resp.data[1]); // file handle
    }
    return -1;
}

// Helper: read from a VFS file using shared buffer (up to 64KB per chunk)
static std::size_t vfs_read(int handle, std::size_t offset, char* buf, std::size_t size) {
    if (!g_vfs_shared_buf) {
        oz::IPCMessage req{};
        req.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsCmd::READ);
        req.data[1] = static_cast<std::uint64_t>(handle);
        req.data[2] = static_cast<std::uint64_t>(offset);
        req.data[3] = static_cast<std::uint64_t>(size);
        
        oz::sys_channel_send(CAP_VFS_SEND, &req);
        
        oz::IPCMessage resp{};
        oz::sys_channel_recv(CAP_VFS_RECV, &resp);
        
        if (resp.data[0] != 0) return 0;
        
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
        req.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsCmd::READ);
        req.data[1] = static_cast<std::uint64_t>(handle);
        req.data[2] = static_cast<std::uint64_t>(offset + total_read);
        req.data[3] = static_cast<std::uint64_t>(chunk);

        oz::IPCMessage resp{};
        oz::sys_channel_send(CAP_VFS_SEND, &req);
        oz::sys_channel_recv(CAP_VFS_RECV, &resp);

        if (resp.data[0] != 0) break;
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

// Helper: get file size
static std::size_t vfs_get_size(int handle) {
    oz::IPCMessage req{};
    req.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsCmd::GET_SIZE);
    req.data[1] = static_cast<std::uint64_t>(handle);
    
    oz::sys_channel_send(CAP_VFS_SEND, &req);
    
    oz::IPCMessage resp{};
    oz::sys_channel_recv(CAP_VFS_RECV, &resp);
    
    if (resp.data[0] == 0) return static_cast<std::size_t>(resp.data[1]);
    return 0;
}

// Helper: close a VFS file
static void vfs_close(int handle) {
    oz::IPCMessage req{};
    req.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsCmd::CLOSE);
    req.data[1] = static_cast<std::uint64_t>(handle);
    
    oz::sys_channel_send(CAP_VFS_SEND, &req);
    
    oz::IPCMessage resp{};
    oz::sys_channel_recv(CAP_VFS_RECV, &resp);
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
    const std::uint8_t* initrd_ptr = reinterpret_cast<const std::uint8_t*>(INITRD_VADDR + boot_info->initrd_offset);

    // 3. Spawn Console Server
    oz::loader::TarFile cons_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "console");
    if (!cons_file.data) {
        oz::sys_debug_print("[Init] ERROR: console not found in initrd!\n\r");
        oz::sys_exit(1);
    }
    auto cons_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, cons_file.data, cons_file.size);
    if (!cons_proc.success) {
        oz::sys_debug_print("[Init] ERROR: Failed to spawn console process!\n\r");
        oz::sys_exit(1);
    }

    std::uint32_t cons_send = 0;
    std::uint32_t cons_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &cons_send, &cons_recv);

    int cs_res = oz::sys_cspace_cap_insert(cons_proc.cspace_cap, 1, cons_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(cons_proc.cspace_cap, oz::CAP_UNTYPED, oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::CapRights::ALL));
    int resume_res = oz::sys_tcb_resume(cons_proc.tcb_cap);

    int self_res = oz::sys_cspace_cap_insert(oz::CAP_SELF_CSPACE, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    (void)cs_res; (void)resume_res; (void)self_res;

    oz::sys_write("Hello from init process (Ring 3)!\n\r");
    oz::sys_write("[Init] Console server spawned successfully.\n\r");

    // 3.5 Spawn Display Server (GUI mode)
    oz::loader::TarFile disp_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "display");
    if (disp_file.data) {
        auto disp_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, disp_file.data, disp_file.size);
        if (disp_proc.success) {
            std::uint32_t init_disp_send = 0;
            std::uint32_t disp_init_recv = 0;
            oz::sys_channel_create(oz::CAP_UNTYPED, &init_disp_send, &disp_init_recv);

            // Slot 1: console send (sys_write)
            oz::sys_cspace_cap_insert(disp_proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
            // Slot 2: display recv (IPC commands)
            oz::sys_cspace_cap_insert(disp_proc.cspace_cap, 2, disp_init_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
            // Slot 240: Untyped
            oz::sys_cspace_cap_insert(disp_proc.cspace_cap, oz::CAP_UNTYPED, oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::CapRights::ALL));
            // Slot 246: FrameBuffer
            oz::sys_cspace_cap_insert(disp_proc.cspace_cap, oz::CAP_FRAMEBUFFER, oz::CAP_FRAMEBUFFER, static_cast<std::uint32_t>(oz::CapRights::ALL));

            oz::sys_tcb_resume(disp_proc.tcb_cap);
            oz::sys_write("[Init] Display server spawned successfully.\n\r");

            // Send framebuffer info to Display Server
            oz::IPCMessage config_msg{};
            config_msg.data[0] = boot_info->fb_width;
            config_msg.data[1] = boot_info->fb_height;
            config_msg.data[2] = boot_info->fb_size;
            config_msg.data[3] = boot_info->fb_phys_addr;
            oz::sys_channel_send(init_disp_send, &config_msg);
            oz::sys_write("[Init] Sent framebuffer config to Display server.\n\r");
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

    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, 2, ahci_init_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, 3, ahci_vfs_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, 4, ahci_vfs_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, oz::CAP_UNTYPED, oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::CapRights::ALL));
    if (ahci_abar_phys != 0) {
        oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, 241, oz::CAP_AHCI_MMIO, static_cast<std::uint32_t>(oz::CapRights::ALL));
    }
    oz::sys_cspace_cap_insert(ahci_proc.cspace_cap, 242, shared_buf_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));

    oz::sys_tcb_resume(ahci_proc.tcb_cap);
    oz::sys_write("[Init] AHCI server spawned successfully.\n\r");

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

    std::uint32_t init_vfs_send = 0;
    std::uint32_t vfs_init_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &init_vfs_send, &vfs_init_recv);

    std::uint32_t vfs_init_send = 0;
    std::uint32_t init_vfs_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &vfs_init_send, &init_vfs_recv);

    oz::sys_cspace_cap_insert(oz::CAP_SELF_CSPACE, CAP_VFS_SEND, init_vfs_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(oz::CAP_SELF_CSPACE, CAP_VFS_RECV, init_vfs_recv, static_cast<std::uint32_t>(oz::CapRights::READ));

    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 2, vfs_ahci_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 3, vfs_ahci_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 4, vfs_init_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 5, vfs_init_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, oz::CAP_UNTYPED, oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 242, shared_buf_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));

    // Shared buffer for VFS client data transfer (16 pages = 64KB)
    std::uint32_t vfs_client_frame = oz::sys_frame_alloc(oz::CAP_UNTYPED, 16);
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 243, vfs_client_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(oz::CAP_SELF_CSPACE, CAP_VFS_SHARED_BUF, vfs_client_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));

    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, CAP_VFS_SHARED_BUF, VFS_SHARED_BUF_VIRT, 0x01, 16 * 4096) == 0) {
        g_vfs_shared_buf = reinterpret_cast<char*>(VFS_SHARED_BUF_VIRT);
        oz::sys_write("[Init] Mapped VFS client shared buffer (64KB).\n\r");
    }

    oz::sys_tcb_resume(vfs_proc.tcb_cap);
    oz::sys_write("[Init] VFS server spawned successfully.\n\r");

    // 6. Spawn Interactive Shell Server
    std::uint32_t xhci_key_send = cons_send;
    oz::loader::TarFile shell_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "shell");
    if (shell_file.data) {
        auto shell_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, shell_file.data, shell_file.size);
        if (shell_proc.success) {
            std::uint32_t shell_input_send = 0;
            std::uint32_t shell_input_recv = 0;
            oz::sys_channel_create(oz::CAP_UNTYPED, &shell_input_send, &shell_input_recv);
            xhci_key_send = shell_input_send;

            // Slot 0: Self VSpace
            oz::sys_cspace_cap_insert(shell_proc.cspace_cap, 0, shell_proc.vspace_cap, static_cast<std::uint32_t>(oz::CapRights::ALL));
            // Slot 1: Console Send
            oz::sys_cspace_cap_insert(shell_proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
            // Slot 2: Shell Input Recv (from xHCI)
            oz::sys_cspace_cap_insert(shell_proc.cspace_cap, 2, shell_input_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
            // Slot 3: VFS Send
            oz::sys_cspace_cap_insert(shell_proc.cspace_cap, 3, init_vfs_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
            // Slot 4: VFS Recv
            oz::sys_cspace_cap_insert(shell_proc.cspace_cap, 4, init_vfs_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
            // Slot 5: VFS Shared Buffer
            oz::sys_cspace_cap_insert(shell_proc.cspace_cap, 5, vfs_client_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));
            // Slot 240: Untyped
            oz::sys_cspace_cap_insert(shell_proc.cspace_cap, oz::CAP_UNTYPED, oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::CapRights::ALL));

            oz::sys_tcb_resume(shell_proc.tcb_cap);
            oz::sys_write("[Init] Interactive Shell spawned successfully.\n\r");
        } else {
            oz::sys_write("[Init] ERROR: Failed to spawn shell process!\n\r");
        }
    }

    // 7. Spawn xHCI Server
    oz::loader::TarFile xhci_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "xhci");
    if (xhci_file.data) {
        auto xhci_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, xhci_file.data, xhci_file.size);
        if (xhci_proc.success) {
            std::uint32_t irq_send = 0;
            std::uint32_t irq_recv = 0;
            oz::sys_channel_create(oz::CAP_UNTYPED, &irq_send, &irq_recv);

            std::uint32_t irq_handler = oz::sys_irq_handler_create(oz::CAP_UNTYPED, 0x40, irq_send);

            oz::sys_cspace_cap_insert(xhci_proc.cspace_cap, 1, xhci_key_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
            oz::sys_cspace_cap_insert(xhci_proc.cspace_cap, 2, irq_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
            oz::sys_cspace_cap_insert(xhci_proc.cspace_cap, 3, irq_handler, static_cast<std::uint32_t>(oz::CapRights::ALL));
            oz::sys_cspace_cap_insert(xhci_proc.cspace_cap, oz::CAP_UNTYPED, oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::CapRights::ALL));
            if (xhci_bar_phys != 0) {
                oz::sys_cspace_cap_insert(xhci_proc.cspace_cap, 241, oz::CAP_XHCI_MMIO, static_cast<std::uint32_t>(oz::CapRights::ALL));
            }

            oz::sys_tcb_resume(xhci_proc.tcb_cap);
            oz::sys_write("[Init] xHCI driver spawned successfully.\n\r");
        }
    }

    // 7. Send AHCI MMIO address to AHCI driver
    oz::IPCMessage msg{};
    msg.data[0] = ahci_abar_phys;
    if (oz::sys_channel_send(CAP_AHCI_SEND, &msg)) {
        oz::sys_write("Sent AHCI MMIO address to ahci server.\n\r");
    } else {
        oz::sys_write("Failed to send AHCI MMIO address!\n\r");
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

    oz::sys_write("Init process completed. Idling...\n\r");
    while (true) {
        __asm__ volatile("pause");
    }
}
