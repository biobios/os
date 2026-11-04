#include "syscall.hpp"
#include "IPCBlockDevice.hpp"
#include "FAT32.hpp"
#include "VFS.hpp"
#include "TarFS.hpp"
#include "../common/ipc/Protocol.hpp"
#include "../common/ipc/StartupInfo.hpp"
#include "../common/utils/utils.hpp"

// Cap slot assignments:
//   1: write to console (sys_write uses this)
//   4: recv file operation requests from all clients (shared request channel)
//   231: shared buffer frame for client 0 (shell) (64KB in Level 1 PT)
//   232: shared buffer frame for client 1 (procman) (64KB in Level 1 PT)

static constexpr std::uint32_t CAP_CLIENT_RECV = 4;
static constexpr std::uint32_t CAP_CLIENT_REPLY_BASE = 5;
static constexpr std::uint32_t MAX_CLIENTS = 2;

static constexpr std::uint32_t CLIENT_BUF_SLOTS[MAX_CLIENTS] = {231, 232};
static constexpr std::uint64_t CLIENT_BUF_BASE_VIRT = 0x300000000000ULL;
static constexpr std::uint64_t CLIENT_BUF_SIZE = 0x200000ULL; // 2MB stride aligned to Level 1 PT

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

// Handle table for open directories (handle 0 is reserved for root directory)
struct OpenDirectory {
    std::uint32_t cluster{0};
    oz::Directory* dir{nullptr};
};
static constexpr int MAX_OPEN_DIRS = 16;
static OpenDirectory open_dirs[MAX_OPEN_DIRS] = {};

static int alloc_dir_handle(std::uint32_t cluster, oz::Directory* d) {
    for (int i = 1; i < MAX_OPEN_DIRS; i++) {
        if (!open_dirs[i].dir && open_dirs[i].cluster == 0) {
            open_dirs[i].cluster = cluster;
            open_dirs[i].dir = d;
            return i;
        }
    }
    return -1;
}

static OpenDirectory* get_dir(int handle) {
    if (handle < 0 || handle >= MAX_OPEN_DIRS) return nullptr;
    return &open_dirs[handle];
}

static void free_dir_handle(int handle) {
    if (handle > 0 && handle < MAX_OPEN_DIRS) {
        if (open_dirs[handle].dir) {
            open_dirs[handle].dir->close();
            delete open_dirs[handle].dir;
        }
        open_dirs[handle].dir = nullptr;
        open_dirs[handle].cluster = 0;
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

struct FileSession {
    oz::File* file{nullptr};
    std::uint32_t vfs_ep{0};
    std::uint64_t badge{0};
    std::size_t offset{0};
    bool in_use{false};
};
static constexpr std::size_t MAX_FILE_SESSIONS = 16;
static FileSession file_sessions[MAX_FILE_SESSIONS] = {};

static void handle_file_session_request(FileSession& sess) {
    oz::IPCMessage req{};
    oz::ReplyToken tok = oz::sys_reply_recv(oz::INVALID_REPLY_TOKEN, sess.vfs_ep, nullptr, &req);
    if (tok == oz::INVALID_REPLY_TOKEN) {
        if (sess.file) {
            sess.file->close();
            delete sess.file;
            sess.file = nullptr;
        }
        oz::sys_channel_bind_notification(sess.vfs_ep, oz::INVALID_CAP_ID, 0);
        oz::sys_cap_delete(sess.vfs_ep);
        sess.vfs_ep = 0;
        sess.in_use = false;
        return;
    }

    auto cmd = oz::ipc::file_get_cmd(req.data[0]);
    std::uint32_t req_len = oz::ipc::file_get_len(req.data[0]);
    oz::IPCMessage resp{};

    switch (cmd) {
        case oz::ipc::FileCmd::READ: {
            std::uint64_t offset = req.data[1];
            if (offset != oz::ipc::FILE_STREAM_OFFSET) {
                sess.offset = offset;
            }
            if (req_len == 0 || req_len > 24) req_len = 24;
            sess.file->seek(sess.offset);

            char buf[24] = {};
            std::size_t bytes_read = sess.file->read(buf, req_len);
            sess.offset += bytes_read;

            resp.data[0] = oz::ipc::file_make_cmd(static_cast<oz::ipc::FileCmd>(oz::ipc::VfsStatus::OK), static_cast<std::uint32_t>(bytes_read));
            oz::utils::memcpy(&resp.data[1], buf, bytes_read > 24 ? 24 : bytes_read);
            break;
        }
        case oz::ipc::FileCmd::WRITE: {
            std::uint64_t offset = req.data[1];
            if (offset != oz::ipc::FILE_STREAM_OFFSET) {
                sess.offset = offset;
            }
            if (req_len > 16) req_len = 16;
            sess.file->seek(sess.offset);

            char buf[16] = {};
            oz::utils::memcpy(buf, &req.data[2], req_len);
            std::size_t bytes_written = sess.file->write(buf, req_len);
            sess.offset += bytes_written;

            resp.data[0] = oz::ipc::file_make_cmd(static_cast<oz::ipc::FileCmd>(oz::ipc::VfsStatus::OK), static_cast<std::uint32_t>(bytes_written));
            break;
        }
        case oz::ipc::FileCmd::GET_SIZE: {
            resp.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsStatus::OK);
            resp.data[1] = sess.file->getSize();
            break;
        }
        case oz::ipc::FileCmd::CLOSE: {
            sess.file->close();
            delete sess.file;
            sess.file = nullptr;
            oz::sys_channel_bind_notification(sess.vfs_ep, oz::INVALID_CAP_ID, 0);
            oz::sys_cap_delete(sess.vfs_ep);
            sess.vfs_ep = 0;
            sess.in_use = false;
            resp.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsStatus::OK);
            break;
        }
        default:
            resp.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsStatus::IO_ERROR);
            break;
    }

    if (sess.vfs_ep != 0 && tok != oz::INVALID_REPLY_TOKEN) {
        oz::sys_reply_recv(tok, 0, &resp, nullptr);
    }
}


static void print_dec(std::uint64_t val) {
    char buf[24];
    int idx = 0;
    if (val == 0) {
        buf[idx++] = '0';
    } else {
        char temp[24];
        int t_idx = 0;
        while (val > 0) {
            temp[t_idx++] = '0' + (val % 10);
            val /= 10;
        }
        while (t_idx > 0) {
            buf[idx++] = temp[--t_idx];
        }
    }
    buf[idx] = '\0';
    oz::sys_debug_print(buf);
}

OZ_PROCESS_ENTRY() {
    oz::sys_write("VFS Server started!\n\r");
    
    oz::VFS vfs;
    oz::TarFileSystem* tar_fs = nullptr;

    // 1. Map InitRD Frame (slot 244) and mount TarFS as initial root
    static constexpr std::uint64_t INITRD_VADDR = 0x500000000000ULL;
    std::size_t initrd_sz = (info ? info->initrd_size : 0);
    if (initrd_sz > 0) {
        if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, oz::CAP_INITRD, INITRD_VADDR, 0x00, initrd_sz) == 0) {
            tar_fs = new oz::TarFileSystem(reinterpret_cast<const std::uint8_t*>(INITRD_VADDR), initrd_sz);
            vfs.mountRoot(tar_fs);
            oz::sys_write("VFS: Mounted InitRD (TarFS) as root filesystem.\n\r");
        }
    }

    // 2. Setup Block Device & FAT32 mount helper
    oz::IPCBlockDevice* block_dev = nullptr;
    oz::BufferCacheManager* cache = nullptr;
    oz::FAT32FileSystem* fat32 = nullptr;
    std::uint32_t g_block_dev_cap = 0;
    std::uint32_t g_block_dev_sector_sz = 512;

    auto mount_block_device = [&](std::uint32_t send_cap, std::uint32_t recv_cap, std::uint32_t sector_size = 512) -> bool {
        if (fat32) return true; // Already mounted
        block_dev = new oz::IPCBlockDevice(send_cap, recv_cap, 0, 0, 0, sector_size);
        if (!block_dev->initialize()) {
            delete block_dev;
            block_dev = nullptr;
            return false;
        }
        cache = new oz::BufferCacheManager();
        cache->init();
        fat32 = new oz::FAT32FileSystem(block_dev, cache);
        if (!fat32->init()) {
            delete fat32; fat32 = nullptr;
            delete cache; cache = nullptr;
            delete block_dev; block_dev = nullptr;
            return false;
        }
        if (!tar_fs) {
            vfs.mountRoot(fat32);
        }
        vfs.mountDisk(fat32);
        oz::sys_write("VFS: FAT32 mounted successfully at /disk!\n\r");
        return true;
    };

    auto ensure_disk_mounted = [&]() -> bool {
        if (fat32) return true;
        if (g_block_dev_cap == 0 || g_block_dev_cap == oz::INVALID_CAP_ID) return false;
        oz::sys_debug_print("[VFS] Lazy mounting FAT32 disk device...\n\r");
        return mount_block_device(g_block_dev_cap, g_block_dev_cap, g_block_dev_sector_sz);
    };

    // Map shared client buffers (slots 231, 245) if present
    char* client_bufs[MAX_CLIENTS] = {};
    for (std::uint32_t i = 0; i < MAX_CLIENTS; i++) {
        std::uint64_t virt = CLIENT_BUF_BASE_VIRT + static_cast<std::uint64_t>(i) * CLIENT_BUF_SIZE;
        std::uint32_t slot = CLIENT_BUF_SLOTS[i];
        if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, slot, virt, 0x01, 16 * 4096) == 0) {
            client_bufs[i] = reinterpret_cast<char*>(virt);
            oz::sys_write("VFS: Client shared buffer mapped.\n\r");
        }
    }

    // Main request loop
    oz::sys_write("VFS: Ready to serve file requests.\n\r");

    static std::uint64_t g_expected_reconnect_token = 0;

    oz::ReplyToken cli_tok = oz::INVALID_REPLY_TOKEN;
    oz::IPCMessage req{};
    oz::IPCMessage resp{};

    std::uint32_t recv_slot = oz::g_slot_allocator.allocate_one();

    while (true) {
        cli_tok = oz::sys_reply_recv(cli_tok, CAP_CLIENT_RECV, (cli_tok != oz::INVALID_REPLY_TOKEN) ? &resp : nullptr, &req, recv_slot);

        if (cli_tok == oz::INVALID_REPLY_TOKEN) {
            continue;
        }

        if (req.has_cap) {
            recv_slot = oz::g_slot_allocator.allocate_one();
        }

        std::uint32_t client_id = oz::ipc::vfs_get_client_id(req.data[0]);
        auto cmd = static_cast<oz::ipc::VfsCmd>(oz::ipc::vfs_get_cmd(req.data[0]));
        resp = oz::IPCMessage{};

        char* client_buf = (client_id < MAX_CLIENTS) ? client_bufs[client_id] : nullptr;
        std::uint32_t reply_cap = (client_id < MAX_CLIENTS) ? (CAP_CLIENT_REPLY_BASE + client_id) : 0;

        switch (cmd) {
            case oz::ipc::VfsCmd::REGISTER_BLOCK_DEV: {
                std::uint64_t token = req.data[3];
                if (g_expected_reconnect_token != 0 && token != g_expected_reconnect_token) {
                    oz::sys_write("VFS: REGISTER_BLOCK_DEV rejected invalid reconnect_token!\n\r");
                    resp.data[0] = 2; // Auth failure
                    break;
                }
                g_expected_reconnect_token = 0;
                if (req.has_cap && req.transferred_cap != oz::INVALID_CAP_ID) {
                    g_block_dev_cap = req.transferred_cap;
                    g_block_dev_sector_sz = req.data[2] ? static_cast<std::uint32_t>(req.data[2]) : 512;
                    oz::sys_debug_print("[VFS] Block device registered (cap=");
                    print_dec(g_block_dev_cap);
                    oz::sys_debug_print(", sector_sz=");
                    print_dec(g_block_dev_sector_sz);
                    oz::sys_debug_print(").\n\r");
                    resp.data[0] = 0; // Success
                } else {
                    resp.data[0] = 1;
                }
                break;
            }
            case oz::ipc::VfsCmd::NOTIFY_DEVICE_SUSPENDED: {
                g_expected_reconnect_token = req.data[3];
                oz::sys_write("VFS: Block device suspended, waiting for recovery token...\n\r");
                resp.data[0] = 0;
                break;
            }
            case oz::ipc::VfsCmd::OPENAT: {
                ensure_disk_mounted();
                std::uint32_t flags = static_cast<std::uint32_t>((req.data[0] >> 48) & 0xFF);
                std::uint32_t base_dir_handle = static_cast<std::uint32_t>((req.data[0] >> 56) & 0xFF);

                char path[25];
                extract_path(req, path, sizeof(path));

                // 1. Strict sandbox check: relative path without '..'
                if (!oz::ipc::vfs_is_valid_relative_path(path)) {
                    oz::sys_write("VFS: [Security] OPENAT rejected invalid relative path (ambient authority violation)!\n\r");
                    resp.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsStatus::INVALID_PATH);
                    break;
                }

                // 2. Resolve base directory cluster
                std::uint32_t base_cluster = fat32 ? fat32->getRootCluster() : 0;
                if (base_dir_handle > 0) {
                    OpenDirectory* d = get_dir(base_dir_handle);
                    if (d && d->cluster != 0) {
                        base_cluster = d->cluster;
                    } else {
                        resp.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsStatus::NOT_FOUND);
                        break;
                    }
                }

                if (flags == static_cast<std::uint32_t>(oz::ipc::VfsOpenFlags::DIRECTORY)) {
                    auto dir = fat32 ? fat32->openDirAt(base_cluster, path) : (tar_fs ? tar_fs->openDir(path) : nullptr);
                    if (dir) {
                        std::uint32_t dir_first_cluster = fat32 ? static_cast<oz::FAT32Directory*>(dir.get())->getFirstCluster() : 1;
                        int handle = alloc_dir_handle(dir_first_cluster, dir.release());
                        if (handle >= 0) {
                            std::uint32_t vfs_ep = 0, client_ep = 0;
                            if (oz::sys_channel_create(oz::CAP_UNTYPED, &vfs_ep, &client_ep)) {
                                resp.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsStatus::OK);
                                resp.data[1] = static_cast<std::uint64_t>(handle);
                                resp.transferred_cap = client_ep;
                                resp.has_cap = true;
                                oz::sys_write("VFS: OPENAT Directory succeeded, transferred new Channel Cap.\n\r");
                            } else {
                                resp.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsStatus::IO_ERROR);
                            }
                        } else {
                            resp.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsStatus::TOO_MANY_OPEN);
                        }
                    } else {
                        resp.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsStatus::NOT_FOUND);
                    }
                } else {
                    auto file = fat32 ? fat32->openFileAt(base_cluster, path) : (tar_fs ? tar_fs->openFile(path) : nullptr);
                    if (file) {
                        std::size_t sz = file->getSize();
                        int handle = alloc_handle(file.get());
                        if (handle >= 0) {
                            int session_idx = -1;
                            for (std::size_t s = 0; s < MAX_FILE_SESSIONS; ++s) {
                                if (!file_sessions[s].in_use) {
                                    session_idx = static_cast<int>(s);
                                    break;
                                }
                            }
                            if (session_idx >= 0) {
                                std::uint32_t vfs_ep = 0, client_ep = 0;
                                if (oz::sys_channel_create(oz::CAP_UNTYPED, &vfs_ep, &client_ep)) {
                                    std::uint64_t badge = 1ULL << (1 + session_idx);
                                    file_sessions[session_idx].file = file.release();
                                    file_sessions[session_idx].vfs_ep = vfs_ep;
                                    file_sessions[session_idx].badge = badge;
                                    file_sessions[session_idx].offset = 0;
                                    file_sessions[session_idx].in_use = true;

                                    resp.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsStatus::OK);
                                    resp.data[1] = static_cast<std::uint64_t>(handle);
                                    resp.data[2] = static_cast<std::uint64_t>(sz);
                                    resp.transferred_cap = client_ep;
                                    resp.has_cap = true;
                                    oz::sys_write("VFS: OPENAT File succeeded, created FileSession & transferred Cap.\n\r");
                                } else {
                                    free_handle(handle);
                                    resp.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsStatus::IO_ERROR);
                                }
                            } else {
                                free_handle(handle);
                                resp.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsStatus::TOO_MANY_OPEN);
                            }
                        } else {
                            resp.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsStatus::TOO_MANY_OPEN);
                        }
                    } else {
                        resp.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsStatus::NOT_FOUND);
                    }
                }
                break;
            }
            case oz::ipc::VfsCmd::OPEN: {
                char path[25];
                extract_path(req, path, sizeof(path));
                oz::sys_debug_print("[VFS] Handling OPEN for path: '");
                oz::sys_debug_print(path);
                oz::sys_debug_print("'\n\r");
                const char* p = path;
                while (*p == '/') p++;
                if (p[0] == 'd' && p[1] == 'i' && p[2] == 's' && p[3] == 'k') {
                    ensure_disk_mounted();
                }
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
                oz::sys_debug_print("[VFS] OPEN completed: status=");
                print_dec(resp.data[0]);
                oz::sys_debug_print(", handle=");
                print_dec(resp.data[1]);
                oz::sys_debug_print("\n\r");
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
                
                f->seek(offset);
                if (client_buf) {
                    if (size > 64 * 1024) size = 64 * 1024;
                    std::size_t bytes_read = f->read(client_buf, size);
                    resp.data[0] = 0; // success
                    resp.data[1] = bytes_read;
                } else {
                    if (size > 24) size = 24;
                    char buf[24] = {};
                    std::size_t bytes_read = f->read(buf, size);
                    resp.data[0] = 0; // success
                    resp.data[1] = bytes_read;
                    // Pack read data into data[2..3]
                    oz::utils::memcpy(&resp.data[2], buf, bytes_read > 16 ? 16 : bytes_read);
                }
                break;
            }
            case oz::ipc::VfsCmd::WRITE: {
                int handle = static_cast<int>(req.data[1]);
                std::size_t offset = static_cast<std::size_t>(req.data[2]);
                std::size_t size = static_cast<std::size_t>(req.data[3]);

                oz::File* f = get_file(handle);
                if (!f) {
                    resp.data[0] = 1;
                    break;
                }

                if (!client_buf) {
                    resp.data[0] = 2; // buffer not available
                    break;
                }

                if (size > 64 * 1024) size = 64 * 1024;
                if (!f->seek(offset)) {
                    resp.data[0] = 3; // seek failed
                    break;
                }

                std::size_t bytes_written = f->write(client_buf, size);
                resp.data[0] = 0; // success
                resp.data[1] = bytes_written;
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
            case oz::ipc::VfsCmd::LIST_DIR: {
                char path[25];
                extract_path(req, path, sizeof(path));
                const char* p = path;
                while (*p == '/') p++;
                if (p[0] == 'd' && p[1] == 'i' && p[2] == 's' && p[3] == 'k') {
                    ensure_disk_mounted();
                }
                auto dir = vfs.openDir(path);
                if (!dir) {
                    resp.data[0] = 1; // not found
                    break;
                }
                
                std::size_t entry_count = 0;
                if (client_buf) {
                    char* out = client_buf;
                    std::size_t max_out = 64 * 1024 - 1;
                    std::size_t curr = 0;
                    char name[64];
                    bool isDir = false;
                    while (dir->readEntry(name, sizeof(name), isDir) && curr + 70 < max_out) {
                        for (int k = 0; name[k] != '\0'; k++) {
                            out[curr++] = name[k];
                        }
                        if (isDir) {
                            out[curr++] = '/';
                        }
                        out[curr++] = '\n';
                        entry_count++;
                    }
                    out[curr] = '\0';
                    resp.data[0] = 0;
                    resp.data[1] = entry_count;
                    resp.data[2] = curr;
                } else {
                    resp.data[0] = 2; // buffer not available
                }
                break;
            }
            default:
                resp.data[0] = static_cast<std::uint64_t>(-1);
                break;
        }
    }
}
