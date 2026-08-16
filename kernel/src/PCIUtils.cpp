#include "PCIUtils.hpp"

#include "PCI.hpp"
#include "utils.hpp"

PCIUtils::PCIFunction PCIUtils::MemorymappedConfigurationSpaceWrapper::getFunction(
    std::uint8_t busNumber, std::uint8_t deviceNumber, std::uint8_t funcNumber) {
    PCI::PCIConfigurationHeaderCommon volatile* ret = nullptr;
    std::size_t size = this->mcfg_desc_table->size();
    for (std::size_t i = 0; i < size; i++) {
        auto desc = this->mcfg_desc_table->getDescription(i);
        if (desc->StartPCIBusNumber <= busNumber && busNumber <= desc->EndPCIBusNumber) {
            std::uint64_t baseAddress = desc->getBaseAddress();
            baseAddress &= 0xFFFFFFFFFFFFF000;
            baseAddress += ((busNumber - desc->StartPCIBusNumber) << 20) +
                           (deviceNumber << 15) + (funcNumber << 12);
            auto retPhysPtr = createPhysicalAddress<PCI::PCIConfigurationHeaderCommon>(baseAddress);
            ret = oz::phys_to_virt(retPhysPtr);
            break;
        }
    }
    return PCIFunction(ret);
}

PCIUtils::MemorymappedConfigurationSpaceWrapper::MemorymappedConfigurationSpaceWrapper(
    PCIe::MemorymappedConfigurationSpaceDescriptionTable* mcfg)
    : mcfg_desc_table{mcfg} {}

void PCIUtils::MemorymappedConfigurationSpaceWrapper::visitBus(
    std::uint8_t busNumber, std::function<void(PCIFunction)> visitor) {
    for (std::uint8_t deviceNum = 0; deviceNum <= PCI::DEVICE_NUMBER_MAX; deviceNum++) {
        visitDevice(busNumber, deviceNum, visitor);
    }
}

void PCIUtils::MemorymappedConfigurationSpaceWrapper::visitDevice(
    std::uint8_t busNumber, std::uint8_t deviceNumber, std::function<void(PCIFunction)> visitor) {
    PCIFunction function = getFunction(busNumber, deviceNumber, 0);
    if (!function || !function.isAvailable()) {
        return;
    }
    
    visitFunction(busNumber, deviceNumber, 0, visitor);

    if (function.isMultifunction()) {
        for (std::uint8_t funcNum = 1; funcNum <= PCI::FUNCTION_NUMBER_MAX; funcNum++) {
            PCIFunction functionVisit = getFunction(busNumber, deviceNumber, funcNum);
            if (functionVisit && functionVisit.isAvailable()) {
                visitFunction(busNumber, deviceNumber, funcNum, visitor);
            }
        }
    }
}

void PCIUtils::MemorymappedConfigurationSpaceWrapper::visitFunction(
    std::uint8_t busNumber, std::uint8_t deviceNumber, std::uint8_t funcNumber,
    std::function<void(PCIFunction)> visitor) {
    
    PCIFunction function = getFunction(busNumber, deviceNumber, funcNumber);
    if (function) {
        visitor(function);
        if (function.isBridge()) {
            std::uint8_t secondaryBusNumber = function.getSecondaryBusNumber();
            visitBus(secondaryBusNumber, visitor);
        }
    }
}

void PCIUtils::MemorymappedConfigurationSpaceWrapper::visitAllFunctions(
    std::function<void(PCIFunction)> visitor) {
    std::uint8_t availableBusNumber = 0;
    PCIFunction function = getFunction(0, 0, 0);

    if (!function || !function.isAvailable()) {
        return;
    }

    if (function.isMultifunction()) {
        availableBusNumber = PCI::FUNCTION_NUMBER_MAX;
    }
    
    for (std::uint8_t busNum = 0; busNum <= availableBusNumber; busNum++) {
        PCIFunction f = getFunction(0, 0, busNum);
        if (f && f.isAvailable()) {
            visitBus(busNum, visitor);
        }
    }
}

PCIUtils::PCIFunction PCIUtils::MemorymappedConfigurationSpaceWrapper::findFunction(
    std::uint8_t baseClass, std::uint8_t subClass, std::uint8_t interface) {
    PCIFunction result(nullptr);
    auto visitor = [&](PCIFunction func) {
        if (!result && func.equalsCode(baseClass, subClass, interface)) {
            result = func;
        }
    };
    visitAllFunctions(std::ref(visitor));
    return result;
}

bool PCIUtils::PCIFunction::isAvailable() const {
    return header->VendorID != PCI::NON_EXISTENT_DEVICE_VENDER_ID;
}

bool PCIUtils::PCIFunction::isMultifunction() const {
    return (header->HeaderType & 0b10000000) != 0;
}

bool PCIUtils::PCIFunction::isBridge() const {
    return equalsCode(PCI::PCI_BASE_CLASS_BRIDGE_DEVICE, PCI::PCI_SUB_CLASS_BRIDGE_PCI_TO_PCI);
}

std::uint8_t PCIUtils::PCIFunction::getSecondaryBusNumber() const {
    auto type1 = reinterpret_cast<PCI::PCIConfigurationHeaderType1 volatile*>(header);
    return type1->SecondaryBusNumber;
}

bool PCIUtils::PCIFunction::equalsCode(std::uint8_t baseClass, std::uint8_t subClass, std::uint8_t interface) const {
    return header->BaseClass == baseClass && header->SubClass == subClass && header->Interface == interface;
}

bool PCIUtils::PCIFunction::equalsCode(std::uint8_t baseClass, std::uint8_t subClass) const {
    return header->BaseClass == baseClass && header->SubClass == subClass;
}

bool PCIUtils::PCIFunction::equalsCode(std::uint8_t baseClass) const {
    return header->BaseClass == baseClass;
}

bool PCIUtils::PCIFunction::findCapability(std::uint8_t capabilityID, std::uint8_t* capabilityOffset) const {
    std::uint8_t capabilityPointer = header->CapabilitiesPointer;
    while (capabilityPointer != 0) {
        auto capHeader = reinterpret_cast<PCI::CapabilityHeader volatile*>(
            reinterpret_cast<std::uint8_t volatile*>(header) + capabilityPointer);
        
        if (capHeader->CapabilityID == capabilityID) {
            *capabilityOffset = capabilityPointer;
            return true;
        }
        capabilityPointer = capHeader->NextCapabilityPointer;
    }
    return false;
}

PCIUtils::MSICapabilityWrapper::MSICapabilityWrapper(PCIFunction function, std::uint8_t capabilityOffset)
    : function(function), capabilityOffset(capabilityOffset) {}

bool PCIUtils::MSICapabilityWrapper::is64BitAddress() {
    auto cap = reinterpret_cast<PCI::MSICapability32 volatile*>(
        reinterpret_cast<std::uint8_t volatile*>(function.header) + capabilityOffset);
    return (cap->MessageControl & 0b10000000) != 0;
}

bool PCIUtils::MSICapabilityWrapper::isPerVectorMasking() {
    auto cap = reinterpret_cast<PCI::MSICapability32 volatile*>(
        reinterpret_cast<std::uint8_t volatile*>(function.header) + capabilityOffset);
    return (cap->MessageControl & 0b100000000) != 0;
}

void PCIUtils::MSICapabilityWrapper::enable() {
    auto cap = reinterpret_cast<PCI::MSICapability32 volatile*>(
        reinterpret_cast<std::uint8_t volatile*>(function.header) + capabilityOffset);
    cap->MessageControl = cap->MessageControl | 0b1;
}

void PCIUtils::MSICapabilityWrapper::disable() {
    auto cap = reinterpret_cast<PCI::MSICapability32 volatile*>(
        reinterpret_cast<std::uint8_t volatile*>(function.header) + capabilityOffset);
    cap->MessageControl = cap->MessageControl & ~0b1;
}

void PCIUtils::MSICapabilityWrapper::setMessageAddress(std::uint64_t address) {
    auto cap = reinterpret_cast<PCI::MSICapability32 volatile*>(
        reinterpret_cast<std::uint8_t volatile*>(function.header) + capabilityOffset);
    cap->MessageAddress = static_cast<std::uint32_t>(address);
}

void PCIUtils::MSICapabilityWrapper::setMessageData(std::uint16_t data) {
    if (is64BitAddress()) {
        auto cap = reinterpret_cast<PCI::MSICapability64 volatile*>(
            reinterpret_cast<std::uint8_t volatile*>(function.header) + capabilityOffset);
        cap->MessageData = data;
    } else {
        auto cap = reinterpret_cast<PCI::MSICapability32 volatile*>(
            reinterpret_cast<std::uint8_t volatile*>(function.header) + capabilityOffset);
        cap->MessageData = data;
    }
}

PCIUtils::MSIXCapabilityWrapper::MSIXCapabilityWrapper(PCIFunction function, std::uint8_t capabilityOffset)
    : function(function), capabilityOffset(capabilityOffset) {
    auto cap = reinterpret_cast<PCI::MSIX::CapabilityStructure volatile*>(
        reinterpret_cast<std::uint8_t volatile*>(function.header) + capabilityOffset);
        
    std::uint32_t tableOffset_TableBIR = cap->TableOffset_TableBIR;
    std::uint32_t pbaOffset_PBABIR = cap->PBAOffset_PBABIR;

    std::uint32_t indexBAR = tableOffset_TableBIR & 0b111;
    std::uint32_t tableOffset = tableOffset_TableBIR & ~0b111;

    std::uint64_t tableBIR =
        (function.header->BaseAddressRegister[indexBAR] & ~0b1111) |
        (static_cast<std::uint64_t>(function.header->BaseAddressRegister[indexBAR + 1]) << 32);
    table = reinterpret_cast<PCI::MSIX::TableEntry volatile*>(tableBIR + tableOffset);

    std::uint32_t pbaBAR = pbaOffset_PBABIR & 0b111;
    std::uint32_t pbaOffset = pbaOffset_PBABIR & ~0b111;

    std::uint64_t pbaBIR =
        (function.header->BaseAddressRegister[pbaBAR] & ~0b1111) |
        (static_cast<std::uint64_t>(function.header->BaseAddressRegister[pbaBAR + 1]) << 32);
    pba = reinterpret_cast<PCI::MSIX::PBAEntry volatile*>(pbaBIR + pbaOffset);
}

void PCIUtils::MSIXCapabilityWrapper::enable() {
    auto cap = reinterpret_cast<PCI::MSIX::CapabilityStructure volatile*>(
        reinterpret_cast<std::uint8_t volatile*>(function.header) + capabilityOffset);
    cap->MessageControl = cap->MessageControl | PCI::MSIX::MSIXEnable;
}

void PCIUtils::MSIXCapabilityWrapper::disable() {
    auto cap = reinterpret_cast<PCI::MSIX::CapabilityStructure volatile*>(
        reinterpret_cast<std::uint8_t volatile*>(function.header) + capabilityOffset);
    cap->MessageControl = cap->MessageControl & ~PCI::MSIX::MSIXEnable;
}

void PCIUtils::MSIXCapabilityWrapper::setEntry(std::uint16_t index, std::uint32_t vectorControl,
                                               std::uint32_t messageData, std::uint64_t messageAddress) {
    table[index].MessageAddressLower32Bits = static_cast<std::uint32_t>(messageAddress);
    table[index].MessageAddressUpper32Bits = static_cast<std::uint32_t>(messageAddress >> 32);
    table[index].MessageData = messageData;
    table[index].Vector_Control = vectorControl;
}

std::uint16_t PCIUtils::MSIXCapabilityWrapper::getTableSize() {
    auto cap = reinterpret_cast<PCI::MSIX::CapabilityStructure volatile*>(
        reinterpret_cast<std::uint8_t volatile*>(function.header) + capabilityOffset);
    return cap->MessageControl & PCI::MSIX::TableSize;
}
