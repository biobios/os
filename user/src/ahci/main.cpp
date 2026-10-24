#include "AHCI.hpp"
#include "syscall.hpp"
#include "ring_buffer.hpp"
#include "../common/ipc/Protocol.hpp"
#include "../common/utils/utils.hpp"

extern "C" void _start() {
    oz::sys_write("AHCI Server started!\n\r");
    
    oz::IPCMessage msg{};
    oz::IPCMessage init_ack{};
    // Block until init sends start signal
    oz::ReplyToken init_tok = oz::sys_reply_recv(oz::INVALID_REPLY_TOKEN, 2, nullptr, &msg);
    if (init_tok == oz::INVALID_REPLY_TOKEN) {
        oz::sys_write("AHCI: Failed to receive start signal from init!\n\r");
        oz::sys_exit(1);
    }
    
    oz::sys_write("Received start signal!\n\r");
    
    // Map ABAR MMIO from device Frame cap (slot 241)
    static constexpr std::uint64_t ABAR_VIRT = 0x200000000000; // Fixed virtual address for MMIO
    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, 241, ABAR_VIRT, 0x10 | 0x01, 4096) != 0) {
        // 0x10 = cache disable, 0x01 = writable
        oz::sys_write("Failed to map AHCI ABAR!\n\r");
        oz::sys_exit(1);
    }
    
    oz::AHCI::Controller ahci(ABAR_VIRT); // Pass virtual address, not physical
    if (!ahci.initialize()) {
        oz::sys_write("AHCI initialization failed!\n\r");
        oz::sys_exit(1);
    }
    
    oz::AHCI::AHCIPortDevice* dev = ahci.getFirstSataDevice();
    if (!dev) {
        oz::sys_write("No SATA device found! Serving error responses.\n\r");
        oz::ReplyToken err_tok = init_tok;
        oz::IPCMessage err_resp{};
        err_resp.data[0] = 1; // error
        oz::IPCMessage err_req{};
        while (true) {
            err_tok = oz::sys_reply_recv(err_tok, 3, (err_tok != oz::INVALID_REPLY_TOKEN) ? &err_resp : nullptr, &err_req);
            if (err_tok == oz::INVALID_REPLY_TOKEN) continue;
            err_resp.data[0] = 1; // error
        }
    }
    
    // Map shared buffer for block I/O
    static constexpr std::uint64_t SHARED_BUF_VIRT = 0x210000000000;
    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, 242, SHARED_BUF_VIRT, 0x01, 16 * 4096) != 0) {
        oz::sys_write("Failed to map shared buffer!\n\r");
        oz::sys_exit(1);
    }

    // Map shared Ring Buffer Frame (slot 244) if available
    static constexpr std::uint64_t RING_BUF_VIRT = 0x220000000000;
    volatile oz::BlockIORing* ring = nullptr;
    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, 244, RING_BUF_VIRT, 0x01, 4096) == 0) {
        ring = reinterpret_cast<volatile oz::BlockIORing*>(RING_BUF_VIRT);
        oz::sys_write("AHCI: Shared Ring Buffer mapped. Notification mode enabled.\n\r");
    }
    
    oz::sys_write("AHCI ready, serving block I/O requests...\n\r");

    static constexpr std::uint32_t NOTIF_AHCI_CAP = 5;
    static constexpr std::uint32_t NOTIF_VFS_CAP = 6;
    static constexpr std::uint64_t BADGE_RING    = 1ULL << 0;
    static constexpr std::uint64_t BADGE_CHANNEL = 1ULL << 1;

    // Bind legacy Channel 3 to Notification so we can wake up for both
    if (ring) {
        oz::sys_channel_bind_notification(3, NOTIF_AHCI_CAP, BADGE_CHANNEL);
    }
    
    // Main request loop
    while (true) {
        if (ring) {
            oz::AsyncBlockReq req{};
            bool got_req = ring->sq.pop(&req);

            if (!got_req) {
                // Ring buffer is empty, wait for notification
                ring->sq.set_need_signal(true);
                got_req = ring->sq.pop(&req);
                if (!got_req) {
                    std::uint64_t badge = 0;
                    oz::sys_notification_wait(NOTIF_AHCI_CAP, &badge);
                    ring->sq.set_need_signal(false);

                    // Check if legacy channel request arrived
                    if (badge & BADGE_CHANNEL) {
                        oz::IPCMessage legacy_req{};
                        oz::IPCMessage legacy_resp{};
                        oz::ReplyToken l_tok = oz::sys_reply_recv(oz::INVALID_REPLY_TOKEN, 3, nullptr, &legacy_req);
                        if (l_tok != oz::INVALID_REPLY_TOKEN) {
                            auto cmd = static_cast<oz::ipc::BlockCmd>(legacy_req.data[0]);
                            if (cmd == oz::ipc::BlockCmd::READ) {
                                void* buf = reinterpret_cast<void*>(SHARED_BUF_VIRT);
                                legacy_resp.data[0] = dev->readSectors(legacy_req.data[1], static_cast<std::uint32_t>(legacy_req.data[2]), buf) ? 0 : 1;
                            } else if (cmd == oz::ipc::BlockCmd::WRITE) {
                                void* buf = reinterpret_cast<void*>(SHARED_BUF_VIRT);
                                legacy_resp.data[0] = dev->writeSectors(legacy_req.data[1], static_cast<std::uint32_t>(legacy_req.data[2]), buf) ? 0 : 1;
                            } else if (cmd == oz::ipc::BlockCmd::GET_INFO) {
                                legacy_resp.data[0] = 0;
                                legacy_resp.data[1] = dev->getSectorSize();
                            } else {
                                legacy_resp.data[0] = static_cast<std::uint64_t>(-1);
                            }
                            oz::sys_reply_recv(l_tok, 0, &legacy_resp, nullptr);
                        }
                    }

                    got_req = ring->sq.pop(&req);
                } else {
                    ring->sq.set_need_signal(false);
                }
            }

            // Process all pending ring requests in batch
            while (got_req) {
                oz::AsyncBlockResp resp{};
                resp.req_id = req.req_id;

                void* buf = reinterpret_cast<void*>(SHARED_BUF_VIRT + req.buf_offset);
                if (req.cmd == static_cast<std::uint32_t>(oz::AsyncBlockCmd::READ)) {
                    bool ok = dev->readSectors(req.lba, req.count, buf);
                    resp.status = ok ? 0 : -1;
                    resp.info = req.count;
                } else if (req.cmd == static_cast<std::uint32_t>(oz::AsyncBlockCmd::WRITE)) {
                    bool ok = dev->writeSectors(req.lba, req.count, buf);
                    resp.status = ok ? 0 : -1;
                    resp.info = req.count;
                } else if (req.cmd == static_cast<std::uint32_t>(oz::AsyncBlockCmd::GET_INFO)) {
                    resp.status = 0;
                    resp.info = dev->getSectorSize();
                } else {
                    resp.status = -1;
                }

                bool need_sig = false;
                ring->cq.push(resp, &need_sig);
                if (need_sig) {
                    oz::sys_notification_signal(NOTIF_VFS_CAP, 1ULL);
                }

                got_req = ring->sq.pop(&req);
            }
        } else {
            // Pure fallback loop for synchronous Channel IPC
            oz::ReplyToken l_tok = init_tok;
            oz::IPCMessage req{};
            oz::IPCMessage resp = init_ack;
            while (true) {
                l_tok = oz::sys_reply_recv(l_tok, 3, (l_tok != oz::INVALID_REPLY_TOKEN) ? &resp : nullptr, &req);
                if (l_tok == oz::INVALID_REPLY_TOKEN) continue;
                
                auto cmd = static_cast<oz::ipc::BlockCmd>(req.data[0]);
                resp = oz::IPCMessage{};
                switch (cmd) {
                    case oz::ipc::BlockCmd::READ: {
                        std::uint64_t lba = req.data[1];
                        std::uint32_t count = static_cast<std::uint32_t>(req.data[2]);
                        void* buf = reinterpret_cast<void*>(SHARED_BUF_VIRT);
                        resp.data[0] = dev->readSectors(lba, count, buf) ? 0 : 1;
                        break;
                    }
                    case oz::ipc::BlockCmd::WRITE: {
                        std::uint64_t lba = req.data[1];
                        std::uint32_t count = static_cast<std::uint32_t>(req.data[2]);
                        void* buf = reinterpret_cast<void*>(SHARED_BUF_VIRT);
                        resp.data[0] = dev->writeSectors(lba, count, buf) ? 0 : 1;
                        break;
                    }
                    case oz::ipc::BlockCmd::GET_INFO: {
                        resp.data[0] = 0;
                        resp.data[1] = dev->getSectorSize();
                        break;
                    }
                    default:
                        resp.data[0] = static_cast<std::uint64_t>(-1);
                        break;
                }
            }
        }
    }
}
