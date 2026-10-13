#pragma once
#include "BlockDevice.hpp"
#include "syscall.hpp"
#include "../common/ipc/Protocol.hpp"
#include "../common/utils/utils.hpp"

namespace oz {

// BlockDevice implementation that proxies read/write to the AHCI server via IPC
class IPCBlockDevice : public BlockDevice {
private:
    std::uint32_t send_cap;  // Cap for sending requests to AHCI
    std::uint32_t recv_cap;  // Cap for receiving responses from AHCI
    std::uint32_t sector_sz{512};
    
    // Shared buffer for data transfer (physical memory shared between VFS and AHCI)
    void* shared_buf{nullptr};
    std::uint64_t shared_buf_phys{0};
    static constexpr std::size_t SHARED_BUF_SIZE = 64 * 1024; // 64KB

public:
    IPCBlockDevice(std::uint32_t send_cap_id, std::uint32_t recv_cap_id)
        : send_cap(send_cap_id), recv_cap(recv_cap_id) {}

    bool initialize() {
        // Map shared buffer Frame (slot 242) into our VSpace
        static constexpr std::uint64_t SHARED_BUF_VIRT = 0x200000000000;
        if (sys_vspace_map(oz::CAP_SELF_VSPACE, 242, SHARED_BUF_VIRT, 0x01) != 0) {
            return false; // 0x01 = writable
        }
        shared_buf = reinterpret_cast<void*>(SHARED_BUF_VIRT);
        shared_buf_phys = sys_frame_phys_addr(242);
        if (!shared_buf_phys) return false;
        
        // Query sector size
        IPCMessage req{};
        req.data[0] = static_cast<std::uint64_t>(ipc::BlockCmd::GET_INFO);
        sys_channel_send(send_cap, &req);
        
        IPCMessage resp{};
        sys_channel_recv(recv_cap, &resp);
        if (resp.data[0] == 0) {
            sector_sz = static_cast<std::uint32_t>(resp.data[1]);
            if (sector_sz == 0) sector_sz = 512;
        }
        
        return true;
    }

    bool readSectors(std::uint64_t lba, std::uint32_t count, void* buffer) override {
        std::uint64_t total_bytes = static_cast<std::uint64_t>(count) * sector_sz;
        if (total_bytes > SHARED_BUF_SIZE) return false;

        IPCMessage req{};
        req.data[0] = static_cast<std::uint64_t>(ipc::BlockCmd::READ);
        req.data[1] = lba;
        req.data[2] = count;
        req.data[3] = shared_buf_phys;
        
        sys_channel_send(send_cap, &req);
        
        IPCMessage resp{};
        sys_channel_recv(recv_cap, &resp);
        
        if (resp.data[0] != 0) return false;
        
        // Copy from shared buffer to caller's buffer
        utils::memcpy(buffer, shared_buf, total_bytes);
        return true;
    }

    bool writeSectors(std::uint64_t lba, std::uint32_t count, const void* buffer) override {
        std::uint64_t total_bytes = static_cast<std::uint64_t>(count) * sector_sz;
        if (total_bytes > SHARED_BUF_SIZE) return false;
        
        // Copy to shared buffer
        utils::memcpy(shared_buf, buffer, total_bytes);
        
        IPCMessage req{};
        req.data[0] = static_cast<std::uint64_t>(ipc::BlockCmd::WRITE);
        req.data[1] = lba;
        req.data[2] = count;
        req.data[3] = shared_buf_phys;
        
        sys_channel_send(send_cap, &req);
        
        IPCMessage resp{};
        sys_channel_recv(recv_cap, &resp);
        
        return resp.data[0] == 0;
    }

    std::uint32_t getSectorSize() const override { return sector_sz; }
};

} // namespace oz
