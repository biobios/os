#include "syscall.hpp"
#include "loader.hpp"
#include "../common/ipc/Protocol.hpp"
#include "../common/ipc/StartupInfo.hpp"
#include "../common/utils/utils.hpp"

// Process Manager does NOT need raw untyped authority from kernel.
// It obtains memory via MemMan!
OZ_MANIFEST((oz::ProcessManifest{
    .magic = oz::ProcessManifest::MAGIC,
    .version = 1,
    .provides = 0,
    .required = oz::ServiceId::CONSOLE | oz::ServiceId::VFS_ROOT,
    .device_needed = oz::DeviceRequirement::NONE,
    .needs_untyped = false,
    .default_data_dir = {}
}));

static constexpr std::uint32_t CAP_CONSOLE_WRITE   = 1;
static constexpr std::uint32_t CAP_PROCMAN_RECV    = 2;
static constexpr std::uint32_t CAP_PROCMAN_REPLY   = 3;
static constexpr std::uint32_t CAP_MEMMAN_SEND     = 4;
static constexpr std::uint32_t CAP_MEMMAN_RECV     = 5;
static constexpr std::uint32_t CAP_VFS_SEND        = 6;
static constexpr std::uint32_t CAP_VFS_RECV        = 7;
static constexpr std::uint32_t CAP_VFS_SHARED_BUF  = 8;
static constexpr std::uint32_t CAP_WM_SEND         = 9;
static constexpr std::uint32_t CAP_DISPLAY_SEND    = 10;
static constexpr std::uint32_t CAP_DRIVMAN_SEND    = 11;
static constexpr std::uint32_t CAP_SCHED_SEND      = 12;
static constexpr std::uint32_t CAP_SHELL_SHARED_BUF = 13;

static constexpr std::uint64_t VFS_SHARED_BUF_VIRT = 0x300000000000ULL;
static constexpr std::uint64_t STAGING_ADDR        = 0x400000000000ULL;
static char* g_shared_buf = nullptr;

static std::uint32_t g_procman_work_untyped = oz::INVALID_CAP_ID;

// ===== Helper: Memory Manager Client =====
static std::uint32_t memman_alloc_untyped(std::size_t size) {
    oz::IPCMessage req{};
    req.data[0] = static_cast<std::uint64_t>(oz::ipc::MemCmd::ALLOC_UNTYPED);
    req.data[1] = size;

    oz::IPCMessage resp{};
    if (!oz::sys_call(CAP_MEMMAN_SEND, &req, &resp)) return oz::INVALID_CAP_ID;

    if (resp.data[0] == 0 && resp.has_cap) {
        return resp.transferred_cap;
    }
    return oz::INVALID_CAP_ID;
}

static bool memman_revoke_untyped(std::uint32_t untyped_cap) {
    if (untyped_cap == oz::INVALID_CAP_ID) return false;

    oz::IPCMessage req{};
    req.data[0] = static_cast<std::uint64_t>(oz::ipc::MemCmd::REVOKE_UNTYPED);
    req.has_cap = true;
    req.transferred_cap = untyped_cap;

    oz::IPCMessage resp{};
    if (!oz::sys_call(CAP_MEMMAN_SEND, &req, &resp)) return false;

    return resp.data[0] == 0;
}

// ===== Helper: VFS Operations =====
static int vfs_open(const char* path) {
    oz::IPCMessage req{};
    req.data[0] = oz::ipc::vfs_make_command(oz::ipc::VfsCmd::OPEN, 2); // Client 2 = ProcMan
    char* dst = reinterpret_cast<char*>(&req.data[1]);
    int i = 0;
    for (; i < 23 && path[i] != '\0'; i++) {
        dst[i] = path[i];
    }
    dst[i] = '\0';

    oz::IPCMessage resp{};
    if (oz::sys_call(CAP_VFS_SEND, &req, &resp)) {
        if (resp.data[0] == 0) return static_cast<int>(resp.data[1]);
    }
    return -1;
}

static std::size_t vfs_read(int handle, std::size_t offset, char* buf, std::size_t size) {
    if (!g_shared_buf) return 0;
    std::size_t total_read = 0;
    while (total_read < size) {
        std::size_t chunk = size - total_read;
        if (chunk > 64 * 1024) chunk = 64 * 1024;

        oz::IPCMessage req{};
        req.data[0] = oz::ipc::vfs_make_command(oz::ipc::VfsCmd::READ, 2);
        req.data[1] = static_cast<std::uint64_t>(handle);
        req.data[2] = static_cast<std::uint64_t>(offset + total_read);
        req.data[3] = static_cast<std::uint64_t>(chunk);

        oz::IPCMessage resp{};
        if (!oz::sys_call(CAP_VFS_SEND, &req, &resp) || resp.data[0] != 0) {
            break;
        }
        std::size_t bytes_read = static_cast<std::size_t>(resp.data[1]);
        if (bytes_read == 0) break;

        for (std::size_t i = 0; i < bytes_read; i++) {
            buf[total_read + i] = g_shared_buf[i];
        }
        total_read += bytes_read;
        if (bytes_read < chunk) break;
    }
    return total_read;
}

static std::size_t vfs_get_size(int handle) {
    oz::IPCMessage req{};
    req.data[0] = oz::ipc::vfs_make_command(oz::ipc::VfsCmd::GET_SIZE, 2);
    req.data[1] = static_cast<std::uint64_t>(handle);
    oz::IPCMessage resp{};
    if (oz::sys_call(CAP_VFS_SEND, &req, &resp)) {
        if (resp.data[0] == 0) return static_cast<std::size_t>(resp.data[1]);
    }
    return 0;
}

static void vfs_close(int handle) {
    oz::IPCMessage req{};
    req.data[0] = oz::ipc::vfs_make_command(oz::ipc::VfsCmd::CLOSE, 2);
    req.data[1] = static_cast<std::uint64_t>(handle);
    oz::IPCMessage resp{};
    oz::sys_call(CAP_VFS_SEND, &req, &resp);
}

// ===== Process & Session Tracking =====
struct SpawnSession {
    bool in_use{false};
    char prog_name[32]{};
    std::uint32_t stdin_cap{0};
    std::uint32_t stdout_cap{0};
    oz::ProcessStartupInfo startup_info{};

    struct DelegatedCap {
        char name[32]{};
        std::uint32_t cap{0};
    };
    DelegatedCap delegated[8]{};
    std::size_t num_delegated{0};

    void reset() {
        in_use = false;
        prog_name[0] = '\0';
        stdin_cap = 0;
        stdout_cap = 0;
        startup_info = oz::ProcessStartupInfo{};
        num_delegated = 0;
    }
};

static SpawnSession g_session{};

struct ProcessEntry {
    bool in_use{false};
    std::uint32_t pid{0};
    std::uint32_t tcb_cap{oz::INVALID_CAP_ID};
    std::uint32_t cspace_cap{oz::INVALID_CAP_ID};
    std::uint32_t vspace_cap{oz::INVALID_CAP_ID};
    std::uint32_t untyped_cap{oz::INVALID_CAP_ID};
    std::uint32_t exit_notif{oz::INVALID_CAP_ID};
};

static constexpr std::size_t MAX_PROCS = 16;
static ProcessEntry g_procs[MAX_PROCS]{};
static std::uint32_t g_next_pid = 1;

static ProcessEntry* find_proc(std::uint32_t pid) {
    for (std::size_t i = 0; i < MAX_PROCS; ++i) {
        if (g_procs[i].in_use && g_procs[i].pid == pid) {
            return &g_procs[i];
        }
    }
    return nullptr;
}

static bool streq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a++ != *b++) return false;
    }
    return *a == *b;
}

static void run_dag_orchestration(std::uint32_t sched_cap, std::uint32_t drivman_cap) {
    oz::sys_write("[ProcMan] Starting Phase 2: DAG Component Orchestration...\n\r");

    std::uint32_t active_services = static_cast<std::uint32_t>(oz::ServiceId::VFS_ROOT);

    std::uint32_t disp_cmd_send = 0;
    std::uint32_t disp_cmd_recv = 0;
    std::uint32_t disp_txt_send = 0;
    std::uint32_t disp_txt_recv = 0;
    std::uint32_t cons_send = 0;
    std::uint32_t cons_recv = 0;
    std::uint32_t wm_req_send = 0;
    std::uint32_t wm_req_recv = 0;

    std::uint32_t kbd_ring_frame = oz::sys_frame_alloc(g_procman_work_untyped, 1);
    std::uint32_t notif_wm_kbd = oz::sys_notification_create(g_procman_work_untyped);

    const char* dag_components[] = {"display", "console", "wm", "ahci", "xhci", "shell"};
    constexpr std::size_t NUM_COMPS = 6;
    bool spawned[NUM_COMPS] = {false};
    std::size_t spawned_count = 0;

    while (spawned_count < NUM_COMPS) {
        bool progress = false;
        for (std::size_t i = 0; i < NUM_COMPS; ++i) {
            if (spawned[i]) continue;
            const char* name = dag_components[i];

            int fd = vfs_open(name);
            if (fd < 0) {
                spawned[i] = true;
                spawned_count++;
                progress = true;
                continue;
            }

            std::size_t sz = vfs_get_size(fd);
            if (sz == 0 || sz > 4 * 1024 * 1024) {
                vfs_close(fd);
                spawned[i] = true;
                spawned_count++;
                progress = true;
                continue;
            }

            std::size_t pages = (sz + 4095) / 4096;
            std::uint32_t buf_frame = oz::sys_frame_alloc(g_procman_work_untyped, pages);
            if (buf_frame == oz::INVALID_CAP_ID) {
                vfs_close(fd);
                break;
            }

            if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, g_procman_work_untyped, buf_frame, STAGING_ADDR, 0x01, pages * 4096) != 0) {
                oz::sys_cap_delete(buf_frame);
                vfs_close(fd);
                break;
            }

            char* staging = reinterpret_cast<char*>(STAGING_ADDR);
            vfs_read(fd, 0, staging, sz);
            vfs_close(fd);

            oz::ProcessManifest manifest{};
            bool has_manifest = oz::loader::get_elf_manifest(reinterpret_cast<const std::uint8_t*>(staging), sz, manifest);

            if (has_manifest && (manifest.required & ~active_services) != 0) {
                for (std::size_t p = 0; p < pages; ++p) {
                    oz::sys_vspace_unmap(oz::CAP_SELF_VSPACE, STAGING_ADDR + p * 4096);
                }
                oz::sys_cap_delete(buf_frame);
                continue;
            }

            oz::sys_write("[ProcMan] Spawning component from DAG: ");
            oz::sys_write(name);
            oz::sys_write("\n\r");

            std::size_t child_ut_size = 2 * 1024 * 1024;
            while (child_ut_size < sz + 512 * 1024) {
                child_ut_size <<= 1;
            }
            std::uint32_t child_untyped = memman_alloc_untyped(child_ut_size);

            oz::ProcessStartupInfo startup_info{};
            std::uint32_t next_slot = 10;

            if (has_manifest && manifest.requires_service(oz::ServiceId::CONSOLE)) {
                startup_info.add_cap(next_slot++, oz::CapRole::STDOUT, "stdout", oz::CapType::STREAM);
            }
            if (has_manifest && manifest.requires_service(oz::ServiceId::DISPLAY)) {
                startup_info.add_cap(next_slot++, oz::CapRole::DISPLAY, "display", oz::CapType::STREAM);
            }
            if (has_manifest && manifest.requires_service(oz::ServiceId::WM)) {
                startup_info.add_cap(next_slot++, oz::CapRole::WM, "wm", oz::CapType::STREAM);
            }
            if (has_manifest && manifest.requires_service(oz::ServiceId::VFS_ROOT)) {
                startup_info.add_cap(next_slot++, oz::CapRole::ROOT_DIR, "/", oz::CapType::DIR);
            }
            if (has_manifest && manifest.device_needed != oz::DeviceRequirement::NONE) {
                startup_info.add_cap(next_slot++, oz::CapRole::DRIVMAN, "drivman", oz::CapType::STREAM);
            }

            bool is_interrupt = (has_manifest && manifest.device_needed == oz::DeviceRequirement::XHCI);

            auto proc = is_interrupt ?
                oz::loader::spawn_interrupt_process(
                    child_untyped, oz::CAP_SELF_VSPACE,
                    reinterpret_cast<const std::uint8_t*>(staging), sz,
                    0x550000000000ULL, &startup_info
                ) :
                oz::loader::spawn_process(
                    child_untyped, oz::CAP_SELF_VSPACE,
                    reinterpret_cast<const std::uint8_t*>(staging), sz,
                    0x550000000000ULL, &startup_info, false
                );

            for (std::size_t p = 0; p < pages; ++p) {
                oz::sys_vspace_unmap(oz::CAP_SELF_VSPACE, STAGING_ADDR + p * 4096);
            }
            oz::sys_cap_delete(buf_frame);

            if (!proc.success) {
                oz::sys_write("[ProcMan] ERROR: Failed to spawn component!\n\r");
                memman_revoke_untyped(child_untyped);
                spawned[i] = true;
                spawned_count++;
                progress = true;
                continue;
            }

            // Specific wiring for each component
            if (has_manifest && manifest.provides_service(oz::ServiceId::DISPLAY)) {
                oz::sys_channel_create(g_procman_work_untyped, &disp_cmd_send, &disp_cmd_recv);
                oz::sys_channel_create(g_procman_work_untyped, &disp_txt_send, &disp_txt_recv);
                oz::sys_cspace_cap_insert(proc.cspace_cap, 1, CAP_CONSOLE_WRITE, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
                oz::sys_cspace_cap_insert(proc.cspace_cap, 2, disp_txt_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
                oz::sys_cspace_cap_insert(proc.cspace_cap, 3, disp_cmd_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
                if (drivman_cap != 0) {
                    oz::sys_cspace_cap_insert(proc.cspace_cap, startup_info.find_slot(oz::CapRole::DRIVMAN), drivman_cap, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT | oz::CapRights::DUPLICATE));
                }
                active_services |= static_cast<std::uint32_t>(oz::ServiceId::DISPLAY);
            } else if (has_manifest && manifest.provides_service(oz::ServiceId::CONSOLE)) {
                oz::sys_channel_create(g_procman_work_untyped, &cons_send, &cons_recv);
                oz::sys_cspace_cap_insert(proc.cspace_cap, 1, cons_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
                if (disp_txt_send != 0) {
                    oz::sys_cspace_cap_insert(proc.cspace_cap, 2, disp_txt_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
                }
                active_services |= static_cast<std::uint32_t>(oz::ServiceId::CONSOLE);
            } else if (has_manifest && manifest.provides_service(oz::ServiceId::WM)) {
                oz::sys_channel_create(g_procman_work_untyped, &wm_req_send, &wm_req_recv);
                oz::sys_cspace_cap_insert(proc.cspace_cap, 0, proc.vspace_cap, static_cast<std::uint32_t>(oz::CapRights::ALL));
                oz::sys_cspace_cap_insert(proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
                oz::sys_cspace_cap_insert(proc.cspace_cap, 2, disp_cmd_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
                oz::sys_cspace_cap_insert(proc.cspace_cap, startup_info.find_slot(oz::CapRole::DISPLAY), disp_cmd_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
                oz::sys_cspace_cap_insert(proc.cspace_cap, 4, wm_req_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
                oz::sys_cspace_cap_insert(proc.cspace_cap, 5, kbd_ring_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));
                oz::sys_cspace_cap_insert(proc.cspace_cap, 6, notif_wm_kbd, static_cast<std::uint32_t>(oz::CapRights::ALL));
                active_services |= static_cast<std::uint32_t>(oz::ServiceId::WM);
            } else if (has_manifest && manifest.provides_service(oz::ServiceId::BLOCK_DEV)) {
                if (cons_send != 0) oz::sys_cspace_cap_insert(proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
                if (drivman_cap != 0) oz::sys_cspace_cap_insert(proc.cspace_cap, startup_info.find_slot(oz::CapRole::DRIVMAN), drivman_cap, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT | oz::CapRights::DUPLICATE));
                oz::sys_cspace_cap_insert(proc.cspace_cap, startup_info.find_slot(oz::CapRole::ROOT_DIR), CAP_VFS_SEND, static_cast<std::uint32_t>(oz::CapRights::ALL));
                active_services |= static_cast<std::uint32_t>(oz::ServiceId::BLOCK_DEV);
            } else if (is_interrupt) { // xHCI
                if (wm_req_send != 0) {
                    oz::sys_cspace_cap_insert(proc.cspace_cap, 1, wm_req_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
                    oz::sys_cspace_cap_insert(proc.cspace_cap, startup_info.find_slot(oz::CapRole::WM), wm_req_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
                }
                oz::sys_cspace_cap_insert(proc.cspace_cap, 4, kbd_ring_frame, static_cast<std::uint32_t>(oz::CapRights::ALL));
                oz::sys_cspace_cap_insert(proc.cspace_cap, 5, notif_wm_kbd, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
                
                // Broker xHCI MMIO & IRQ from DrivMan into xHCI's CSpace
                if (drivman_cap != 0) {
                    // 1. Get MMIO Frame from DrivMan
                    oz::IPCMessage req{};
                    req.data[0] = static_cast<std::uint64_t>(oz::ipc::DrivCmd::GET_DEVICE_CAP);
                    const char* dname = "xhci";
                    for (int k = 0; k < 4; ++k) reinterpret_cast<char*>(&req.data[1])[k] = dname[k];
                    oz::IPCMessage resp{};
                    if (oz::sys_call(drivman_cap, &req, &resp) && resp.data[0] == 0 && resp.has_cap) {
                        oz::sys_cspace_cap_move(proc.cspace_cap, 241, resp.transferred_cap);
                    }

                    // 2. Create IRQ channel and get IRQ Handler from DrivMan
                    std::uint32_t irq_send = 0, irq_recv = 0;
                    oz::sys_channel_create(g_procman_work_untyped, &irq_send, &irq_recv);
                    oz::sys_cspace_cap_move(proc.cspace_cap, 2, irq_recv);

                    oz::IPCMessage irq_req{};
                    irq_req.data[0] = static_cast<std::uint64_t>(oz::ipc::DrivCmd::GET_DEVICE_CAP);
                    const char* iname = "xhci_irq";
                    for (int k = 0; k < 8; ++k) reinterpret_cast<char*>(&irq_req.data[1])[k] = iname[k];
                    irq_req.has_cap = true;
                    irq_req.transferred_cap = irq_send;
                    oz::IPCMessage irq_resp{};
                    if (oz::sys_call(drivman_cap, &irq_req, &irq_resp) && irq_resp.data[0] == 0 && irq_resp.has_cap) {
                        oz::sys_cspace_cap_move(proc.cspace_cap, 3, irq_resp.transferred_cap);
                    }
                }
            } else { // Shell
                if (cons_send != 0) {
                    oz::sys_cspace_cap_insert(proc.cspace_cap, 1, cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
                    oz::sys_cspace_cap_insert(proc.cspace_cap, startup_info.find_slot(oz::CapRole::STDOUT), cons_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
                }
                oz::sys_cspace_cap_insert(proc.cspace_cap, 3, CAP_VFS_SEND, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
                oz::sys_cspace_cap_insert(proc.cspace_cap, startup_info.find_slot(oz::CapRole::ROOT_DIR), CAP_VFS_SEND, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
                if (disp_cmd_send != 0) {
                    oz::sys_cspace_cap_insert(proc.cspace_cap, startup_info.find_slot(oz::CapRole::DISPLAY), disp_cmd_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
                }
                if (wm_req_send != 0) {
                    oz::sys_cspace_cap_insert(proc.cspace_cap, startup_info.find_slot(oz::CapRole::WM), wm_req_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
                }
                // ProcMan channels for Shell to spawn commands
                std::uint32_t sh_p_send = 0, sh_p_recv = 0;
                oz::sys_channel_create(g_procman_work_untyped, &sh_p_send, &sh_p_recv);
                oz::sys_cspace_cap_insert(proc.cspace_cap, 6, sh_p_send, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
                oz::sys_cspace_cap_insert(proc.cspace_cap, 7, sh_p_recv, static_cast<std::uint32_t>(oz::CapRights::READ));
                // VFS client buffer for Shell
                oz::sys_cspace_cap_insert(proc.cspace_cap, 5, CAP_SHELL_SHARED_BUF, static_cast<std::uint32_t>(oz::CapRights::ALL));
                oz::sys_vspace_map_with_pagetables(proc.vspace_cap, child_untyped, CAP_SHELL_SHARED_BUF, 0x300000000000ULL, 0x01, 16 * 4096);
            }

            // Untyped for child
            if (has_manifest && manifest.needs_untyped) {
                std::size_t ut_size = 4 * 1024 * 1024;
                if (streq(name, "display")) {
                    ut_size = 16 * 1024 * 1024;
                } else if (streq(name, "wm")) {
                    ut_size = 8 * 1024 * 1024;
                }
                std::uint32_t app_untyped = memman_alloc_untyped(ut_size);
                if (app_untyped != oz::INVALID_CAP_ID) {
                    oz::sys_cspace_cap_insert(proc.cspace_cap, oz::CAP_UNTYPED, app_untyped, static_cast<std::uint32_t>(oz::CapRights::ALL));
                }
            }

            // Register with Sched daemon
            oz::IPCMessage reg_msg{};
            if (is_interrupt) {
                reg_msg.data[0] = static_cast<uint64_t>(oz::ipc::SchedCmd::REGISTER_IRQ_HANDLER);
                reg_msg.data[1] = 0;
                reg_msg.data[2] = 10000;
            } else {
                reg_msg.data[0] = static_cast<uint64_t>(oz::ipc::SchedCmd::REGISTER_THREAD);
                reg_msg.data[1] = 1;
                reg_msg.data[2] = 10000;
            }
            constexpr std::uint32_t TCB_COPY_SLOT = 160;
            oz::sys_cap_copy(TCB_COPY_SLOT, proc.tcb_cap, static_cast<std::uint32_t>(oz::CapRights::ALL));
            reg_msg.has_cap = true;
            reg_msg.transferred_cap = TCB_COPY_SLOT;
            oz::IPCMessage reg_resp{};
            oz::sys_call(sched_cap, &reg_msg, &reg_resp);

            // Resume process!
            if (!is_interrupt) {
                oz::sys_tcb_resume(proc.tcb_cap);
            }
            oz::sys_write("[ProcMan] Process resumed and registered with Sched: ");
            oz::sys_write(name);
            oz::sys_write("\n\r");

            spawned[i] = true;
            spawned_count++;
            progress = true;
            break;
        }

        if (!progress) {
            oz::sys_write("[ProcMan] ERROR: DAG dependency deadlock detected!\n\r");
            break;
        }
    }

    oz::sys_write("[ProcMan] Phase 2 DAG Component Orchestration complete!\n\r");
}

OZ_PROCESS_ENTRY() {
    oz::LocalNamespace::init(info);
    oz::sys_write("[ProcMan] Process Manager server started (Ring 3)!\n\r");

    // 1. Request initial working UntypedMemory (8MB) from MemMan for internal objects & staging
    g_procman_work_untyped = memman_alloc_untyped(8 * 1024 * 1024);
    if (g_procman_work_untyped == oz::INVALID_CAP_ID) {
        oz::sys_write("[ProcMan] FATAL: Failed to obtain working memory from MemMan!\n\r");
        oz::sys_exit(1);
    }
    oz::sys_write("[ProcMan] Obtained working UntypedMemory from MemMan successfully.\n\r");

    // 2. Map VFS shared buffer (64KB)
    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, g_procman_work_untyped, CAP_VFS_SHARED_BUF, VFS_SHARED_BUF_VIRT, 0x01, 16 * 4096) == 0) {
        g_shared_buf = reinterpret_cast<char*>(VFS_SHARED_BUF_VIRT);
        oz::sys_write("[ProcMan] Mapped VFS client shared buffer (64KB).\n\r");
    }

    // 3. Ensure staging intermediate pagetables
    oz::sys_vspace_ensure_pagetables_range(oz::CAP_SELF_VSPACE, g_procman_work_untyped, STAGING_ADDR, 16 * 1024 * 1024);
    oz::sys_vspace_ensure_pagetables_range(oz::CAP_SELF_VSPACE, g_procman_work_untyped, 0x550000000000ULL, 16 * 1024 * 1024);

    std::uint32_t sched_cap = oz::LocalNamespace::get_cap("sched");
    std::uint32_t drivman_cap = oz::LocalNamespace::get_cap(oz::CapRole::DRIVMAN);

    if (sched_cap != 0) {
        run_dag_orchestration(sched_cap, drivman_cap);
    }

    oz::sys_write("[ProcMan] Process Manager ready to serve spawn/wait requests.\n\r");

    oz::ReplyToken token = oz::INVALID_REPLY_TOKEN;
    oz::IPCMessage resp{};
    oz::IPCMessage req{};

    while (true) {
        token = oz::sys_reply_recv(token, CAP_PROCMAN_RECV, (token != oz::INVALID_REPLY_TOKEN) ? &resp : nullptr, &req);
        if (token == oz::INVALID_REPLY_TOKEN) {
            continue;
        }

        auto cmd = static_cast<oz::ipc::ProcCmd>(req.data[0]);
        resp = oz::IPCMessage{};

        if (cmd == oz::ipc::ProcCmd::SPAWN_INIT) {
            g_session.reset();
            g_session.in_use = true;

            const char* src_name = reinterpret_cast<const char*>(&req.data[1]);
            std::size_t i = 0;
            for (; i < 23 && src_name[i] != '\0'; ++i) {
                g_session.prog_name[i] = src_name[i];
            }
            g_session.prog_name[i] = '\0';

            if (req.has_cap) {
                g_session.stdin_cap = req.transferred_cap;
            }

            resp.data[0] = 0;
            resp.data[1] = 1; // session_id = 1
            resp.has_cap = false;
        } else if (cmd == oz::ipc::ProcCmd::SPAWN_SET_STDOUT) {
            if (req.has_cap) {
                g_session.stdout_cap = req.transferred_cap;
            }
            resp.data[0] = 0;
            resp.has_cap = false;
        } else if (cmd == oz::ipc::ProcCmd::SPAWN_ADD_ARG) {
            const char* arg_src = reinterpret_cast<const char*>(&req.data[2]);
            char arg_buf[17] = {};
            for (int k = 0; k < 16 && arg_src[k] != '\0'; ++k) {
                arg_buf[k] = arg_src[k];
            }
            g_session.startup_info.add_arg(arg_buf);

            resp.data[0] = 0;
            resp.has_cap = false;
        } else if (cmd == oz::ipc::ProcCmd::SPAWN_ADD_CAP) {
            if (g_session.num_delegated < 8 && req.has_cap) {
                const char* name_src = reinterpret_cast<const char*>(&req.data[2]);
                std::size_t idx = g_session.num_delegated++;
                std::size_t k = 0;
                for (; k < 15 && name_src[k] != '\0'; ++k) {
                    g_session.delegated[idx].name[k] = name_src[k];
                }
                g_session.delegated[idx].name[k] = '\0';
                g_session.delegated[idx].cap = req.transferred_cap;
            }
            resp.data[0] = 0;
            resp.has_cap = false;

        } else if (cmd == oz::ipc::ProcCmd::SPAWN_COMMIT) {
            if (!g_session.in_use) {
                resp.data[0] = 1;
                continue;
            }

            // 1. Open executable via VFS
            int fd = vfs_open(g_session.prog_name);
            if (fd < 0) {
                oz::sys_write("[ProcMan] ERROR: Cannot open executable: ");
                oz::sys_write(g_session.prog_name);
                oz::sys_write("\n\r");
                resp.data[0] = 2; // File not found
                continue;
            }

            std::size_t sz = vfs_get_size(fd);
            if (sz == 0 || sz > 2 * 1024 * 1024) {
                oz::sys_write("[ProcMan] ERROR: Invalid executable size\n\r");
                vfs_close(fd);
                resp.data[0] = 3; // Invalid size
                continue;
            }

            // 2. Request dedicated child UntypedMemory from MemMan
            std::size_t needed = sz + 64 * 1024 + 1024 * 1024;
            std::size_t child_ut_size = 2 * 1024 * 1024;
            while (child_ut_size < needed) {
                child_ut_size <<= 1;
            }

            std::uint32_t child_untyped = memman_alloc_untyped(child_ut_size);
            if (child_untyped == oz::INVALID_CAP_ID) {
                oz::sys_write("[ProcMan] ERROR: MemMan failed to allocate child UntypedMemory\n\r");
                vfs_close(fd);
                resp.data[0] = 4; // Out of memory
                continue;
            }

            // 3. Allocate staging buffer to read ELF
            std::size_t pages = (sz + 4095) / 4096;
            std::uint32_t buf_frame = oz::sys_frame_alloc(g_procman_work_untyped, pages);
            if (buf_frame == oz::INVALID_CAP_ID) {
                oz::sys_write("[ProcMan] ERROR: sys_frame_alloc failed for staging buffer!\n\r");
                vfs_close(fd);
                memman_revoke_untyped(child_untyped);
                resp.data[0] = 5;
                continue;
            }
            int map_res = oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, g_procman_work_untyped, buf_frame, STAGING_ADDR, 0x01, pages * 4096);
            if (map_res != 0) {
                oz::sys_write("[ProcMan] ERROR: sys_vspace_map_with_pagetables failed for staging buffer!\n\r");
                oz::sys_cap_delete(buf_frame);
                vfs_close(fd);
                memman_revoke_untyped(child_untyped);
                resp.data[0] = 5;
                continue;
            }

            char* staging = reinterpret_cast<char*>(STAGING_ADDR);
            std::size_t read_bytes = vfs_read(fd, 0, staging, sz);
            vfs_close(fd);

            if (read_bytes < sz) {
                oz::sys_write("[ProcMan] ERROR: Failed to read complete ELF\n\r");
                for (std::size_t p = 0; p < pages; ++p) {
                    oz::sys_vspace_unmap(oz::CAP_SELF_VSPACE, STAGING_ADDR + p * 4096);
                }
                oz::sys_cap_delete(buf_frame);
                memman_revoke_untyped(child_untyped);
                resp.data[0] = 6;
                continue;
            }

            // 4. Parse Manifest
            oz::ProcessManifest manifest{};
            bool has_manifest = oz::loader::get_elf_manifest(reinterpret_cast<const std::uint8_t*>(staging), sz, manifest);

            std::uint32_t next_slot = 1;

            // STDIN
            std::uint32_t child_stdin_slot = 0;
            if (g_session.stdin_cap != 0) {
                child_stdin_slot = next_slot++;
                g_session.startup_info.add_cap(child_stdin_slot, oz::CapRole::STDIN, "stdin", oz::CapType::STREAM);
            }

            // STDOUT
            std::uint32_t child_stdout_slot = 0;
            std::uint32_t actual_stdout_cap = 0;
            if (g_session.stdout_cap != 0) {
                child_stdout_slot = next_slot++;
                actual_stdout_cap = g_session.stdout_cap;
                g_session.startup_info.add_cap(child_stdout_slot, oz::CapRole::STDOUT, "stdout", oz::CapType::STREAM);
            } else if (has_manifest && manifest.requires_service(oz::ServiceId::CONSOLE)) {
                child_stdout_slot = next_slot++;
                actual_stdout_cap = CAP_CONSOLE_WRITE;
                g_session.startup_info.add_cap(child_stdout_slot, oz::CapRole::STDOUT, "stdout", oz::CapType::STREAM);
            }

            // WM
            std::uint32_t child_wm_slot = 0;
            if (has_manifest && manifest.requires_service(oz::ServiceId::WM)) {
                child_wm_slot = next_slot++;
                g_session.startup_info.add_cap(child_wm_slot, oz::CapRole::WM, "wm", oz::CapType::STREAM);
            }

            // Display
            std::uint32_t child_display_slot = 0;
            if (has_manifest && manifest.requires_service(oz::ServiceId::DISPLAY)) {
                child_display_slot = next_slot++;
                g_session.startup_info.add_cap(child_display_slot, oz::CapRole::DISPLAY, "display", oz::CapType::STREAM);
            }

            // VFS Root
            std::uint32_t child_vfs_slot = 0;
            if (has_manifest && manifest.requires_service(oz::ServiceId::VFS_ROOT)) {
                child_vfs_slot = next_slot++;
                g_session.startup_info.add_cap(child_vfs_slot, oz::CapRole::ROOT_DIR, "/", oz::CapType::DIR);
            }

            // Delegated Caps
            for (std::size_t d = 0; d < g_session.num_delegated; ++d) {
                std::uint32_t slot = next_slot++;
                g_session.startup_info.add_cap(slot, oz::CapRole::CUSTOM, g_session.delegated[d].name, oz::CapType::FILE);
            }

            // 5. Spawn Process
            auto proc = oz::loader::spawn_process(
                child_untyped, oz::CAP_SELF_VSPACE,
                reinterpret_cast<const std::uint8_t*>(staging), sz,
                has_manifest ? &g_session.startup_info : nullptr
            );

            // Clean staging
            for (std::size_t p = 0; p < pages; ++p) {
                oz::sys_vspace_unmap(oz::CAP_SELF_VSPACE, STAGING_ADDR + p * 4096);
            }
            oz::sys_cap_delete(buf_frame);

            if (!proc.success) {
                oz::sys_write("[ProcMan] ERROR: loader::spawn_process failed!\n\r");
                memman_revoke_untyped(child_untyped);
                resp.data[0] = 7; // Spawn failed
                continue;
            }

            // 6. Insert capabilities into child's CSpace
            if (child_stdin_slot != 0 && g_session.stdin_cap != 0) {
                oz::sys_cspace_cap_insert(proc.cspace_cap, child_stdin_slot, g_session.stdin_cap,
                    static_cast<std::uint32_t>(oz::CapRights::READ | oz::CapRights::WRITE | oz::CapRights::GRANT));
            }
            if (child_stdout_slot != 0 && actual_stdout_cap != 0) {
                oz::sys_cspace_cap_insert(proc.cspace_cap, child_stdout_slot, actual_stdout_cap,
                    static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT | oz::CapRights::DUPLICATE));
            }
            if (child_wm_slot != 0) {
                oz::sys_cspace_cap_insert(proc.cspace_cap, child_wm_slot, CAP_WM_SEND,
                    static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT | oz::CapRights::DUPLICATE));
            }
            if (child_display_slot != 0) {
                oz::sys_cspace_cap_insert(proc.cspace_cap, child_display_slot, CAP_DISPLAY_SEND,
                    static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT | oz::CapRights::DUPLICATE));
            }
            if (child_vfs_slot != 0) {
                oz::sys_cspace_cap_insert(proc.cspace_cap, child_vfs_slot, CAP_VFS_SEND,
                    static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT | oz::CapRights::DUPLICATE));
            }

            // If child needs UntypedMemory for mapping (e.g. Surface FrameCap), allocate and insert
            if (has_manifest && manifest.needs_untyped) {
                std::uint32_t app_untyped = memman_alloc_untyped(2 * 1024 * 1024);
                if (app_untyped != oz::INVALID_CAP_ID) {
                    oz::sys_cspace_cap_insert(proc.cspace_cap, oz::CAP_UNTYPED, app_untyped,
                        static_cast<std::uint32_t>(oz::CapRights::ALL));
                }
            }

            std::uint32_t cur_del_slot = next_slot - g_session.num_delegated;
            for (std::size_t d = 0; d < g_session.num_delegated; ++d) {
                oz::sys_cspace_cap_insert(proc.cspace_cap, cur_del_slot++, g_session.delegated[d].cap,
                    static_cast<std::uint32_t>(oz::CapRights::READ | oz::CapRights::WRITE | oz::CapRights::GRANT));
            }

            // 7. Create exit notification
            std::uint32_t exit_notif = oz::sys_notification_create(g_procman_work_untyped);
            if (exit_notif != oz::INVALID_CAP_ID) {
                oz::sys_tcb_set_exit_notification(proc.tcb_cap, exit_notif, 0);
            }

            // 8. Register in ProcessTable
            ProcessEntry* pe = nullptr;
            for (std::size_t i = 0; i < MAX_PROCS; ++i) {
                if (!g_procs[i].in_use) {
                    pe = &g_procs[i];
                    break;
                }
            }
            if (!pe) pe = &g_procs[0]; // fallback overwrite

            std::uint32_t pid = g_next_pid++;
            pe->in_use = true;
            pe->pid = pid;
            pe->tcb_cap = proc.tcb_cap;
            pe->cspace_cap = proc.cspace_cap;
            pe->vspace_cap = proc.vspace_cap;
            pe->untyped_cap = child_untyped;
            pe->exit_notif = exit_notif;

            // 9. Register with Sched daemon if present
            std::uint32_t sched_cap = oz::LocalNamespace::get_cap("sched", CAP_SCHED_SEND);
            if (sched_cap != 0) {
                oz::IPCMessage reg_msg{};
                reg_msg.data[0] = static_cast<uint64_t>(oz::ipc::SchedCmd::REGISTER_THREAD);
                reg_msg.data[1] = 1;
                reg_msg.data[2] = 10000;
                constexpr std::uint32_t S_TCB_SLOT = 160;
                oz::sys_cap_copy(S_TCB_SLOT, proc.tcb_cap, static_cast<std::uint32_t>(oz::CapRights::ALL));
                reg_msg.has_cap = true;
                reg_msg.transferred_cap = S_TCB_SLOT;
                oz::IPCMessage reg_resp{};
                oz::sys_call(sched_cap, &reg_msg, &reg_resp);
            }

            // 10. Resume process
            oz::sys_tcb_resume(proc.tcb_cap);
            oz::sys_write("[ProcMan] Process spawned and running, pid=");
            char pid_buf[4] = {static_cast<char>('0' + (pid % 10)), '\n', '\r', '\0'};
            oz::sys_write(pid_buf);

            g_session.reset();

            // Copy TCB cap to a temporary slot to transfer it to caller (e.g. Init)
            constexpr std::uint32_t PROCMAN_COPY_TCB_SLOT = 150;
            oz::sys_cap_copy(PROCMAN_COPY_TCB_SLOT, proc.tcb_cap, static_cast<std::uint32_t>(oz::CapRights::ALL));

            resp.data[0] = 0; // Success
            resp.data[1] = pid;
            resp.has_cap = true;
            resp.transferred_cap = PROCMAN_COPY_TCB_SLOT;

        } else if (cmd == oz::ipc::ProcCmd::WAIT) {
            std::uint32_t pid = static_cast<std::uint32_t>(req.data[1]);
            ProcessEntry* pe = find_proc(pid);
            if (!pe) {
                resp.data[0] = 1; // PID not found
                resp.data[1] = 0;
                continue;
            }

            int exit_code = 0;
            if (pe->exit_notif != oz::INVALID_CAP_ID) {
                std::uint64_t badge = 0;
                oz::sys_notification_wait(pe->exit_notif, &badge);
                exit_code = static_cast<int>(badge & 0xFFFFFFFFULL);
                oz::sys_cap_delete(pe->exit_notif);
                pe->exit_notif = oz::INVALID_CAP_ID;
            }

            // Cleanup kernel capabilities
            if (pe->tcb_cap != oz::INVALID_CAP_ID) {
                oz::sys_cap_delete(pe->tcb_cap);
                pe->tcb_cap = oz::INVALID_CAP_ID;
            }
            if (pe->cspace_cap != oz::INVALID_CAP_ID) {
                oz::sys_cap_delete(pe->cspace_cap);
                pe->cspace_cap = oz::INVALID_CAP_ID;
            }
            if (pe->vspace_cap != oz::INVALID_CAP_ID) {
                oz::sys_cap_delete(pe->vspace_cap);
                pe->vspace_cap = oz::INVALID_CAP_ID;
            }

            // Reclaim entire child UntypedMemory via MemMan!
            if (pe->untyped_cap != oz::INVALID_CAP_ID) {
                memman_revoke_untyped(pe->untyped_cap);
                pe->untyped_cap = oz::INVALID_CAP_ID;
            }

            pe->in_use = false;

            resp.data[0] = 0; // OK
            resp.data[1] = static_cast<std::uint64_t>(exit_code);
            resp.has_cap = false;

        } else if (cmd == oz::ipc::ProcCmd::CREATE_PIPE) {
            std::uint32_t pipe_w = 0;
            std::uint32_t pipe_r = 0;
            if (oz::sys_channel_create(g_procman_work_untyped, &pipe_w, &pipe_r)) {
                resp.data[0] = 0; // Success
                resp.data[1] = pipe_w;
                resp.has_cap = true;
                resp.transferred_cap = pipe_r;
            } else {
                resp.data[0] = 1; // Failed
                resp.has_cap = false;
            }

        } else {
            resp.data[0] = 0xFF; // Unknown
            resp.has_cap = false;
        }
    }
}
