#include "drivers/usb/USBUtils.hpp"

namespace USBUtils {

USB::InterfaceDescriptor* findInterface(USB::ConfigurationDescriptor* conf_desc, std::uint8_t class_code, std::uint8_t subclass_code, std::uint8_t protocol) {
    std::uint8_t* ptr = reinterpret_cast<std::uint8_t*>(conf_desc);
    std::uint8_t* end = ptr + static_cast<std::uint16_t>(conf_desc->total_length);
    
    while (ptr < end) {
        USB::DescriptorHeader* header = reinterpret_cast<USB::DescriptorHeader*>(ptr);
        if (header->length == 0) break;
        
        if (header->descriptor_type == static_cast<std::uint8_t>(USB::DescriptorType::Interface)) {
            USB::InterfaceDescriptor* intf = reinterpret_cast<USB::InterfaceDescriptor*>(ptr);
            if (intf->interface_class == class_code && 
                intf->interface_subclass == subclass_code && 
                intf->interface_protocol == protocol) {
                return intf;
            }
        }
        ptr += header->length;
    }
    return nullptr;
}

USB::EndpointDescriptor* findEndpoint(USB::ConfigurationDescriptor* conf_desc, USB::InterfaceDescriptor* intf, bool is_in, std::uint8_t type) {
    std::uint8_t* ptr = reinterpret_cast<std::uint8_t*>(intf) + intf->length;
    std::uint8_t* end = reinterpret_cast<std::uint8_t*>(conf_desc) + static_cast<std::uint16_t>(conf_desc->total_length);
    
    while (ptr < end) {
        USB::DescriptorHeader* header = reinterpret_cast<USB::DescriptorHeader*>(ptr);
        if (header->length == 0) break;
        
        if (header->descriptor_type == static_cast<std::uint8_t>(USB::DescriptorType::Interface)) {
            break;
        }
        
        if (header->descriptor_type == static_cast<std::uint8_t>(USB::DescriptorType::Endpoint)) {
            USB::EndpointDescriptor* ep = reinterpret_cast<USB::EndpointDescriptor*>(ptr);
            bool ep_is_in = (ep->endpoint_address & 0x80) != 0;
            std::uint8_t ep_type = ep->attributes & 0x03;
            
            if (ep_is_in == is_in && ep_type == type) {
                return ep;
            }
        }
        ptr += header->length;
    }
    return nullptr;
}

} // namespace USBUtils
