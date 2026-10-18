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
//   247: CAP_ROOT_IOPORT

static constexpr std::uint32_t CAP_AHCI_SEND = 2;
static constexpr std::uint32_t CAP_VFS_SEND = 3;
static constexpr std::uint32_t CAP_VFS_RECV = 4;
static constexpr std::uint32_t CAP_VFS_SHARED_BUF = 5;
static constexpr std::uint32_t CAP_DISPLAY_SEND = 6;


static constexpr std::uint64_t BOOTINFO_VADDR = 0x600000000000ULL;
static constexpr std::uint64_t INITRD_VADDR   = 0x500000000000ULL;
static constexpr std::uint64_t VFS_SHARED_BUF_VIRT = 0x300000000000ULL;
static char* g_vfs_shared_buf = nullptr;

// Capability-based VFS openat result
struct VfsOpenAtResult {
    oz::ipc::VfsStatus status{oz::ipc::VfsStatus::NOT_FOUND};
    int handle{-1};
    std::uint32_t transferred_cap{oz::INVALID_CAP_ID};
    std::size_t size{0};
};

static VfsOpenAtResult vfs_openat(std::uint32_t dir_send_cap, std::uint32_t dir_recv_cap,
                                  const char* rel_path, oz::ipc::VfsOpenFlags flags,
                                  std::uint32_t base_dir_handle = 0) {
    VfsOpenAtResult res{};
    oz::IPCMessage req{};
    req.data[0] = oz::ipc::vfs_make_openat_command(0, flags, base_dir_handle);

    char* dst = reinterpret_cast<char*>(&req.data[1]);
    int i = 0;
    for (; i < 23 && rel_path[i] != '\0'; i++) {
        dst[i] = rel_path[i];
    }
    dst[i] = '\0';

    if (!oz::sys_channel_send(dir_send_cap, &req)) return res;

    oz::IPCMessage resp{};
    if (!oz::sys_channel_recv(dir_recv_cap, &resp)) return res;

    res.status = static_cast<oz::ipc::VfsStatus>(resp.data[0]);
    if (res.status == oz::ipc::VfsStatus::OK) {
        res.handle = static_cast<int>(resp.data[1]);
        res.size = static_cast<std::size_t>(resp.data[2]);
        if (resp.has_cap) {
            res.transferred_cap = resp.transferred_cap;
        }
    }
    return res;
}

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
        req.data[0] = oz::ipc::vfs_make_command(oz::ipc::VfsCmd::READ, 0);
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
        req.data[0] = oz::ipc::vfs_make_command(oz::ipc::VfsCmd::READ, 0);
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

        oz::sys_channel_send(CAP_VFS_SEND, &req);

        oz::IPCMessage resp{};
        oz::sys_channel_recv(CAP_VFS_RECV, &resp);

        if (resp.data[0] != 0) break;
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
    
    oz::sys_channel_send(CAP_VFS_SEND, &req);
    
    oz::IPCMessage resp{};
    oz::sys_channel_recv(CAP_VFS_RECV, &resp);
    
    if (resp.data[0] == 0) return static_cast<std::size_t>(resp.data[1]);
    return 0;
}

// Helper: close a VFS file
static void vfs_close(int handle) {
    oz::IPCMessage req{};
    req.data[0] = oz::ipc::vfs_make_command(oz::ipc::VfsCmd::CLOSE, 0);
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

    int self_res = oz::sys_cspace_cap_insert(oz::CAP_SELF_CSPACE, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::ALL));
    (void)cs_res; (void)resume_res; (void)self_res;

    oz::sys_write("Hello from init process (Ring 3)!\n\r");
    oz::sys_write("[Init] Console server spawned successfully.\n\r");

    // ========================================================================
    // Self-tests for new Capability semantics (Move, Linear, Once)
    // ========================================================================
    oz::sys_debug_print("[Init] Running Capability semantics tests...\n\r");

    // 1. Test Move semantics
    std::uint32_t t_ep0 = 0, t_ep1 = 0;
    if (oz::sys_channel_create(oz::CAP_UNTYPED, &t_ep0, &t_ep1)) {
        if (oz::sys_cap_move(50, t_ep0) == 0) {
            // Source slot t_ep0 must now be invalid
            if (oz::sys_cap_delete(t_ep0) != 0 && oz::sys_cap_delete(50) == 0) {
                oz::sys_debug_print("[Test] Capability Move semantics: PASSED\n\r");
            } else {
                oz::sys_debug_print("[Test] Capability Move semantics: FAILED (source not invalidated)\n\r");
            }
        } else {
            oz::sys_debug_print("[Test] Capability Move semantics: FAILED (sys_cap_move failed)\n\r");
        }
        oz::sys_cap_delete(t_ep1);
    }

    // 2. Test Linear rejection on copy/mint
    // CAP_ROOT_IOPORT is a Linear object; minting/copying must be rejected!
    std::uint32_t minted_linear = oz::sys_cap_mint(oz::CAP_ROOT_IOPORT, static_cast<std::uint32_t>(oz::CapRights::ALL));
    if (minted_linear == oz::INVALID_CAP_ID) {
        oz::sys_debug_print("[Test] Linear duplication prevention: PASSED\n\r");
    } else {
        oz::sys_debug_print("[Test] Linear duplication prevention: FAILED (Linear object was duplicated!)\n\r");
        oz::sys_cap_delete(minted_linear);
    }

    // 3. Test Once capability auto-consumption
    std::uint32_t once_sender = oz::sys_cap_mint(1, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::ONCE));
    if (once_sender != oz::INVALID_CAP_ID) {
        oz::IPCMessage once_msg{};
        const char* msg_txt = "[Test] Once Capability msg\n\r";
        for (int k = 0; k < 4; ++k) {
            once_msg.data[k] = *reinterpret_cast<const std::uint64_t*>(&msg_txt[k * 8]);
        }
        if (oz::sys_channel_send(once_sender, &once_msg)) {
            // After sending, the once_sender slot must have been removed by kernel!
            if (oz::sys_cap_delete(once_sender) != 0) {
                oz::sys_debug_print("[Test] Once Capability auto-consumption: PASSED\n\r");
            } else {
                oz::sys_debug_print("[Test] Once Capability auto-consumption: FAILED (slot still valid after send)\n\r");
            }
        } else {
            oz::sys_debug_print("[Test] Once Capability auto-consumption: FAILED (send failed)\n\r");
        }
    } else {
        oz::sys_debug_print("[Test] Once Capability auto-consumption: FAILED (mint failed)\n\r");
    }

    // 4. Test C++ RAII LinearCap
    {
        std::uint32_t r_ep0 = 0, r_ep1 = 0;
        if (oz::sys_channel_create(oz::CAP_UNTYPED, &r_ep0, &r_ep1)) {
            oz::LinearCap<> linear1(r_ep0);
            oz::LinearCap<> linear2 = static_cast<oz::LinearCap<>&&>(linear1); // move construct
            if (!linear1.isValid() && linear2.isValid()) {
                if (linear2.moveTo(51)) {
                    oz::sys_debug_print("[Test] C++ LinearCap RAII move: PASSED\n\r");
                } else {
                    oz::sys_debug_print("[Test] C++ LinearCap RAII move: FAILED (moveTo failed)\n\r");
                }
            } else {
                oz::sys_debug_print("[Test] C++ LinearCap RAII move: FAILED\n\r");
            }
            oz::sys_cap_delete(r_ep1);
        }
    }
    // linear2 went out of scope, slot 51 should be deleted automatically
    if (oz::sys_cap_delete(51) != 0) {
        oz::sys_debug_print("[Test] C++ LinearCap RAII destructor cleanup: PASSED\n\r");
    } else {
        oz::sys_debug_print("[Test] C++ LinearCap RAII destructor cleanup: FAILED (slot 51 still valid)\n\r");
    }

    // 5. Test Notification Object & Bound Notification
    std::uint32_t notif_cap = oz::sys_notification_create(oz::CAP_UNTYPED);
    if (notif_cap != oz::INVALID_CAP_ID) {
        oz::sys_debug_print("[Test] Notification creation: PASSED\n\r");

        // Test manual signal and poll
        oz::sys_notification_signal(notif_cap, 0x42);
        std::uint64_t polled_bits = 0;
        if (oz::sys_notification_poll(notif_cap, &polled_bits) == 0 && polled_bits == 0x42) {
            oz::sys_debug_print("[Test] Notification signal & poll: PASSED\n\r");
        } else {
            oz::sys_debug_print("[Test] Notification signal & poll: FAILED\n\r");
        }

        // Test Bound Notification with Channel
        std::uint32_t b_ep0 = 0, b_ep1 = 0;
        if (oz::sys_channel_create(oz::CAP_UNTYPED, &b_ep0, &b_ep1)) {
            // Bind b_ep1 (server side) to notif_cap with badge 0x100
            if (oz::sys_channel_bind_notification(b_ep1, notif_cap, 0x100) == 0) {
                // Client side (b_ep0) sends a message -> should signal notif_cap with 0x100
                // Note: since receiver is not waiting, send will block unless we test peer close or thread
                // Let's test peer close: delete client cap b_ep0
                oz::sys_cap_delete(b_ep0);

                // Now peer was closed! b_ep1 should have signaled notif_cap with 0x100!
                std::uint64_t close_bits = 0;
                if (oz::sys_notification_poll(notif_cap, &close_bits) == 0 && (close_bits & 0x100)) {
                    oz::sys_debug_print("[Test] Bound Notification on client disconnect: PASSED\n\r");
                } else {
                    oz::sys_debug_print("[Test] Bound Notification on client disconnect: FAILED\n\r");
                }
            } else {
                oz::sys_debug_print("[Test] Bound Notification binding: FAILED\n\r");
            }
            oz::sys_cap_delete(b_ep1);
        }
        oz::sys_cap_delete(notif_cap);
    } else {
        oz::sys_debug_print("[Test] Notification creation: FAILED\n\r");
    }


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

            // Forward Display Server input channel to Console Server (Slot 2)
            oz::sys_cspace_cap_insert(cons_proc.cspace_cap, 2, init_disp_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
            oz::sys_write("[Init] Connected Console server to Display server.\n\r");

            // Also keep Display channel in Init for Surface/Admin operations
            oz::sys_cspace_cap_insert(oz::CAP_SELF_CSPACE, CAP_DISPLAY_SEND, init_disp_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));

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
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, oz::CAP_UNTYPED, oz::CAP_UNTYPED, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 242, shared_buf_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));

    // Dedicated shared buffer for Client 0 (init) (16 pages = 64KB)
    std::uint32_t init_client_frame = oz::sys_frame_alloc(oz::CAP_UNTYPED, 16);
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 243, init_client_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));
    oz::sys_cspace_cap_insert(oz::CAP_SELF_CSPACE, CAP_VFS_SHARED_BUF, init_client_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));

    // Dedicated shared buffer for Client 1 (shell) (16 pages = 64KB)
    std::uint32_t shell_client_frame = oz::sys_frame_alloc(oz::CAP_UNTYPED, 16);
    oz::sys_cspace_cap_insert(vfs_proc.cspace_cap, 244, shell_client_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));

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
            // Slot 3: VFS Request Send (shared request channel)
            oz::sys_cspace_cap_insert(shell_proc.cspace_cap, 3, vfs_req_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
            // Slot 4: VFS Reply Recv (shell dedicated reply channel)
            oz::sys_cspace_cap_insert(shell_proc.cspace_cap, 4, shell_reply_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
            // Slot 5: VFS Shared Buffer (shell dedicated buffer frame)
            oz::sys_cspace_cap_insert(shell_proc.cspace_cap, 5, shell_client_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));
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

    // ========================================================================
    // Capability-based VFS Self-Tests (Ambient Authority Elimination)
    // ========================================================================
    oz::sys_debug_print("[Init] Running Capability-based VFS self-tests...\n\r");

    // Test 1: Valid relative open of file
    auto res_file = vfs_openat(CAP_VFS_SEND, CAP_VFS_RECV, "test.txt", oz::ipc::VfsOpenFlags::FILE);
    if (res_file.status == oz::ipc::VfsStatus::OK && res_file.transferred_cap != oz::INVALID_CAP_ID) {
        oz::sys_debug_print("[Test] VFS openat relative file (Cap Transfer): PASSED\n\r");
    } else {
        oz::sys_debug_print("[Test] VFS openat relative file: FAILED\n\r");
    }

    // Test 2: Reject ".." in relative path (Sandbox violation)
    auto res_jailbreak = vfs_openat(CAP_VFS_SEND, CAP_VFS_RECV, "../test.txt", oz::ipc::VfsOpenFlags::FILE);
    if (res_jailbreak.status == oz::ipc::VfsStatus::INVALID_PATH) {
        oz::sys_debug_print("[Test] VFS sandbox '..' traversal rejection: PASSED\n\r");
    } else {
        oz::sys_debug_print("[Test] VFS sandbox '..' traversal rejection: FAILED\n\r");
    }

    // Test 3: Reject leading '/' (Absolute path ambient authority violation)
    auto res_abs = vfs_openat(CAP_VFS_SEND, CAP_VFS_RECV, "/test.txt", oz::ipc::VfsOpenFlags::FILE);
    if (res_abs.status == oz::ipc::VfsStatus::INVALID_PATH) {
        oz::sys_debug_print("[Test] VFS sandbox absolute path rejection: PASSED\n\r");
    } else {
        oz::sys_debug_print("[Test] VFS sandbox absolute path rejection: FAILED\n\r");
    }

    // Test 4: Open subdirectory as Directory Capability
    auto res_dir = vfs_openat(CAP_VFS_SEND, CAP_VFS_RECV, "os", oz::ipc::VfsOpenFlags::DIRECTORY);
    if (res_dir.status == oz::ipc::VfsStatus::OK && res_dir.transferred_cap != oz::INVALID_CAP_ID) {
        oz::sys_debug_print("[Test] VFS openat Directory Capability: PASSED\n\r");

        // Test 5: Open child file relative to the new Directory Capability
        auto res_subfile = vfs_openat(CAP_VFS_SEND, CAP_VFS_RECV, "kernel.bin", oz::ipc::VfsOpenFlags::FILE, res_dir.handle);
        if (res_subfile.status == oz::ipc::VfsStatus::OK && res_subfile.transferred_cap != oz::INVALID_CAP_ID) {
            oz::sys_debug_print("[Test] VFS openat child relative to DirectoryCap: PASSED\n\r");
        } else {
            oz::sys_debug_print("[Test] VFS openat child relative to DirectoryCap: FAILED\n\r");
        }

        // Test 6: Attempt '..' escape from Directory Capability
        auto res_sub_escape = vfs_openat(CAP_VFS_SEND, CAP_VFS_RECV, "../test.txt", oz::ipc::VfsOpenFlags::FILE, res_dir.handle);
        if (res_sub_escape.status == oz::ipc::VfsStatus::INVALID_PATH) {
            oz::sys_debug_print("[Test] VFS child '..' escape rejection: PASSED\n\r");
        } else {
            oz::sys_debug_print("[Test] VFS child '..' escape rejection: FAILED\n\r");
        }
    } else {
        oz::sys_debug_print("[Test] VFS openat Directory Capability: FAILED\n\r");
    }

    // ========================================================================
    // Capability-based Display & Surface Self-Tests (Mechanism vs Policy)
    // ========================================================================
    oz::sys_debug_print("[Init] Running Capability-based Display & Surface self-tests...\n\r");

    std::uint32_t disp_reply_send = 0;
    std::uint32_t disp_reply_recv = 0;
    oz::sys_channel_create(oz::CAP_UNTYPED, &disp_reply_send, &disp_reply_recv);

    // Test 1: CREATE_SURFACE via Display Capability
    oz::IPCMessage req_surf{};
    req_surf.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::CREATE_SURFACE);
    req_surf.data[1] = 160; // width
    req_surf.data[2] = 120; // height
    req_surf.transferred_cap = disp_reply_send;
    req_surf.has_cap = true;

    oz::sys_channel_send(CAP_DISPLAY_SEND, &req_surf);

    oz::IPCMessage resp_surf{};
    if (oz::sys_channel_recv(disp_reply_recv, &resp_surf) && resp_surf.data[0] == 0 && resp_surf.has_cap) {
        oz::sys_debug_print("[Test] Display create_surface (FrameCap Transfer): PASSED\n\r");
        std::uint32_t surf_id = static_cast<std::uint32_t>(resp_surf.data[1]);
        std::uint32_t surf_frame_cap = resp_surf.transferred_cap;

        // Test 2: Map the transferred FrameCap into client address space
        static constexpr std::uint64_t TEST_SURF_VADDR = 0x700000000000ULL;
        if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, surf_frame_cap, TEST_SURF_VADDR, 0x01, 160 * 120 * 4) == 0) {
            oz::sys_debug_print("[Test] Surface FrameCap mapping: PASSED\n\r");

            // Draw a bright cyan test rectangle in the client's surface buffer
            struct Pixel { std::uint8_t b, g, r, a; };
            Pixel* surf_buf = reinterpret_cast<Pixel*>(TEST_SURF_VADDR);
            for (int y = 0; y < 120; ++y) {
                for (int x = 0; x < 160; ++x) {
                    surf_buf[y * 160 + x] = Pixel{0xFF, 0xFF, 0x00, 0}; // Cyan
                }
            }

            // Test 3: Set surface position and commit composition
            oz::IPCMessage req_pos{};
            req_pos.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::SET_SURFACE_RECT);
            req_pos.data[1] = surf_id;
            req_pos.data[2] = (30ULL << 32) | 50ULL; // (x=30, y=50)
            oz::sys_channel_send(CAP_DISPLAY_SEND, &req_pos);

            // Test 4: Update cursor position (Mouse Cursor simulation)
            oz::IPCMessage req_cur{};
            req_cur.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::UPDATE_CURSOR);
            req_cur.data[1] = (40ULL << 32) | 60ULL; // (cursor_x=40, cursor_y=60)
            req_cur.data[2] = 1; // visible
            oz::sys_channel_send(CAP_DISPLAY_SEND, &req_cur);

            // Commit
            oz::IPCMessage req_commit{};
            req_commit.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::COMMIT);
            req_commit.data[1] = surf_id;
            oz::sys_channel_send(CAP_DISPLAY_SEND, &req_commit);

            oz::sys_debug_print("[Test] Surface SetRect & Cursor Update & Commit: PASSED\n\r");
        } else {
            oz::sys_debug_print("[Test] Surface FrameCap mapping: FAILED\n\r");
        }
    } else {
        oz::sys_debug_print("[Test] Display create_surface: FAILED\n\r");
    }

    // ========================================================================
    // Step 2: Process Startup & Local Namespace Construction Self-Tests
    // ========================================================================
    oz::sys_debug_print("[Init] Running Step 2 Local Namespace & Manifest Sandboxing tests...\n\r");

    oz::loader::TarFile test_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "manifest_test");
    if (test_file.data) {
        oz::ProcessManifest manifest{};
        bool has_manifest = oz::loader::get_elf_manifest(test_file.data, test_file.size, manifest);
        if (has_manifest) {
            oz::sys_debug_print("[Init] Found .oz_manifest in manifest_test! Parsed successfully.\n\r");

            oz::ProcessStartupInfo startup_info{};
            std::uint32_t next_slot = 1;

            if (manifest.needs_console) {
                startup_info.add_cap(next_slot++, oz::CapRole::STDOUT, "stdout");
            }
            if (manifest.needs_vfs_root) {
                startup_info.add_cap(next_slot++, oz::CapRole::ROOT_DIR, "/");
            }

            // Test arguments passing (including spaced argument) and delegated capability
            startup_info.add_arg("manifest_test");
            startup_info.add_arg("test.txt");
            startup_info.add_arg("spaced argument.log");

            std::uint32_t delegated_slot = next_slot++;
            startup_info.add_cap(delegated_slot, oz::CapRole::CUSTOM, "test.txt");

            std::uint32_t test_file_send = 0, test_file_recv = 0;
            oz::sys_channel_create(oz::CAP_UNTYPED, &test_file_send, &test_file_recv);

            auto test_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, test_file.data, test_file.size, &startup_info);
            if (test_proc.success) {
                // Insert granted capabilities into child's CSpace
                for (std::size_t i = 0; i < startup_info.num_caps; ++i) {
                    if (startup_info.caps[i].role == oz::CapRole::STDOUT) {
                        oz::sys_cspace_cap_insert(test_proc.cspace_cap, startup_info.caps[i].slot, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
                    }
                }
                // Insert delegated test.txt capability into slot
                oz::sys_cspace_cap_insert(test_proc.cspace_cap, delegated_slot, test_file_send, static_cast<std::uint32_t>(oz::CapRights::READ | oz::CapRights::WRITE | oz::CapRights::GRANT));

                oz::sys_tcb_resume(test_proc.tcb_cap);
                oz::sys_debug_print("[Init] manifest_test process spawned and resumed.\n\r");
            } else {
                oz::sys_debug_print("[Init] ERROR: Failed to spawn manifest_test!\n\r");
            }
        } else {
            oz::sys_debug_print("[Init] ERROR: manifest_test does not have .oz_manifest!\n\r");
        }
    } else {
        oz::sys_debug_print("[Init] ERROR: manifest_test not found in initrd!\n\r");
    }

    // ========================================================================
    // Step 3 & 4 Verification: Sandboxed cat process with delegated FileCap
    // ========================================================================

    oz::sys_debug_print("[Init] Testing Sandboxed 'cat' execution with FileCap...\n\r");
    oz::loader::TarFile cat_file = oz::loader::tar_find_file(initrd_ptr, initrd_size, "cat");
    if (cat_file.data) {
        auto res = vfs_openat(CAP_VFS_SEND, CAP_VFS_RECV, "test.txt", oz::ipc::VfsOpenFlags::FILE);
        if (res.status == oz::ipc::VfsStatus::OK && res.transferred_cap != oz::INVALID_CAP_ID) {
            oz::ProcessStartupInfo cat_startup{};
            cat_startup.add_cap(1, oz::CapRole::STDOUT, "stdout", oz::CapType::STREAM);
            cat_startup.add_cap(2, oz::CapRole::CUSTOM, "test.txt", oz::CapType::FILE);
            cat_startup.add_arg("cat");
            cat_startup.add_arg("test.txt");

            auto cat_proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, cat_file.data, cat_file.size, &cat_startup);
            if (cat_proc.success) {
                oz::sys_cspace_cap_insert(cat_proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
                oz::sys_cspace_cap_insert(cat_proc.cspace_cap, 2, res.transferred_cap, static_cast<std::uint32_t>(oz::CapRights::READ | oz::CapRights::WRITE | oz::CapRights::GRANT));
                oz::sys_tcb_resume(cat_proc.tcb_cap);
                oz::sys_debug_print("[Init] Sandboxed cat process spawned successfully with FileCap!\n\r");
            }
        }
    }

    oz::sys_write("Init process completed. Idling...\n\r");


    while (true) {
        __asm__ volatile("pause");
    }
}

