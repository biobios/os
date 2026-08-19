#include "drivers/keyboard/HIDKeyboard.hpp"
#include "utils/utils.hpp" // For dprint

namespace HID {

static const char keycode_map[] = {
    0, 0, 0, 0, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', // 0x00 - 0x10
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', // 0x11 - 0x1D
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', // 0x1E - 0x27
    '\n', 0, '\b', '\t', ' ', '-', '=', '[', ']', '\\', 0, ';', '\'', '`', ',', '.', '/' // 0x28 - 0x38
};

static const char shift_keycode_map[] = {
    0, 0, 0, 0, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', // 0x00 - 0x10
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', // 0x11 - 0x1D
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', // 0x1E - 0x27
    '\n', 0, '\b', '\t', ' ', '_', '+', '{', '}', '|', 0, ':', '"', '~', '<', '>', '?' // 0x28 - 0x38
};

Keyboard::Keyboard() {}

char Keyboard::keycodeToAscii(std::uint8_t keycode, bool shift) {
    if (keycode < sizeof(keycode_map)) {
        return shift ? shift_keycode_map[keycode] : keycode_map[keycode];
    }
    return 0;
}

void Keyboard::processReport(const std::uint8_t* report) {
    Modifiers mods;
    std::uint8_t raw_mods = report[0];
    mods.left_ctrl   = (raw_mods & (1 << 0)) != 0;
    mods.left_shift  = (raw_mods & (1 << 1)) != 0;
    mods.left_alt    = (raw_mods & (1 << 2)) != 0;
    mods.left_gui    = (raw_mods & (1 << 3)) != 0;
    mods.right_ctrl  = (raw_mods & (1 << 4)) != 0;
    mods.right_shift = (raw_mods & (1 << 5)) != 0;
    mods.right_alt   = (raw_mods & (1 << 6)) != 0;
    mods.right_gui   = (raw_mods & (1 << 7)) != 0;

    bool shift_pressed = mods.left_shift || mods.right_shift;

    // Check for newly pressed keys
    for (int i = 2; i < 8; ++i) {
        std::uint8_t key = report[i];
        if (key == 0) continue;

        bool is_new_key = true;
        for (int j = 2; j < 8; ++j) {
            if (prev_report_[j] == key) {
                is_new_key = false;
                break;
            }
        }

        if (is_new_key) {
            KeyEvent event;
            event.keycode = key;
            event.ascii = keycodeToAscii(key, shift_pressed);
            event.state = KeyState::Pressed;
            event.modifiers = mods;
            buffer_.push(event);
        }
    }

    // Check for released keys
    for (int i = 2; i < 8; ++i) {
        std::uint8_t prev_key = prev_report_[i];
        if (prev_key == 0) continue;

        bool is_released = true;
        for (int j = 2; j < 8; ++j) {
            if (report[j] == prev_key) {
                is_released = false;
                break;
            }
        }

        if (is_released) {
            KeyEvent event;
            event.keycode = prev_key;
            event.ascii = keycodeToAscii(prev_key, shift_pressed);
            event.state = KeyState::Released;
            event.modifiers = mods;
            buffer_.push(event);
        }
    }

    // Save previous report
    for (int i = 0; i < 8; ++i) {
        prev_report_[i] = report[i];
    }
}

bool Keyboard::pop(KeyEvent& out_event) {
    return buffer_.pop(out_event);
}

} // namespace HID

