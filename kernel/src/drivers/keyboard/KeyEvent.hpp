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

} // namespace HID
