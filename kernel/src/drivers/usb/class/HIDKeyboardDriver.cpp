#include "drivers/usb/class/HIDKeyboardDriver.hpp"
#include "drivers/usb/USBUtils.hpp"

namespace USBClassDriver {

std::uint8_t HIDKeyboardDriver::parseConfiguration(USB::ConfigurationDescriptor* conf_desc, std::uint16_t& out_max_packet_size, std::uint8_t& out_interval) {
    USB::InterfaceDescriptor* intf = USBUtils::findInterface(conf_desc, 3, 1, 1);
    if (!intf) {
        return 0;
    }
    
    USB::EndpointDescriptor* ep = USBUtils::findEndpoint(conf_desc, intf, true, 3);
    if (!ep) {
        return 0;
    }
    
    out_max_packet_size = ep->max_packet_size;
    out_interval = ep->interval;
    return ep->endpoint_address;
}

} // namespace USBClassDriver
