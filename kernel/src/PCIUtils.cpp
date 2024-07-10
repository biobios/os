#include "PCIUtils.hpp"

#include "utils.hpp"

PCIUtils::PCIFunctionConfigurationSpaceWrapper*
PCIUtils::MemorymappedConfigurationSpaceWrapper::getFunction(
    std::uint8_t busNumber, std::uint8_t deviceNumber,
    std::uint8_t funcNumber) {
    PCI::PCIConfigurationHeaderCommon* ret = nullptr;
    std::size_t size = this->mcfg_desc_table->size();
    for (std::size_t i = 0; i < size; i++) {
        PCIe::MemorymappedConfigurationSpaceDescription* desc =
            mcfg_desc_table->getDescription(i);
        if (desc->StartPCIBusNumber <= busNumber &&
            desc->EndPCIBusNumber >= busNumber) {
            std::uint64_t baseAddress = desc->BaseAddress.getData();
            baseAddress &= 0xFFFFFFFFFFFFF000;
            baseAddress += ((busNumber - desc->StartPCIBusNumber) << 20) +
                           (deviceNumber << 15) + (funcNumber << 12);
            ret = reinterpret_cast<PCI::PCIConfigurationHeaderCommon*>(
                baseAddress);
            break;
        }
    }
    if (ret == nullptr) {
        dprint(
            "PCIUtils::MemorymappedConfigurationSpaceWrapper::getFunction: ret "
            "== nullptr\n\r");
        while (true) {
            __asm__ volatile("hlt");
        }

        return InvalidFunctionConfigurationSpaceWrapper::getInstance();
    }
    return new MemorymappedFunctionConfigurationSpaceWrapper(ret);
}

void PCIUtils::PCIConfigurationSpaceWrapper::visitBus(
    std::uint8_t busNumber,
    std::function<void(PCIFunctionConfigurationSpaceWrapper&)>* visitor) {
    for (std::uint8_t deviceNum = 0; deviceNum <= PCI::DEVICE_NUMBER_MAX;
         deviceNum++) {
        visitDevice(busNumber, deviceNum, visitor);
    }
}

void PCIUtils::PCIConfigurationSpaceWrapper::visitDevice(
    std::uint8_t busNumber, std::uint8_t deviceNumber,
    std::function<void(PCIFunctionConfigurationSpaceWrapper&)>* visitor) {
    PCIFunctionConfigurationSpaceWrapper* function =
        getFunction(busNumber, deviceNumber, 0);
    if (!function->isAvailable()) {
        delete function;
        return;
    }

    visitFunction(busNumber, deviceNumber, 0, visitor);

    if (function->isMultifunction()) {
        for (std::uint8_t funcNum = 1; funcNum <= PCI::FUNCTION_NUMBER_MAX;
             funcNum++) {
            PCIFunctionConfigurationSpaceWrapper* functionVisit =
                getFunction(busNumber, deviceNumber, funcNum);
            if (functionVisit->isAvailable()) {
                visitFunction(busNumber, deviceNumber, funcNum, visitor);
            }
            delete functionVisit;
        }
    }
    delete function;
}

void PCIUtils::PCIConfigurationSpaceWrapper::visitFunction(
    std::uint8_t busNumber, std::uint8_t deviceNumber, std::uint8_t funcNumber,
    std::function<void(PCIFunctionConfigurationSpaceWrapper&)>* visitor) {
    PCIFunctionConfigurationSpaceWrapper* function =
        getFunction(busNumber, deviceNumber, funcNumber);
    (*visitor)(*function);
    if (function->isBridge()) {
        std::uint8_t secondaryBusNumber = function->getSecondaryBusNumber();
        visitBus(secondaryBusNumber, visitor);
    }
    delete function;
}

PCIUtils::MemorymappedConfigurationSpaceWrapper::
    MemorymappedConfigurationSpaceWrapper(
        PCIe::MemorymappedConfigurationSpaceDescriptionTable* mcfg)
    : mcfg_desc_table{mcfg} {}

void PCIUtils::PCIConfigurationSpaceWrapper::visitAllFunctions(
    std::function<void(PCIFunctionConfigurationSpaceWrapper&)>* visitor) {
    std::uint8_t availableBusNumber = 0;
    PCIFunctionConfigurationSpaceWrapper* function = getFunction(0, 0, 0);

    if (!function->isAvailable()) {
        delete function;
        return;
    }

    if (function->isMultifunction()) {
        availableBusNumber = PCI::FUNCTION_NUMBER_MAX;
    }
    delete function;
    for (std::uint8_t busNum = 0; busNum <= availableBusNumber; busNum++) {
        function = getFunction(0, 0, busNum);
        if (function->isAvailable()) {
            visitBus(busNum, visitor);
        }
        delete function;
    }
}

namespace {
class FindVisitor {
    std::uint8_t baseClass;
    std::uint8_t subClass;
    std::uint8_t interface;
    PCIUtils::PCIFunctionConfigurationSpaceWrapper** out;

   public:
    FindVisitor(PCIUtils::PCIFunctionConfigurationSpaceWrapper** _out,
                std::uint8_t _baseClass, std::uint8_t _subClass,
                std::uint8_t _interface)
        : baseClass{_baseClass},
          subClass{_subClass},
          interface{_interface},
          out{_out} {}
    void operator()(PCIUtils::PCIFunctionConfigurationSpaceWrapper& config) {
        if (*out != nullptr) return;
        if (config.equalsCode(baseClass, subClass, interface)) {
            *out = config.clone();
        }
    }
    bool isFound() { return *out != nullptr; }
};
}  // namespace

bool PCIUtils::PCIConfigurationSpaceWrapper::findFunction(
    std::uint8_t baseClass, std::uint8_t subClass, std::uint8_t interface,
    PCIFunctionConfigurationSpaceWrapper** out) {
    FindVisitor visitor{out, baseClass, subClass, interface};
    std::reference_wrapper<FindVisitor> visitorWrapper{visitor};
    std::function<void(PCIFunctionConfigurationSpaceWrapper&)> visitorPtr{
        visitorWrapper};
    visitAllFunctions(&visitorPtr);
    return visitor.isFound();
}

bool PCIUtils::PCIFunctionConfigurationSpaceWrapper::isAvailable() {
    return read32(offsetof(PCI::PCIConfigurationHeaderCommon, VendorID)) !=
           PCI::NON_EXISTENT_DEVICE_VENDER_ID;
}

bool PCIUtils::PCIFunctionConfigurationSpaceWrapper::isMultifunction() {
    return (read8(offsetof(PCI::PCIConfigurationHeaderCommon, HeaderType)) &
            0b10000000) != 0;
}

bool PCIUtils::PCIFunctionConfigurationSpaceWrapper::isBridge() {
    return equalsCode(PCI::PCI_BASE_CLASS_BRIDGE_DEVICE,
                      PCI::PCI_SUB_CLASS_BRIDGE_PCI_TO_PCI);
}

std::uint8_t
PCIUtils::PCIFunctionConfigurationSpaceWrapper::getSecondaryBusNumber() {
    return read8(
        offsetof(PCI::PCIConfigurationHeaderType1, SecondaryBusNumber));
}

bool PCIUtils::PCIFunctionConfigurationSpaceWrapper::equalsCode(
    std::uint8_t baseClass, std::uint8_t subClass, std::uint8_t interface) {
    return read8(offsetof(PCI::PCIConfigurationHeaderCommon, BaseClass)) ==
               baseClass &&
           read8(offsetof(PCI::PCIConfigurationHeaderCommon, SubClass)) ==
               subClass &&
           read8(offsetof(PCI::PCIConfigurationHeaderCommon, Interface)) ==
               interface;
}

bool PCIUtils::PCIFunctionConfigurationSpaceWrapper::equalsCode(
    std::uint8_t baseClass, std::uint8_t subClass) {
    return read8(offsetof(PCI::PCIConfigurationHeaderCommon, BaseClass)) ==
               baseClass &&
           read8(offsetof(PCI::PCIConfigurationHeaderCommon, SubClass)) ==
               subClass;
}

bool PCIUtils::PCIFunctionConfigurationSpaceWrapper::equalsCode(
    std::uint8_t baseClass) {
    return read8(offsetof(PCI::PCIConfigurationHeaderCommon, BaseClass)) ==
           baseClass;
}

PCIUtils::PCIFunctionConfigurationSpaceWrapper*
PCIUtils::MemorymappedFunctionConfigurationSpaceWrapper::clone() {
    return new MemorymappedFunctionConfigurationSpaceWrapper(*this);
}

bool PCIUtils::MemorymappedFunctionConfigurationSpaceWrapper::findCapability(
    std::uint8_t capabilityID, std::uint8_t* capabilityOffset) {
    std::uint8_t capabilityPointer =
        read8(offsetof(PCI::PCIConfigurationHeaderCommon, CapabilitiesPointer));
    while (capabilityPointer != 0) {
        std::uint8_t capabilityIDInHeader = read8(
            capabilityPointer + offsetof(PCI::CapabilityHeader, CapabilityID));
        if (capabilityIDInHeader == capabilityID) {
            *capabilityOffset = capabilityPointer;
            return true;
        }
        capabilityPointer =
            read8(capabilityPointer +
                  offsetof(PCI::CapabilityHeader, NextCapabilityPointer));
    }
    return false;
}

PCIUtils::MemorymappedFunctionConfigurationSpaceWrapper::
    MemorymappedFunctionConfigurationSpaceWrapper(
        PCI::PCIConfigurationHeaderCommon* configHeader)
    : configCommonPtr{configHeader} {}

std::uint8_t PCIUtils::MemorymappedFunctionConfigurationSpaceWrapper::read8(
    std::uint8_t offset) {
    return reinterpret_cast<std::uint8_t volatile*>(configCommonPtr)[offset];
}

void PCIUtils::MemorymappedFunctionConfigurationSpaceWrapper::write8(
    std::uint8_t offset, std::uint8_t value) {
    reinterpret_cast<std::uint8_t volatile*>(configCommonPtr)[offset] = value;
}

std::uint16_t PCIUtils::MemorymappedFunctionConfigurationSpaceWrapper::read16(
    std::uint8_t offset) {
    return *reinterpret_cast<std::uint16_t volatile*>(
        reinterpret_cast<std::uint8_t volatile*>(configCommonPtr) + offset);
}

void PCIUtils::MemorymappedFunctionConfigurationSpaceWrapper::write16(
    std::uint8_t offset, std::uint16_t value) {
    *reinterpret_cast<std::uint16_t volatile*>(
        reinterpret_cast<std::uint8_t volatile*>(configCommonPtr) + offset) =
        value;
}

std::uint32_t PCIUtils::MemorymappedFunctionConfigurationSpaceWrapper::read32(
    std::uint8_t offset) {
    return *reinterpret_cast<std::uint32_t volatile*>(
        reinterpret_cast<std::uint8_t volatile*>(configCommonPtr) + offset);
}

void PCIUtils::MemorymappedFunctionConfigurationSpaceWrapper::write32(
    std::uint8_t offset, std::uint32_t value) {
    *reinterpret_cast<std::uint32_t volatile*>(
        reinterpret_cast<std::uint8_t volatile*>(configCommonPtr) + offset) =
        value;
}

PCIUtils::InvalidFunctionConfigurationSpaceWrapper*
PCIUtils::InvalidFunctionConfigurationSpaceWrapper::getInstance() {
    static InvalidFunctionConfigurationSpaceWrapper instance;
    return &instance;
}

std::uint8_t PCIUtils::InvalidFunctionConfigurationSpaceWrapper::read8(
    std::uint8_t offset) {
    return ~0;
}

void PCIUtils::InvalidFunctionConfigurationSpaceWrapper::write8(
    std::uint8_t offset, std::uint8_t value) {}

std::uint16_t PCIUtils::InvalidFunctionConfigurationSpaceWrapper::read16(
    std::uint8_t offset) {
    return ~0;
}

void PCIUtils::InvalidFunctionConfigurationSpaceWrapper::write16(
    std::uint8_t offset, std::uint16_t value) {}

std::uint32_t PCIUtils::InvalidFunctionConfigurationSpaceWrapper::read32(
    std::uint8_t offset) {
    return ~0;
}

void PCIUtils::InvalidFunctionConfigurationSpaceWrapper::write32(
    std::uint8_t offset, std::uint32_t value) {}

PCIUtils::PCIFunctionConfigurationSpaceWrapper*
PCIUtils::InvalidFunctionConfigurationSpaceWrapper::clone() {
    return getInstance();
}

bool PCIUtils::InvalidFunctionConfigurationSpaceWrapper::findCapability(
    std::uint8_t capabilityID, std::uint8_t* capabilityOffset) {
    return false;
}

void PCIUtils::InvalidFunctionConfigurationSpaceWrapper::operator delete(
    InvalidFunctionConfigurationSpaceWrapper* ptr, std::destroying_delete_t) {}

void PCIUtils::MSICapabilityWrapper::write8(std::uint8_t offset,
                                            std::uint8_t value) {
    function->write8(capabilityOffset + offset, value);
}

void PCIUtils::MSICapabilityWrapper::write16(std::uint8_t offset,
                                             std::uint16_t value) {
    function->write16(capabilityOffset + offset, value);
}

void PCIUtils::MSICapabilityWrapper::write32(std::uint8_t offset,
                                             std::uint32_t value) {
    function->write32(capabilityOffset + offset, value);
}

std::uint8_t PCIUtils::MSICapabilityWrapper::read8(std::uint8_t offset) {
    return function->read8(capabilityOffset + offset);
}

std::uint16_t PCIUtils::MSICapabilityWrapper::read16(std::uint8_t offset) {
    return function->read16(capabilityOffset + offset);
}

std::uint32_t PCIUtils::MSICapabilityWrapper::read32(std::uint8_t offset) {
    return function->read32(capabilityOffset + offset);
}

PCIUtils::MSICapabilityWrapper::MSICapabilityWrapper(
    PCIFunctionConfigurationSpaceWrapper& function,
    std::uint8_t capabilityOffset)
    : capabilityOffset{capabilityOffset} {
    this->function = function.clone();
}

bool PCIUtils::MSICapabilityWrapper::is64BitAddress() {
    return (read16(offsetof(PCI::MSICapability32, MessageControl)) &
            0b10000000) != 0;
}

bool PCIUtils::MSICapabilityWrapper::isPerVectorMasking() {
    return (read16(offsetof(PCI::MSICapability32, MessageControl)) &
            0b100000000) != 0;
}

void PCIUtils::MSICapabilityWrapper::enable() {
    write16(offsetof(PCI::MSICapability32, MessageControl),
            read16(offsetof(PCI::MSICapability32, MessageControl)) | 0b1);
}

void PCIUtils::MSICapabilityWrapper::disable() {
    write16(offsetof(PCI::MSICapability32, MessageControl),
            read16(offsetof(PCI::MSICapability32, MessageControl)) & ~0b1);
}

void PCIUtils::MSICapabilityWrapper::setMessageAddress(std::uint64_t address) {
    write32(offsetof(PCI::MSICapability32, MessageAddress), address);
}

void PCIUtils::MSICapabilityWrapper::setMessageData(std::uint16_t data) {
    if (is64BitAddress()) {
        write16(offsetof(PCI::MSICapability64, MessageData), data);
    } else {
        write16(offsetof(PCI::MSICapability32, MessageData), data);
    }
}

PCIUtils::MSICapabilityWrapper::~MSICapabilityWrapper() { delete function; }
