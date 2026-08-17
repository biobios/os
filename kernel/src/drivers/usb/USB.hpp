#pragma once

#include <cstdint>

namespace USB {

struct SetupData {
    std::uint8_t request_type;
    std::uint8_t request;
    std::uint16_t value;
    std::uint16_t index;
    std::uint16_t length;
} __attribute__((packed));

enum class RequestType : std::uint8_t {
    Standard = 0,
    Class = 1,
    Vendor = 2,
};

enum class RequestRecipient : std::uint8_t {
    Device = 0,
    Interface = 1,
    Endpoint = 2,
    Other = 3,
};

enum class StandardRequest : std::uint8_t {
    GetStatus = 0,
    ClearFeature = 1,
    SetFeature = 3,
    SetAddress = 5,
    GetDescriptor = 6,
    SetDescriptor = 7,
    GetConfiguration = 8,
    SetConfiguration = 9,
    GetInterface = 10,
    SetInterface = 11,
    SynchFrame = 12,
};

enum class DescriptorType : std::uint8_t {
    Device = 1,
    Configuration = 2,
    String = 3,
    Interface = 4,
    Endpoint = 5,
    DeviceQualifier = 6,
    OtherSpeedConfiguration = 7,
    InterfacePower = 8,
    HID = 0x21,
    Report = 0x22,
};

struct DescriptorHeader {
    std::uint8_t length;
    std::uint8_t descriptor_type;
} __attribute__((packed));

struct DeviceDescriptor {
    std::uint8_t length;
    std::uint8_t descriptor_type;
    std::uint16_t usb_release;
    std::uint8_t device_class;
    std::uint8_t device_subclass;
    std::uint8_t device_protocol;
    std::uint8_t max_packet_size;
    std::uint16_t vendor_id;
    std::uint16_t product_id;
    std::uint16_t device_release;
    std::uint8_t manufacturer;
    std::uint8_t product;
    std::uint8_t serial_number;
    std::uint8_t num_configurations;
} __attribute__((packed));

struct ConfigurationDescriptor {
    std::uint8_t length;
    std::uint8_t descriptor_type;
    std::uint16_t total_length;
    std::uint8_t num_interfaces;
    std::uint8_t configuration_value;
    std::uint8_t configuration_id;
    std::uint8_t attributes;
    std::uint8_t max_power;
} __attribute__((packed));

struct InterfaceDescriptor {
    std::uint8_t length;
    std::uint8_t descriptor_type;
    std::uint8_t interface_number;
    std::uint8_t alternate_setting;
    std::uint8_t num_endpoints;
    std::uint8_t interface_class;
    std::uint8_t interface_subclass;
    std::uint8_t interface_protocol;
    std::uint8_t interface_id;
} __attribute__((packed));

struct EndpointDescriptor {
    std::uint8_t length;
    std::uint8_t descriptor_type;
    std::uint8_t endpoint_address;
    std::uint8_t attributes;
    std::uint16_t max_packet_size;
    std::uint8_t interval;
} __attribute__((packed));

} // namespace USB
