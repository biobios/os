#include "syscall.hpp"
#include "loader.hpp"
#include "../common/ipc/Protocol.hpp"
#include "../common/ipc/StartupInfo.hpp"
#include "../common/utils/utils.hpp"
#include "../../lib/io.hpp"

// Shell declares its requirements statically
OZ_MANIFEST((oz::ProcessManifest{
    .magic = oz::ProcessManifest::MAGIC,
    .version = 1,
    .needs_console = true,
    .needs_vfs_root = true,
    .needs_display = false,
    .needs_wm = false,
    .needs_untyped = true,
    .default_data_dir = {}
}));

static constexpr std::uint32_t CAP_CONSOLE_WRITE   = 1;
static constexpr std::uint32_t CAP_SHELL_INPUT     = 2;
static constexpr std::uint32_t CAP_VFS_SEND        = 3;
static constexpr std::uint32_t CAP_VFS_RECV        = 4;
static constexpr std::uint32_t CAP_VFS_SHARED_BUF  = 5;

static constexpr std::uint64_t VFS_SHARED_BUF_VIRT = 0x300000000000ULL;
static char* g_shared_buf = nullptr;

static int vfs_open(const char* path) {
    oz::IPCMessage req{};
    req.data[0] = oz::ipc::vfs_make_command(oz::ipc::VfsCmd::OPEN, 1);
    char* dst = reinterpret_cast<char*>(&req.data[1]);
    int i = 0;
    for (; i < 23 && path[i] != '\0'; i++) {
        dst[i] = path[i];
    }
    dst[i] = '\0';

    oz::IPCMessage resp{};
    if (oz::sys_channel_send(CAP_VFS_SEND, &req) && oz::sys_channel_recv(CAP_VFS_RECV, &resp)) {
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
        req.data[0] = oz::ipc::vfs_make_command(oz::ipc::VfsCmd::READ, 1);
        req.data[1] = static_cast<std::uint64_t>(handle);
        req.data[2] = static_cast<std::uint64_t>(offset + total_read);
        req.data[3] = static_cast<std::uint64_t>(chunk);

        oz::IPCMessage resp{};
        if (!oz::sys_channel_send(CAP_VFS_SEND, &req) || !oz::sys_channel_recv(CAP_VFS_RECV, &resp)) {
            break;
        }
        if (resp.data[0] != 0) break;
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

static std::size_t vfs_write(int handle, std::size_t offset, const char* buf, std::size_t size) {
    if (!g_shared_buf || size == 0) return 0;
    std::size_t total_written = 0;
    while (total_written < size) {
        std::size_t chunk = size - total_written;
        if (chunk > 64 * 1024) chunk = 64 * 1024;

        for (std::size_t i = 0; i < chunk; i++) {
            g_shared_buf[i] = buf[total_written + i];
        }

        oz::IPCMessage req{};
        req.data[0] = oz::ipc::vfs_make_command(oz::ipc::VfsCmd::WRITE, 1);
        req.data[1] = static_cast<std::uint64_t>(handle);
        req.data[2] = static_cast<std::uint64_t>(offset + total_written);
        req.data[3] = static_cast<std::uint64_t>(chunk);

        oz::IPCMessage resp{};
        if (!oz::sys_channel_send(CAP_VFS_SEND, &req) || !oz::sys_channel_recv(CAP_VFS_RECV, &resp)) {
            break;
        }
        if (resp.data[0] != 0) break;
        std::size_t bytes_written = static_cast<std::size_t>(resp.data[1]);
        if (bytes_written == 0) break;

        total_written += bytes_written;
        if (bytes_written < chunk) break;
    }
    return total_written;
}

static std::size_t vfs_get_size(int handle) {
    oz::IPCMessage req{};
    req.data[0] = oz::ipc::vfs_make_command(oz::ipc::VfsCmd::GET_SIZE, 1);
    req.data[1] = static_cast<std::uint64_t>(handle);
    oz::IPCMessage resp{};
    if (oz::sys_channel_send(CAP_VFS_SEND, &req) && oz::sys_channel_recv(CAP_VFS_RECV, &resp)) {
        if (resp.data[0] == 0) return static_cast<std::size_t>(resp.data[1]);
    }
    return 0;
}

static void vfs_close(int handle) {
    oz::IPCMessage req{};
    req.data[0] = oz::ipc::vfs_make_command(oz::ipc::VfsCmd::CLOSE, 1);
    req.data[1] = static_cast<std::uint64_t>(handle);
    oz::IPCMessage resp{};
    if (oz::sys_channel_send(CAP_VFS_SEND, &req)) {
        oz::sys_channel_recv(CAP_VFS_RECV, &resp);
    }
}

static bool vfs_list_dir(const char* path) {
    oz::IPCMessage req{};
    req.data[0] = oz::ipc::vfs_make_command(oz::ipc::VfsCmd::LIST_DIR, 1);
    char* dst = reinterpret_cast<char*>(&req.data[1]);
    int i = 0;
    for (; i < 23 && path[i] != '\0'; i++) {
        dst[i] = path[i];
    }
    dst[i] = '\0';

    oz::IPCMessage resp{};
    if (!oz::sys_channel_send(CAP_VFS_SEND, &req) || !oz::sys_channel_recv(CAP_VFS_RECV, &resp)) {
        return false;
    }
    if (resp.data[0] == 0 && g_shared_buf) {
        oz::sys_write(g_shared_buf);
        return true;
    }
    return false;
}

struct VfsOpenAtResult {
    oz::ipc::VfsStatus status{oz::ipc::VfsStatus::NOT_FOUND};
    int handle{-1};
    std::uint32_t transferred_cap{oz::INVALID_CAP_ID};
    std::size_t size{0};
};

static VfsOpenAtResult vfs_openat(const char* rel_path, oz::ipc::VfsOpenFlags flags) {
    VfsOpenAtResult res{};
    oz::IPCMessage req{};
    req.data[0] = oz::ipc::vfs_make_openat_command(1, flags, 0); // Client 1 = Shell

    char* dst = reinterpret_cast<char*>(&req.data[1]);
    int i = 0;
    for (; i < 23 && rel_path[i] != '\0'; i++) {
        dst[i] = rel_path[i];
    }
    dst[i] = '\0';

    if (!oz::sys_channel_send(CAP_VFS_SEND, &req)) return res;

    oz::IPCMessage resp{};
    if (!oz::sys_channel_recv(CAP_VFS_RECV, &resp)) return res;

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

static bool str_equals(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return false;
        a++;
        b++;
    }
    return *a == *b;
}

static bool str_starts_with(const char* str, const char* prefix) {
    while (*prefix) {
        if (*str != *prefix) return false;
        str++;
        prefix++;
    }
    return true;
}

static const char* skip_whitespace(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

struct CommandArgs {
    static constexpr std::size_t MAX_ARGS = 16;
    static constexpr std::size_t MAX_ARG_LEN = 128;
    static constexpr std::size_t MAX_DELEGATED = 16;

    char args[MAX_ARGS][MAX_ARG_LEN]{};
    std::size_t argc{0};

    char redirect_stdin[32]{};

    struct DelegatedFile {
        char path[32]{};
        std::uint32_t cap_slot{0};
        std::uint32_t cap{0};
    };
    DelegatedFile delegated[MAX_DELEGATED]{};
    std::size_t num_delegated{0};

    bool add_delegated(const char* p) {
        if (num_delegated >= MAX_DELEGATED || !p) return false;
        for (std::size_t i = 0; i < num_delegated; ++i) {
            bool same = true;
            for (std::size_t j = 0; delegated[i].path[j] != '\0' || p[j] != '\0'; ++j) {
                if (delegated[i].path[j] != p[j]) { same = false; break; }
            }
            if (same) return true;
        }
        std::size_t i = 0;
        for (; i < sizeof(delegated[num_delegated].path) - 1 && p[i] != '\0'; ++i) {
            delegated[num_delegated].path[i] = p[i];
        }
        delegated[num_delegated].path[i] = '\0';
        delegated[num_delegated].cap = 0;
        num_delegated++;
        return true;
    }
};

static bool parse_command_line(const char* line, CommandArgs& out) {
    out.argc = 0;
    out.num_delegated = 0;
    out.redirect_stdin[0] = '\0';

    const char* p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        // Check for STDIN redirect: < @"path" or < "path" or < path
        if (*p == '<') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            bool is_at_quote = false;
            if (p[0] == '@' && p[1] == '\"') {
                p += 2;
                is_at_quote = true;
            } else if (*p == '\"') {
                p++;
                is_at_quote = true;
            }
            std::size_t rlen = 0;
            while (*p) {
                if (is_at_quote && *p == '\"') {
                    p++;
                    break;
                }
                if (!is_at_quote && (*p == ' ' || *p == '\t' || *p == '\0')) {
                    break;
                }
                if (rlen + 1 < sizeof(out.redirect_stdin)) {
                    out.redirect_stdin[rlen++] = *p;
                }
                p++;
            }
            out.redirect_stdin[rlen] = '\0';
            continue;
        }

        if (out.argc >= CommandArgs::MAX_ARGS) break;

        char* dst = out.args[out.argc];
        std::size_t dlen = 0;
        constexpr std::size_t max_dlen = CommandArgs::MAX_ARG_LEN - 1;
        bool in_regular_quote = false;

        while (*p) {
            // Capability delegation: @"path" (e.g. @"memo.txt" or --file=@"my doc.txt")
            if (!in_regular_quote && p[0] == '@' && p[1] == '\"') {
                p += 2; // skip @"
                char cap_path[32]{};
                std::size_t cplen = 0;
                while (*p && *p != '\"') {
                    if (cplen + 1 < sizeof(cap_path)) {
                        cap_path[cplen++] = *p;
                    }
                    if (dlen < max_dlen) {
                        dst[dlen++] = *p; // Preserves path (including spaces) inside argv
                    }
                    p++;
                }
                cap_path[cplen] = '\0';
                if (*p == '\"') {
                    p++; // skip closing quote
                }
                out.add_delegated(cap_path);
                continue;
            }

            // Standard double quotes: "..."
            if (*p == '\"') {
                in_regular_quote = !in_regular_quote;
                p++;
                continue;
            }

            // Whitespace outside quotes marks token boundary
            if (!in_regular_quote && (*p == ' ' || *p == '\t')) {
                break;
            }

            if (dlen < max_dlen) {
                dst[dlen++] = *p;
            }
            p++;
        }

        dst[dlen] = '\0';
        out.argc++;
    }

    return out.argc > 0;
}

static std::size_t str_len(const char* s) {
    std::size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

struct ProcessHandle {
    std::uint32_t tcb_cap{oz::INVALID_CAP_ID};
    std::uint32_t cspace_cap{oz::INVALID_CAP_ID};
    std::uint32_t vspace_cap{oz::INVALID_CAP_ID};
    std::uint32_t untyped_cap{oz::INVALID_CAP_ID};
    std::uint32_t exit_notif{oz::INVALID_CAP_ID};

    int wait_and_reclaim() {
        int exit_code = 0;
        if (exit_notif != oz::INVALID_CAP_ID) {
            std::uint64_t badge = 0;
            oz::sys_notification_wait(exit_notif, &badge);
            exit_code = static_cast<int>(badge & 0xFFFFFFFFULL);
            oz::sys_cap_delete(exit_notif);
            exit_notif = oz::INVALID_CAP_ID;
        }
        if (untyped_cap != oz::INVALID_CAP_ID) {
            oz::sys_untyped_revoke(untyped_cap);
            oz::sys_cap_delete(untyped_cap);
            untyped_cap = oz::INVALID_CAP_ID;
        }
        if (tcb_cap != oz::INVALID_CAP_ID) {
            oz::sys_cap_delete(tcb_cap);
            tcb_cap = oz::INVALID_CAP_ID;
        }
        if (cspace_cap != oz::INVALID_CAP_ID) {
            oz::sys_cap_delete(cspace_cap);
            cspace_cap = oz::INVALID_CAP_ID;
        }
        if (vspace_cap != oz::INVALID_CAP_ID) {
            oz::sys_cap_delete(vspace_cap);
            vspace_cap = oz::INVALID_CAP_ID;
        }
        return exit_code;
    }
};

static bool execute_spawn(const CommandArgs& cmd_args, std::uint32_t override_stdin_cap = 0, std::uint32_t override_stdout_cap = 0, ProcessHandle* out_handle = nullptr) {
    if (cmd_args.argc == 0) return false;

    std::size_t prog_idx = 0;
    if (str_equals(cmd_args.args[0], "spawn")) {
        if (cmd_args.argc < 2) {
            oz::sys_write("Usage: spawn <binary> [args...]\n\r");
            return false;
        }
        prog_idx = 1;
    }

    const char* prog_name = cmd_args.args[prog_idx];
    int fd = vfs_open(prog_name);
    if (fd < 0) {
        oz::sys_write("Cannot open executable: ");
        oz::sys_write(prog_name);
        oz::sys_write("\n\r");
        return false;
    }

    std::size_t sz = vfs_get_size(fd);
    if (sz == 0 || sz > 2 * 1024 * 1024) {
        oz::sys_write("Invalid executable size\n\r");
        vfs_close(fd);
        return false;
    }

    std::size_t pages = (sz + 4095) / 4096;
    std::uint32_t buf_frame = oz::sys_frame_alloc(oz::CAP_UNTYPED, pages);
    if (buf_frame == static_cast<std::uint32_t>(-1)) {
        oz::sys_write("Failed to allocate memory for executable\n\r");
        vfs_close(fd);
        return false;
    }

    static constexpr std::uint64_t STAGING_ADDR = 0x400000000000ULL;
    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, buf_frame, STAGING_ADDR, 0x01, pages * 4096) != 0) {
        oz::sys_write("Failed to map staging memory\n\r");
        vfs_close(fd);
        return false;
    }

    char* staging = reinterpret_cast<char*>(STAGING_ADDR);
    std::size_t read_bytes = vfs_read(fd, 0, staging, sz);
    vfs_close(fd);

    if (read_bytes < sz) {
        oz::sys_write("Failed to read complete executable\n\r");
        oz::sys_vspace_unmap(oz::CAP_SELF_VSPACE, STAGING_ADDR);
        return false;
    }

    oz::ProcessManifest manifest{};
    bool has_manifest = oz::loader::get_elf_manifest(reinterpret_cast<const std::uint8_t*>(staging), sz, manifest);

    oz::ProcessStartupInfo startup_info{};
    std::uint32_t next_slot = 1;

    // 1. Resolve STDIN capability (Override, Redirect, or Default)
    std::uint32_t child_stdin_cap = 0;
    std::uint32_t child_stdin_slot = 0;
    if (override_stdin_cap != 0) {
        child_stdin_slot = next_slot++;
        child_stdin_cap = override_stdin_cap;
        startup_info.add_cap(child_stdin_slot, oz::CapRole::STDIN, "stdin", oz::CapType::STREAM);
    } else if (cmd_args.redirect_stdin[0] != '\0') {
        auto res = vfs_openat(cmd_args.redirect_stdin, oz::ipc::VfsOpenFlags::FILE);
        if (res.status == oz::ipc::VfsStatus::OK && res.transferred_cap != oz::INVALID_CAP_ID) {
            child_stdin_slot = next_slot++;
            child_stdin_cap = res.transferred_cap;
            startup_info.add_cap(child_stdin_slot, oz::CapRole::STDIN, cmd_args.redirect_stdin, oz::CapType::FILE);
            oz::sys_write("[Shell] STDIN redirected from: ");
            oz::sys_write(cmd_args.redirect_stdin);
            oz::sys_write("\n\r");
        } else {
            oz::sys_write("[Shell] Warning: Failed to open redirect file: ");
            oz::sys_write(cmd_args.redirect_stdin);
            oz::sys_write("\n\r");
        }
    }

    // 2. Resolve STDOUT capability (Override or Console)
    std::uint32_t child_stdout_cap = 0;
    std::uint32_t child_stdout_slot = 0;
    if (override_stdout_cap != 0) {
        child_stdout_slot = next_slot++;
        child_stdout_cap = override_stdout_cap;
        startup_info.add_cap(child_stdout_slot, oz::CapRole::STDOUT, "stdout", oz::CapType::STREAM);
    } else if (has_manifest && manifest.needs_console) {
        child_stdout_slot = next_slot++;
        child_stdout_cap = CAP_CONSOLE_WRITE;
        startup_info.add_cap(child_stdout_slot, oz::CapRole::STDOUT, "stdout", oz::CapType::STREAM);
    }

    // Standard root directory if requested
    if (has_manifest && manifest.needs_vfs_root) {
        startup_info.add_cap(next_slot++, oz::CapRole::ROOT_DIR, "/");
    }

    // 3. Resolve delegated capabilities for @"path" arguments (Powerbox pattern!)
    CommandArgs::DelegatedFile delegated_caps[CommandArgs::MAX_DELEGATED]{};
    std::size_t num_delegated_caps = 0;

    for (std::size_t i = 0; i < cmd_args.num_delegated; ++i) {
        const char* path = cmd_args.delegated[i].path;
        auto res = vfs_openat(path, oz::ipc::VfsOpenFlags::FILE);
        if (res.status == oz::ipc::VfsStatus::OK && res.transferred_cap != oz::INVALID_CAP_ID) {
            std::uint32_t slot = next_slot++;
            startup_info.add_cap(slot, oz::CapRole::CUSTOM, path, oz::CapType::FILE);
            delegated_caps[num_delegated_caps].cap = res.transferred_cap;
            delegated_caps[num_delegated_caps].cap_slot = slot;
            for (std::size_t k = 0; k < sizeof(delegated_caps[num_delegated_caps].path); ++k) {
                delegated_caps[num_delegated_caps].path[k] = path[k];
            }
            num_delegated_caps++;
            oz::sys_write("[Powerbox] Delegated file cap: ");
            oz::sys_write(path);
            oz::sys_write("\n\r");
        } else {
            oz::sys_write("[Powerbox] Warning: Could not open delegated file: ");
            oz::sys_write(path);
            oz::sys_write("\n\r");
        }
    }

    // 4. Add arguments to StartupInfo (argv starting from program name)
    for (std::size_t i = prog_idx; i < cmd_args.argc; ++i) {
        startup_info.add_arg(cmd_args.args[i]);
    }

    // Allocate dedicated child UntypedMemory (at least 2MB, aligned power of two)
    std::size_t needed = sz + 64 * 1024 + 1024 * 1024;
    std::size_t child_ut_size = 2 * 1024 * 1024;
    while (child_ut_size < needed) {
        child_ut_size <<= 1;
    }

    std::uint32_t child_untyped = oz::sys_untyped_retype(
        oz::CAP_UNTYPED,
        static_cast<std::uint32_t>(oz::ObjectType::UntypedMemory),
        child_ut_size
    );
    if (child_untyped == oz::INVALID_CAP_ID) {
        oz::sys_write("Failed to allocate Untyped memory for child process\n\r");
        oz::sys_vspace_unmap(oz::CAP_SELF_VSPACE, STAGING_ADDR);
        return false;
    }

    auto proc = oz::loader::spawn_process(
        child_untyped, oz::CAP_SELF_VSPACE,
        reinterpret_cast<const std::uint8_t*>(staging), sz,
        has_manifest ? &startup_info : nullptr
    );
    oz::sys_vspace_unmap(oz::CAP_SELF_VSPACE, STAGING_ADDR);

    if (proc.success) {
        if (child_stdin_cap != 0) {
            oz::sys_cspace_cap_insert(proc.cspace_cap, child_stdin_slot, child_stdin_cap, static_cast<std::uint32_t>(oz::CapRights::READ | oz::CapRights::GRANT));
        }
        if (child_stdout_cap != 0) {
            oz::sys_cspace_cap_insert(proc.cspace_cap, child_stdout_slot, child_stdout_cap, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
        }
        if (!has_manifest) {
            // Forward console output capability to legacy slot 1
            oz::sys_cspace_cap_insert(proc.cspace_cap, 1, CAP_CONSOLE_WRITE, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
        }
        // Insert delegated file capabilities into child's CSpace
        for (std::size_t i = 0; i < num_delegated_caps; ++i) {
            oz::sys_cspace_cap_insert(
                proc.cspace_cap,
                delegated_caps[i].cap_slot,
                delegated_caps[i].cap,
                static_cast<std::uint32_t>(oz::CapRights::READ | oz::CapRights::WRITE | oz::CapRights::GRANT)
            );
        }
        // Create exit notification for child process
        std::uint32_t exit_notif = oz::sys_notification_create(oz::CAP_UNTYPED);
        if (exit_notif != oz::INVALID_CAP_ID) {
            oz::sys_tcb_set_exit_notification(proc.tcb_cap, exit_notif, 0);
        }

        ProcessHandle handle{
            .tcb_cap = proc.tcb_cap,
            .cspace_cap = proc.cspace_cap,
            .vspace_cap = proc.vspace_cap,
            .untyped_cap = child_untyped,
            .exit_notif = exit_notif,
        };

        oz::sys_tcb_resume(proc.tcb_cap);
        oz::sys_write("Spawned process: ");
        oz::sys_write(prog_name);
        oz::sys_write("\n\r");

        if (out_handle) {
            *out_handle = handle;
        } else {
            handle.wait_and_reclaim();
        }
        return true;
    } else {
        oz::sys_untyped_revoke(child_untyped);
        oz::sys_cap_delete(child_untyped);
        oz::sys_write("Failed to spawn process\n\r");
        return false;
    }
}

static bool execute_pipeline(const char* line) {
    const char* pipe_pos = nullptr;
    for (const char* p = line; *p; ++p) {
        if (*p == '|') {
            pipe_pos = p;
            break;
        }
    }
    if (!pipe_pos) return false;

    char left_cmd[128]{};
    char right_cmd[128]{};

    std::size_t llen = pipe_pos - line;
    if (llen >= sizeof(left_cmd)) llen = sizeof(left_cmd) - 1;
    for (std::size_t i = 0; i < llen; ++i) left_cmd[i] = line[i];
    left_cmd[llen] = '\0';

    std::size_t rlen = 0;
    const char* rp = pipe_pos + 1;
    while (*rp && rlen + 1 < sizeof(right_cmd)) {
        right_cmd[rlen++] = *rp++;
    }
    right_cmd[rlen] = '\0';

    CommandArgs cmd_left{}, cmd_right{};
    if (!parse_command_line(left_cmd, cmd_left) || !parse_command_line(right_cmd, cmd_right)) {
        oz::sys_write("Pipeline syntax error\n\r");
        return true;
    }

    std::uint32_t pipe_w = 0, pipe_r = 0;
    if (!oz::sys_channel_create(oz::CAP_UNTYPED, &pipe_w, &pipe_r)) {
        oz::sys_write("Failed to create pipeline channel\n\r");
        return true;
    }

    oz::sys_write("[Pipeline] Created Channel pipe: write / read\n\r");

    ProcessHandle handle_right{};
    ProcessHandle handle_left{};

    // Spawn right-side command first with pipe_r connected to STDIN (non-blocking)
    execute_spawn(cmd_right, pipe_r, 0, &handle_right);
    oz::sys_cap_delete(pipe_r); // Shell drops its own copy of pipe_r so only right-side owns it

    // If left-side is "echo", write directly to pipe_w and close
    if (str_equals(cmd_left.args[0], "echo")) {
        for (std::size_t i = 1; i < cmd_left.argc; ++i) {
            const char* arg = cmd_left.args[i];
            oz::io::write_cap(pipe_w, arg, str_len(arg));
            if (i + 1 < cmd_left.argc) {
                oz::io::write_cap(pipe_w, " ", 1);
            }
        }
        oz::io::write_cap(pipe_w, "\n\r", 2);
        oz::sys_cap_delete(pipe_w); // Close write end to signal EOF to right-side
    } else {
        // Spawn left-side command with pipe_w connected to STDOUT (non-blocking)
        execute_spawn(cmd_left, 0, pipe_w, &handle_left);
        oz::sys_cap_delete(pipe_w); // Shell drops its own copy of pipe_w so only left-side owns it
    }

    // Wait for pipeline children to exit and reclaim their resources
    handle_left.wait_and_reclaim();
    handle_right.wait_and_reclaim();

    return true;
}

static void execute_command(const char* line) {
    line = skip_whitespace(line);
    if (*line == '\0') return;

    // Check for pipeline first: cmd1 | cmd2
    if (execute_pipeline(line)) {
        return;
    }

    CommandArgs cmd_args{};
    if (!parse_command_line(line, cmd_args)) return;

    const char* cmd0 = cmd_args.args[0];

    if (str_equals(cmd0, "help")) {
        oz::sys_write("Available commands:\n\r");
        oz::sys_write("  help              - Show this help message\n\r");
        oz::sys_write("  clear             - Clear console screen\n\r");
        oz::sys_write("  ls [path]         - List directory contents via VFS\n\r");
        oz::sys_write("  cat <path>        - Display file contents via VFS\n\r");
        oz::sys_write("  spawn <bin> [args]- Load and execute ELF binary with Powerbox delegation\n\r");
        oz::sys_write("  echo <text>       - Echo text back to console\n\r");
        oz::sys_write("  app < @\"file\"     - STDIN redirection with File Capability\n\r");
        oz::sys_write("  app1 | app2       - Inter-process Channel pipeline\n\r");
    } else if (str_equals(cmd0, "clear")) {
        oz::sys_write("\033[2J\033[H");
    } else if (str_equals(cmd0, "echo")) {
        for (std::size_t i = 1; i < cmd_args.argc; ++i) {
            oz::sys_write(cmd_args.args[i]);
            if (i + 1 < cmd_args.argc) oz::sys_write(" ");
        }
        oz::sys_write("\n\r");
    } else if (str_equals(cmd0, "ls")) {
        const char* path = (cmd_args.argc > 1) ? cmd_args.args[1] : "/";
        if (!vfs_list_dir(path)) {
            oz::sys_write("Failed to list directory: ");
            oz::sys_write(path);
            oz::sys_write("\n\r");
        }
    } else if (str_equals(cmd0, "cat")) {
        // Built-in fallback cat or spawn cat
        if (cmd_args.argc > 1) {
            const char* path = cmd_args.args[1];
            int fd = vfs_open(path);
            if (fd >= 0) {
                std::size_t sz = vfs_get_size(fd);
                alignas(8) char read_buf[512];
                std::size_t offset = 0;
                while (offset < sz) {
                    std::size_t to_read = sz - offset;
                    if (to_read > sizeof(read_buf) - 1) to_read = sizeof(read_buf) - 1;
                    std::size_t n = vfs_read(fd, offset, read_buf, to_read);
                    if (n == 0) break;
                    read_buf[n] = '\0';
                    oz::sys_write(read_buf);
                    offset += n;
                }
                vfs_close(fd);
                oz::sys_write("\n\r");
                return;
            }
        }
        execute_spawn(cmd_args);
    } else if (str_equals(cmd0, "spawn")) {
        execute_spawn(cmd_args);
    } else {
        // Try executing cmd0 directly as an ELF binary from VFS
        int fd = vfs_open(cmd0);
        if (fd >= 0) {
            vfs_close(fd);
            execute_spawn(cmd_args);
        } else {
            oz::sys_write("Unknown command: ");
            oz::sys_write(cmd0);
            oz::sys_write("\n\rType 'help' for available commands.\n\r");
        }
    }
}

extern "C" void _start() {
    oz::sys_write("\n\r=== OZ OS Interactive Shell ===\n\r");
    oz::sys_write("Type 'help' for a list of commands.\n\r\n\r");

    // Ensure staging area intermediate page tables exist in shell's VSpace using CAP_UNTYPED
    oz::sys_vspace_ensure_pagetables_range(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, 0x550000000000ULL, 16 * 1024 * 1024);

    // Map VFS shared buffer if available
    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, CAP_VFS_SHARED_BUF, VFS_SHARED_BUF_VIRT, 0x01, 16 * 4096) == 0) {
        g_shared_buf = reinterpret_cast<char*>(VFS_SHARED_BUF_VIRT);
    }

    char line_buf[256] = {};
    std::size_t line_len = 0;

    oz::sys_write("oz> ");

    oz::IPCMessage msg;
    while (true) {
        if (!oz::sys_channel_recv(CAP_SHELL_INPUT, &msg)) continue;

        // Unpack ASCII character from msg.data[0]
        char c = static_cast<char>(msg.data[0] & 0xFF);
        if (c == 0) continue;

        if (c == '\r' || c == '\n') {
            oz::sys_write("\n\r");
            line_buf[line_len] = '\0';
            execute_command(line_buf);
            line_len = 0;
            line_buf[0] = '\0';
            oz::sys_write("oz> ");
        } else if (c == '\b' || c == 0x7F) {
            if (line_len > 0) {
                line_len--;
                line_buf[line_len] = '\0';
                oz::sys_write("\b \b");
            }
        } else if (c >= 32 && c <= 126) {
            if (line_len + 1 < sizeof(line_buf)) {
                line_buf[line_len++] = c;
                line_buf[line_len] = '\0';
                char echo[2] = {c, '\0'};
                oz::sys_write(echo);
            }
        }
    }
}
