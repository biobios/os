#include "syscall.hpp"
#include "AHCI.hpp"
#include "../common/utils/utils.hpp"
#include <cstddef>

namespace oz {
namespace AHCI {

// Helper for allocating and mapping DMA memory
struct DmaBuffer {
    void* virt;
    std::uint64_t phys;
};

static DmaBuffer allocDmaPages(std::size_t num_pages) {
    static std::uint64_t bump_virt = 0x300000000000;
    std::uint32_t cap = sys_frame_alloc(CAP_UNTYPED, num_pages);
    if (!cap) return {nullptr, 0};
    if (sys_vspace_map_with_pagetables(CAP_SELF_VSPACE, CAP_UNTYPED, cap, bump_virt, 0x01, num_pages * 4096) != 0) {
        return {nullptr, 0};
    }
    std::uint64_t phys = sys_frame_phys_addr(cap);
    void* virt = reinterpret_cast<void*>(bump_virt);
    oz::utils::memset(virt, 0, num_pages * 4096);
    bump_virt += num_pages * 4096;
    return {virt, phys};
}

// --- Controller ---

Controller::Controller(std::uint64_t abar_phys)
    : abar_phys_(abar_phys), abar(nullptr), implemented_ports(0) {

}

bool Controller::initialize() {
    sys_write("[AHCI] Initializing Controller...\n\r");
    if (abar_phys_ == 0) return false;
    
    // abar_phys_ now holds the virtual address from main.cpp
    abar = reinterpret_cast<HBA_MEM volatile*>(abar_phys_);

    // Enable AHCI mode
    abar->ghc |= 0x80000000; // AE (AHCI Enable)
    
    // Reset HBA (GHC.HR = 1, wait for it to clear)
    abar->ghc |= 0x00000001; // HR
    for (int t = 0; t < 1000000; t++) {
        if (!(abar->ghc & 0x00000001)) break;
        __asm__ volatile("pause");
    }
    
    // Re-enable AHCI mode after reset
    abar->ghc |= 0x80000000; // AE (AHCI Enable)
    
    implemented_ports = abar->pi;

    // Debug
    {
        // print abar_phys_
        char dbg[64] = "[AHCI] ABAR phys=XXXXXXXX\n\r";
        const char hx[] = "0123456789ABCDEF";
        for (int h = 0; h < 8; h++) {
            dbg[20 + h] = hx[(abar_phys_ >> (28 - h * 4)) & 0xF];
        }
        sys_write(dbg);
        // print abar address
        dbg[20] = '0';
        dbg[21] = 'x';
        std::uint64_t abar_addr = reinterpret_cast<std::uint64_t>(abar);
        for (int h = 0; h < 16; h++) {
            dbg[22 + h] = hx[(abar_addr >> (60 - h * 4)) & 0xF];
        }
        sys_write(dbg);
    }

    probePorts();
    return true;
}

void Controller::probePorts() {
    for (int i = 0; i < 32; i++) {
        if (implemented_ports & (1 << i)) {
            HBA_PORT volatile* port = &abar->ports[i];
            
            // Stop command engine
            port->cmd &= ~(HBA_PxCMD_ST | HBA_PxCMD_FRE); // Clear ST and FRE
            // Wait for CR and FR to clear
            for (int t = 0; t < 1000000; t++) {
                if (!(port->cmd & (HBA_PxCMD_CR | HBA_PxCMD_FR))) break; // CR and FR
                __asm__ volatile("pause");
            }

            // SUD (Spin-Up Device) と POD (Power On Device) に対応しているか確認し有効化
            if (abar->cap & (1 << 27)) { // CAP.SSS (Staggered Spin-up Supported)
                port->cmd |= HBA_PxCMD_SUD;
            }
            if (abar->cap & (1 << 17)) { // CAP.CPD (Cold Presence Detect Supported)
                port->cmd |= HBA_PxCMD_POD;
            }

            // 物理メモリ割り当て (Command List: 1 page, Received FIS: 1 page)
            DmaBuffer cl_buf = allocDmaPages(1);
            DmaBuffer fb_buf = allocDmaPages(1);
            if (!cl_buf.virt || !fb_buf.virt) {
                char dbg[64] = "[AHCI] Port X: failed to allocate DMA memory!\n\r";
                dbg[12] = '0' + static_cast<char>(i);
                sys_write(dbg);
                continue;
            }

            port->clb = static_cast<std::uint32_t>(cl_buf.phys);
            port->clbu = static_cast<std::uint32_t>(cl_buf.phys >> 32);
            port->fb = static_cast<std::uint32_t>(fb_buf.phys);
            port->fbu = static_cast<std::uint32_t>(fb_buf.phys >> 32);

            // SERR と割り込みステータスをクリア
            port->serr = 0xFFFFFFFF;
            port->is = 0xFFFFFFFF;

            // FIS受信の有効化 (PxFB/PxFBU 設定後に有効化)
            port->cmd |= HBA_PxCMD_FRE;
            
            // Issue COMRESET via SControl
            port->sctl = (port->sctl & ~0x0F) | 0x01; // DET = 1 (COMRESET)
            for (int t = 0; t < 100000; t++) { __asm__ volatile("pause"); } // Small delay (at least 1ms)
            port->sctl &= ~0x0F; // DET = 0 (No action)
            
            // Wait for device detection
            for (int t = 0; t < 1000000; t++) {
                std::uint32_t ssts = port->ssts;
                std::uint8_t det = ssts & 0x0F;
                if (det == 3) break;
                __asm__ volatile("pause");
            }

            std::uint32_t ssts = port->ssts;
            std::uint8_t ipm = (ssts >> 8) & 0x0F;
            std::uint8_t det = ssts & 0x0F;

            // Debug: print port status
            {
                char dbg[64] = "[AHCI] Port X: det=X ipm=X sig=XXXXXXXX\n\r";
                dbg[12] = '0' + static_cast<char>(i);
                dbg[19] = '0' + det;
                dbg[25] = '0' + ipm;
                const char hx[] = "0123456789ABCDEF";
                std::uint32_t sig = port->sig;
                for (int h = 0; h < 8; h++) {
                    dbg[31 + h] = hx[(sig >> (28 - h * 4)) & 0xF];
                }
                sys_write(dbg);
            }
            
            if (det == 3 && ipm == 1) { // Device present and active
                std::uint32_t sig = port->sig;
                if (sig == SATA_SIG_ATAPI) {
                    char dbg[64] = "[AHCI] Port X is ATAPI device, skipping.\n\r";
                    dbg[12] = '0' + static_cast<char>(i);
                    sys_write(dbg);
                    continue;
                }

                bool ready = false;
                for (int t = 0; t < 100000; t++) {
                    port->serr = 0xFFFFFFFF; // Clear SError
                    std::uint32_t tfd = port->tfd;
                    if ((tfd & (0x80 | 0x08)) == 0) {
                        ready = true;
                        break; // Not busy and no error
                    }
                    __asm__ volatile("pause");
                }
                if (!ready) {
                    char dbg[64] = "[AHCI] Port X busy timeout, skipping.\n\r";
                    dbg[12] = '0' + static_cast<char>(i);
                    sys_write(dbg);
                    continue;
                }

                // print: sig
                sig = port->sig;
                {
                    char dbg[64] = "[AHCI] Port X: sig=XXXXXXXX\n\r";
                    dbg[12] = '0' + static_cast<char>(i);
                    const char hx[] = "0123456789ABCDEF";
                    for (int h = 0; h < 8; h++) {
                        dbg[19 + h] = hx[(sig >> (28 - h * 4)) & 0xF];
                    }
                    sys_write(dbg);
                }
                if (sig == SATA_SIG_ATA) {
                    ports[i] = std::make_unique<AHCIPortDevice>(
                        this, port, i,
                        reinterpret_cast<HBA_CMD_HEADER volatile*>(cl_buf.virt),
                        reinterpret_cast<void volatile*>(fb_buf.virt)
                    );
                    if (!ports[i]->initialize()) {
                        ports[i].reset();
                    }
                }
            }
        }
    }
}

AHCIPortDevice* Controller::getFirstSataDevice() {
    for (int i = 0; i < 32; i++) {
        if (ports[i]) {
            return ports[i].get();
        }
    }
    return nullptr;
}

// --- AHCIPortDevice ---

AHCIPortDevice::AHCIPortDevice(Controller* ctrl, HBA_PORT volatile* port_ptr, std::uint8_t port_num,
                               HBA_CMD_HEADER volatile* cl, void volatile* fb)
    : port(port_ptr), controller(ctrl), port_num(port_num),
      cmd_list(cl), fis_recv(fb), sector_size(512),
      dma_buffer(nullptr), dma_buffer_phys(0), dma_buffer_size(0) {
    for (int i = 0; i < 32; i++) cmd_tbl[i] = nullptr;
}

bool AHCIPortDevice::initialize() {
    // 1. Allocate memory for command list if not already allocated
    if (!cmd_list) {
        DmaBuffer cl = allocDmaPages(1);
        if (!cl.virt) return false;
        cmd_list = reinterpret_cast<HBA_CMD_HEADER volatile*>(cl.virt);
        port->clb = static_cast<std::uint32_t>(cl.phys);
        port->clbu = static_cast<std::uint32_t>(cl.phys >> 32);
    }

    // 2. Allocate memory for FIS receive area if not already allocated
    if (!fis_recv) {
        DmaBuffer fb = allocDmaPages(1);
        if (!fb.virt) return false;
        fis_recv = reinterpret_cast<void volatile*>(fb.virt);
        port->fb = static_cast<std::uint32_t>(fb.phys);
        port->fbu = static_cast<std::uint32_t>(fb.phys >> 32);
    }

    // 3. Allocate memory for command tables
    for (int i = 0; i < 32; i++) {
        cmd_list[i].prdtl = 8; // 8 PRDT entries max per command table
        
        DmaBuffer ct = allocDmaPages(1);
        if (!ct.virt) return false;
        cmd_tbl[i] = reinterpret_cast<HBA_CMD_TBL volatile*>(ct.virt);
        
        cmd_list[i].ctba = static_cast<std::uint32_t>(ct.phys);
        cmd_list[i].ctbau = static_cast<std::uint32_t>(ct.phys >> 32);
    }

    // 4. Start port command engine
    while (port->cmd & HBA_PxCMD_CR);
    
    port->cmd |= HBA_PxCMD_FRE;
    port->cmd |= HBA_PxCMD_ST;

    // 5. Allocate DMA bounce buffer (64KB)
    dma_buffer_size = 65536;
    DmaBuffer dma = allocDmaPages(16);
    if (!dma.virt) return false;
    
    dma_buffer = dma.virt;
    dma_buffer_phys = dma.phys;

    return true;
}

int AHCIPortDevice::findFreeCommandSlot() {
    std::uint32_t slots = (port->sact | port->ci);
    for (int i = 0; i < 32; i++) {
        if ((slots & (1 << i)) == 0) return i;
    }
    return -1;
}

bool AHCIPortDevice::readSectors(std::uint64_t lba, std::uint32_t count, void* buffer) {
    port->is = static_cast<std::uint32_t>(-1); // Clear pending interrupt bits
    
    int slot = findFreeCommandSlot();
    if (slot == -1) return false;

    HBA_CMD_HEADER volatile* cmdheader = &cmd_list[slot];
    cmdheader->cfl = sizeof(FIS_REG_H2D) / sizeof(std::uint32_t); // Command FIS size
    cmdheader->w = 0; // Read
    cmdheader->prdtl = 1; // Only 1 PRDT entry for now
    
    HBA_CMD_TBL volatile* cmdtbl = cmd_tbl[slot];
    oz::utils::memset((void*)cmdtbl, 0, sizeof(HBA_CMD_TBL) + sizeof(HBA_PRDT_ENTRY));
    
    // Set up PRDT using DMA bounce buffer
    if (count * sector_size > dma_buffer_size) return false;
    
    cmdtbl->prdt_entry[0].dba = static_cast<std::uint32_t>(dma_buffer_phys);
    cmdtbl->prdt_entry[0].dbau = static_cast<std::uint32_t>(dma_buffer_phys >> 32);
    cmdtbl->prdt_entry[0].dbc = (count * sector_size) - 1; // 0-indexed byte count
    cmdtbl->prdt_entry[0].i = 1;
    
    // Set up Command FIS
    FIS_REG_H2D* cmdfis = reinterpret_cast<FIS_REG_H2D*>((void*)cmdtbl->cfis);
    
    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c = 1; // Command
    cmdfis->command = ATA_CMD_READ_DMA_EX;
    
    cmdfis->lba0 = static_cast<std::uint8_t>(lba);
    cmdfis->lba1 = static_cast<std::uint8_t>(lba >> 8);
    cmdfis->lba2 = static_cast<std::uint8_t>(lba >> 16);
    cmdfis->device = 1 << 6; // LBA mode
    
    cmdfis->lba3 = static_cast<std::uint8_t>(lba >> 24);
    cmdfis->lba4 = static_cast<std::uint8_t>(lba >> 32);
    cmdfis->lba5 = static_cast<std::uint8_t>(lba >> 40);
    
    cmdfis->countl = count & 0xFF;
    cmdfis->counth = (count >> 8) & 0xFF;
    
    // Wait until port is no longer busy before issuing command
    int spin = 0;
    while ((port->tfd & (0x80 | 0x08)) && spin < 1000000) {
        spin++;
    }
    if (spin == 1000000) {
        return false;
    }
    
    port->ci = 1 << slot; // Issue command
    
    // Wait for completion
    while (1) {
        if ((port->ci & (1 << slot)) == 0) {
            break;
        }
        if (port->is & (1 << 30)) { // Task file error
            return false;
        }
    }
    
    if (port->is & (1 << 30)) { // Check error one last time
        return false;
    }
    
    
    oz::utils::memcpy(buffer, dma_buffer, count * sector_size);
    return true;
}

bool AHCIPortDevice::writeSectors(std::uint64_t lba, std::uint32_t count, const void* buffer) {
    port->is = static_cast<std::uint32_t>(-1); // Clear pending interrupt bits
    
    int slot = findFreeCommandSlot();
    if (slot == -1) return false;

    HBA_CMD_HEADER volatile* cmdheader = &cmd_list[slot];
    cmdheader->cfl = sizeof(FIS_REG_H2D) / sizeof(std::uint32_t); // Command FIS size
    cmdheader->w = 1; // Write
    cmdheader->prdtl = 1; // Only 1 PRDT entry for now
    
    HBA_CMD_TBL volatile* cmdtbl = cmd_tbl[slot];
    oz::utils::memset((void*)cmdtbl, 0, sizeof(HBA_CMD_TBL) + sizeof(HBA_PRDT_ENTRY));
    
    // Set up PRDT using DMA bounce buffer
    if (count * sector_size > dma_buffer_size) return false;
    oz::utils::memcpy(dma_buffer, buffer, count * sector_size);
    
    cmdtbl->prdt_entry[0].dba = static_cast<std::uint32_t>(dma_buffer_phys);
    cmdtbl->prdt_entry[0].dbau = static_cast<std::uint32_t>(dma_buffer_phys >> 32);
    cmdtbl->prdt_entry[0].dbc = (count * sector_size) - 1; // 0-indexed byte count
    cmdtbl->prdt_entry[0].i = 1;
    
    // Set up Command FIS
    FIS_REG_H2D* cmdfis = reinterpret_cast<FIS_REG_H2D*>((void*)cmdtbl->cfis);
    
    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c = 1; // Command
    cmdfis->command = ATA_CMD_WRITE_DMA_EX;
    
    cmdfis->lba0 = static_cast<std::uint8_t>(lba);
    cmdfis->lba1 = static_cast<std::uint8_t>(lba >> 8);
    cmdfis->lba2 = static_cast<std::uint8_t>(lba >> 16);
    cmdfis->device = 1 << 6; // LBA mode
    
    cmdfis->lba3 = static_cast<std::uint8_t>(lba >> 24);
    cmdfis->lba4 = static_cast<std::uint8_t>(lba >> 32);
    cmdfis->lba5 = static_cast<std::uint8_t>(lba >> 40);
    
    cmdfis->countl = count & 0xFF;
    cmdfis->counth = (count >> 8) & 0xFF;
    
    // Wait until port is no longer busy before issuing command
    int spin = 0;
    while ((port->tfd & (0x80 | 0x08)) && spin < 1000000) {
        spin++;
    }
    if (spin == 1000000) {
        return false;
    }
    
    port->ci = 1 << slot; // Issue command
    
    // Wait for completion
    while (1) {
        if ((port->ci & (1 << slot)) == 0) {
            break;
        }
        if (port->is & (1 << 30)) { // Task file error
            return false;
        }
    }
    
    if (port->is & (1 << 30)) { // Check error one last time
        return false;
    }
    
    
    return true;
}

} // namespace AHCI
} // namespace oz
