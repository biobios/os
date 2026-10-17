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
};

// VFS Protocol Helpers: data[0] carries client_id (high 32 bits) and VfsCmd (low 32 bits)
inline std::uint32_t vfs_get_cmd(std::uint64_t data0) {
    return static_cast<std::uint32_t>(data0 & 0xFFFFFFFFULL);
}

inline std::uint32_t vfs_get_client_id(std::uint64_t data0) {
    return static_cast<std::uint32_t>(data0 >> 32);
}

inline std::uint64_t vfs_make_command(VfsCmd cmd, std::uint32_t client_id = 0) {
    return (static_cast<std::uint64_t>(client_id) << 32) | static_cast<std::uint64_t>(cmd);
}

} // namespace ipc
} // namespace oz

