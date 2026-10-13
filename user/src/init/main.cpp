#include "syscall.hpp"
#include "../common/utils/BootInfo.hpp"
#include "../common/ipc/Protocol.hpp"

// Cap slot assignments for init:
//   1: write to console (sys_write uses this)
//   2: send to AHCI (MMIO address)
//   3: send to VFS (file operation requests)
//   4: recv from VFS (file operation responses)

static constexpr std::uint32_t CAP_AHCI_SEND = 2;
static constexpr std::uint32_t CAP_VFS_SEND = 3;
static constexpr std::uint32_t CAP_VFS_RECV = 4;

// Helper: send a VFS file open request
static int vfs_open(const char* path) {
    oz::IPCMessage req{};
    req.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsCmd::OPEN);
    
    // Pack path into data[1..3] (up to 24 chars)
    char* dst = reinterpret_cast<char*>(&req.data[1]);
    int i = 0;
    for (; i < 23 && path[i] != '\0'; i++) {
        dst[i] = path[i];
    }
    dst[i] = '\0';
    
    oz::sys_channel_send(CAP_VFS_SEND, &req);
    
    oz::IPCMessage resp{};
    oz::sys_channel_recv(CAP_VFS_RECV, &resp);
    
    if (resp.data[0] == 0) {
        return static_cast<int>(resp.data[1]); // file handle
    }
    return -1;
}

// Helper: read from a VFS file
static std::size_t vfs_read(int handle, std::size_t offset, char* buf, std::size_t size) {
    oz::IPCMessage req{};
    req.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsCmd::READ);
    req.data[1] = static_cast<std::uint64_t>(handle);
    req.data[2] = static_cast<std::uint64_t>(offset);
    req.data[3] = static_cast<std::uint64_t>(size);
    
    oz::sys_channel_send(CAP_VFS_SEND, &req);
    
    oz::IPCMessage resp{};
    oz::sys_channel_recv(CAP_VFS_RECV, &resp);
    
    if (resp.data[0] != 0) return 0;
    
    std::size_t bytes_read = static_cast<std::size_t>(resp.data[1]);
    // Data is packed in resp.data[2..3]
    const char* src = reinterpret_cast<const char*>(&resp.data[2]);
    std::size_t to_copy = bytes_read > 16 ? 16 : bytes_read;
    if (to_copy > size) to_copy = size;
    for (std::size_t i = 0; i < to_copy; i++) {
        buf[i] = src[i];
    }
    return to_copy;
}

// Helper: get file size
static std::size_t vfs_get_size(int handle) {
    oz::IPCMessage req{};
    req.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsCmd::GET_SIZE);
    req.data[1] = static_cast<std::uint64_t>(handle);
    
    oz::sys_channel_send(CAP_VFS_SEND, &req);
    
    oz::IPCMessage resp{};
    oz::sys_channel_recv(CAP_VFS_RECV, &resp);
    
    if (resp.data[0] == 0) return static_cast<std::size_t>(resp.data[1]);
    return 0;
}

// Helper: close a VFS file
static void vfs_close(int handle) {
    oz::IPCMessage req{};
    req.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsCmd::CLOSE);
    req.data[1] = static_cast<std::uint64_t>(handle);
    
    oz::sys_channel_send(CAP_VFS_SEND, &req);
    
    oz::IPCMessage resp{};
    oz::sys_channel_recv(CAP_VFS_RECV, &resp);
}

extern "C" void _start() {
    oz::sys_write("Hello from init process (Ring 3)!\n\r");
    
    // Map BootInfo Frame (slot 243) into our VSpace
    static constexpr std::uint64_t BOOTINFO_VIRT = 0x600000000000;
    if (oz::sys_vspace_map(oz::CAP_SELF_VSPACE, 243, BOOTINFO_VIRT, 0x00) != 0) {
        // 0x00 = read-only
        oz::sys_write("Failed to map BootInfo!\n\r");
        oz::sys_exit(1);
    }
    
    const volatile auto* boot_info = reinterpret_cast<const volatile oz::BootInfo*>(BOOTINFO_VIRT);
    std::uint64_t ahci_abar = boot_info->ahci_abar_phys;
    
    // Send AHCI MMIO address to AHCI driver
    oz::IPCMessage msg{};
    msg.data[0] = ahci_abar;
    
    if (oz::sys_channel_send(CAP_AHCI_SEND, &msg)) {
        oz::sys_write("Sent AHCI MMIO address to ahci server.\n\r");
    } else {
        oz::sys_write("Failed to send AHCI MMIO address!\n\r");
    }
    
    // Wait a bit for VFS to initialize (it needs AHCI to be ready first)
    // In a real system, we'd use a synchronization mechanism.
    // For now, the blocking IPC will handle ordering.
    oz::sys_write("Attempting to open test.txt via VFS...\n\r");
    
    int handle = vfs_open("test.txt");
    if (handle >= 0) {
        oz::sys_write("File opened! handle=");
        char num[4] = {static_cast<char>('0' + handle), '\n', '\r', '\0'};
        oz::sys_write(num);
        
        std::size_t size = vfs_get_size(handle);
        oz::sys_write("File size: ");
        // Simple number to string
        char size_str[16];
        int pos = 0;
        if (size == 0) {
            size_str[pos++] = '0';
        } else {
            char tmp[16];
            int tmp_pos = 0;
            std::size_t s = size;
            while (s > 0) {
                tmp[tmp_pos++] = '0' + static_cast<char>(s % 10);
                s /= 10;
            }
            for (int i = tmp_pos - 1; i >= 0; i--) {
                size_str[pos++] = tmp[i];
            }
        }
        size_str[pos++] = '\n';
        size_str[pos++] = '\r';
        size_str[pos] = '\0';
        oz::sys_write(size_str);
        
        // Read first 16 bytes
        char buf[17] = {};
        std::size_t bytes = vfs_read(handle, 0, buf, 16);
        buf[bytes] = '\0';
        oz::sys_write("Content: ");
        oz::sys_write(buf);
        oz::sys_write("\n\r");
        
        vfs_close(handle);
        oz::sys_write("File closed.\n\r");
    } else {
        oz::sys_write("test.txt not found (this is OK if disk has no test.txt).\n\r");
    }
    
    oz::sys_write("Init process completed. Idling...\n\r");
    
    while (true) {
        __asm__ volatile("pause");
    }
}
