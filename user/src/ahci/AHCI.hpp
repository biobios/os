#include <memory>
#pragma once
#include "BlockDevice.hpp"
#include "AHCIStructures.hpp"


namespace oz {
namespace AHCI {

class Controller;

class AHCIPortDevice : public BlockDevice {
private:
    HBA_PORT volatile* port;
    Controller* controller;
    std::uint8_t port_num;

    // Allocated memory for port
    HBA_CMD_HEADER volatile* cmd_list;
    void volatile* fis_recv;
    HBA_CMD_TBL volatile* cmd_tbl[32]; // Maximum 32 command slots
    
    std::uint32_t sector_size;

    void* dma_buffer;
    std::uint64_t dma_buffer_phys;
    std::size_t dma_buffer_size;

    int findFreeCommandSlot();
    bool waitForPortReady();

public:
    AHCIPortDevice(Controller* ctrl, HBA_PORT volatile* port_ptr, std::uint8_t port_num,
                   HBA_CMD_HEADER volatile* cl = nullptr, void volatile* fb = nullptr);
    ~AHCIPortDevice() override = default;

    bool initialize();

    bool readSectors(std::uint64_t lba, std::uint32_t count, void* buffer) override;
    bool writeSectors(std::uint64_t lba, std::uint32_t count, const void* buffer) override;

    std::uint32_t getSectorSize() const override { return sector_size; }
};

class Controller {
public:
    Controller(std::uint64_t abar_phys);
    ~Controller() = default;

    bool initialize();
    
    // Check ports and return the first valid SATA device
    AHCIPortDevice* getFirstSataDevice();

private:
    std::uint64_t abar_phys_;
    HBA_MEM volatile* abar;
    
    std::unique_ptr<AHCIPortDevice> ports[32];
    std::uint32_t implemented_ports;

    void probePorts();
};

} // namespace AHCI
} // namespace oz
