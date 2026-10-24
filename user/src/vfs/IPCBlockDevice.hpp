#pragma once
#include "BlockDevice.hpp"
#include "syscall.hpp"
#include "ring_buffer.hpp"
#include "../common/ipc/Protocol.hpp"
#include "../common/utils/utils.hpp"

namespace oz {

// BlockDevice implementation that proxies read/write to the AHCI server
// via Shared-Memory RingBuffer + Notification (with legacy Channel fallback)
class IPCBlockDevice : public BlockDevice {
private:
    std::uint32_t send_cap{0};        // Cap for sending legacy requests to AHCI
    std::uint32_t recv_cap{0};        // Cap for receiving legacy responses from AHCI
    std::uint32_t notif_ahci_cap{0};  // Cap for signaling AHCI (Notification)
    std::uint32_t notif_vfs_cap{0};   // Cap for receiving completion from AHCI (Notification)
    std::uint32_t ring_buf_cap{0};    // Cap for shared BlockIORing frame
    std::uint32_t sector_sz{512};
    
    // Shared buffer for data transfer (physical memory shared between VFS and AHCI)
    void* shared_buf{nullptr};
    std::uint64_t shared_buf_phys{0};
    static constexpr std::size_t SHARED_BUF_SIZE = 64 * 1024; // 64KB

    // Shared Ring Buffer
    volatile BlockIORing* ring_{nullptr};
    bool use_ring_{false};
    std::uint64_t next_req_id_{0};

    bool waitResponse(std::uint64_t target_req_id, AsyncBlockResp* out_resp) {
        while (true) {
            AsyncBlockResp resp{};
            if (ring_->cq.pop(&resp)) {
                if (resp.req_id == target_req_id) {
                    if (out_resp) *out_resp = resp;
                    return true;
                }
                continue;
            }

            ring_->cq.set_need_signal(true);
            // Double check before blocking
            if (ring_->cq.pop(&resp)) {
                ring_->cq.set_need_signal(false);
                if (resp.req_id == target_req_id) {
                    if (out_resp) *out_resp = resp;
                    return true;
                }
                continue;
            }

            std::uint64_t badge = 0;
            sys_notification_wait(notif_vfs_cap, &badge);
            ring_->cq.set_need_signal(false);
        }
    }

public:
    IPCBlockDevice(std::uint32_t send_cap_id, std::uint32_t recv_cap_id,
                   std::uint32_t notif_ahci = 8, std::uint32_t notif_vfs = 9,
                   std::uint32_t ring_cap = 250)
        : send_cap(send_cap_id), recv_cap(recv_cap_id),
          notif_ahci_cap(notif_ahci), notif_vfs_cap(notif_vfs),
          ring_buf_cap(ring_cap) {}

    bool initialize() {
        // 1. Map shared data buffer Frame (slot 242) into our VSpace
        static constexpr std::uint64_t SHARED_BUF_VIRT = 0x200000000000;
        if (sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, 242, SHARED_BUF_VIRT, 0x01, 16 * 4096) != 0) {
            return false; // 0x01 = writable
        }
        shared_buf = reinterpret_cast<void*>(SHARED_BUF_VIRT);
        shared_buf_phys = sys_frame_phys_addr(242);
        if (!shared_buf_phys) return false;

        // 2. Map shared Ring Buffer Frame (slot 250) if available
        static constexpr std::uint64_t RING_BUF_VIRT = 0x205000000000;
        if (sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, ring_buf_cap, RING_BUF_VIRT, 0x01, 4096) == 0) {
            ring_ = reinterpret_cast<volatile BlockIORing*>(RING_BUF_VIRT);
            utils::memset(const_cast<void*>(reinterpret_cast<volatile void*>(ring_)), 0, sizeof(BlockIORing));
            use_ring_ = true;
            sys_write("VFS: IPCBlockDevice using RingBuffer + Notification mode.\n\r");
        } else {
            sys_write("VFS: IPCBlockDevice falling back to synchronous Channel IPC.\n\r");
        }
        
        // 3. Query sector size
        if (use_ring_) {
            AsyncBlockReq req{};
            req.req_id = ++next_req_id_;
            req.cmd = static_cast<std::uint32_t>(AsyncBlockCmd::GET_INFO);
            
            bool need_signal = false;
            while (!ring_->sq.push(req, &need_signal)) {}
            if (need_signal) {
                sys_notification_signal(notif_ahci_cap, 1ULL);
            }

            AsyncBlockResp resp{};
            waitResponse(req.req_id, &resp);
            sector_sz = resp.info;
            if (sector_sz == 0) sector_sz = 512;
        } else {
            IPCMessage req{};
            req.data[0] = static_cast<std::uint64_t>(ipc::BlockCmd::GET_INFO);
            IPCMessage resp{};
            if (sys_call(send_cap, &req, &resp)) {
                if (resp.data[0] == 0) {
                    sector_sz = static_cast<std::uint32_t>(resp.data[1]);
                    if (sector_sz == 0) sector_sz = 512;
                }
            }
        }
        
        return true;
    }

    bool readSectors(std::uint64_t lba, std::uint32_t count, void* buffer) override {
        std::uint64_t total_bytes = static_cast<std::uint64_t>(count) * sector_sz;
        if (total_bytes > SHARED_BUF_SIZE) return false;

        if (use_ring_) {
            AsyncBlockReq req{};
            req.req_id = ++next_req_id_;
            req.cmd = static_cast<std::uint32_t>(AsyncBlockCmd::READ);
            req.lba = lba;
            req.count = count;
            req.buf_offset = 0;

            bool need_signal = false;
            while (!ring_->sq.push(req, &need_signal)) {}
            if (need_signal) {
                sys_notification_signal(notif_ahci_cap, 1ULL);
            }

            AsyncBlockResp resp{};
            waitResponse(req.req_id, &resp);

            if (resp.status != 0) return false;
            utils::memcpy(buffer, shared_buf, total_bytes);
            return true;
        }

        // Fallback: Synchronous Channel IPC
        IPCMessage req{};
        req.data[0] = static_cast<std::uint64_t>(ipc::BlockCmd::READ);
        req.data[1] = lba;
        req.data[2] = count;
        req.data[3] = shared_buf_phys;
        
        IPCMessage resp{};
        if (!sys_call(send_cap, &req, &resp) || resp.data[0] != 0) return false;
        
        utils::memcpy(buffer, shared_buf, total_bytes);
        return true;
    }

    bool writeSectors(std::uint64_t lba, std::uint32_t count, const void* buffer) override {
        std::uint64_t total_bytes = static_cast<std::uint64_t>(count) * sector_sz;
        if (total_bytes > SHARED_BUF_SIZE) return false;
        
        utils::memcpy(shared_buf, buffer, total_bytes);

        if (use_ring_) {
            AsyncBlockReq req{};
            req.req_id = ++next_req_id_;
            req.cmd = static_cast<std::uint32_t>(AsyncBlockCmd::WRITE);
            req.lba = lba;
            req.count = count;
            req.buf_offset = 0;

            bool need_signal = false;
            while (!ring_->sq.push(req, &need_signal)) {}
            if (need_signal) {
                sys_notification_signal(notif_ahci_cap, 1ULL);
            }

            AsyncBlockResp resp{};
            waitResponse(req.req_id, &resp);
            return resp.status == 0;
        }

        // Fallback: Synchronous Channel IPC
        IPCMessage req{};
        req.data[0] = static_cast<std::uint64_t>(ipc::BlockCmd::WRITE);
        req.data[1] = lba;
        req.data[2] = count;
        req.data[3] = shared_buf_phys;
        
        IPCMessage resp{};
        if (!sys_call(send_cap, &req, &resp)) return false;
        
        return resp.data[0] == 0;
    }

    std::uint32_t getSectorSize() const override { return sector_sz; }
};

} // namespace oz
