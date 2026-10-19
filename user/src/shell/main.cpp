#include "syscall.hpp"
#include "../common/ipc/Protocol.hpp"
#include "../common/ipc/StartupInfo.hpp"
#include "../common/utils/utils.hpp"
#include "../display/Font.hpp"
#include "../../lib/io.hpp"

struct Pixel {
    std::uint8_t b{0};
    std::uint8_t g{0};
    std::uint8_t r{0};
    std::uint8_t a{0};
};

// Shell declares its requirements statically (User Process with GUI Terminal capability)
OZ_MANIFEST((oz::ProcessManifest{
    .magic = oz::ProcessManifest::MAGIC,
    .version = 1,
    .needs_console = true,
    .needs_vfs_root = true,
    .needs_display = true,
    .needs_wm = true,
    .needs_untyped = true,
    .default_data_dir = {}
}));

static constexpr std::uint32_t CAP_CONSOLE_WRITE   = 1;
static constexpr std::uint32_t CAP_SHELL_INPUT     = 2;
static constexpr std::uint32_t CAP_VFS_SEND        = 3;
static constexpr std::uint32_t CAP_VFS_RECV        = 4;
static constexpr std::uint32_t CAP_VFS_SHARED_BUF  = 5;
static constexpr std::uint32_t CAP_PROCMAN_SEND    = 6;
static constexpr std::uint32_t CAP_PROCMAN_RECV    = 7;

static constexpr std::uint64_t VFS_SHARED_BUF_VIRT = 0x300000000000ULL;
static char* g_shared_buf = nullptr;

// GUI Terminal state
static bool g_gui_mode = false;
static std::uint32_t g_surf_id = 0;
static std::uint32_t g_win_id = 0;
static std::uint32_t g_cap_display = 0;
static std::uint32_t g_cap_wm = 0;
static std::uint32_t g_input_recv = 0;
static Pixel* g_win_buffer = nullptr;
static std::uint32_t g_cursor_col = 0;
static std::uint32_t g_cursor_row = 0;

static constexpr Pixel BG_COLOR{0x2b, 0x1f, 0x18, 0};      // Dark charcoal/slate (BGR format)
static constexpr Pixel FG_TEXT{0xee, 0xee, 0xee, 0};       // Off-white
static constexpr Pixel FG_PROMPT{0xff, 0xff, 0x00, 0};     // Cyan (b=0xff, g=0xff, r=0x00)

static void term_draw_char(std::uint32_t col, std::uint32_t row, char c, Pixel fg, Pixel bg) {
    if (!g_win_buffer || col >= 80 || row >= 24) return;
    if (c < 32 || c > 126) return;

    std::uint32_t px = col * FONT_WIDTH;
    std::uint32_t py = row * FONT_HEIGHT;

    for (int r = 0; r < FONT_HEIGHT; ++r) {
        for (int c_idx = 0; c_idx < FONT_WIDTH; ++c_idx) {
            Pixel p = font_bitmap[c - 32][r][c_idx] ? fg : bg;
            g_win_buffer[(py + r) * 640 + (px + c_idx)] = p;
        }
    }
}

static void term_clear_cell(std::uint32_t col, std::uint32_t row, Pixel bg) {
    if (!g_win_buffer || col >= 80 || row >= 24) return;
    std::uint32_t px = col * FONT_WIDTH;
    std::uint32_t py = row * FONT_HEIGHT;
    for (int r = 0; r < FONT_HEIGHT; ++r) {
        for (int c = 0; c < FONT_WIDTH; ++c) {
            g_win_buffer[(py + r) * 640 + (px + c)] = bg;
        }
    }
}

static void term_scroll() {
    if (!g_win_buffer) return;
    std::size_t line_pixels = 640 * FONT_HEIGHT;
    std::size_t total_pixels = 640 * 384;
    __builtin_memcpy(g_win_buffer, g_win_buffer + line_pixels, (total_pixels - line_pixels) * sizeof(Pixel));

    Pixel bg = BG_COLOR;
    std::uint32_t bg_val = *reinterpret_cast<const std::uint32_t*>(&bg);
    std::uint64_t bg_pair = (static_cast<std::uint64_t>(bg_val) << 32) | bg_val;
    Pixel* clear_start = g_win_buffer + (total_pixels - line_pixels);
    for (std::size_t i = 0; i < line_pixels; i += 2) {
        *reinterpret_cast<std::uint64_t*>(&clear_start[i]) = bg_pair;
    }
}

static void term_clear() {
    if (!g_win_buffer) return;
    Pixel bg = BG_COLOR;
    std::uint32_t bg_val = *reinterpret_cast<const std::uint32_t*>(&bg);
    std::uint64_t bg_pair = (static_cast<std::uint64_t>(bg_val) << 32) | bg_val;
    for (std::size_t i = 0; i < 640 * 384; i += 2) {
        *reinterpret_cast<std::uint64_t*>(&g_win_buffer[i]) = bg_pair;
    }
    g_cursor_col = 0;
    g_cursor_row = 0;
}

static void term_commit(std::uint32_t dx = 0, std::uint32_t dy = 0, std::uint32_t dw = 0, std::uint32_t dh = 0) {
    if (!g_gui_mode || g_cap_display == 0) return;
    oz::IPCMessage commit_msg{};
    commit_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::DisplayCmd::COMMIT);
    commit_msg.data[1] = g_surf_id;
    commit_msg.data[2] = (static_cast<std::uint64_t>(dx) << 32) | dy;
    commit_msg.data[3] = (static_cast<std::uint64_t>(dw) << 32) | dh;
    oz::sys_channel_send(g_cap_display, &commit_msg);
}

static void term_put_char(char c) {
    if (c == '\n') {
        g_cursor_col = 0;
        g_cursor_row++;
        if (g_cursor_row >= 24) {
            term_scroll();
            g_cursor_row = 23;
        }
        return;
    }
    if (c == '\r') {
        g_cursor_col = 0;
        return;
    }
    if (c == '\b' || c == 0x7F) {
        if (g_cursor_col > 0) {
            g_cursor_col--;
            term_clear_cell(g_cursor_col, g_cursor_row, BG_COLOR);
        }
        return;
    }
    if (c < 32 || c > 126) return;

    if (g_cursor_col >= 80) {
        g_cursor_col = 0;
        g_cursor_row++;
        if (g_cursor_row >= 24) {
            term_scroll();
            g_cursor_row = 23;
        }
    }

    term_draw_char(g_cursor_col, g_cursor_row, c, FG_TEXT, BG_COLOR);
    g_cursor_col++;
}

static void shell_write(const char* str) {
    if (g_gui_mode) {
        bool scrolled = false;
        std::uint32_t min_row = g_cursor_row;
        std::uint32_t max_row = g_cursor_row;

        while (*str) {
            if (str[0] == '\033' && str[1] == '[' && str[2] == '2' && str[3] == 'J') {
                term_clear();
                scrolled = true;
                str += 4;
                continue;
            }
            std::uint32_t prev_row = g_cursor_row;
            term_put_char(*str);
            if (g_cursor_row < prev_row) {
                scrolled = true;
            }
            if (g_cursor_row < min_row) min_row = g_cursor_row;
            if (g_cursor_row > max_row) max_row = g_cursor_row;
            str++;
        }

        if (scrolled) {
            term_commit(); // Full surface commit
        } else {
            // Commit only the affected lines (640 x N rows)
            std::uint32_t dy = min_row * FONT_HEIGHT;
            std::uint32_t dh = (max_row - min_row + 1) * FONT_HEIGHT;
            term_commit(0, dy, 640, dh);
        }
    } else {
        oz::sys_write(str);
    }
}

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
        shell_write(g_shared_buf);
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

// ===== ProcMan IPC Client Helpers =====
static bool procman_spawn_init(const char* prog_name, std::uint32_t stdin_cap, std::uint32_t* out_sess) {
    oz::IPCMessage req{};
    req.data[0] = static_cast<std::uint64_t>(oz::ipc::ProcCmd::SPAWN_INIT);
    char* dst = reinterpret_cast<char*>(&req.data[1]);
    for (int i = 0; i < 23 && prog_name[i] != '\0'; ++i) dst[i] = prog_name[i];
    if (stdin_cap != 0) {
        req.has_cap = true;
        req.transferred_cap = stdin_cap;
    }
    if (!oz::sys_channel_send(CAP_PROCMAN_SEND, &req)) return false;
    oz::IPCMessage resp{};
    if (!oz::sys_channel_recv(CAP_PROCMAN_RECV, &resp) || resp.data[0] != 0) return false;
    if (out_sess) *out_sess = static_cast<std::uint32_t>(resp.data[1]);
    return true;
}

static bool procman_spawn_set_stdout(std::uint32_t stdout_cap) {
    oz::IPCMessage req{};
    req.data[0] = static_cast<std::uint64_t>(oz::ipc::ProcCmd::SPAWN_SET_STDOUT);
    req.has_cap = true;
    req.transferred_cap = stdout_cap;
    if (!oz::sys_channel_send(CAP_PROCMAN_SEND, &req)) return false;
    oz::IPCMessage resp{};
    return oz::sys_channel_recv(CAP_PROCMAN_RECV, &resp) && resp.data[0] == 0;
}

static bool procman_spawn_add_arg(const char* arg) {
    oz::IPCMessage req{};
    req.data[0] = static_cast<std::uint64_t>(oz::ipc::ProcCmd::SPAWN_ADD_ARG);
    char* dst = reinterpret_cast<char*>(&req.data[2]);
    for (int i = 0; i < 15 && arg[i] != '\0'; ++i) dst[i] = arg[i];
    if (!oz::sys_channel_send(CAP_PROCMAN_SEND, &req)) return false;
    oz::IPCMessage resp{};
    return oz::sys_channel_recv(CAP_PROCMAN_RECV, &resp) && resp.data[0] == 0;
}

static bool procman_spawn_add_cap(const char* name, std::uint32_t cap) {
    oz::IPCMessage req{};
    req.data[0] = static_cast<std::uint64_t>(oz::ipc::ProcCmd::SPAWN_ADD_CAP);
    char* dst = reinterpret_cast<char*>(&req.data[2]);
    for (int i = 0; i < 15 && name[i] != '\0'; ++i) dst[i] = name[i];
    req.has_cap = true;
    req.transferred_cap = cap;
    if (!oz::sys_channel_send(CAP_PROCMAN_SEND, &req)) return false;
    oz::IPCMessage resp{};
    return oz::sys_channel_recv(CAP_PROCMAN_RECV, &resp) && resp.data[0] == 0;
}

static std::uint32_t procman_spawn_commit() {
    oz::IPCMessage req{};
    req.data[0] = static_cast<std::uint64_t>(oz::ipc::ProcCmd::SPAWN_COMMIT);
    if (!oz::sys_channel_send(CAP_PROCMAN_SEND, &req)) return 0;
    oz::IPCMessage resp{};
    if (!oz::sys_channel_recv(CAP_PROCMAN_RECV, &resp) || resp.data[0] != 0) return 0;
    return static_cast<std::uint32_t>(resp.data[1]); // pid
}

static int procman_wait(std::uint32_t pid) {
    if (pid == 0) return -1;
    oz::IPCMessage req{};
    req.data[0] = static_cast<std::uint64_t>(oz::ipc::ProcCmd::WAIT);
    req.data[1] = pid;
    if (!oz::sys_channel_send(CAP_PROCMAN_SEND, &req)) return -1;
    oz::IPCMessage resp{};
    if (!oz::sys_channel_recv(CAP_PROCMAN_RECV, &resp) || resp.data[0] != 0) return -1;
    return static_cast<int>(resp.data[1]); // exit_code
}

static bool procman_create_pipe(std::uint32_t* out_w, std::uint32_t* out_r) {
    oz::IPCMessage req{};
    req.data[0] = static_cast<std::uint64_t>(oz::ipc::ProcCmd::CREATE_PIPE);
    if (!oz::sys_channel_send(CAP_PROCMAN_SEND, &req)) return false;
    oz::IPCMessage resp{};
    if (!oz::sys_channel_recv(CAP_PROCMAN_RECV, &resp) || resp.data[0] != 0 || !resp.has_cap) return false;
    *out_w = static_cast<std::uint32_t>(resp.data[1]);
    *out_r = resp.transferred_cap;
    return true;
}


static bool execute_spawn(const CommandArgs& cmd_args, std::uint32_t override_stdin_cap = 0, std::uint32_t override_stdout_cap = 0, std::uint32_t* out_pid = nullptr) {
    if (cmd_args.argc == 0) return false;

    std::size_t prog_idx = 0;
    if (str_equals(cmd_args.args[0], "spawn")) {
        if (cmd_args.argc < 2) {
            shell_write("Usage: spawn <binary> [args...]\n\r");
            return false;
        }
        prog_idx = 1;
    }

    const char* prog_name = cmd_args.args[prog_idx];

    // 1. Resolve STDIN capability (Override, Redirect, or Default)
    std::uint32_t child_stdin_cap = 0;
    if (override_stdin_cap != 0) {
        child_stdin_cap = override_stdin_cap;
    } else if (cmd_args.redirect_stdin[0] != '\0') {
        auto res = vfs_openat(cmd_args.redirect_stdin, oz::ipc::VfsOpenFlags::FILE);
        if (res.status == oz::ipc::VfsStatus::OK && res.transferred_cap != oz::INVALID_CAP_ID) {
            child_stdin_cap = res.transferred_cap;
            shell_write("[Shell] STDIN redirected from: ");
            shell_write(cmd_args.redirect_stdin);
            shell_write("\n\r");
        } else {
            shell_write("[Shell] Warning: Failed to open redirect file: ");
            shell_write(cmd_args.redirect_stdin);
            shell_write("\n\r");
        }
    }

    std::uint32_t sess = 0;
    if (!procman_spawn_init(prog_name, child_stdin_cap, &sess)) {
        shell_write("Failed to initialize spawn request\n\r");
        return false;
    }

    // 2. Set STDOUT override if provided
    if (override_stdout_cap != 0) {
        procman_spawn_set_stdout(override_stdout_cap);
    }

    // 3. Resolve delegated capabilities for @"path" arguments (Powerbox pattern)
    for (std::size_t i = 0; i < cmd_args.num_delegated; ++i) {
        const char* path = cmd_args.delegated[i].path;
        auto res = vfs_openat(path, oz::ipc::VfsOpenFlags::FILE);
        if (res.status == oz::ipc::VfsStatus::OK && res.transferred_cap != oz::INVALID_CAP_ID) {
            procman_spawn_add_cap(path, res.transferred_cap);
            shell_write("[Powerbox] Delegated file cap: ");
            shell_write(path);
            shell_write("\n\r");
        } else {
            shell_write("[Powerbox] Warning: Could not open delegated file: ");
            shell_write(path);
            shell_write("\n\r");
        }
    }

    // 4. Add arguments (argv starting from program name)
    for (std::size_t i = prog_idx; i < cmd_args.argc; ++i) {
        procman_spawn_add_arg(cmd_args.args[i]);
    }

    // 5. Commit spawn
    std::uint32_t pid = procman_spawn_commit();
    if (pid == 0) {
        shell_write("ProcMan failed to spawn process: ");
        shell_write(prog_name);
        shell_write("\n\r");
        return false;
    }

    shell_write("Spawned process (via ProcMan): ");
    shell_write(prog_name);
    shell_write("\n\r");

    if (out_pid) {
        *out_pid = pid;
    } else {
        procman_wait(pid);
    }
    return true;
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
        shell_write("Pipeline syntax error\n\r");
        return true;
    }

    std::uint32_t pipe_w = 0, pipe_r = 0;
    if (!procman_create_pipe(&pipe_w, &pipe_r)) {
        shell_write("Failed to create pipeline channel via ProcMan\n\r");
        return true;
    }

    shell_write("[Pipeline] Created Channel pipe via ProcMan: write / read\n\r");

    std::uint32_t pid_right = 0;
    std::uint32_t pid_left = 0;

    // Spawn right-side command first with pipe_r connected to STDIN (non-blocking)
    execute_spawn(cmd_right, pipe_r, 0, &pid_right);
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
        execute_spawn(cmd_left, 0, pipe_w, &pid_left);
        oz::sys_cap_delete(pipe_w); // Shell drops its own copy of pipe_w so only left-side owns it
    }

    // Wait for pipeline children to exit and reclaim their resources
    if (pid_left != 0) procman_wait(pid_left);
    if (pid_right != 0) procman_wait(pid_right);

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
        shell_write("Available commands:\n\r");
        shell_write("  help              - Show this help message\n\r");
        shell_write("  clear             - Clear console screen\n\r");
        shell_write("  ls [path]         - List directory contents via VFS\n\r");
        shell_write("  cat <path>        - Display file contents via VFS\n\r");
        shell_write("  spawn <bin> [args]- Load and execute ELF binary with Powerbox delegation\n\r");
        shell_write("  echo <text>       - Echo text back to console\n\r");
        shell_write("  app < @\"file\"     - STDIN redirection with File Capability\n\r");
        shell_write("  app1 | app2       - Inter-process Channel pipeline\n\r");
    } else if (str_equals(cmd0, "clear")) {
        shell_write("\033[2J\033[H");
    } else if (str_equals(cmd0, "echo")) {
        for (std::size_t i = 1; i < cmd_args.argc; ++i) {
            shell_write(cmd_args.args[i]);
            if (i + 1 < cmd_args.argc) shell_write(" ");
        }
        shell_write("\n\r");
    } else if (str_equals(cmd0, "ls")) {
        const char* path = (cmd_args.argc > 1) ? cmd_args.args[1] : "/";
        if (!vfs_list_dir(path)) {
            shell_write("Failed to list directory: ");
            shell_write(path);
            shell_write("\n\r");
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
                    shell_write(read_buf);
                    offset += n;
                }
                vfs_close(fd);
                shell_write("\n\r");
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
            shell_write("Unknown command: ");
            shell_write(cmd0);
            shell_write("\n\rType 'help' for available commands.\n\r");
        }
    }
}

static bool init_gui_terminal(std::uint32_t cap_wm, std::uint32_t cap_display) {
    if (cap_wm == 0 || cap_display == 0) return false;

    // 1. Request window from WM (640x384)
    oz::IPCMessage req{};
    req.data[0] = static_cast<std::uint64_t>(oz::ipc::WMCmd::CREATE_WINDOW);
    req.data[1] = 640;
    req.data[2] = 384;

    std::uint32_t reply_send = 0, reply_recv = 0;
    if (!oz::sys_channel_create(oz::CAP_UNTYPED, &reply_send, &reply_recv)) {
        oz::sys_debug_print("[Shell] init_gui_terminal: failed to create reply channel!\n\r");
        return false;
    }
    req.transferred_cap = reply_send;
    req.has_cap = true;

    oz::sys_debug_print("[Shell] init_gui_terminal: Sending CREATE_WINDOW...\n\r");
    if (!oz::sys_channel_send(cap_wm, &req)) {
        oz::sys_debug_print("[Shell] init_gui_terminal: sys_channel_send failed!\n\r");
        oz::sys_cap_delete(reply_recv);
        return false;
    }

    oz::IPCMessage resp{};
    if (!oz::sys_channel_recv(reply_recv, &resp)) {
        oz::sys_debug_print("[Shell] init_gui_terminal: sys_channel_recv failed!\n\r");
        oz::sys_cap_delete(reply_recv);
        return false;
    }
    oz::sys_cap_delete(reply_recv);

    if (resp.data[0] != 0 || !resp.has_cap) {
        oz::sys_debug_print("[Shell] init_gui_terminal: CREATE_WINDOW returned error or no cap!\n\r");
        return false;
    }

    g_win_id = static_cast<std::uint32_t>(resp.data[1]);
    g_surf_id = static_cast<std::uint32_t>(resp.data[2]);
    std::uint32_t surf_frame = resp.transferred_cap;

    oz::sys_debug_print("[Shell] init_gui_terminal: Window created! Mapping surface frame...\n\r");

    // 2. Map surface frame into virtual memory
    static constexpr std::uint64_t SHELL_SURF_VADDR = 0x600000000000ULL;
    std::size_t surf_size = (640 * 384 * sizeof(Pixel) + 4095) & ~4095ULL;
    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, surf_frame, SHELL_SURF_VADDR, 0x01, surf_size) != 0) {
        oz::sys_debug_print("[Shell] init_gui_terminal: sys_vspace_map_with_pagetables failed!\n\r");
        return false;
    }
    g_win_buffer = reinterpret_cast<Pixel*>(SHELL_SURF_VADDR);

    // 3. Create input channel and bind to WM
    std::uint32_t input_send = 0;
    if (!oz::sys_channel_create(oz::CAP_UNTYPED, &input_send, &g_input_recv)) {
        oz::sys_debug_print("[Shell] init_gui_terminal: failed to create input channel!\n\r");
        return false;
    }

    oz::IPCMessage bind_msg{};
    bind_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::WMCmd::BIND_INPUT_CHANNEL);
    bind_msg.data[1] = g_win_id;
    bind_msg.transferred_cap = input_send;
    bind_msg.has_cap = true;
    oz::sys_channel_send(cap_wm, &bind_msg);

    // 4. Set Window Title
    oz::IPCMessage title_msg{};
    title_msg.data[0] = static_cast<std::uint64_t>(oz::ipc::WMCmd::SET_TITLE);
    title_msg.data[1] = g_win_id;
    char title_chars[16] = "Terminal";
    for (int k = 0; k < 2; ++k) {
        title_msg.data[k + 2] = *reinterpret_cast<std::uint64_t*>(&title_chars[k * 8]);
    }
    oz::sys_channel_send(cap_wm, &title_msg);

    g_cap_wm = cap_wm;
    g_cap_display = cap_display;
    g_gui_mode = true;

    oz::sys_debug_print("[Shell] calling term_clear...\n\r");
    term_clear();
    oz::sys_debug_print("[Shell] term_clear done, calling term_commit...\n\r");
    term_commit();
    oz::sys_debug_print("[Shell] init_gui_terminal: Terminal GUI successfully initialized!\n\r");
    return true;
}

OZ_PROCESS_ENTRY() {
    oz::sys_debug_print("[Shell] Process started in OZ_PROCESS_ENTRY!\n\r");
    oz::LocalNamespace::init(info);

    // 1. Resolve Capabilities from Local Namespace
    std::uint32_t cap_wm = oz::LocalNamespace::get_cap(oz::CapRole::WM);
    std::uint32_t cap_display = oz::LocalNamespace::get_cap(oz::CapRole::DISPLAY);
    oz::sys_debug_print("[Shell] Resolved caps: cap_wm=");
    char num_buf[4] = {static_cast<char>('0' + (cap_wm % 10)), ' ', static_cast<char>('0' + (cap_display % 10)), '\0'};
    oz::sys_debug_print(num_buf);
    oz::sys_debug_print("\n\r");

    // 2. Try initializing GUI Terminal Window
    if (cap_wm != 0 && cap_display != 0) {
        init_gui_terminal(cap_wm, cap_display);
    }

    shell_write("\n\r=== OZ OS Interactive Shell ===\n\r");
    shell_write("Type 'help' for a list of commands.\n\r\n\r");

    // VFS shared buffer is pre-mapped by Init at VFS_SHARED_BUF_VIRT
    g_shared_buf = reinterpret_cast<char*>(VFS_SHARED_BUF_VIRT);

    char line_buf[256] = {};
    std::size_t line_len = 0;

    shell_write("oz> ");

    while (true) {
        oz::IPCMessage msg{};
        char c = 0;
        if (g_gui_mode) {
            if (!oz::sys_channel_recv(g_input_recv, &msg)) continue;
            if (msg.data[0] != static_cast<std::uint64_t>(oz::ipc::WMEventType::KEY_DOWN)) continue;
            c = static_cast<char>(msg.data[1] & 0xFF);
        } else {
            if (!oz::sys_channel_recv(CAP_SHELL_INPUT, &msg)) continue;
            c = static_cast<char>(msg.data[0] & 0xFF);
        }

        if (c == 0) continue;

        if (c == '\r' || c == '\n') {
            shell_write("\n\r");
            line_buf[line_len] = '\0';
            execute_command(line_buf);
            line_len = 0;
            line_buf[0] = '\0';
            shell_write("oz> ");
        } else if (c == '\b' || c == 0x7F) {
            if (line_len > 0) {
                line_len--;
                line_buf[line_len] = '\0';
                shell_write("\b \b");
            }
        } else if (c >= 32 && c <= 126) {
            if (line_len + 1 < sizeof(line_buf)) {
                line_buf[line_len++] = c;
                line_buf[line_len] = '\0';
                char echo[2] = {c, '\0'};
                shell_write(echo);
            }
        }
    }
}
