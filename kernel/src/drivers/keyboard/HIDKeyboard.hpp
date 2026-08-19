#pragma once

#include <cstdint>
#include "utils/RingBuffer.hpp"
#include "drivers/keyboard/KeyEvent.hpp"

namespace HID {

class Keyboard {
public:
    Keyboard();
    void processReport(const std::uint8_t* report);
    bool pop(KeyEvent& out_event);

private:
    std::uint8_t prev_report_[8]{};
    oz::utils::RingBuffer<KeyEvent, 256> buffer_;
    
    char keycodeToAscii(std::uint8_t keycode, bool shift);
};

} // namespace HID
