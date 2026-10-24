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
    .needs_console = true,
    .needs_vfs_root = true,
    .needs_display = false,
    .needs_wm = false,
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

extern "C" void _start() {
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
            } else if (has_manifest && manifest.needs_console) {
                child_stdout_slot = next_slot++;
                actual_stdout_cap = CAP_CONSOLE_WRITE;
                g_session.startup_info.add_cap(child_stdout_slot, oz::CapRole::STDOUT, "stdout", oz::CapType::STREAM);
            }

            // WM
            std::uint32_t child_wm_slot = 0;
            if (has_manifest && manifest.needs_wm) {
                child_wm_slot = next_slot++;
                g_session.startup_info.add_cap(child_wm_slot, oz::CapRole::WM, "wm", oz::CapType::STREAM);
            }

            // Display
            std::uint32_t child_display_slot = 0;
            if (has_manifest && manifest.needs_display) {
                child_display_slot = next_slot++;
                g_session.startup_info.add_cap(child_display_slot, oz::CapRole::DISPLAY, "display", oz::CapType::STREAM);
            }

            // VFS Root
            std::uint32_t child_vfs_slot = 0;
            if (has_manifest && manifest.needs_vfs_root) {
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

            // 9. Resume process
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
