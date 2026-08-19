#pragma once

#include <cstdint>
#include "utils/RingBuffer.hpp"

namespace HID {

class Keyboard {
public:
    Keyboard();
    void processReport(const std::uint8_t* report);
    bool pop(char& out_char);

private:
    std::uint8_t prev_report_[8]{};
    oz::utils::RingBuffer<char, 256> buffer_;
    
    char keycodeToAscii(std::uint8_t keycode, bool shift);
};

} // namespace HID
