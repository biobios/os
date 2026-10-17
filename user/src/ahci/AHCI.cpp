#include "syscall.hpp"
#include "AHCI.hpp"
#include "../common/utils/utils.hpp"
#include <cstddef>

namespace oz {
namespace AHCI {

// --- Controller ---

Controller::Controller(std::uint64_t abar_phys)
    : abar_phys_(abar_phys), abar(nullptr), implemented_ports(0) {

}

bool Controller::initialize() {
    sys_write("[AHCI] Initializing Controller...\n\r");
    if (abar_phys_ == 0) return false;
    
    // abar_phys_ now holds the virtual address from main.cpp
    abar = reinterpret_cast<HBA_MEM volatile*>(abar_phys_);

    // Enable AHCI mode and reset
    abar->ghc |= 0x80000000; // AE (AHCI Enable)
    
    // Optional: Reset HBA (abar->ghc |= 1, wait for it to clear)
    
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
        // char dbg[64] = "[AHCI] Port X: det=X ipm=X sig=XXXXXXXX\n\r";
        // dbg[12] = '0' + static_cast<char>(i);
        // dbg[19] = '0' + det;
        // dbg[25] = '0' + ipm;
        // const char hx[] = "0123456789ABCDEF";
        // std::uint32_t sig = port->sig;
        // for (int h = 0; h < 8; h++) {
        //     dbg[31 + h] = hx[(sig >> (28 - h * 4)) & 0xF];
        // }
        // sys_write(dbg);
    }

    probePorts();
    return true;
}

void Controller::probePorts() {
    for (int i = 0; i < 32; i++) {
        if (implemented_ports & (1 << i)) {
            HBA_PORT volatile* port = &abar->ports[i];
            
            // Stop command engine
            port->cmd &= ~(0x0001 | 0x0010); // Clear ST and FRE
            // Wait for CR and FR to clear
            for (int t = 0; t < 1000000; t++) {
                if (!(port->cmd & (0x8000 | 0x4000))) break; // CR and FR
                __asm__ volatile("pause");
            }

            // TODO: 物理メモリ割り当て

            // FIS受信の有効化
            port->cmd |= 0x0010; // FRE

            // TODO: SUDとPODに対応しているか確認し有効化する
            
            // Issue COMRESET via SControl
            port->sctl = (port->sctl & ~0x0F) | 0x01; // DET = 1 (COMRESET)
            for (int t = 0; t < 100000; t++) { __asm__ volatile("pause"); } // Small delay
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
                    ports[i] = std::make_unique<AHCIPortDevice>(this, port, i);
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

AHCIPortDevice::AHCIPortDevice(Controller* ctrl, HBA_PORT volatile* port_ptr, std::uint8_t port_num)
    : port(port_ptr), controller(ctrl), port_num(port_num), sector_size(512) {
    cmd_list = nullptr;
    fis_recv = nullptr;
    for (int i = 0; i < 32; i++) cmd_tbl[i] = nullptr;
}

bool AHCIPortDevice::initialize() {
    // 1. Stop port
    port->cmd &= ~HBA_PxCMD_ST;
    port->cmd &= ~HBA_PxCMD_FRE;
    
    while (port->cmd & HBA_PxCMD_FR || port->cmd & HBA_PxCMD_CR) {
        // wait
    }

    // Allocate memory for DMA buffers from untyped capability
    // We will map these into our address space at fixed locations
    // since we don't have a fully general virtual memory manager in AHCI.
    static std::uint64_t bump_virt = 0x300000000000;
    
    // 2. Allocate memory for command list
    std::uint32_t cl_cap = sys_frame_alloc(CAP_UNTYPED, 1);
    if (sys_vspace_map_with_pagetables(CAP_SELF_VSPACE, CAP_UNTYPED, cl_cap, bump_virt, 0x01, 4096) != 0) return false;
    cmd_list = reinterpret_cast<HBA_CMD_HEADER volatile*>(bump_virt);
    oz::utils::memset((void*)cmd_list, 0, 4096);
    
    std::uint64_t cl_phys = sys_frame_phys_addr(cl_cap);
    port->clb = static_cast<std::uint32_t>(cl_phys);
    port->clbu = static_cast<std::uint32_t>(cl_phys >> 32);
    bump_virt += 4096;

    // 3. Allocate memory for FIS receive area
    std::uint32_t fb_cap = sys_frame_alloc(CAP_UNTYPED, 1);
    if (sys_vspace_map_with_pagetables(CAP_SELF_VSPACE, CAP_UNTYPED, fb_cap, bump_virt, 0x01, 4096) != 0) return false;
    fis_recv = reinterpret_cast<void*>(bump_virt);
    oz::utils::memset((void*)fis_recv, 0, 4096);
    
    std::uint64_t fb_phys = sys_frame_phys_addr(fb_cap);
    port->fb = static_cast<std::uint32_t>(fb_phys);
    port->fbu = static_cast<std::uint32_t>(fb_phys >> 32);
    bump_virt += 4096;

    // 4. Allocate memory for command tables
    for (int i = 0; i < 32; i++) {
        cmd_list[i].prdtl = 8; // 8 PRDT entries max per command table
        
        std::uint32_t ct_cap = sys_frame_alloc(CAP_UNTYPED, 1);
        if (sys_vspace_map_with_pagetables(CAP_SELF_VSPACE, CAP_UNTYPED, ct_cap, bump_virt, 0x01, 4096) != 0) return false;
        cmd_tbl[i] = reinterpret_cast<HBA_CMD_TBL volatile*>(bump_virt);
        oz::utils::memset((void*)cmd_tbl[i], 0, 4096);
        
        std::uint64_t ct_phys = sys_frame_phys_addr(ct_cap);
        cmd_list[i].ctba = static_cast<std::uint32_t>(ct_phys);
        cmd_list[i].ctbau = static_cast<std::uint32_t>(ct_phys >> 32);
        bump_virt += 4096;
    }

    // 5. Start port
    while (port->cmd & HBA_PxCMD_CR);
    
    port->cmd |= HBA_PxCMD_FRE;
    port->cmd |= HBA_PxCMD_ST;

    // Allocate DMA bounce buffer (64KB)
    dma_buffer_size = 65536;
    std::uint32_t dma_cap = sys_frame_alloc(CAP_UNTYPED, 16);
    if (!dma_cap) return false;
    if (sys_vspace_map_with_pagetables(CAP_SELF_VSPACE, CAP_UNTYPED, dma_cap, bump_virt, 0x01, 16 * 4096) != 0) return false;
    
    dma_buffer_phys = sys_frame_phys_addr(dma_cap);
    dma_buffer = (void*)bump_virt;
    bump_virt += 16 * 4096;


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
