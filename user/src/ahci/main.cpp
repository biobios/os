#include "AHCI.hpp"
#include "syscall.hpp"
#include "ring_buffer.hpp"
#include "../common/ipc/Protocol.hpp"
#include "../common/ipc/StartupInfo.hpp"
#include "../common/utils/utils.hpp"

OZ_MANIFEST((oz::ProcessManifest{
    .magic = oz::ProcessManifest::MAGIC,
    .version = 1,
    .provides = static_cast<std::uint32_t>(oz::ServiceId::BLOCK_DEV),
    .required = static_cast<std::uint32_t>(oz::ServiceId::VFS_ROOT),
    .device_needed = oz::DeviceRequirement::AHCI,
    .needs_untyped = true,
    .default_data_dir = {}
}));

static void to_hex_str(std::uint64_t val, char* out) {
    const char* hex = "0123456789ABCDEF";
    for (int i = 15; i >= 0; --i) {
        out[i] = hex[val & 0xF];
        val >>= 4;
    }
    out[16] = '\0';
}

OZ_PROCESS_ENTRY() {
    oz::LocalNamespace::init(info);
    oz::sys_write("AHCI Server started!\n\r");

    std::uint32_t drivman_cap = oz::LocalNamespace::get_cap(oz::CapRole::DRIVMAN);
    std::uint32_t vfs_cap = oz::LocalNamespace::get_cap(oz::CapRole::ROOT_DIR);
    std::uint32_t mmio_cap = 241;

    oz::ReplyToken init_tok = oz::INVALID_REPLY_TOKEN;
    oz::IPCMessage init_ack{};

    std::uint64_t reconnect_token = 0;
    std::uint16_t instance_id = 0;

    if (drivman_cap != 0) {
        // Phase 2: Driver Session Handshake
        oz::IPCMessage sess_req{};
        sess_req.data[0] = static_cast<std::uint64_t>(oz::ipc::DrivCmd::INIT_DEVICE_SESSION);
        oz::IPCMessage sess_resp{};
        if (oz::sys_call(drivman_cap, &sess_req, &sess_resp) && sess_resp.data[0] == 0) {
            instance_id = static_cast<std::uint16_t>((sess_resp.data[1] >> 16) & 0xFFFF);
            reconnect_token = sess_resp.data[3];
            oz::sys_write("[AHCI] Initialized device session with DrivMan.\n\r");

            // Request MMIO resource (res_type = 1, res_idx = 0)
            oz::IPCMessage res_req{};
            res_req.data[0] = static_cast<std::uint64_t>(oz::ipc::DrivCmd::GET_DEVICE_RESOURCE);
            res_req.data[1] = 1; // MMIO_BAR
            res_req.data[2] = 0; // ABAR
            oz::IPCMessage res_resp{};
            if (oz::sys_call_recv_cap(drivman_cap, &res_req, &res_resp) && res_resp.data[0] == 0 && res_resp.has_cap) {
                mmio_cap = res_resp.transferred_cap;
                oz::sys_write("[AHCI] Obtained ABAR MMIO frame from DrivMan.\n\r");
            } else {
                oz::sys_write("[AHCI] Failed to obtain ABAR MMIO from DrivMan!\n\r");
                oz::sys_exit(1);
            }
        } else {
            // Fallback to legacy GET_DEVICE_CAP
            oz::IPCMessage req{};
            req.data[0] = static_cast<std::uint64_t>(oz::ipc::DrivCmd::GET_DEVICE_CAP);
            const char* dname = "ahci";
            for (int i = 0; i < 4; ++i) reinterpret_cast<char*>(&req.data[1])[i] = dname[i];
            oz::IPCMessage resp{};
            if (oz::sys_call_recv_cap(drivman_cap, &req, &resp) && resp.data[0] == 0 && resp.has_cap) {
                mmio_cap = resp.transferred_cap;
                oz::sys_write("[AHCI] Obtained MMIO frame from DrivMan (legacy).\n\r");
            } else {
                oz::sys_write("[AHCI] Failed to obtain MMIO from DrivMan!\n\r");
                oz::sys_exit(1);
            }
        }
    } else {
        // Standalone test fallback: wait for start signal from init
        oz::IPCMessage msg{};
        init_tok = oz::sys_reply_recv(oz::INVALID_REPLY_TOKEN, 2, nullptr, &msg);
        if (init_tok == oz::INVALID_REPLY_TOKEN) {
            oz::sys_write("AHCI: Failed to receive start signal from init!\n\r");
            oz::sys_exit(1);
        }
        oz::sys_write("Received start signal!\n\r");
    }
    
    // Map ABAR MMIO from device Frame cap
    static constexpr std::uint64_t ABAR_VIRT = 0x200000000000; // Fixed virtual address for MMIO
    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, mmio_cap, ABAR_VIRT, 0x10 | 0x01, 4096) != 0) {
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
    std::uint32_t shared_buf_pt_cap = oz::INVALID_CAP_ID;

    // Check if slot 242 is already present (e.g. in vtest)
    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, 242, SHARED_BUF_VIRT, 0x01, 16 * 4096) == 0) {
        shared_buf_pt_cap = 242;
    } else {
        // Allocate 16 pages for shared block buffer as a shared Level 1 PageTable
        shared_buf_pt_cap = oz::sys_alloc_shared_pt(oz::CAP_UNTYPED, 16, 0x01);
        if (shared_buf_pt_cap != oz::INVALID_CAP_ID) {
            oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, shared_buf_pt_cap, SHARED_BUF_VIRT, 0x01, 16 * 4096);
        }
    }

    // Dynamic registration with VFS if VFS channel is provided
    std::uint32_t block_recv_cap = 3;
    if (vfs_cap != 0) {
        std::uint32_t blk_send = 0, blk_recv = 0;
        if (oz::sys_channel_create(oz::CAP_UNTYPED, &blk_send, &blk_recv)) {
            block_recv_cap = blk_recv;
            oz::IPCMessage reg_req{};
            reg_req.data[0] = static_cast<std::uint64_t>(oz::ipc::VfsCmd::REGISTER_BLOCK_DEV);
            const char* sname = "sata0";
            for (int i = 0; i < 5; ++i) reinterpret_cast<char*>(&reg_req.data[1])[i] = sname[i];
            reg_req.data[2] = dev ? dev->getSectorSize() : 512;
            reg_req.data[3] = reconnect_token;
            reg_req.has_cap = true;
            reg_req.transferred_cap = blk_send;
            oz::IPCMessage reg_resp{};
            if (oz::sys_call(vfs_cap, &reg_req, &reg_resp) && reg_resp.data[0] == 0) {
                oz::sys_write("[AHCI] Registered block device 'sata0' with VFS.\n\r");
            } else {
                oz::sys_write("[AHCI] WARNING: REGISTER_BLOCK_DEV returned non-zero.\n\r");
            }

            if (drivman_cap != 0) {
                oz::IPCMessage on_req{};
                on_req.data[0] = static_cast<std::uint64_t>(oz::ipc::DrivCmd::NOTIFY_DEVICE_ONLINE);
                oz::IPCMessage on_resp{};
                oz::sys_call(drivman_cap, &on_req, &on_resp);
            }
        }
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
                                if (shared_buf_pt_cap != oz::INVALID_CAP_ID) {
                                    std::uint32_t minted = oz::sys_cap_mint(shared_buf_pt_cap, static_cast<std::uint32_t>(oz::CapRights::ALL));
                                    if (minted != oz::INVALID_CAP_ID) {
                                        legacy_resp.has_cap = true;
                                        legacy_resp.transferred_cap = minted;
                                    }
                                }
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
                l_tok = oz::sys_reply_recv(l_tok, block_recv_cap, (l_tok != oz::INVALID_REPLY_TOKEN) ? &resp : nullptr, &req);
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
                        if (shared_buf_pt_cap != oz::INVALID_CAP_ID) {
                            std::uint32_t minted = oz::sys_cap_mint(shared_buf_pt_cap, static_cast<std::uint32_t>(oz::CapRights::ALL));
                            if (minted != oz::INVALID_CAP_ID) {
                                resp.has_cap = true;
                                resp.transferred_cap = minted;
                            }
                        }
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
