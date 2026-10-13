#include "syscall.hpp"
#include "IPCBlockDevice.hpp"
#include "FAT32.hpp"
#include "VFS.hpp"
#include "../common/ipc/Protocol.hpp"
#include "../common/utils/utils.hpp"

// Cap slot assignments:
//   1: write to console (sys_write uses this)
//   2: send block I/O requests to AHCI
//   3: recv block I/O responses from AHCI
//   4: recv file operation requests from clients
//   5: send file operation responses to clients

static constexpr std::uint32_t CAP_AHCI_SEND = 2;
static constexpr std::uint32_t CAP_AHCI_RECV = 3;
static constexpr std::uint32_t CAP_CLIENT_RECV = 4;
static constexpr std::uint32_t CAP_CLIENT_SEND = 5;

// Simple handle table for open files
static constexpr int MAX_OPEN_FILES = 16;
static oz::File* open_files[MAX_OPEN_FILES] = {};

static int alloc_handle(oz::File* f) {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!open_files[i]) {
            open_files[i] = f;
            return i;
        }
    }
    return -1;
}

static oz::File* get_file(int handle) {
    if (handle < 0 || handle >= MAX_OPEN_FILES) return nullptr;
    return open_files[handle];
}

static void free_handle(int handle) {
    if (handle >= 0 && handle < MAX_OPEN_FILES) {
        open_files[handle] = nullptr;
    }
}

// Extract a path string packed in IPCMessage data[1..3] (up to 24 bytes)
static void extract_path(const oz::IPCMessage& msg, char* path, std::size_t max_len) {
    const char* src = reinterpret_cast<const char*>(&msg.data[1]);
    std::size_t i = 0;
    std::size_t src_max = 24; // 3 * 8 bytes
    if (max_len - 1 < src_max) src_max = max_len - 1;
    for (; i < src_max && src[i] != '\0'; i++) {
        path[i] = src[i];
    }
    path[i] = '\0';
}

extern "C" void _start() {
    oz::sys_write("VFS Server started!\n\r");
    
    // Initialize IPC-based block device connecting to AHCI server
    oz::IPCBlockDevice block_dev(CAP_AHCI_SEND, CAP_AHCI_RECV);
    if (!block_dev.initialize()) {
        oz::sys_write("VFS: Failed to initialize block device!\n\r");
        oz::sys_exit(1);
    }
    oz::sys_write("VFS: Block device connected to AHCI.\n\r");

    // Initialize buffer cache
    oz::BufferCacheManager cache;
    cache.init();

    // Initialize FAT32 filesystem
    oz::FAT32FileSystem fat32(&block_dev, &cache);
    if (!fat32.init()) {
        oz::sys_write("VFS: Failed to mount FAT32!\n\r");
        oz::sys_exit(1);
    }
    oz::sys_write("VFS: FAT32 mounted successfully!\n\r");

    // Mount as root
    oz::VFS vfs;
    vfs.mountRoot(&fat32);

    // Main request loop
    oz::sys_write("VFS: Ready to serve file requests.\n\r");
    while (true) {
        oz::IPCMessage req{};
        if (!oz::sys_channel_recv(CAP_CLIENT_RECV, &req)) continue;

        auto cmd = static_cast<oz::ipc::VfsCmd>(req.data[0]);
        oz::IPCMessage resp{};

        switch (cmd) {
            case oz::ipc::VfsCmd::OPEN: {
                char path[25];
                extract_path(req, path, sizeof(path));
                auto file = vfs.open(path);
                if (file) {
                    int handle = alloc_handle(file.release());
                    if (handle >= 0) {
                        resp.data[0] = 0; // success
                        resp.data[1] = static_cast<std::uint64_t>(handle);
                    } else {
                        resp.data[0] = 2; // too many open files
                    }
                } else {
                    resp.data[0] = 1; // not found
                }
                break;
            }
            case oz::ipc::VfsCmd::READ: {
                int handle = static_cast<int>(req.data[1]);
                std::size_t offset = static_cast<std::size_t>(req.data[2]);
                std::size_t size = static_cast<std::size_t>(req.data[3]);
                
                oz::File* f = get_file(handle);
                if (!f) {
                    resp.data[0] = 1;
                    break;
                }
                
                // Limit read size to what fits in IPC response (24 bytes in data[1..3])
                if (size > 24) size = 24;
                
                f->seek(offset);
                char buf[24] = {};
                std::size_t bytes_read = f->read(buf, size);
                
                resp.data[0] = 0; // success
                resp.data[1] = bytes_read;
                // Pack read data into data[2..3]
                oz::utils::memcpy(&resp.data[2], buf, bytes_read > 16 ? 16 : bytes_read);
                break;
            }
            case oz::ipc::VfsCmd::CLOSE: {
                int handle = static_cast<int>(req.data[1]);
                oz::File* f = get_file(handle);
                if (f) {
                    f->close();
                    delete f;
                    free_handle(handle);
                    resp.data[0] = 0;
                } else {
                    resp.data[0] = 1;
                }
                break;
            }
            case oz::ipc::VfsCmd::GET_SIZE: {
                int handle = static_cast<int>(req.data[1]);
                oz::File* f = get_file(handle);
                if (f) {
                    resp.data[0] = 0;
                    resp.data[1] = f->getSize();
                } else {
                    resp.data[0] = 1;
                }
                break;
            }
            default:
                resp.data[0] = static_cast<std::uint64_t>(-1);
                break;
        }

        oz::sys_channel_send(CAP_CLIENT_SEND, &resp);
    }
}
