#pragma once
#include <cstdint>

namespace oz {
namespace AHCI {

#pragma pack(push, 1)

// Command List Structure
struct HBA_CMD_HEADER {
    std::uint8_t  cfl:5;    // Command FIS length in DWORDS, 2 ~ 16
    std::uint8_t  a:1;      // ATAPI
    std::uint8_t  w:1;      // Write, 1: H2D, 0: D2H
    std::uint8_t  p:1;      // Prefetchable
    
    std::uint8_t  r:1;      // Reset
    std::uint8_t  b:1;      // BIST
    std::uint8_t  c:1;      // Clear busy upon R_OK
    std::uint8_t  rsv0:1;   // Reserved
    std::uint8_t  pmp:4;    // Port multiplier port
    
    std::uint16_t prdtl;    // Physical region descriptor table length in entries
    
    std::uint32_t prdbc;    // Physical region descriptor byte count transferred
    
    std::uint32_t ctba;     // Command table descriptor base address
    std::uint32_t ctbau;    // Command table descriptor base address upper 32 bits
    
    std::uint32_t rsv1[4];  // Reserved
};

// Physical Region Descriptor Table Entry
struct HBA_PRDT_ENTRY {
    std::uint32_t dba;      // Data base address
    std::uint32_t dbau;     // Data base address upper 32 bits
    std::uint32_t rsv0;     // Reserved
    
    std::uint32_t dbc:22;   // Byte count, 4M max
    std::uint32_t rsv1:9;   // Reserved
    std::uint32_t i:1;      // Interrupt on completion
};

// Command Table
struct HBA_CMD_TBL {
    std::uint8_t  cfis[64]; // Command FIS
    std::uint8_t  acmd[16]; // ATAPI command, 12 or 16 bytes
    std::uint8_t  rsv[48];  // Reserved
    
    HBA_PRDT_ENTRY prdt_entry[1]; // PRDT entries, up to 65535
};

// FIS Types
enum FIS_TYPE {
    FIS_TYPE_REG_H2D    = 0x27, // Register FIS - host to device
    FIS_TYPE_REG_D2H    = 0x34, // Register FIS - device to host
    FIS_TYPE_DMA_ACT    = 0x39, // DMA activate FIS - device to host
    FIS_TYPE_DMA_SETUP  = 0x41, // DMA setup FIS - bidirectional
    FIS_TYPE_DATA       = 0x46, // Data FIS - bidirectional
    FIS_TYPE_BIST       = 0x58, // BIST activate FIS - bidirectional
    FIS_TYPE_PIO_SETUP  = 0x5F, // PIO setup FIS - device to host
    FIS_TYPE_DEV_BITS   = 0xA1, // Set device bits FIS - device to host
};

// Register FIS - Host to Device
struct FIS_REG_H2D {
    std::uint8_t  fis_type; // FIS_TYPE_REG_H2D
    std::uint8_t  pmport:4; // Port multiplier
    std::uint8_t  rsv0:3;   // Reserved
    std::uint8_t  c:1;      // 1: Command, 0: Control
    std::uint8_t  command;  // Command register
    std::uint8_t  featurel; // Feature register, 7:0
    
    std::uint8_t  lba0;     // LBA low register, 7:0
    std::uint8_t  lba1;     // LBA mid register, 15:8
    std::uint8_t  lba2;     // LBA high register, 23:16
    std::uint8_t  device;   // Device register
    
    std::uint8_t  lba3;     // LBA register, 31:24
    std::uint8_t  lba4;     // LBA register, 39:32
    std::uint8_t  lba5;     // LBA register, 47:40
    std::uint8_t  featureh; // Feature register, 15:8
    
    std::uint8_t  countl;   // Count register, 7:0
    std::uint8_t  counth;   // Count register, 15:8
    std::uint8_t  icc;      // Isochronous command completion
    std::uint8_t  control;  // Control register
    
    std::uint8_t  rsv1[4];  // Reserved
};

// Port Registers
struct HBA_PORT {
    std::uint32_t clb;      // Command list base address, 1K-byte aligned
    std::uint32_t clbu;     // Command list base address upper 32 bits
    std::uint32_t fb;       // FIS base address, 256-byte aligned
    std::uint32_t fbu;      // FIS base address upper 32 bits
    std::uint32_t is;       // Interrupt status
    std::uint32_t ie;       // Interrupt enable
    std::uint32_t cmd;      // Command and status
    std::uint32_t rsv0;     // Reserved
    std::uint32_t tfd;      // Task file data
    std::uint32_t sig;      // Signature
    std::uint32_t ssts;     // SATA status (SCR0:SStatus)
    std::uint32_t sctl;     // SATA control (SCR2:SControl)
    std::uint32_t serr;     // SATA error (SCR1:SError)
    std::uint32_t sact;     // SATA active (SCR3:SActive)
    std::uint32_t ci;       // Command issue
    std::uint32_t sntf;     // SATA notification (SCR4:SNotification)
    std::uint32_t fbs;      // FIS-based switch control
    std::uint32_t rsv1[11]; // Reserved
    std::uint32_t vendor[4]; // Vendor specific
};

// HBA Memory Registers
struct HBA_MEM {
    std::uint32_t cap;      // Host capability
    std::uint32_t ghc;      // Global host control
    std::uint32_t is;       // Interrupt status
    std::uint32_t pi;       // Ports implemented
    std::uint32_t vs;       // Version
    std::uint32_t ccc_ctl;  // Command completion coalescing control
    std::uint32_t ccc_pts;  // Command completion coalescing ports
    std::uint32_t em_loc;   // Enclosure management location
    std::uint32_t em_ctl;   // Enclosure management control
    std::uint32_t cap2;     // Host capabilities extended
    std::uint32_t bohc;     // BIOS/OS handoff control and status
    
    std::uint8_t  rsv[0xA0-0x2C];
    
    std::uint8_t  vendor[0x100-0xA0];
    
    HBA_PORT ports[32];     // Port control registers
};

#pragma pack(pop)

// Signature constants
constexpr std::uint32_t SATA_SIG_ATA   = 0x00000101; // SATA drive
constexpr std::uint32_t SATA_SIG_ATAPI = 0xEB140101; // SATAPI drive
constexpr std::uint32_t SATA_SIG_SEMB  = 0xC33C0101; // Enclosure management bridge
constexpr std::uint32_t SATA_SIG_PM    = 0x96690101; // Port multiplier

// Command register flags
constexpr std::uint32_t HBA_PxCMD_ST   = 0x0001; // Start
constexpr std::uint32_t HBA_PxCMD_SUD  = 0x0002; // Spin up device
constexpr std::uint32_t HBA_PxCMD_POD  = 0x0004; // Power on device
constexpr std::uint32_t HBA_PxCMD_CLO  = 0x0008; // Command list override
constexpr std::uint32_t HBA_PxCMD_FRE  = 0x0010; // FIS receive enable
constexpr std::uint32_t HBA_PxCMD_FR   = 0x4000; // FIS receive running
constexpr std::uint32_t HBA_PxCMD_CR   = 0x8000; // Command list running

// ATA commands
constexpr std::uint8_t ATA_CMD_READ_DMA_EX = 0x25;
constexpr std::uint8_t ATA_CMD_WRITE_DMA_EX = 0x35;

} // namespace AHCI
} // namespace oz
