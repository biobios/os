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

} // namespace ipc

} // namespace oz


