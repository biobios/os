#pragma once

#include "PCI.hpp"
#include "PCIe.hpp"
#include "Address.hpp"
#include <cstdint>
#include <functional>

namespace PCIUtils {

class PCIFunction {
public:
    PCI::PCIConfigurationHeaderCommon volatile* header;

    PCIFunction(PCI::PCIConfigurationHeaderCommon volatile* header = nullptr) : header(header) {}

    bool isAvailable() const;
    bool isMultifunction() const;
    bool isBridge() const;
    std::uint8_t getSecondaryBusNumber() const;
    bool equalsCode(std::uint8_t baseClass, std::uint8_t subClass, std::uint8_t interface) const;
    bool equalsCode(std::uint8_t baseClass, std::uint8_t subClass) const;
    bool equalsCode(std::uint8_t baseClass) const;
    bool findCapability(std::uint8_t capabilityID, std::uint8_t* capabilityOffset) const;

    explicit operator bool() const { return header != nullptr; }
};

class MemorymappedConfigurationSpaceWrapper : public oz::PhysicalAddressProvider {
    PCIe::MemorymappedConfigurationSpaceDescriptionTable* mcfg_desc_table;
public:
    MemorymappedConfigurationSpaceWrapper(PCIe::MemorymappedConfigurationSpaceDescriptionTable* mcfg);
    PCIFunction getFunction(std::uint8_t busNumber, std::uint8_t deviceNumber, std::uint8_t funcNumber);
    PCIFunction findFunction(std::uint8_t baseClass, std::uint8_t subClass, std::uint8_t interface);

    void visitBus(std::uint8_t busNumber, std::function<void(PCIFunction)> visitor);
    void visitDevice(std::uint8_t busNumber, std::uint8_t deviceNumber, std::function<void(PCIFunction)> visitor);
    void visitFunction(std::uint8_t busNumber, std::uint8_t deviceNumber, std::uint8_t funcNumber, std::function<void(PCIFunction)> visitor);
    void visitAllFunctions(std::function<void(PCIFunction)> visitor);
};

class MSICapabilityWrapper {
    PCIFunction function;
    std::uint8_t capabilityOffset;
public:
    MSICapabilityWrapper(PCIFunction function, std::uint8_t capabilityOffset);
    bool is64BitAddress();
    bool isPerVectorMasking();
    void enable();
    void disable();
    void setMessageAddress(std::uint64_t address);
    void setMessageData(std::uint16_t data);
};

class MSIXCapabilityWrapper {
    PCIFunction function;
    std::uint8_t capabilityOffset;
    PCI::MSIX::TableEntry volatile* table;
    PCI::MSIX::PBAEntry volatile* pba;
public:
    MSIXCapabilityWrapper(PCIFunction function, std::uint8_t capabilityOffset);
    void enable();
    void disable();
    void setEntry(std::uint16_t index, std::uint32_t vectorControl, std::uint32_t messageData, std::uint64_t messageAddress);
    std::uint16_t getTableSize();
};
}  // namespace PCIUtils