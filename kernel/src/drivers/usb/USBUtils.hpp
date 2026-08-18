#pragma once
#include "drivers/usb/USB.hpp"

namespace USBUtils {

// Config descriptorの全体から、特定のInterfaceDescriptorを探す
USB::InterfaceDescriptor* findInterface(USB::ConfigurationDescriptor* conf_desc, std::uint8_t class_code, std::uint8_t subclass_code, std::uint8_t protocol);

// Interfaceに属するEndpointDescriptorを探す
// is_in: trueならIN, falseならOUT
// type: 3 ならInterruptなど
USB::EndpointDescriptor* findEndpoint(USB::ConfigurationDescriptor* conf_desc, USB::InterfaceDescriptor* intf, bool is_in, std::uint8_t type);

} // namespace USBUtils
