#include "syscall.hpp"
#include "loader.hpp"
#include "../common/ipc/Protocol.hpp"
#include "../common/utils/utils.hpp"

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

static void execute_command(const char* line) {
    line = skip_whitespace(line);
    if (*line == '\0') return;

    if (str_equals(line, "help")) {
        oz::sys_write("Available commands:\n\r");
        oz::sys_write("  help         - Show this help message\n\r");
        oz::sys_write("  clear        - Clear console screen\n\r");
        oz::sys_write("  ls [path]    - List directory contents via VFS\n\r");
        oz::sys_write("  cat <path>   - Display file contents via VFS\n\r");
        oz::sys_write("  spawn <path> - Load and execute ELF binary from VFS\n\r");
        oz::sys_write("  echo <text>  - Echo text back to console\n\r");
    } else if (str_equals(line, "clear")) {
        oz::sys_write("\033[2J\033[H");
    } else if (str_starts_with(line, "echo ")) {
        oz::sys_write(line + 5);
        oz::sys_write("\n\r");
    } else if (str_equals(line, "ls") || str_starts_with(line, "ls ")) {
        const char* path = line + 2;
        path = skip_whitespace(path);
        if (*path == '\0') path = "/";
        if (!vfs_list_dir(path)) {
            oz::sys_write("Failed to list directory: ");
            oz::sys_write(path);
            oz::sys_write("\n\r");
        }
    } else if (str_starts_with(line, "cat ")) {
        const char* path = skip_whitespace(line + 4);
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
    } else if (str_starts_with(line, "spawn ")) {
        const char* path = skip_whitespace(line + 6);
        int fd = vfs_open(path);
        if (fd < 0) {
            oz::sys_write("Cannot open executable: ");
            oz::sys_write(path);
            oz::sys_write("\n\r");
            return;
        }
        std::size_t sz = vfs_get_size(fd);
        if (sz == 0 || sz > 2 * 1024 * 1024) {
            oz::sys_write("Invalid executable size\n\r");
            vfs_close(fd);
            return;
        }

        // Allocate a buffer using Frame
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

        auto proc = oz::loader::spawn_process(oz::CAP_UNTYPED, oz::CAP_SELF_VSPACE, reinterpret_cast<const std::uint8_t*>(staging), sz);
        oz::sys_vspace_unmap(oz::CAP_SELF_VSPACE, STAGING_ADDR);

        if (proc.success) {
            // Forward console output capability
            oz::sys_cspace_cap_insert(proc.cspace_cap, 1, CAP_CONSOLE_WRITE, static_cast<std::uint32_t>(oz::CapRights::WRITE | oz::CapRights::GRANT));
            oz::sys_tcb_resume(proc.tcb_cap);
            oz::sys_write("Spawned process: ");
            oz::sys_write(path);
            oz::sys_write("\n\r");
        } else {
            oz::sys_write("Failed to spawn process\n\r");
        }
    } else {
        oz::sys_write("Unknown command: ");
        oz::sys_write(line);
        oz::sys_write("\n\rType 'help' for available commands.\n\r");
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
