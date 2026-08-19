#pragma once

#include "drivers/usb/USB.hpp"
#include "drivers/keyboard/HIDKeyboard.hpp"
#include <cstdint>

#include "drivers/usb/class/ClassDriver.hpp"

namespace USBClassDriver {

class HIDKeyboardDriver : public ClassDriver {
public:
    HIDKeyboardDriver() = default;

    std::uint8_t matchAndParseConfiguration(USB::ConfigurationDescriptor* conf_desc, std::uint16_t& out_max_packet_size, std::uint8_t& out_interval) override {
        return parseConfiguration(conf_desc, out_max_packet_size, out_interval);
    }

    std::uint8_t parseConfiguration(USB::ConfigurationDescriptor* conf_desc, std::uint16_t& out_max_packet_size, std::uint8_t& out_interval);
    
    void processReport(std::uint8_t* report_buffer) override {
        keyboard_.processReport(report_buffer);
    }

private:
    HID::Keyboard keyboard_;
};

} // namespace USBClassDriver
