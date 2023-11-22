#pragma once

#include "PCI.hpp"
#include "PCIe.hpp"
#include <functional>
#include <cstdint>
#include <new>

namespace PCIUtils {
class PCIFunctionConfigurationSpaceWrapper{
public:
    virtual std::uint8_t read8(std::uint8_t offset) = 0;
    virtual void write8(std::uint8_t offset, std::uint8_t value) = 0;
    virtual std::uint16_t read16(std::uint8_t offset) = 0;
    virtual void write16(std::uint8_t offset, std::uint16_t value) = 0;
    virtual std::uint32_t read32(std::uint8_t offset) = 0;
    virtual void write32(std::uint8_t offset, std::uint32_t value) = 0;
    virtual PCIFunctionConfigurationSpaceWrapper* clone() = 0;
    virtual bool findCapability(std::uint8_t capabilityID, std::uint8_t* capabilityOffset) = 0;
    bool isAvailable();
    bool isMultifunction();
    bool isBridge();
    std::uint8_t getSecondaryBusNumber();
    bool equalsCode(std::uint8_t baseClass, std::uint8_t subClass, std::uint8_t interface);
    bool equalsCode(std::uint8_t baseClass, std::uint8_t subClass);
    bool equalsCode(std::uint8_t baseClass);
    virtual ~PCIFunctionConfigurationSpaceWrapper() = default;
};

class MemorymappedFunctionConfigurationSpaceWrapper : virtual public PCIFunctionConfigurationSpaceWrapper{
protected:
    PCI::PCIConfigurationHeaderCommon volatile* configCommonPtr;
public:
    MemorymappedFunctionConfigurationSpaceWrapper(PCI::PCIConfigurationHeaderCommon* configHeader);
    std::uint8_t read8(std::uint8_t offset) override;
    void write8(std::uint8_t offset, std::uint8_t value) override;
    std::uint16_t read16(std::uint8_t offset) override;
    void write16(std::uint8_t offset, std::uint16_t value) override;
    std::uint32_t read32(std::uint8_t offset) override;
    void write32(std::uint8_t offset, std::uint32_t value) override;
    PCIFunctionConfigurationSpaceWrapper* clone() override;
    bool findCapability(std::uint8_t capabilityID, std::uint8_t* capabilityOffset) override;
    virtual ~MemorymappedFunctionConfigurationSpaceWrapper() = default;
};

class InvalidFunctionConfigurationSpaceWrapper : virtual public PCIFunctionConfigurationSpaceWrapper{
    InvalidFunctionConfigurationSpaceWrapper(){};
public:
    static InvalidFunctionConfigurationSpaceWrapper* getInstance();
    std::uint8_t read8(std::uint8_t offset) override;
    void write8(std::uint8_t offset, std::uint8_t value) override;
    std::uint16_t read16(std::uint8_t offset) override;
    void write16(std::uint8_t offset, std::uint16_t value) override;
    std::uint32_t read32(std::uint8_t offset) override;
    void write32(std::uint8_t offset, std::uint32_t value) override;
    PCIFunctionConfigurationSpaceWrapper* clone() override;
    bool findCapability(std::uint8_t capabilityID, std::uint8_t* capabilityOffset) override;
    virtual ~InvalidFunctionConfigurationSpaceWrapper() = default;
    static void operator delete(InvalidFunctionConfigurationSpaceWrapper* ptr, std::destroying_delete_t);
};

class PCIConfigurationSpaceWrapper {
    void visitBus(std::uint8_t busNumber, std::function<void(PCIFunctionConfigurationSpaceWrapper&)>* visitor);
    void visitDevice(std::uint8_t busNumber, std::uint8_t deviceNumber, std::function<void(PCIFunctionConfigurationSpaceWrapper&)>* visitor);
    void visitFunction(std::uint8_t busNumber, std::uint8_t deviceNumber, std::uint8_t funcNumber, std::function<void(PCIFunctionConfigurationSpaceWrapper&)>* visitor);
   public:
    virtual PCIFunctionConfigurationSpaceWrapper* getFunction(std::uint8_t busNumber, std::uint8_t deviceNumber, std::uint8_t funcNumber) = 0;
    void visitAllFunctions(std::function<void(PCIFunctionConfigurationSpaceWrapper&)>* visitor);
    bool findFunction(std::uint8_t baseClass, std::uint8_t subClass, std::uint8_t interface, PCIFunctionConfigurationSpaceWrapper** out);
    virtual ~PCIConfigurationSpaceWrapper() {};
};

class MemorymappedConfigurationSpaceWrapper : public PCIConfigurationSpaceWrapper {
    PCIe::MemorymappedConfigurationSpaceDescriptionTable* mcfg_desc_table;
    public:
    MemorymappedConfigurationSpaceWrapper(
        PCIe::MemorymappedConfigurationSpaceDescriptionTable* mcfg);
    PCIFunctionConfigurationSpaceWrapper* getFunction(std::uint8_t busNumber, std::uint8_t deviceNumber, std::uint8_t funcNumber) override;
    virtual ~MemorymappedConfigurationSpaceWrapper() = default;
};

class MSICapabilityWrapper {
    PCIFunctionConfigurationSpaceWrapper* function;
    std::uint8_t capabilityOffset;
    void write8(std::uint8_t offset, std::uint8_t value);
    void write16(std::uint8_t offset, std::uint16_t value);
    void write32(std::uint8_t offset, std::uint32_t value);
    std::uint8_t read8(std::uint8_t offset);
    std::uint16_t read16(std::uint8_t offset);
    std::uint32_t read32(std::uint8_t offset);
    public:
    MSICapabilityWrapper(PCIFunctionConfigurationSpaceWrapper& function, std::uint8_t capabilityOffset);
    bool is64BitAddress();
    bool isPerVectorMasking();
    void enable();
    void disable();
    void setMessageAddress(std::uint64_t address);
    void setMessageData(std::uint16_t data);
    ~MSICapabilityWrapper();
};
}  // namespace PCIUtils