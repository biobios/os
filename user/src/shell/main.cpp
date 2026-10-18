#include "syscall.hpp"
#include "loader.hpp"
#include "../common/ipc/Protocol.hpp"
#include "../common/ipc/StartupInfo.hpp"
#include "../common/utils/utils.hpp"

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

    const char* p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

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

static void execute_spawn(const CommandArgs& cmd_args) {
    if (cmd_args.argc == 0) return;

    std::size_t prog_idx = 0;
    if (str_equals(cmd_args.args[0], "spawn")) {
        if (cmd_args.argc < 2) {
            oz::sys_write("Usage: spawn <binary> [args...]\n\r");
            return;
        }
        prog_idx = 1;
    }

    const char* prog_name = cmd_args.args[prog_idx];
    int fd = vfs_open(prog_name);
    if (fd < 0) {
        oz::sys_write("Cannot open executable: ");
        oz::sys_write(prog_name);
        oz::sys_write("\n\r");
        return;
    }

    std::size_t sz = vfs_get_size(fd);
    if (sz == 0 || sz > 2 * 1024 * 1024) {
        oz::sys_write("Invalid executable size\n\r");
        vfs_close(fd);
        return;
    }

    std::size_t pages = (sz + 4095) / 4096;
    std::uint32_t buf_frame = oz::sys_frame_alloc(oz::CAP_UNTYPED, pages);
    if (buf_frame == static_cast<std::uint32_t>(-1)) {
        oz::sys_write("Failed to allocate memory for executable\n\r");
        vfs_close(fd);
        return;
    }

    static constexpr std::uint64_t STAGING_ADDR = 0x400000000000ULL;
    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, buf_frame, STAGING_ADDR, 0x01, pages * 4096) != 0) {
        oz::sys_write("Failed to map staging memory\n\r");
        vfs_close(fd);
        return;
    }

    char* staging = reinterpret_cast<char*>(STAGING_ADDR);
    std::size_t read_bytes = vfs_read(fd, 0, staging, sz);
    vfs_close(fd);

    if (read_bytes < sz) {
        oz::sys_write("Failed to read complete executable\n\r");
        oz::sys_vspace_unmap(oz::CAP_SELF_VSPACE, STAGING_ADDR);
        return;
    }

    oz::ProcessManifest manifest{};
    bool has_manifest = oz::loader::get_elf_manifest(reinterpret_cast<const std::uint8_t*>(staging), sz, manifest);

    oz::ProcessStartupInfo startup_info{};
    std::uint32_t next_slot = 1;

    // 1. Standard capabilities from Manifest
    if (has_manifest) {
        if (manifest.needs_console) {
            startup_info.add_cap(next_slot++, oz::CapRole::STDOUT, "stdout");
        }
        if (manifest.needs_vfs_root) {
            startup_info.add_cap(next_slot++, oz::CapRole::ROOT_DIR, "/");
        }
    }

    // 2. Resolve delegated capabilities for @"path" arguments (Powerbox pattern!)
    CommandArgs::DelegatedFile delegated_caps[CommandArgs::MAX_DELEGATED]{};
    std::size_t num_delegated_caps = 0;

    for (std::size_t i = 0; i < cmd_args.num_delegated; ++i) {
        const char* path = cmd_args.delegated[i].path;
        auto res = vfs_openat(path, oz::ipc::VfsOpenFlags::FILE);
        if (res.status == oz::ipc::VfsStatus::OK && res.transferred_cap != oz::INVALID_CAP_ID) {
            std::uint32_t slot = next_slot++;
            startup_info.add_cap(slot, oz::CapRole::CUSTOM, path);
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

    // 3. Add arguments to StartupInfo (argv starting from program name)
    for (std::size_t i = prog_idx; i < cmd_args.argc; ++i) {
        startup_info.add_arg(cmd_args.args[i]);
    }

    auto proc = oz::loader::spawn_process(
        oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE,
        reinterpret_cast<const std::uint8_t*>(staging), sz,
        has_manifest ? &startup_info : nullptr
    );
    oz::sys_vspace_unmap(oz::CAP_SELF_VSPACE, STAGING_ADDR);

    if (proc.success) {
        if (has_manifest) {
            for (std::size_t i = 0; i < startup_info.num_caps; ++i) {
                if (startup_info.caps[i].role == oz::CapRole::STDOUT) {
                    oz::sys_cspace_cap_insert(proc.cspace_cap, startup_info.caps[i].slot, CAP_CONSOLE_WRITE, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
                }
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
        } else {
            // Forward console output capability to legacy slot 1
            oz::sys_cspace_cap_insert(proc.cspace_cap, 1, CAP_CONSOLE_WRITE, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
        }
        oz::sys_tcb_resume(proc.tcb_cap);
        oz::sys_write("Spawned process: ");
        oz::sys_write(prog_name);
        oz::sys_write("\n\r");
    } else {
        oz::sys_write("Failed to spawn process\n\r");
    }
}

static void execute_command(const char* line) {
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
        if (cmd_args.argc < 2) {
            oz::sys_write("Usage: cat <path>\n\r");
            return;
        }
        const char* path = cmd_args.args[1];
        int fd = vfs_open(path);
        if (fd < 0) {
            oz::sys_write("Cannot open file: ");
            oz::sys_write(path);
            oz::sys_write("\n\r");
            return;
        }
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
