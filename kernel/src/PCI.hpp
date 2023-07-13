#pragma once

#include <cstdint>

namespace PCI{
    struct PCIConfigurationHeaderCommon{
        std::uint16_t VendorID;
        std::uint16_t DeviceID;
        std::uint16_t Command;
        std::uint16_t Status;
        std::uint8_t RevisionID;
        std::uint8_t Interface;
        std::uint8_t SubClass;
        std::uint8_t BaseClass;
        std::uint8_t CacheLineSize;
        std::uint8_t MasterLatencyTimer;
        std::uint8_t HeaderType;
        std::uint8_t BIST;
        std::uint32_t BaseAddressRegister[6];
        std::uint32_t Reserved1;
        std::uint16_t SubsystemVendorID;
        std::uint16_t SubsystemDeviceID;
        std::uint32_t Reserved2;
        std::uint8_t CapabilitiesPointer;
        std::uint8_t Reserved3[3];
        std::uint32_t Reserved4;
        std::uint8_t InterruptLine;
        std::uint8_t InterruptPin;
        std::uint8_t something[2];
    };

    struct PCIConfigurationHeaderType0{
        std::uint16_t VendorID;
        std::uint16_t DeviceID;
        std::uint16_t Command;
        std::uint16_t Status;
        std::uint8_t RevisionID;
        std::uint8_t Interface;
        std::uint8_t SubClass;
        std::uint8_t BaseClass;
        std::uint8_t CacheLineSize;
        std::uint8_t MasterLatencyTimer;
        std::uint8_t HeaderType;
        std::uint8_t BIST;
        std::uint32_t BaseAddressRegister[6];
        std::uint32_t Reserved1;
        std::uint16_t SubsystemVendorID;
        std::uint16_t SubsystemDeviceID;
        std::uint32_t Reserved2;
        std::uint8_t CapabilitiesPointer;
        std::uint8_t Reserved3[3];
        std::uint32_t Reserved4;
        std::uint8_t InterruptLine;
        std::uint8_t InterruptPin;
        std::uint8_t something[2];
    };

    struct PCIConfigurationHeaderType1{
        std::uint16_t VendorID;
        std::uint16_t DeviceID;
        std::uint16_t Command;
        std::uint16_t Status;
        std::uint8_t RevisionID;
        std::uint8_t Interface;
        std::uint8_t SubClass;
        std::uint8_t BaseClass;
        std::uint8_t CacheLineSize;
        std::uint8_t MasterLatencyTimer;
        std::uint8_t HeaderType;
        std::uint8_t BIST;
        std::uint32_t BaseAddressRegister[2];
        std::uint8_t PrimaryBusNumber;
        std::uint8_t SecondaryBusNumber;
        std::uint8_t SubordinateBusNumber;
        std::uint8_t SecondaryLatencyTimer;
        std::uint8_t IOBase;
        std::uint8_t IOLimit;
        std::uint16_t SecondaryStatus;
        std::uint16_t MemoryBase;
        std::uint16_t MemoryLimit;
        std::uint16_t PrefetchableMemoryBase;
        std::uint16_t PrefetchableMemoryLimit;
        std::uint32_t PrefetchableBaseUpper32Bits;
        std::uint32_t PrefetchableLimitUpper32Bits;
        std::uint16_t IOBaseUpper16Bits;
        std::uint16_t IOLimitUpper16Bits;
        std::uint8_t CapabilitiesPointer;
        std::uint8_t Reserved[3];
        std::uint32_t ExpansionROMBaseAddress;
        std::uint8_t InterruptLine;
        std::uint8_t InterruptPin;
        std::uint16_t BridgeControl;
    };

    constexpr std::uint16_t NON_EXISTENT_DEVICE_VENDER_ID = 0xFFFF;
    constexpr std::uint8_t BUS_NUMBER_MAX = 0xFF;
    constexpr std::uint8_t DEVICE_NUMBER_MAX = 0x1F;
    constexpr std::uint8_t FUNCTION_NUMBER_MAX = 0x7;
    constexpr std::uint8_t PCI_BASE_CLASS_BRIDGE_DEVICE = 0x06;
    constexpr std::uint8_t PCI_SUB_CLASS_BRIDGE_PCI_TO_PCI = 0x04;
    constexpr std::uint8_t PCI_BASE_CLASS_SERIAL_BUS_CONTROLLER = 0x0C;
    constexpr std::uint8_t PCI_SUB_CLASS_USB_CONTROLLER = 0x03;
    constexpr std::uint8_t PCI_INTERFACE_USB_XHCI = 0x30;
}