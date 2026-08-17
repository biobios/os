#pragma once

#include <cstdint>

namespace HID {

class Keyboard {
public:
    Keyboard();
    void processReport(const std::uint8_t* report);

private:
    std::uint8_t prev_report_[8]{};
    
    char keycodeToAscii(std::uint8_t keycode, bool shift);
};

} // namespace HID
