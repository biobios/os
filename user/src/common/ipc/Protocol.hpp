#pragma once
#include <cstdint>

namespace oz {
namespace ipc {

// ===== Block Device IPC Protocol =====
// Used between VFS server and AHCI (block device) server
//
// Request messages (VFS → AHCI):
//   data[0] = command (BlockCmd)
//   data[1] = LBA (sector number)
//   data[2] = count (number of sectors)
//   data[3] = buffer physical address (for DMA)
//
// Response messages (AHCI → VFS):
//   data[0] = status (0 = success, non-zero = error)
//   data[1] = sector size (for BLOCK_GET_INFO)

enum class BlockCmd : std::uint64_t {
    READ         = 1,   // Read sectors: lba, count, phys_buf
    WRITE        = 2,   // Write sectors: lba, count, phys_buf
    GET_INFO     = 3,   // Get sector size and device info
};

// ===== VFS IPC Protocol =====
// Used between client processes and VFS server
//
// Request messages (Client → VFS):
//   data[0] = command (VfsCmd)
//   data[1..3] = command-specific args
//
// Response messages (VFS → Client):
//   data[0] = status (0 = success)
//   data[1..3] = command-specific return values

enum class VfsCmd : std::uint64_t {
    OPEN         = 1,   // Open file:  data[1..3] = path (packed chars)
    READ         = 2,   // Read file:  data[1] = handle, data[2] = offset, data[3] = size
    WRITE        = 3,   // Write file: data[1] = handle, data[2] = offset, data[3] = size
    CLOSE        = 4,   // Close file: data[1] = handle
    GET_SIZE     = 5,   // Get size:   data[1] = handle
    LIST_DIR     = 6,   // List dir:   data[1..3] = path (packed chars)
    OPENAT       = 7,   // Open file/dir relative to directory:
                        //   data[0] low 32 = VfsCmd, high 32 = flags (1=File, 2=Dir)
                        //   data[1..3] = rel_path (packed chars)
                        // Response:
                        //   data[0] = status (0: ok, 1: not found, 2: invalid path / '..' rejected)
                        //   transferred_cap = new FileCap or DirectoryCap Channel
};

enum class VfsOpenFlags : std::uint32_t {
    FILE      = 1,
    DIRECTORY = 2,
};

enum class VfsStatus : std::uint64_t {
    OK           = 0,
    NOT_FOUND    = 1,
    INVALID_PATH = 2, // Relative path violates sandbox (e.g. contains '..' or starts with '/')
    TOO_MANY_OPEN= 3,
    IO_ERROR     = 4,
};

// ===== Dedicated File Channel Protocol =====
// Used when communicating directly with a File Capability Channel
enum class FileCmd : std::uint32_t {
    READ     = 1, // data[0] = cmd | (size << 32), data[1] = offset (or FILE_STREAM_OFFSET)
                  // response: data[0] = status | (bytes_read << 32), data[1..3] = payload (up to 24 bytes)
    WRITE    = 2, // data[0] = cmd | (size << 32), data[1] = offset, data[2..3] = payload
    GET_SIZE = 3, // response: data[0] = status, data[1] = size
    CLOSE    = 4, // response: data[0] = status
};

static constexpr std::uint64_t FILE_STREAM_OFFSET = 0xFFFFFFFFFFFFFFFFULL;

inline std::uint64_t file_make_cmd(FileCmd cmd, std::uint32_t len = 0) {
    return static_cast<std::uint64_t>(cmd) | (static_cast<std::uint64_t>(len) << 32);
}

inline FileCmd file_get_cmd(std::uint64_t data0) {
    return static_cast<FileCmd>(data0 & 0xFFFFFFFFULL);
}

inline std::uint32_t file_get_len(std::uint64_t data0) {
    return static_cast<std::uint32_t>((data0 >> 32) & 0xFFFFFFFFULL);
}

// ===== Stream Channel Protocol (Pipes / STDIN / STDOUT) =====
// data[0] = length of payload (1..24 bytes)
// data[1..3] = payload bytes
inline std::uint64_t stream_make_header(std::uint32_t len) {
    return static_cast<std::uint64_t>(len);
}

inline std::uint32_t stream_get_len(std::uint64_t data0) {
    return static_cast<std::uint32_t>(data0 & 0xFFFFFFFFULL);
}


// Validates that a path is relative and does NOT contain ".." anywhere.
// Ambient authority prevention: ".." traversal is strictly forbidden.
inline bool vfs_is_valid_relative_path(const char* path) {
    if (!path || *path == '\0') return false;
    if (*path == '/') return false; // Absolute path forbidden

    for (std::size_t i = 0; path[i] != '\0'; ++i) {
        if (path[i] == '.' && path[i + 1] == '.') {
            // Check if it forms a ".." path component
            bool left_boundary = (i == 0 || path[i - 1] == '/');
            bool right_boundary = (path[i + 2] == '/' || path[i + 2] == '\0');
            if (left_boundary && right_boundary) {
                return false; // Forbidden!
            }
        }
    }
    return true;
}

// VFS Protocol Helpers: data[0] carries client_id/flags (high 32 bits) and VfsCmd (low 32 bits)
inline std::uint32_t vfs_get_cmd(std::uint64_t data0) {
    return static_cast<std::uint32_t>(data0 & 0xFFFFFFFFULL);
}

inline std::uint32_t vfs_get_client_id(std::uint64_t data0) {
    return static_cast<std::uint32_t>((data0 >> 32) & 0xFFFFULL);
}

inline std::uint64_t vfs_make_command(VfsCmd cmd, std::uint32_t client_id = 0) {

    return (static_cast<std::uint64_t>(client_id) << 32) | static_cast<std::uint64_t>(cmd);
}

inline std::uint64_t vfs_make_openat_command(std::uint32_t client_id, VfsOpenFlags flags, std::uint32_t base_dir_handle = 0) {
    return (static_cast<std::uint64_t>(base_dir_handle & 0xFF) << 56) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(flags) & 0xFF) << 48) |
           (static_cast<std::uint64_t>(client_id) << 32) |
           static_cast<std::uint64_t>(VfsCmd::OPENAT);
}

// ===== Display Server IPC Protocol =====
// Pure mechanism: Surface composition and pixel blitting
enum class DisplayCmd : std::uint64_t {
    CREATE_SURFACE         = 1, // data[1] = width, data[2] = height -> returns surface_id & FrameCap
    COMMIT                 = 2, // data[1] = surface_id (surface updated)
    DESTROY_SURFACE        = 3, // data[1] = surface_id
    // Admin / Window Manager commands
    SET_SURFACE_RECT       = 10, // data[1] = surface_id, data[2] = (x << 32) | y, data[3] = (w << 32) | h
    SET_SURFACE_VISIBILITY = 11, // data[1] = surface_id, data[2] = visible (0/1), data[3] = z_order
    UPDATE_CURSOR          = 12, // data[1] = (x << 32) | y, data[2] = visible
    GET_DISPLAY_INFO       = 13, // returns width, height, bpp
    SET_SURFACE_BORDER     = 14, // data[1] = surface_id, data[2] = border_width, data[3] = border_color (ARGB)
};

// ===== Window Manager IPC Protocol =====
// Policy: Windows, layouts, focus, input filtering
enum class WMCmd : std::uint64_t {
    CREATE_WINDOW          = 10, // Client -> WM: data[1] = width, data[2] = height
                                 // Returns: data[1] = win_id, data[2] = surface_id, transferred_cap = InputChannelCap
    DESTROY_WINDOW         = 11, // Client -> WM: data[1] = win_id
    SET_TITLE              = 12, // Client -> WM: data[1] = win_id, data[2..3] = title chars
    FOCUS_WINDOW           = 13, // data[1] = win_id
    MINIMIZE_WINDOW        = 14, // data[1] = win_id
    RESTORE_MINIMIZED      = 15, // Restores last minimized window
    ATTACH_OVERLAY         = 16, // data[1] = parent_win_id, data[2] = overlay_win_id
    BIND_INPUT_CHANNEL     = 17, // Client -> WM: data[1] = win_id, transferred_cap = client_input_send
    GET_WINDOW_INFO        = 18, // Client -> WM: data[1] = win_id -> returns x, y, w, h, is_focused
};

enum class WMModifier : std::uint32_t {
    NONE  = 0,
    SHIFT = 1 << 0,
    CTRL  = 1 << 1,
    ALT   = 1 << 2,
    SUPER = 1 << 3,
};

enum class WMEventType : std::uint64_t {
    KEY_DOWN               = 100, // data[1] = ascii, data[2] = scancode, data[3] = modifiers
    KEY_UP                 = 101,
    MOUSE_MOVE             = 102, // data[1] = rel_x, data[2] = rel_y
    MOUSE_BUTTON           = 103, // data[1] = buttons, data[2] = rel_x, data[3] = rel_y
    FOCUS_CHANGED          = 104, // data[1] = 1 (gained) or 0 (lost)
    WINDOW_RESIZED         = 105, // data[1] = new_width, data[2] = new_height
};

// ===== Memory Manager IPC Protocol =====
enum class MemCmd : std::uint64_t {
    ALLOC_UNTYPED  = 1, // Client -> MemMan: data[1] = size_bytes (aligned power-of-two)
                        // Resp: data[0] = status (0=OK), transferred_cap = UntypedMemory Cap
    REVOKE_UNTYPED = 2, // Client -> MemMan: transferred_cap = UntypedMemory Cap to revoke & reclaim
                        // Resp: data[0] = status (0=OK)
    GET_INFO       = 3, // Client -> MemMan: returns data[1] = total_bytes, data[2] = free_bytes
};

// ===== Process Manager IPC Protocol =====
enum class ProcCmd : std::uint64_t {
    SPAWN_INIT       = 1, // data[1..3] = prog_name (packed chars, up to 24 chars)
                          // transferred_cap (optional) = stdin cap (if has_cap is true)
                          // Resp: data[0] = status, data[1] = session_id
    SPAWN_SET_STDOUT = 2, // data[1] = session_id, transferred_cap = stdout cap
                          // Resp: data[0] = status
    SPAWN_ADD_ARG    = 3, // data[1] = session_id, data[2..3] = arg string (up to 16 chars)
                          // Resp: data[0] = status
    SPAWN_ADD_CAP    = 4, // data[1] = session_id, data[2..3] = name (up to 16 chars), transferred_cap = cap
                          // Resp: data[0] = status
    SPAWN_COMMIT     = 5, // data[1] = session_id -> launches process
                          // Resp: data[0] = status, data[1] = pid
    WAIT             = 6, // data[1] = pid -> waits for exit and reclaims resources
                          // Resp: data[0] = status, data[1] = exit_code
    KILL             = 7, // data[1] = pid
                          // Resp: data[0] = status
    CREATE_PIPE      = 8, // Requests a new Channel pipe (write-end returned in data[1], read-end in transferred_cap)
                          // Resp: data[0] = status, data[1] = pipe_write_cap, transferred_cap = pipe_read_cap
};

} // namespace ipc

} // namespace oz



