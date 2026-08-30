#include "drivers/storage/ahci/AHCI.hpp"
#include "utils/utils.hpp"
#include <cstddef>

namespace oz {
namespace AHCI {

// --- Controller ---

Controller::Controller(PCIUtils::PCIFunction pci_func)
    : pci_function_(pci_func), abar(nullptr), implemented_ports(0) {

}

bool Controller::initialize(oz::x86_64::FrameManager& fm) {
    if (!pci_function_) return false;

    // Get ABAR (BAR5)
    std::uint32_t abar_phys = pci_function_.header->BaseAddressRegister[5] & 0xFFFFFFF0;
    if (abar_phys == 0) return false;

    // Convert to virtual address
    auto phys_addr = createPhysicalAddress<std::uint8_t>(abar_phys);
    abar = reinterpret_cast<HBA_MEM volatile*>(phys_to_virt(phys_addr));

    // Enable AHCI mode and reset
    abar->ghc |= 0x80000000; // AE (AHCI Enable)
    
    // Optional: Reset HBA (abar->ghc |= 1, wait for it to clear)
    
    implemented_ports = abar->pi;

    probePorts(fm);
    return true;
}

void Controller::probePorts(oz::x86_64::FrameManager& fm) {
    for (int i = 0; i < 32; i++) {
        if (implemented_ports & (1 << i)) {
            HBA_PORT volatile* port = &abar->ports[i];
            std::uint32_t ssts = port->ssts;
            
            std::uint8_t ipm = (ssts >> 8) & 0x0F;
            std::uint8_t det = ssts & 0x0F;
            
            if (det == 3 && ipm == 1) { // Device present and active
                if (port->sig == SATA_SIG_ATA) {
                    ports[i] = std::impl::make_unique<AHCIPortDevice>(this, port, i);
                    if (!ports[i]->initialize(fm)) {
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

bool AHCIPortDevice::initialize(oz::x86_64::FrameManager& fm) {
    // 1. Stop port
    port->cmd &= ~HBA_PxCMD_ST;
    port->cmd &= ~HBA_PxCMD_FRE;
    
    while (port->cmd & HBA_PxCMD_FR || port->cmd & HBA_PxCMD_CR) {
        // wait
    }

    // 2. Allocate memory for command list
    auto cl_pfd = fm.allocatePages(1);
    auto cl_phys = physical_address_cast<std::uint8_t>(fm.getPhysicalAddress(cl_pfd));
    cmd_list = reinterpret_cast<HBA_CMD_HEADER volatile*>(phys_to_virt(cl_phys));
    oz::utils::memset((void*)cmd_list, 0, 4096);

    port->clb = static_cast<std::uint32_t>(cl_phys.get());
    port->clbu = static_cast<std::uint32_t>(cl_phys.get() >> 32);

    // 3. Allocate memory for FIS receive area
    auto fb_pfd = fm.allocatePages(1);
    auto fb_phys = physical_address_cast<std::uint8_t>(fm.getPhysicalAddress(fb_pfd));
    fis_recv = phys_to_virt(fb_phys);
    oz::utils::memset((void*)fis_recv, 0, 4096);

    port->fb = static_cast<std::uint32_t>(fb_phys.get());
    port->fbu = static_cast<std::uint32_t>(fb_phys.get() >> 32);

    // 4. Allocate memory for command tables
    for (int i = 0; i < 32; i++) {
        cmd_list[i].prdtl = 8; // 8 PRDT entries max per command table
        
        auto ct_pfd = fm.allocatePages(1);
        auto ct_phys = physical_address_cast<std::uint8_t>(fm.getPhysicalAddress(ct_pfd));
        cmd_tbl[i] = reinterpret_cast<HBA_CMD_TBL volatile*>(phys_to_virt(ct_phys));
        oz::utils::memset((void*)cmd_tbl[i], 0, 4096);
        
        cmd_list[i].ctba = static_cast<std::uint32_t>(ct_phys.get());
        cmd_list[i].ctbau = static_cast<std::uint32_t>(ct_phys.get() >> 32);
    }

    // 5. Start port
    while (port->cmd & HBA_PxCMD_CR);
    
    port->cmd |= HBA_PxCMD_FRE;
    port->cmd |= HBA_PxCMD_ST;

    // Allocate DMA bounce buffer (64KB)
    dma_buffer_size = 65536;
    auto dma_pfd = fm.allocatePages(16);
    if (!dma_pfd) return false;
    auto dma_phys = physical_address_cast<std::uint8_t>(fm.getPhysicalAddress(dma_pfd));
    dma_buffer_phys = dma_phys.get();
    dma_buffer = phys_to_virt(dma_phys);


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
