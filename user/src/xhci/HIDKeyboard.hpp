#pragma once
#include <cstdint>

namespace HID {

enum class KeyState : std::uint8_t {
    Released = 0,
    Pressed = 1
};

struct Modifiers {
    bool left_ctrl : 1;
    bool left_shift : 1;
    bool left_alt : 1;
    bool left_gui : 1;
    bool right_ctrl : 1;
    bool right_shift : 1;
    bool right_alt : 1;
    bool right_gui : 1;
};

struct KeyEvent {
    std::uint8_t keycode;
    char ascii;
    KeyState state;
    Modifiers modifiers;
};

class Keyboard {
public:
    Keyboard() = default;
    // Process a raw 8-byte HID report. Returns number of events generated.
    // Events are stored internally and can be popped.
    void processReport(const std::uint8_t* report);
    bool pop(KeyEvent& out_event);
private:
    std::uint8_t prev_report_[8]{};
    // Simple ring buffer
    static constexpr int BUF_SIZE = 64;
    KeyEvent buffer_[BUF_SIZE]{};
    int head_{0};
    int tail_{0};
    int count_{0};
    void push(const KeyEvent& e);
    char keycodeToAscii(std::uint8_t keycode, bool shift);
};

} // namespace HID
