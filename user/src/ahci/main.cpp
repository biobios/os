#include "AHCI.hpp"
#include "syscall.hpp"
#include "../common/ipc/Protocol.hpp"
#include "../common/utils/utils.hpp"

extern "C" void _start() {
    oz::sys_write("AHCI Server started!\n\r");
    
    oz::IPCMessage msg{};
    // Block until init sends start signal (AHCI MMIO address was for old design)
    oz::sys_channel_recv(2, &msg);
    
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
        while (true) {
            oz::IPCMessage req{};
            if (!oz::sys_channel_recv(3, &req)) continue;
            oz::IPCMessage resp{};
            resp.data[0] = 1; // error
            oz::sys_channel_send(4, &resp);
        }
    }
    
    // Map shared buffer for block I/O
    static constexpr std::uint64_t SHARED_BUF_VIRT = 0x210000000000;
    if (oz::sys_vspace_map_with_pagetables(oz::CAP_SELF_VSPACE, oz::CAP_UNTYPED, 242, SHARED_BUF_VIRT, 0x01, 16 * 4096) != 0) {
        oz::sys_write("Failed to map shared buffer!\n\r");
        oz::sys_exit(1);
    }
    
    oz::sys_write("AHCI ready, serving block I/O requests...\n\r");
    
    // Main request loop - receive block I/O requests on cap slot 3
    while (true) {
        oz::IPCMessage req{};
        if (!oz::sys_channel_recv(3, &req)) continue;
        
        auto cmd = static_cast<oz::ipc::BlockCmd>(req.data[0]);
        oz::IPCMessage resp{};
        
        switch (cmd) {
            case oz::ipc::BlockCmd::READ: {
                std::uint64_t lba = req.data[1];
                std::uint32_t count = static_cast<std::uint32_t>(req.data[2]);
                
                void* buf = reinterpret_cast<void*>(SHARED_BUF_VIRT);
                if (dev->readSectors(lba, count, buf)) {
                    resp.data[0] = 0; // success
                } else {
                    resp.data[0] = 1; // error
                }
                break;
            }
            case oz::ipc::BlockCmd::WRITE: {
                std::uint64_t lba = req.data[1];
                std::uint32_t count = static_cast<std::uint32_t>(req.data[2]);
                
                void* buf = reinterpret_cast<void*>(SHARED_BUF_VIRT);
                if (dev->writeSectors(lba, count, buf)) {
                    resp.data[0] = 0; // success
                } else {
                    resp.data[0] = 1; // error
                }
                break;
            }
            case oz::ipc::BlockCmd::GET_INFO: {
                resp.data[0] = 0; // success
                resp.data[1] = dev->getSectorSize();
                break;
            }
            default:
                resp.data[0] = static_cast<std::uint64_t>(-1); // unknown command
                break;
        }
        
        // Send response back on cap slot 4 (response channel)
        oz::sys_channel_send(4, &resp);
    }
}
